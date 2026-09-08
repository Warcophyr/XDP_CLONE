from tqdm import tqdm
from time import sleep
import csv
import statistics
import os
import shlex
import subprocess as sp
import sys
import threading

sys.path.append("/trex/v3.08/automation/trex_control_plane/interactive")
from trex.stl.api import STLClient, STLProfile

import bench_common as bench
from bench_common import launch_program, stop_program

PROFILE_FILE = bench.profile("zipf-profile.py")
RESULTS_CSV_FILE = bench.result("ndr_results.csv")
SUMMARY_CSV_FILE = bench.result("ndr_summary.csv")

PORTS = [0]
TREX_SERVER = "100.78.72.16"

WARMUP_SECONDS = 2
MEASURE_SECONDS = 4
SUCCESS_THRESHOLD = 0.99
RX_CAP_MPPS = 30.0

NDR_START_TX_MPPS = 0.25
NDR_BINARY_STEPS = 8
NDR_MIN_RATE_STEP_MPPS = 0.02
CONFIRM_REPETITIONS = 3


def _value_from_candidates(stats_dict, candidates, metric_name):
    for key in candidates:
        if key in stats_dict:
            return float(stats_dict[key])
    raise RuntimeError(
        f"Missing {metric_name} in TRex stats. Available keys: {list(stats_dict.keys())}"
    )


def _read_port_counters(stats, port_id=0):
    port_stats = stats.get(port_id)
    if port_stats is None:
        port_stats = stats.get(str(port_id))
    if port_stats is None:
        raise RuntimeError(f"No stats found for port {port_id}")

    tx_pkts = _value_from_candidates(
        port_stats, ["opackets", "tx_pkts", "tx_packets"], "tx packets"
    )
    rx_pkts = _value_from_candidates(
        port_stats, ["ipackets", "rx_pkts", "rx_packets"], "rx packets"
    )
    return {"tx_pkts": tx_pkts, "rx_pkts": rx_pkts}


def _fanout_multiplier(configured_copies):
    # expected_rx = tx * (copies + 1): original packet + N clones
    return float(configured_copies + 1)


def _tx_cap_mpps(configured_copies):
    fanout = _fanout_multiplier(configured_copies)
    return RX_CAP_MPPS / fanout


def _mpps_to_pps_string(rate_mpps):
    pps = max(1, int(round(rate_mpps * 1_000_000)))
    return f"{pps}pps"


def _mean_std(values):
    if not values:
        return None, None
    mean = statistics.fmean(values)
    stddev = statistics.stdev(values) if len(values) > 1 else 0.0
    return mean, stddev


def setup_trex():
    client = STLClient(server=TREX_SERVER)
    client.connect()
    client.acquire(ports=PORTS, force=True)
    client.reset(ports=PORTS)

    streams = STLProfile.load_py(
        PROFILE_FILE,
        direction=0,
        port_id=0,
    ).get_streams()
    client.add_streams(streams, ports=PORTS)
    return client


def run_probe(client, configured_copies, requested_tx_mpps):
    tx_cap = _tx_cap_mpps(configured_copies)
    requested_tx_mpps = min(requested_tx_mpps, tx_cap)
    fanout = _fanout_multiplier(configured_copies)
    mult = _mpps_to_pps_string(requested_tx_mpps)

    tqdm.write(
        f"Probe @ copies={configured_copies} tx_target={requested_tx_mpps:.4f} Mpps "
        f"(tx_cap={tx_cap:.4f}, fanout={fanout:.0f}x, rx_cap={RX_CAP_MPPS:.2f})"
    )

    try:
        client.start(ports=PORTS, force=True, mult=mult)
        sleep(WARMUP_SECONDS)
        before = _read_port_counters(client.get_stats(ports=PORTS), port_id=PORTS[0])
        sleep(MEASURE_SECONDS)
        after = _read_port_counters(client.get_stats(ports=PORTS), port_id=PORTS[0])
    finally:
        client.stop(ports=PORTS)

    tx_delta = max(0.0, after["tx_pkts"] - before["tx_pkts"])
    rx_delta = max(0.0, after["rx_pkts"] - before["rx_pkts"])

    measured_tx_mpps = tx_delta / (MEASURE_SECONDS * 1_000_000.0)
    measured_rx_mpps = rx_delta / (MEASURE_SECONDS * 1_000_000.0)
    # expected_rx = tx * fanout: each sent packet becomes (copies+1) packets at the receiver
    expected_rx_mpps = measured_tx_mpps * fanout
    delivered_input_equiv_mpps = measured_rx_mpps / fanout

    delivery_ratio = 0.0
    if expected_rx_mpps > 0:
        delivery_ratio = measured_rx_mpps / expected_rx_mpps

    success = delivery_ratio >= SUCCESS_THRESHOLD

    tqdm.write(
        "Measured: "
        f"tx={measured_tx_mpps:.4f} Mpps "
        f"rx={measured_rx_mpps:.4f} Mpps "
        f"expected_rx={expected_rx_mpps:.4f} Mpps "
        f"ratio={delivery_ratio:.4f} "
        f"input_eq={delivered_input_equiv_mpps:.4f} Mpps "
        f"{'OK' if success else 'LOSS'}"
    )

    return {
        "requested_tx_mpps": requested_tx_mpps,
        "tx_cap_mpps": tx_cap,
        # True when the requested rate was clamped to RX_CAP_MPPS / fanout. A
        # probe that succeeded while capped says "no loss up to the cap", not
        # "this is the maximum": the search never got to look higher.
        "rate_capped": requested_tx_mpps >= tx_cap - 1e-9,
        "measured_tx_mpps": measured_tx_mpps,
        "measured_rx_mpps": measured_rx_mpps,
        "expected_rx_mpps": expected_rx_mpps,
        "delivered_input_equiv_mpps": delivered_input_equiv_mpps,
        "delivery_ratio": delivery_ratio,
        "fanout_multiplier": fanout,
        "success": success,
    }


def find_ndr(client, configured_copies):
    """Binary search for the highest TX rate with delivery_ratio >= SUCCESS_THRESHOLD."""
    tx_cap = _tx_cap_mpps(configured_copies)
    search_probes = []
    best_probe = None

    start_rate = min(NDR_START_TX_MPPS, tx_cap)
    first_probe = run_probe(client, configured_copies, start_rate)
    search_probes.append({"phase": "search", "repetition": 0, **first_probe})

    low = 0.0
    high = None

    if first_probe["success"]:
        best_probe = first_probe
        low = first_probe["requested_tx_mpps"]
        current = low

        # Exponential ramp-up until first failure or cap
        while current < tx_cap:
            next_rate = min(current * 2.0, tx_cap)
            if next_rate - current < NDR_MIN_RATE_STEP_MPPS:
                break
            probe = run_probe(client, configured_copies, next_rate)
            search_probes.append({"phase": "search", "repetition": 0, **probe})
            if probe["success"]:
                best_probe = probe
                low = next_rate
                current = next_rate
                if next_rate >= tx_cap:
                    return best_probe, tx_cap, search_probes
            else:
                high = next_rate
                break

        if high is None:
            return best_probe, low, search_probes
    else:
        high = first_probe["requested_tx_mpps"]

    # Binary search between low (last pass) and high (first fail)
    for _ in range(NDR_BINARY_STEPS):
        if high - low < NDR_MIN_RATE_STEP_MPPS:
            break
        mid = (low + high) / 2.0
        if mid <= 0.0:
            break
        probe = run_probe(client, configured_copies, mid)
        search_probes.append({"phase": "search", "repetition": 0, **probe})
        if probe["success"]:
            best_probe = probe
            low = mid
        else:
            high = mid

    return best_probe, low if best_probe else None, search_probes


def confirm_ndr(client, configured_copies, ndr_rate):
    """Run CONFIRM_REPETITIONS probes at ndr_rate to validate the result."""
    confirm_probes = []
    for repetition in range(1, CONFIRM_REPETITIONS + 1):
        probe = run_probe(client, configured_copies, ndr_rate)
        confirm_probes.append({"phase": "confirm", "repetition": repetition, **probe})
    return confirm_probes


def save_probe_results_csv(results, csv_file=RESULTS_CSV_FILE):
    fieldnames = [
        "application",
        "configured_copies",
        "probe_id",
        "phase",
        "repetition",
        "requested_tx_mpps",
        "tx_cap_mpps",
        "rate_capped",
        "measured_tx_mpps",
        "measured_rx_mpps",
        "expected_rx_mpps",
        "delivered_input_equiv_mpps",
        "fanout_multiplier",
        "delivery_ratio",
        "success",
    ]
    with open(csv_file, "w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(results)


def save_summary_csv(per_copy_summary, csv_file=SUMMARY_CSV_FILE):
    fieldnames = [
        "scope",
        "application",
        "configured_copies",
        "tx_cap_mpps",
        "rate_capped",
        "ndr_tx_mpps",
        "ndr_rx_mpps",
        "ndr_input_equiv_mpps",
        "fanout_multiplier",
        "confirm_tx_mpps_mean",
        "confirm_tx_mpps_stddev",
        "confirm_rx_mpps_mean",
        "confirm_rx_mpps_stddev",
        "confirm_input_equiv_mpps_mean",
        "confirm_input_equiv_mpps_stddev",
        "confirm_delivery_ratio_mean",
        "confirm_delivery_ratio_stddev",
        "success_threshold",
    ]

    app_max_rows = []
    by_app = {}
    for row in per_copy_summary:
        if row["ndr_input_equiv_mpps"] is None:
            continue
        app_name = row["application"]
        prev = by_app.get(app_name)
        if prev is None or row["ndr_input_equiv_mpps"] > prev["ndr_input_equiv_mpps"]:
            by_app[app_name] = row

    for app_name, row in by_app.items():
        # configured_copies comes *after* the expansion: the dict below used to
        # put "ALL" first and then let row's own value overwrite it, so every
        # per_application line was labelled with a copy count instead.
        app_max_rows.append({"scope": "per_application", "application": app_name,
                             **{k: row[k] for k in fieldnames[2:]},
                             "configured_copies": "ALL"})

    with open(csv_file, "w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=fieldnames)
        writer.writeheader()
        for row in per_copy_summary:
            writer.writerow({"scope": "per_copies", **row})
        for row in app_max_rows:
            writer.writerow(row)


def main():
    command_configs = bench.THROUGHPUT_APPS

    # Before anything else: the NIC has to be in the one configuration these
    # tests assume, or they produce a full set of meaningless numbers.
    bench.check_nic(
        need_inline=any(c.get("inline") for c in command_configs.values())
    )


    client = setup_trex()
    probe_rows = []
    per_copy_summary = []

    try:
        for app_name, config in command_configs.items():
            base_command = config["base_command"]
            for configured_copies in config["clones"]:
                tqdm.write(f"\n=== {app_name} | configured_copies={configured_copies} ===")
                process = launch_program(base_command, configured_copies)
                if process is None:
                    raise RuntimeError(
                        f"Failed to start {app_name} with configured_copies={configured_copies}"
                    )

                try:
                    fanout = _fanout_multiplier(configured_copies)
                    tx_cap = _tx_cap_mpps(configured_copies)

                    ndr_probe, ndr_rate, search_probes = find_ndr(client, configured_copies)

                    for index, probe in enumerate(search_probes, start=1):
                        probe_rows.append({
                            "application": app_name,
                            "configured_copies": configured_copies,
                            "probe_id": index,
                            **probe,
                        })
                    save_probe_results_csv(probe_rows)

                    if ndr_rate is None or ndr_probe is None:
                        tqdm.write(
                            f"No NDR found: no rate met the success threshold "
                            f"(ratio >= {SUCCESS_THRESHOLD:.2f})."
                        )
                        per_copy_summary.append({
                            "application": app_name,
                            "configured_copies": configured_copies,
                            "tx_cap_mpps": tx_cap,
                            "rate_capped": None,
                            "ndr_tx_mpps": None,
                            "ndr_rx_mpps": None,
                            "ndr_input_equiv_mpps": None,
                            "fanout_multiplier": fanout,
                            "confirm_tx_mpps_mean": None,
                            "confirm_tx_mpps_stddev": None,
                            "confirm_rx_mpps_mean": None,
                            "confirm_rx_mpps_stddev": None,
                            "confirm_input_equiv_mpps_mean": None,
                            "confirm_input_equiv_mpps_stddev": None,
                            "confirm_delivery_ratio_mean": None,
                            "confirm_delivery_ratio_stddev": None,
                            "success_threshold": SUCCESS_THRESHOLD,
                        })
                    else:
                        tqdm.write(
                            f"NDR found: tx={ndr_probe['measured_tx_mpps']:.4f} Mpps "
                            f"rx={ndr_probe['measured_rx_mpps']:.4f} Mpps "
                            f"input_eq={ndr_probe['delivered_input_equiv_mpps']:.4f} Mpps "
                            f"(ratio={ndr_probe['delivery_ratio']:.4f}) "
                            f"— confirming with {CONFIRM_REPETITIONS} runs..."
                        )

                        confirm_probes = confirm_ndr(client, configured_copies, ndr_rate)
                        base_index = len(probe_rows) + 1
                        for index, probe in enumerate(confirm_probes, start=base_index):
                            probe_rows.append({
                                "application": app_name,
                                "configured_copies": configured_copies,
                                "probe_id": index,
                                **probe,
                            })
                        save_probe_results_csv(probe_rows)

                        tx_vals = [p["measured_tx_mpps"] for p in confirm_probes]
                        rx_vals = [p["measured_rx_mpps"] for p in confirm_probes]
                        input_vals = [p["delivered_input_equiv_mpps"] for p in confirm_probes]
                        ratio_vals = [p["delivery_ratio"] for p in confirm_probes]

                        tx_mean, tx_std = _mean_std(tx_vals)
                        rx_mean, rx_std = _mean_std(rx_vals)
                        input_mean, input_std = _mean_std(input_vals)
                        ratio_mean, ratio_std = _mean_std(ratio_vals)

                        per_copy_summary.append({
                            "application": app_name,
                            "configured_copies": configured_copies,
                            "tx_cap_mpps": tx_cap,
                            "rate_capped": ndr_probe["rate_capped"],
                            "ndr_tx_mpps": ndr_probe["measured_tx_mpps"],
                            "ndr_rx_mpps": ndr_probe["measured_rx_mpps"],
                            "ndr_input_equiv_mpps": ndr_probe["delivered_input_equiv_mpps"],
                            "fanout_multiplier": fanout,
                            "confirm_tx_mpps_mean": tx_mean,
                            "confirm_tx_mpps_stddev": tx_std,
                            "confirm_rx_mpps_mean": rx_mean,
                            "confirm_rx_mpps_stddev": rx_std,
                            "confirm_input_equiv_mpps_mean": input_mean,
                            "confirm_input_equiv_mpps_stddev": input_std,
                            "confirm_delivery_ratio_mean": ratio_mean,
                            "confirm_delivery_ratio_stddev": ratio_std,
                            "success_threshold": SUCCESS_THRESHOLD,
                        })
                        tqdm.write(
                            f"Confirmation: "
                            f"rx_mean={rx_mean:.4f} Mpps "
                            f"input_eq_mean={input_mean:.4f} Mpps "
                            f"ratio_mean={ratio_mean:.4f}"
                        )

                    save_summary_csv(per_copy_summary)
                finally:
                    stop_program(process)
    finally:
        save_probe_results_csv(probe_rows)
        save_summary_csv(per_copy_summary)
        client.release(ports=PORTS)
        client.disconnect()


if __name__ == "__main__":
    main()
