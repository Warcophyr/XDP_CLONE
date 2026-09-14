#!/usr/bin/env python3
"""Summarise a sweep of the broadcast offload.

One row per (ranks, size, schedule, point), aggregated over the repetitions as
the median, with the spread of the repetitions beside it. The packets the root
put on the wire per broadcast come along because they are what says the offload
happened at all: it is N-1 for the naive point and 1 for every offloaded one.
"""
import argparse
import csv
import os
import statistics as st
from collections import defaultdict

ORDER = ["mpi", "udp", "tc", "xdp", "xdp-inline"]
LABEL = {"mpi": "MPI_Bcast", "udp": "UDP naive", "tc": "TC clone",
         "xdp": "XDP_CLONE", "xdp-inline": "XDP_CLONE inline"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--outdir")
    a = ap.parse_args()
    outdir = a.outdir or os.path.dirname(os.path.abspath(a.csv))
    os.makedirs(outdir, exist_ok=True)

    rows = defaultdict(list)
    dropped = 0
    for r in csv.DictReader(open(a.csv)):
        if r.get("ok") != "1":
            dropped += 1
            continue
        rows[(int(r["ranks"]), int(r["bytes"]), r["algo"], r["mode"])].append(r)
    if dropped:
        print(f"({dropped} rows dropped: ok=0)\n")

    summary = {}
    for k, rs in rows.items():
        lat = [float(r["latency_us"]) for r in rs]
        pk = [float(r["root_packets_per_bcast"]) for r in rs
              if r["root_packets_per_bcast"]]
        sq = [float(r["dut_softirq_cores"]) for r in rs
              if r.get("dut_softirq_cores") not in (None, "")]
        summary[k] = dict(n=len(rs), lat=st.median(lat),
                          sd=st.stdev(lat) if len(lat) > 1 else 0.0,
                          pk=st.median(pk) if pk else None,
                          sq=st.median(sq) if sq else None)

    for nranks in sorted({k[0] for k in summary}):
        for size in sorted({k[1] for k in summary if k[0] == nranks}):
            print(f"=== {nranks} ranks, {size} B " + "=" * 40)
            algos = sorted({k[2] for k in summary if k[:2] == (nranks, size)})
            print(f"{'point':<20}" + "".join(f"{a:>22}" for a in algos))
            print(f"{'':<20}" + "".join(f"{'us  (sd)   pkts/bcast':>22}" for _ in algos))
            for m in ORDER:
                cells = []
                for al in algos:
                    s = summary.get((nranks, size, al, m))
                    if not s:
                        cells.append(" " * 22)
                    else:
                        pk = f"{s['pk']:.0f}" if s["pk"] is not None else "-"
                        cells.append(f"{s['lat']:9.2f} ({s['sd']:4.2f}) {pk:>6}")
                if any(c.strip() for c in cells):
                    print(f"{LABEL[m]:<20}" + "".join(cells))
            print()

    path = os.path.join(outdir, "summary.csv")
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["ranks", "bytes", "algo", "mode", "reps", "latency_us",
                    "latency_sd", "root_packets_per_bcast",
                    "dut_softirq_cores"])
        for k in sorted(summary, key=lambda k: (k[0], k[1], k[2], ORDER.index(k[3]))):
            s = summary[k]
            w.writerow([k[0], k[1], k[2], k[3], s["n"], f"{s['lat']:.3f}",
                        f"{s['sd']:.3f}",
                        f"{s['pk']:.3f}" if s["pk"] is not None else "",
                        f"{s['sq']:.4f}" if s["sq"] is not None else ""])
    print(f"summary in {path}")


if __name__ == "__main__":
    main()
