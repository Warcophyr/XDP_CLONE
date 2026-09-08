from tqdm import tqdm
from time import sleep
import csv
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
RESULTS_CSV_FILE = bench.result("throughput_results.csv")
SUMMARY_CSV_FILE = bench.result("throughput_summary.csv")

PORTS = [0]
TREX_SERVER = "100.78.72.16"

WARMUP_SECONDS = 2
MEASURE_SECONDS = 4
SUCCESS_THRESHOLD = 0.99
RX_CAP_MPPS = 30.0

START_TX_MPPS = 0.25
BINARY_STEPS = 8
MIN_RATE_STEP_MPPS = 0.02


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
    # Confirmed requirement: expected_rx = tx * (copies + 1).
    return float(configured_copies + 1)


def _tx_cap_mpps(configured_copies):
    fanout = _fanout_multiplier(configured_copies)
    return (RX_CAP_MPPS / fanout)


def _mpps_to_pps_string(rate_mpps):
    pps = max(1, int(round(rate_mpps * 1_000_000)))
    return f"{pps}pps"


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
        f"(tx_cap={tx_cap:.4f}, rx_cap={RX_CAP_MPPS:.2f})"
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


def find_max_throughput(client, configured_copies):
    tx_cap = _tx_cap_mpps(configured_copies)
    probes = []
    best_success_probe = None

    start_rate = min(START_TX_MPPS, tx_cap)
    first_probe = run_probe(client, configured_copies, start_rate)
    probes.append(first_probe)

    low = 0.0
    high = None

    if first_probe["success"]:
        best_success_probe = first_probe
        low = first_probe["requested_tx_mpps"]
        current = low

        while current < tx_cap:
            next_rate = min(current * 2.0, tx_cap)
            if next_rate - current < MIN_RATE_STEP_MPPS:
                break

            probe = run_probe(client, configured_copies, next_rate)
            probes.append(probe)

            if probe["success"]:
                best_success_probe = probe
                low = next_rate
                current = next_rate
                if next_rate >= tx_cap:
                    return best_success_probe, probes
            else:
                high = next_rate
                break

        if high is None:
            return best_success_probe, probes
    else:
        high = first_probe["requested_tx_mpps"]

    for _ in range(BINARY_STEPS):
        if high - low < MIN_RATE_STEP_MPPS:
            break
        mid = (low + high) / 2.0
        if mid <= 0.0:
            break

        probe = run_probe(client, configured_copies, mid)
        probes.append(probe)

        if probe["success"]:
            best_success_probe = probe
            low = mid
        else:
            high = mid

    return best_success_probe, probes


def save_probe_results_csv(results, csv_file=RESULTS_CSV_FILE):
    fieldnames = [
        "application",
        "configured_copies",
        "probe_id",
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
        "max_no_loss_tx_mpps",
        "max_no_loss_rx_mpps",
        "max_no_loss_input_equiv_mpps",
        "fanout_multiplier",
        "delivery_ratio",
        "success_threshold",
    ]

    app_max_rows = []
    by_app = {}
    for row in per_copy_summary:
        if row["max_no_loss_input_equiv_mpps"] is None:
            continue
        app_name = row["application"]
        prev = by_app.get(app_name)
        if (
            prev is None
            or row["max_no_loss_input_equiv_mpps"] > prev["max_no_loss_input_equiv_mpps"]
        ):
            by_app[app_name] = row

    for app_name, row in by_app.items():
        app_max_rows.append(
            {
                "scope": "per_application",
                "application": app_name,
                "configured_copies": "ALL",
                "tx_cap_mpps": row["tx_cap_mpps"],
                "rate_capped": row["rate_capped"],
                "max_no_loss_tx_mpps": row["max_no_loss_tx_mpps"],
                "max_no_loss_rx_mpps": row["max_no_loss_rx_mpps"],
                "max_no_loss_input_equiv_mpps": row["max_no_loss_input_equiv_mpps"],
                "fanout_multiplier": row["fanout_multiplier"],
                "delivery_ratio": row["delivery_ratio"],
                "success_threshold": SUCCESS_THRESHOLD,
            }
        )

    with open(csv_file, "w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=fieldnames)
        writer.writeheader()
        for row in per_copy_summary:
            writer.writerow(
                {
                    "scope": "per_copies",
                    "application": row["application"],
                    "configured_copies": row["configured_copies"],
                    "tx_cap_mpps": row["tx_cap_mpps"],
                    "rate_capped": row["rate_capped"],
                    "max_no_loss_tx_mpps": row["max_no_loss_tx_mpps"],
                    "max_no_loss_rx_mpps": row["max_no_loss_rx_mpps"],
                    "max_no_loss_input_equiv_mpps": row["max_no_loss_input_equiv_mpps"],
                    "fanout_multiplier": row["fanout_multiplier"],
                    "delivery_ratio": row["delivery_ratio"],
                    "success_threshold": SUCCESS_THRESHOLD,
                }
            )
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

    bench.set_governor("performance")


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
                    best_probe, probes = find_max_throughput(client, configured_copies)

                    for index, probe in enumerate(probes, start=1):
                        row = {
                            "application": app_name,
                            "configured_copies": configured_copies,
                            "probe_id": index,
                        }
                        row.update(probe)
                        probe_rows.append(row)
                    save_probe_results_csv(probe_rows)

                    fanout = _fanout_multiplier(configured_copies)
                    tx_cap = _tx_cap_mpps(configured_copies)

                    if best_probe is None:
                        per_copy_summary.append(
                            {
                                "application": app_name,
                                "configured_copies": configured_copies,
                                "tx_cap_mpps": tx_cap,
                                "rate_capped": None,
                                "max_no_loss_tx_mpps": None,
                                "max_no_loss_rx_mpps": None,
                                "max_no_loss_input_equiv_mpps": None,
                                "fanout_multiplier": fanout,
                                "delivery_ratio": None,
                            }
                        )
                        tqdm.write(
                            "No throughput point met the success threshold "
                            f"(ratio >= {SUCCESS_THRESHOLD:.2f})."
                        )
                    else:
                        per_copy_summary.append(
                            {
                                "application": app_name,
                                "configured_copies": configured_copies,
                                "tx_cap_mpps": tx_cap,
                                "rate_capped": best_probe["rate_capped"],
                                "max_no_loss_tx_mpps": best_probe["measured_tx_mpps"],
                                "max_no_loss_rx_mpps": best_probe["measured_rx_mpps"],
                                "max_no_loss_input_equiv_mpps": best_probe[
                                    "delivered_input_equiv_mpps"
                                ],
                                "fanout_multiplier": best_probe["fanout_multiplier"],
                                "delivery_ratio": best_probe["delivery_ratio"],
                            }
                        )
                        tqdm.write(
                            "Best no-loss throughput: "
                            f"tx={best_probe['measured_tx_mpps']:.4f} Mpps "
                            f"rx={best_probe['measured_rx_mpps']:.4f} Mpps "
                            f"input_eq={best_probe['delivered_input_equiv_mpps']:.4f} Mpps "
                            f"(ratio={best_probe['delivery_ratio']:.4f})"
                        )

                    save_summary_csv(per_copy_summary)
                finally:
                    stop_program(process)
    finally:
        bench.set_governor("schedutil")
        save_probe_results_csv(probe_rows)
        save_summary_csv(per_copy_summary)
        client.release(ports=PORTS)
        client.disconnect()


if __name__ == "__main__":
    main()
