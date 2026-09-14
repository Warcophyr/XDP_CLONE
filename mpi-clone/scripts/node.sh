#!/usr/bin/env bash
# The grecale half of a run: the TC duplication point, and the MPI job itself.
#
#   sudo ./node.sh tc-start <ranks> <gateway-mac>
#   sudo ./node.sh tc-stop
#   sudo ./node.sh run <ranks> <mode> <algo> <bytes> <iters> <logfile>
#
# Every rank lives in a namespace of its own, and mpirun runs in one more. Three
# things are not optional and each of them, left out, produces a set of numbers
# that measures something else entirely:
#
#   plm_rsh_agent      Open MPI launches over ssh; there is no sshd in a
#                      namespace, so the agent enters one instead.
#   a hostname each    otherwise every rank reports `grecale`, Open MPI puts
#                      them on one node and moves messages through shared
#                      memory: a broadcast came out at 1.78 us with 27 packets
#                      on the wire for the whole run.
#   btl_tcp_disable_family 6
#                      IPv6 link-local between two macvlans on one parent is
#                      short-circuited in software and never reaches the wire.
#
# HWLOC_COMPONENTS=-gl belongs to the same list: without it Open MPI hangs in a
# namespace with no output at all, probing X displays for GPUs over an abstract
# socket that is scoped to the network namespace.

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$here")
run=/tmp/mpiclone-run
NS=${NS:-elec-r}
LAUNCH_NS=${LAUNCH_NS:-elec-cl}
DEV=${DEV:-mv}
OSU=${OSU:-/opt/osu/libexec/osu-micro-benchmarks/mpi/collective}
MPICLONE_CPUS=${MPICLONE_CPUS:-2-15}
export MPICLONE_CPUS
read -ra REPLICA_CPU_LIST <<< "$(
    echo "$MPICLONE_CPUS" | tr ',' ' ' | while read -r spec; do
        for part in $spec; do
            case "$part" in
                *-*) for (( c = ${part%%-*}; c <= ${part##*-}; c++ )); do echo "$c"; done ;;
                *)   echo "$part" ;;
            esac
        done
    done | tr '\n' ' ')"
FANOUT_IP=${FANOUT_IP:-192.168.101.1}
AGENT=${AGENT:-$here/nsagent.sh}

mkdir -p "$run"

cmd_tc_start() {
    local n=$1 gw=$2 i
    for (( i = 0; i < n; i++ )); do
        ip netns exec "$NS$i" setsid "$root/bpf/mpi_tc" "$DEV" -g "$gw" \
            -r "$root/ranks.conf" -o "$root/bpf/mpi_tc.bpf.o" \
            > "$run/tc$i.log" 2>&1 &
    done
    sleep 1.5
    local up
    up=$(grep -l '^ready' "$run"/tc*.log 2>/dev/null | wc -l)
    [ "$up" -eq "$n" ] || {
        echo "TC up on $up of $n namespaces:" >&2
        grep -h Error "$run"/tc*.log | head -3 >&2
        exit 1
    }
    echo "TC duplication point on $n namespaces"
}

cmd_tc_stop() {
    local i
    for (( i = 0; i < 64; i++ )); do
        ip netns list | awk '{print $1}' | grep -qx "$NS$i" || break
        ip netns exec "$NS$i" pkill -f "bpf/mpi_tc" 2>/dev/null || true
    done
    rm -f "$run"/tc*.log
    echo stopped
}

cmd_run() {
    local n=$1 mode=$2 algo=$3 bytes=$4 iters=$5 log=$6 i hosts yield=()
    hosts="${NS}0"
    for (( i = 1; i < n; i++ )); do hosts="$hosts,${NS}$i"; done

    # More ranks than cores: Open MPI spins in opal_progress() by default, and
    # with three spinning ranks to a core everything serialises -- a 31-rank
    # broadcast came out at 5.5 ms against 24 us at seven. It switches to
    # sched_yield() when it detects oversubscription, but here every rank is
    # its own node with one slot, so it never does. Told explicitly instead.
    if [ "$n" -gt "${#REPLICA_CPU_LIST[@]}" ]; then
        yield=(--mca mpi_yield_when_idle 1)
    fi

    rm -f "$log"
    ip netns exec "$LAUNCH_NS" env HWLOC_COMPONENTS=-gl DISPLAY= \
        taskset -c "${LAUNCH_CORE:-1}" mpirun --allow-run-as-root \
            --mca plm_rsh_agent "$AGENT" \
            --mca pml ob1 --mca btl tcp,self \
            --mca btl_tcp_if_include "$DEV" --mca oob_tcp_if_include "$DEV" \
            --mca btl_tcp_disable_family 6 \
            --bind-to none "${yield[@]}" \
            -x LD_PRELOAD="$root/libmpiclone.so" \
            -x MPICLONE_MODE="$mode" -x MPICLONE_ALGO="$algo" \
            -x MPICLONE_FANOUT="$FANOUT_IP" -x MPICLONE_VERBOSE=1 \
            --host "$hosts" -np "$n" \
            "$OSU/osu_bcast" -m "$bytes:$bytes" -i "$iters" -x 100 \
        > "$log" 2>&1
}

case "${1:-}" in
    tc-start) shift; cmd_tc_start "$@" ;;
    tc-stop)  cmd_tc_stop ;;
    run)      shift; cmd_run "$@" ;;
    *) echo "usage: $0 {tc-start <n> <gw-mac>|tc-stop|run <n> <mode> <algo> <bytes> <iters> <log>}" >&2; exit 1 ;;
esac
