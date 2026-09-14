#!/usr/bin/env bash
# The whole experiment: five points, three schedules, several rank counts and
# message sizes, repeated. Runs on maestrale.
#
#   ./sweep.sh --ranks 3 5 7 --algos linear binomial ring --reps 3 \
#              --sizes 8 1024 --out results/e1.csv
#
# Both machines are pinned to `performance` with interrupt coalescing off for
# the duration and put back afterwards: a broadcast here is tens of
# microseconds and the mlx5 default adapts rx-usecs to the load, so without
# that the numbers would describe the coalescing.

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$here")

ETH=${ETH:-enp52s0f1np1}
GRECALE=${GRECALE:-grecale}
GRECALE_ETH=${GRECALE_ETH:-enp172s0f0np0}
# cluster.sh and tune.sh are electrode's: the two experiments want the same
# topology and the same machine state, and a second copy would only drift.
TUNE=${TUNE:-$(dirname "$root")/electrode/scripts/tune.sh}
REMOTE_TUNE=${REMOTE_TUNE:-XDP_CLONE/electrode/scripts/tune.sh}

modes=(mpi udp tc xdp xdp-inline)
algos=(linear binomial ring)
ranks=(3 5 7)
sizes=(8)
iters=1000
reps=3
out="$root/results/sweep-$(date +%Y%m%d_%H%M%S).csv"

while [ $# -gt 0 ]; do
    case "$1" in
        --modes) shift; modes=(); while [ $# -gt 0 ] && [[ $1 != --* ]]; do modes+=("$1"); shift; done ;;
        --algos) shift; algos=(); while [ $# -gt 0 ] && [[ $1 != --* ]]; do algos+=("$1"); shift; done ;;
        --ranks) shift; ranks=(); while [ $# -gt 0 ] && [[ $1 != --* ]]; do ranks+=("$1"); shift; done ;;
        --sizes) shift; sizes=(); while [ $# -gt 0 ] && [[ $1 != --* ]]; do sizes+=("$1"); shift; done ;;
        --iters) iters=$2; shift 2 ;;
        --reps)  reps=$2; shift 2 ;;
        --out)   out=$2; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
done

mkdir -p "$(dirname "$out")"

untune() {
    sudo "$TUNE" off "$ETH" >/dev/null 2>&1 || true
    ssh "$GRECALE" "sudo \$HOME/$REMOTE_TUNE off $GRECALE_ETH" >/dev/null 2>&1 || true
}
trap untune EXIT

sudo "$TUNE" on "$ETH"
ssh "$GRECALE" "sudo \$HOME/$REMOTE_TUNE on $GRECALE_ETH"

total=$(( ${#ranks[@]} * ${#modes[@]} * ${#algos[@]} * ${#sizes[@]} * reps ))
i=0

for n in "${ranks[@]}"; do
    ssh "$GRECALE" "sudo \$HOME/XDP_CLONE/electrode/scripts/cluster.sh up $n" >/dev/null
    for rep in $(seq 1 "$reps"); do
        for m in "${modes[@]}"; do
            for a in "${algos[@]}"; do
                # The schedule is ours; MPI_Bcast has its own and ignores it, so
                # running the native point three times would only add noise.
                [ "$m" = mpi ] && [ "$a" != linear ] && continue
                for s in "${sizes[@]}"; do
                    i=$(( i + 1 ))
                    printf '[%d/%d] n=%s %-10s %-8s %sB rep=%s  ' \
                        "$i" "$total" "$n" "$m" "$a" "$s" "$rep"
                    "$here/run.sh" --mode "$m" --algo "$a" --ranks "$n" \
                        --bytes "$s" --iters "$iters" --rep "$rep" \
                        --out "$out" --keep-topology || echo "  (run failed)"
                done
            done
        done
    done
done

echo
echo "results in $out"

# The summary is part of the run, not a step to remember afterwards.
if ! python3 "$here/report.py" "$out"; then
    echo "the sweep finished but the summary failed; the rows are in $out" >&2
fi
