#!/usr/bin/env python3
"""Turn one osu_bcast log into one CSV row.

osu_bcast prints one line per message size -- "<bytes> <avg latency us>" -- and
the wrapper adds, per rank, how many packets it put on the wire and how many
receives timed out.  Both are kept: the packet count is what says the offload
actually happened, and a non-zero timeout count is what says the run is not a
measurement.
"""
import argparse
import csv
import os
import re
import sys

RESULT = re.compile(r"^\s*(\d+)\s+([0-9.]+)\s*$", re.M)
STATS = re.compile(r"mpi-clone: rank (\d+)\s+(\d+) broadcasts, (\d+) packets sent, "
                   r"(\d+) timeouts")

FIELDS = ["mode", "algo", "ranks", "bytes", "iters", "rep", "latency_us",
          "root_packets_per_bcast", "total_packets", "timeouts", "ok", "note"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    for f, t in (("mode", str), ("algo", str), ("ranks", int), ("bytes", int),
                 ("iters", int), ("rep", int)):
        ap.add_argument(f"--{f}", type=t, required=(f != "rep"), default=0)
    ap.add_argument("--out")
    a = ap.parse_args()

    text = open(a.log, errors="replace").read()
    row = {f: "" for f in FIELDS}
    row.update(mode=a.mode, algo=a.algo, ranks=a.ranks, bytes=a.bytes,
               iters=a.iters, rep=a.rep)

    stats = STATS.findall(text)
    timeouts = sum(int(t) for _, _, _, t in stats)
    total = sum(int(p) for _, _, p, _ in stats)
    root = [(int(b), int(p)) for r, b, p, _ in stats if int(r) == 0]
    row["timeouts"] = timeouts
    row["total_packets"] = total or ""
    if root and root[0][0]:
        row["root_packets_per_bcast"] = round(root[0][1] / root[0][0], 3)

    m = RESULT.findall(text)
    if not m:
        row["ok"] = 0
        row["note"] = "osu_bcast printed no result"
        print(f"FAILED {a.mode}/{a.algo} n={a.ranks}: {row['note']}", file=sys.stderr)
        for line in [l for l in text.strip().splitlines() if l.strip()][-3:]:
            print("  " + line, file=sys.stderr)
    else:
        row["latency_us"] = float(m[-1][1])
        row["ok"] = 1 if timeouts == 0 else 0
        if timeouts:
            row["note"] = "datagrams lost"

    print(",".join(f"{k}={row[k]}" for k in
                   ("mode", "algo", "ranks", "bytes", "latency_us",
                    "root_packets_per_bcast", "ok")))

    if a.out:
        new = not os.path.exists(a.out)
        with open(a.out, "a", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=FIELDS)
            if new:
                w.writeheader()
            w.writerow(row)


if __name__ == "__main__":
    main()
