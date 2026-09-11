
from tqdm import tqdm
from time import sleep
import csv
import math
import statistics
import sys
import subprocess as sp
import shlex
import threading
import os
import inspect
sys.path.append("/trex/v3.08/automation/trex_control_plane/interactive")
from trex.stl.api import STLClient, STLProfile

import bench_common as bench
from bench_common import launch_program, stop_program

PROFILE_FILE = bench.profile("clonlat.py")

# Must match profiles/clonlat.py: pg_id 1 is the uncloned probe that carries
# the latency histogram, pg_id 2 the cloned load whose rx/tx ratio is the
# fan-out actually observed.
PG_PROBE = 1
PG_LOAD = 2
RESULTS_CSV_FILE = bench.result("latency_results.csv")
SUMMARY_CSV_FILE = bench.result("latency_summary.csv")
LATENCY_METRICS = [
    "min",
    "avg",
    "max",
    "p50",
    "p90",
    "p95",
    "p99",
    "rx_tx_ratio",
    "copies",
    "probe_samples",
    "probe_tx",
]

PORTS = [0]  # single port loopback

def launch_trex(client):
    client.start(ports=PORTS, force=True)
    # aspetta wamup traffico a pps
    tqdm.write("Waiting for traffic to reach expected rate before stopping warmup...")
    sleep(1)
    return

def stop_trex(client):
    tqdm.write("Stopping TRex traffic...")
    client.stop(ports=PORTS)
    # while True:
    #     stats = client.get_stats(ports=PORTS)
    #     if stats[0]['tx_pps'] < 2:
    #         break
    #     sleep(1)

    # tqdm.write("Traffic fully stopped.")

def _pgid_entry(stats_section, pg_id):
    if pg_id in stats_section:
        return stats_section[pg_id]
    pg_id_str = str(pg_id)
    if pg_id_str in stats_section:
        return stats_section[pg_id_str]
    raise RuntimeError(f"No latency stats available for pg_id={pg_id}.")


def _counter_total(counter_values, counter_name):
    if not isinstance(counter_values, dict) or not counter_values:
        raise RuntimeError(f"Missing {counter_name} stats.")

    if "total" in counter_values:
        return float(counter_values["total"])

    total = 0.0
    found = False
    for key, value in counter_values.items():
        if isinstance(key, int) or (isinstance(key, str) and key.isdigit()):
            total += float(value)
            found = True

    if not found:
        raise RuntimeError(f"Cannot compute total for {counter_name}.")
    return total


def latency_stats(client, pg_id=PG_PROBE, load_pg_id=PG_LOAD):
    """Latency of the probe stream, and the fan-out the load stream really got.

    Two pg_ids on purpose. The probe is never cloned, so it yields one sample
    per packet sent and its latency is the machine's; the load stream is the one
    that gets cloned, so its rx/tx ratio is what says whether the application
    actually produced the copies it was asked for.
    """
    wanted = [pg_id] if load_pg_id is None else [pg_id, load_pg_id]
    stats = client.get_pgid_stats(wanted)
    latency_section = stats.get("latency")
    if not latency_section:
        raise RuntimeError("No latency section returned by TRex.")
    flow_section = stats.get("flow_stats")
    if not flow_section:
        raise RuntimeError("No flow_stats section returned by TRex.")

    latency_entry = _pgid_entry(latency_section, pg_id)
    latency_values = latency_entry.get("latency", {})
    histogram = latency_values.get("histogram", {})

    if load_pg_id is None:
        rx_tx_ratio = 1
        num_copies = 0
    else:
        load_entry = _pgid_entry(flow_section, load_pg_id)
        rx_total = _counter_total(load_entry.get("rx_pkts", {}), "rx_pkts")
        tx_total = _counter_total(load_entry.get("tx_pkts", {}), "tx_pkts")
        if tx_total <= 0:
            raise RuntimeError("tx_pkts total is zero, cannot compute copies.")
        rx_tx_ratio = int(rx_total / tx_total + 0.5)
        num_copies = rx_tx_ratio - 1

    probe_entry = _pgid_entry(flow_section, pg_id)
    probe_tx = _counter_total(probe_entry.get("tx_pkts", {}), "tx_pkts")

    # Percentiles and the maximum both come out of the histogram, which is the
    # only part of TRex's latency stats that clear_pgid_stats() really makes
    # relative to the measurement window -- see bench_common.py. total_max is
    # rebuilt by the client from the last server sampling interval alone, which
    # is what used to put percentiles above the maximum in this very CSV.
    percentiles = bench.histogram_percentiles(histogram)
    if percentiles is None:
        tqdm.write("⚠️ Latency histogram is empty, percentiles will be set to nan.")
        percentiles = {p: float("nan") for p in [50, 90, 95, 99]}
        max_latency = float(latency_values["total_max"])
    else:
        max_latency = percentiles.pop("max")
    result = {
        "min": float(latency_values["total_min"]),
        "avg": float(latency_values["average"]),
        "max": max_latency,
        "p50": percentiles[50],
        "p90": percentiles[90],
        "p95": percentiles[95],
        "p99": percentiles[99],
        "rx_pkts": rx_total if load_pg_id is not None else 0.0,
        "tx_pkts": tx_total if load_pg_id is not None else 0.0,
        "probe_samples": float(sum(int(v) for v in histogram.values())),
        "probe_tx": probe_tx,
        "rx_tx_ratio": rx_tx_ratio,
        "copies": num_copies,
    }

    tqdm.write(
        "Latency (us): "
        f"min={result['min']:.2f} avg={result['avg']:.2f} max={result['max']:.2f} "
        f"p50={result['p50']:.2f} p90={result['p90']:.2f} "
        f"p95={result['p95']:.2f} p99={result['p99']:.2f} "
        f"rx/tx={result['rx_tx_ratio']} copies={result['copies']} "
    )
    return result

def save_latency_results_csv(results, csv_file=RESULTS_CSV_FILE):
    if not results:
        raise ValueError("No latency results to save.")

    fieldnames = ["application", "configured_copies", "repetition", *LATENCY_METRICS]
    with open(csv_file, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            row = {
                "application": result["application"],
                "configured_copies": result["configured_copies"],
                "repetition": result["repetition"],
            }
            row.update({metric: result[metric] for metric in LATENCY_METRICS})
            writer.writerow(row)

    # tqdm.write(f"Saved detailed latency results to {csv_file}")

def save_latency_summary_csv(results, csv_file=SUMMARY_CSV_FILE):
    if not results:
        raise ValueError("No latency results to summarize.")

    fieldnames = ["application", "configured_copies", "repetitions"]
    for metric in LATENCY_METRICS:
        fieldnames.extend([f"{metric}_mean", f"{metric}_stddev"])

    grouped_results = {}
    for result in results:
        key = (result["application"], result["configured_copies"])
        grouped_results.setdefault(key, []).append(result)

    with open(csv_file, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for (application, configured_copies), group in grouped_results.items():
            summary_row = {
                "application": application,
                "configured_copies": configured_copies,
                "repetitions": len(group),
            }
            for metric in LATENCY_METRICS:
                values = [result[metric] for result in group]
                clean = [v for v in values if not math.isnan(v)]
                summary_row[f"{metric}_mean"] = statistics.fmean(clean) if clean else float("nan")
                summary_row[f"{metric}_stddev"] = (
                    statistics.stdev(clean) if len(clean) > 1 else 0.0
                )
            writer.writerow(summary_row)

    # tqdm.write(f"Saved latency summary to {csv_file}")

def setup_trex():
    # The profile reads these; it is exec'd by TRex's loader, so the
    # environment is the simplest way to hand it the rates.
    os.environ["CLONLAT_LOAD_PPS"] = str(bench.LATENCY_LOAD_PPS)
    os.environ["CLONLAT_PROBE_PPS"] = str(bench.LATENCY_PROBE_PPS)
    tqdm.write(
        f"Latency: probe {bench.LATENCY_PROBE_PPS} pps uncloned, "
        f"fan-out load {bench.LATENCY_LOAD_PPS} pps"
    )

    client = STLClient(server="100.78.72.16")
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

def main():
    repetitions = 5
    command_configs = bench.LATENCY_APPS

    # Before anything else: the NIC has to be in the one configuration these
    # tests assume, or they produce a full set of meaningless numbers.
    bench.check_nic(
        need_inline=any(c.get("inline") for c in command_configs.values())
    )

    # Latency on a governor that ramps is mostly measuring the governor. The
    # throughput tests already did this; this one did not.
    bench.set_governor("performance")

    client = setup_trex()
    latency_results = []
    total_iterations = repetitions * sum(
        len(config.get("clones", [])) for config in command_configs.values()
    )
    pbar = tqdm(total=total_iterations, desc="Esperimento", unit="test", leave=True)

    launch_trex(client)
    try:
        for app_name, config in command_configs.items():
            base_command = config["base_command"]
            for configured_copies in config.get("clones", []):
                tqdm.write(f"Starting {app_name} with configured copies={configured_copies}")
                process = launch_program(base_command, configured_copies)
                if process is None:
                    raise RuntimeError(
                        f"Failed to start {app_name} with configured copies={configured_copies}"
                    )

                try:
                    for repetition in range(1, repetitions + 1):
                        sleep(2)
                        client.clear_pgid_stats(clear_flow_stats=True, clear_latency_stats=True)
                        sleep(5)
                        result = latency_stats(
                            client,
                            load_pg_id=PG_LOAD if bench.LATENCY_LOAD_PPS else None,
                        )
                        result["application"] = app_name
                        result["configured_copies"] = configured_copies
                        result["repetition"] = repetition
                        latency_results.append(result)
                        save_latency_results_csv(latency_results)
                        pbar.update(1)
                finally:
                    stop_program(process)
    finally:
        pbar.close()
        if latency_results:
            save_latency_summary_csv(latency_results)
        stop_trex(client)
        client.release(ports=PORTS)
        client.disconnect()
        bench.set_governor("schedutil")

if __name__ == "__main__":
    main()
