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
# Percentage of the fanout's frames -- the original and all its copies -- that
# has to come back for a rate to count as delivered. 100.0 is a true no-drop
# rate; 99.9 leaves room for the odd frame lost to something other than the
# device, at the price of allowing one in a thousand.
#
# The number is only worth anything because the probe now stops the traffic and
# lets the pipeline drain before reading the final counters (see run_probe).
# Reading while the generator was still sending counted every frame in the XDP
# SQ and on the wire as sent but not received, which at these rates is a whole
# pipeline of systematic loss -- enough that the old 99.0 was largely measuring
# the pipeline depth rather than anything the device did.
#
# Whatever it is set to, ndr_lost_pkts in the summary says how many frames the
# accepted rate actually lost, so the claim stays auditable.
MIN_DELIVERED_PCT = 99.9

# Long enough for the SQ, the wire and TRex's own receive path to empty after
# the generator stops.
DRAIN_SECONDS = 1.0
RX_CAP_MPPS = 30.0

# Rate of the one timestamped stream the profile adds for the latency figures.
# Small enough to be noise in the offered load (1 kpps against several Mpps),
# and it is cloned like every other packet, so it does not skew the delivery
# ratio either -- both sides of it scale with the fanout.
LATENCY_PPS = 1000

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
        latency_pps=LATENCY_PPS,
    ).get_streams()
    client.add_streams(streams, ports=PORTS)
    return client


def run_probe(client, configured_copies, requested_tx_mpps, latency=False):
    tx_cap = _tx_cap_mpps(configured_copies)
    requested_tx_mpps = min(requested_tx_mpps, tx_cap)
    fanout = _fanout_multiplier(configured_copies)
    mult = _mpps_to_pps_string(requested_tx_mpps)

    tqdm.write(
        f"Probe @ copies={configured_copies} tx_target={requested_tx_mpps:.4f} Mpps "
        f"(tx_cap={tx_cap:.4f}, fanout={fanout:.0f}x, rx_cap={RX_CAP_MPPS:.2f})"
    )

    lat = None
    try:
        client.start(ports=PORTS, force=True, mult=mult)
        sleep(WARMUP_SECONDS)
        if latency:
            # After the warmup, so that the latency window is the measurement
            # window and not the ramp.
            client.clear_pgid_stats(clear_flow_stats=True, clear_latency_stats=True)
        before = _read_port_counters(client.get_stats(ports=PORTS), port_id=PORTS[0])
        sleep(MEASURE_SECONDS)
        if latency:
            lat = bench.latency_sample(client)
        # Stop first, then let everything in flight come back, and only then
        # read. Reading while the generator is still sending counts packets as
        # transmitted that have not had time to be received yet -- a bias of a
        # whole pipeline's worth of frames, which is precisely what stops a
        # zero-loss criterion from ever being met.
        client.stop(ports=PORTS)
        sleep(DRAIN_SECONDS)
        after = _read_port_counters(client.get_stats(ports=PORTS), port_id=PORTS[0])
    finally:
        client.stop(ports=PORTS)

    tx_delta = max(0.0, after["tx_pkts"] - before["tx_pkts"])
    rx_delta = max(0.0, after["rx_pkts"] - before["rx_pkts"])

    # Rates over the measurement window: the generator stopped at the end of it,
    # so tx_delta belongs to exactly MEASURE_SECONDS. rx_delta also covers the
    # drain, which is the point -- it is every frame that eventually came back.
    measured_tx_mpps = tx_delta / (MEASURE_SECONDS * 1_000_000.0)
    measured_rx_mpps = rx_delta / (MEASURE_SECONDS * 1_000_000.0)
    # expected_rx = tx * fanout: each sent packet becomes (copies+1) packets at
    # the receiver, the original and its copies.
    expected_rx_mpps = measured_tx_mpps * fanout
    delivered_input_equiv_mpps = measured_rx_mpps / fanout

    # Both in packets rather than in rates: same quotient, but it lines up with
    # the loss count and there is one division less to reason about. Negative
    # loss means more came back than the fanout accounts for, i.e. something
    # else is on the link.
    expected_rx_pkts = tx_delta * fanout
    lost_pkts = int(round(expected_rx_pkts - rx_delta))

    delivery_ratio = 0.0
    if expected_rx_pkts > 0:
        delivery_ratio = rx_delta / expected_rx_pkts

    # What the fanout actually was, which is the only thing that can tell "the
    # device dropped frames" apart from "the device never cloned". Assumed
    # everywhere else, measured here.
    measured_fanout = rx_delta / tx_delta if tx_delta > 0 else 0.0

    success = delivery_ratio >= MIN_DELIVERED_PCT / 100.0

    tqdm.write(
        "Measured: "
        f"tx={measured_tx_mpps:.4f} Mpps "
        f"rx={measured_rx_mpps:.4f} Mpps "
        f"expected_rx={expected_rx_mpps:.4f} Mpps "
        f"fanout={measured_fanout:.2f}/{fanout:.0f} "
        f"delivered={delivery_ratio * 100:.3f}% lost={lost_pkts} "
        f"{'OK' if success else 'LOSS'}"
    )

    if lat:
        tqdm.write(
            "Latency (us): "
            f"min={lat['lat_min']:.1f} avg={lat['lat_avg']:.1f} "
            f"p50={lat['lat_p50']:.1f} p90={lat['lat_p90']:.1f} "
            f"p99={lat['lat_p99']:.1f} max={lat['lat_max']:.1f} "
            f"(dup={lat['lat_dup']} seq_err={lat['lat_seq_err']})"
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
        "measured_fanout": measured_fanout,
        "lost_pkts": lost_pkts,
        "success": success,
        **(lat or {}),
    }


def find_ndr(client, configured_copies):
    """Binary search for the highest TX rate that delivers the whole fanout."""
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
        probe = run_probe(client, configured_copies, ndr_rate, latency=True)
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
        "measured_fanout",
        "lost_pkts",
        "delivery_ratio",
        "success",
        *bench.LATENCY_FIELDS,
        "lat_dup",
        "lat_seq_err",
    ]
    with open(csv_file, "w", newline="", encoding="utf-8") as file:
        # Only the confirm probes carry latency; the search ones leave those
        # columns empty.
        writer = csv.DictWriter(file, fieldnames=fieldnames, restval="")
        writer.writeheader()
        writer.writerows(results)


def save_summary_csv(per_copy_summary, csv_file=SUMMARY_CSV_FILE):
    """One row per (application, copy count), and nothing else.

    There used to be a 'scope' column that read per_copies on every row and a
    trailing per_application row holding whichever copy count happened to score
    highest -- neither of which said anything the rest of the table did not.
    Gone, along with fanout_multiplier and ndr_input_equiv_mpps: both are
    functions of configured_copies alone (fanout = copies + 1, input_equiv =
    ndr_rx_mpps / fanout), so nothing is lost by leaving them to the reader.

    ndr_tx_mpps and ndr_rx_mpps are the mean over the CONFIRM_REPETITIONS
    probes taken at the NDR rate, which is what makes a standard deviation
    meaningful next to them. The latency columns come from the same probes, so
    they describe the device *at* its no-drop rate rather than under some other
    load.
    """
    fieldnames = [
        "application",
        "configured_copies",
        "tx_cap_mpps",
        "rate_capped",
        "ndr_tx_mpps",
        "ndr_tx_mpps_stddev",
        "ndr_rx_mpps",
        "ndr_rx_mpps_stddev",
        "ndr_fanout",
        "ndr_lost_pkts",
        *bench.LATENCY_FIELDS,
    ]

    with open(csv_file, "w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=fieldnames, restval="")
        writer.writeheader()
        writer.writerows(per_copy_summary)


def _summarize(app_name, configured_copies, tx_cap, rate_capped, confirm_probes):
    """Collapse the confirmation probes into the one summary row."""
    row = {
        "application": app_name,
        "configured_copies": configured_copies,
        "tx_cap_mpps": tx_cap,
        "rate_capped": rate_capped,
    }

    for key, field in (("measured_tx_mpps", "ndr_tx_mpps"),
                       ("measured_rx_mpps", "ndr_rx_mpps")):
        mean, stddev = _mean_std([p[key] for p in confirm_probes])
        row[field] = mean
        row[f"{field}_stddev"] = stddev

    # Not redundant with configured_copies, unlike the fanout_multiplier that
    # used to sit here: this is the fanout that was *measured*, so a row where
    # it sits at 1.0 says the device never cloned, and a row where it matches
    # copies + 1 with ndr_lost_pkts at 0 says every frame of the fanout came
    # back. Which is the whole claim an NDR makes.
    row["ndr_fanout"] = _mean_std([p["measured_fanout"] for p in confirm_probes])[0]
    row["ndr_lost_pkts"] = max(p["lost_pkts"] for p in confirm_probes)

    for field in bench.LATENCY_FIELDS:
        values = [p[field] for p in confirm_probes if field in p]
        row[field] = _mean_std(values)[0] if values else None

    return row


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
                            f"No NDR found: no rate delivered "
                            f"{MIN_DELIVERED_PCT}% of the fanout. If "
                            f"measured_fanout in ndr_results.csv is far from "
                            f"{_fanout_multiplier(configured_copies):.0f}, the "
                            f"device is not cloning rather than dropping."
                        )
                        per_copy_summary.append({
                            "application": app_name,
                            "configured_copies": configured_copies,
                            "tx_cap_mpps": tx_cap,
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

                        per_copy_summary.append(_summarize(
                            app_name, configured_copies, tx_cap,
                            ndr_probe["rate_capped"], confirm_probes))

                        row = per_copy_summary[-1]
                        tqdm.write(
                            "Confirmation: "
                            f"tx={row['ndr_tx_mpps']:.4f}±{row['ndr_tx_mpps_stddev']:.4f} "
                            f"rx={row['ndr_rx_mpps']:.4f}±{row['ndr_rx_mpps_stddev']:.4f} Mpps"
                            + (f" p50={row['lat_p50']:.1f} p99={row['lat_p99']:.1f} us"
                               if row.get("lat_p50") is not None else
                               "  (no latency samples)")
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
