#!/usr/bin/env bash
# One measurement of one point. Runs on maestrale -- the DUT and the XDP
# duplication point -- and drives grecale over ssh.
#
#   ./run.sh --mode xdp --algo binomial --ranks 7 --bytes 8
#
# The four points differ in exactly one thing, where the broadcast is
# duplicated:
#
#   mpi         nowhere: Open MPI's own MPI_Bcast, over TCP
#   udp         nowhere: our schedule, one packet per destination from the rank
#   tc          on the sending rank's own TC egress hook
#   xdp         on this node, XDP_CLONE_TX
#   xdp-inline  the same, with each copy's header inline in its WQE and every
#               frame out of one shared RX page
#
# udp, tc and xdp put the *same* schedule and the same bytes on the wire; only
# the duplication moves. mpi is there to say what a real MPI does today.

set -euo pipefail

ETH=${ETH:-enp52s0f1np1}
GRECALE=${GRECALE:-grecale}
REMOTE=${REMOTE:-XDP_CLONE/mpi-clone}
FANOUT_IP=${FANOUT_IP:-192.168.101.1}

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$here")
REMOTE_ROOT=$(ssh "$GRECALE" "echo \$HOME/$REMOTE")

mode=xdp algo=linear ranks=7 bytes=8 iters=1000 rep=0 out="" keep=0

while [ $# -gt 0 ]; do
    case "$1" in
        --mode)  mode=$2; shift 2 ;;
        --algo)  algo=$2; shift 2 ;;
        --ranks) ranks=$2; shift 2 ;;
        --bytes) bytes=$2; shift 2 ;;
        --iters) iters=$2; shift 2 ;;
        --rep)   rep=$2; shift 2 ;;
        --out)   out=$2; shift 2 ;;
        --keep-topology) keep=1; shift ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
done

case "$mode" in
    mpi|udp|tc)  wire_mode=$mode; object=mpi_fanout.bpf.o ;;
    xdp)         wire_mode=xdp;   object=mpi_fanout.bpf.o ;;
    xdp-inline)  wire_mode=xdp;   object=mpi_fanout_inline.bpf.o ;;
    *) echo "mode must be mpi, udp, tc, xdp or xdp-inline" >&2; exit 1 ;;
esac

rsh() { ssh "$GRECALE" "$@"; }
fanout_pid=/tmp/mpiclone-fanout.pid

cleanup() {
    rsh "sudo $REMOTE_ROOT/scripts/node.sh tc-stop" >/dev/null 2>&1 || true
    if [ -r "$fanout_pid" ]; then
        sudo kill -9 "$(cat "$fanout_pid")" 2>/dev/null || true
        sudo rm -f "$fanout_pid"
    fi
    local stale; stale=$(pgrep -x mpi_fanout || true)
    [ -n "$stale" ] && sudo kill -9 $stale 2>/dev/null || true
    sudo bpftool net detach xdp dev "$ETH" 2>/dev/null || true
    return 0
}
trap cleanup EXIT

# The clone actions only exist on the legacy-RQ path, and the TX descriptor is
# only read on the regular-WQE path. Either flag left on gives a complete set of
# plausible numbers for something else.
flags=$(sudo ethtool --show-priv-flags "$ETH")
for f in rx_striding_rq xdp_tx_mpwqe; do
    echo "$flags" | grep -qE "^$f *: off" || {
        echo "$ETH has $f on; the fan-out needs it off:" >&2
        echo "  sudo ethtool --set-priv-flags $ETH $f off" >&2
        exit 1
    }
done

cleanup

if [ "$keep" = 0 ]; then
    rsh "sudo \$HOME/XDP_CLONE/electrode/scripts/cluster.sh up $ranks" >/dev/null
fi
rsh "sudo $REMOTE_ROOT/scripts/ranks.sh > $REMOTE_ROOT/ranks.conf" 2>/dev/null
scp -q "$GRECALE:$REMOTE_ROOT/ranks.conf" "$root/ranks.conf"

# The duplication point is also the router for everything else, so it comes up
# for every mode, this one included when it has nothing to duplicate.
sudo setsid nohup "$root/bpf/mpi_fanout" "$ETH" -o "$root/bpf/$object" \
    -r "$root/ranks.conf" -f "$FANOUT_IP" \
    > /tmp/mpiclone-fanout.log 2>&1 < /dev/null &
for _ in $(seq 50); do grep -q '^ready' /tmp/mpiclone-fanout.log && break; sleep 0.1; done
grep -q '^ready' /tmp/mpiclone-fanout.log || {
    echo "the fan-out node did not come up:" >&2
    cat /tmp/mpiclone-fanout.log >&2; exit 1; }
awk '/^pid /{print $2}' /tmp/mpiclone-fanout.log | sudo tee "$fanout_pid" >/dev/null

if [ "$mode" = tc ]; then
    gw=$(cat /sys/class/net/"$ETH"/address)
    rsh "sudo $REMOTE_ROOT/scripts/node.sh tc-start $ranks $gw" >/dev/null
fi

# What the DUT costs, measured across the whole job.
#
# The loader process is not the thing to watch: it sits in pause() and burns
# nothing, because the work is the XDP program, which runs in NAPI softirq on
# whichever core takes the interrupt. Softirq time in cores is the number that
# means something; the loader's own CPU is recorded beside it to show it is nil.
cpu_snapshot() {
    awk '/^cpu /{busy=$2+$3+$4+$7+$8+$9; printf "%d %d %d\n", busy, $8, busy+$5+$6}' /proc/stat
}
proc_cpu() {
    [ -r "/proc/$1/stat" ] && awk '{print $14+$15}' "/proc/$1/stat" || echo 0
}

fpid=$(cat "$fanout_pid" 2>/dev/null || echo 0)
read -r cpu_b0 cpu_sq0 cpu_t0 <<< "$(cpu_snapshot)"
proc0=$(proc_cpu "$fpid")

rsh "sudo rm -f /tmp/mpiclone-run/osu.log" >/dev/null 2>&1 || true
rsh "sudo $REMOTE_ROOT/scripts/node.sh run $ranks $wire_mode $algo $bytes $iters /tmp/mpiclone-run/osu.log" || true

read -r cpu_b1 cpu_sq1 cpu_t1 <<< "$(cpu_snapshot)"
proc1=$(proc_cpu "$fpid")

ncpu=$(nproc)
hz=$(getconf CLK_TCK)
# The parentheses matter: `>` in an awk printf argument list is a redirection.
dut_busy=$(awk -v d=$((cpu_b1 - cpu_b0)) -v t=$((cpu_t1 - cpu_t0)) -v n="$ncpu" \
    'BEGIN { printf "%.4f", (t > 0 ? d / t * n : 0) }')
dut_softirq=$(awk -v d=$((cpu_sq1 - cpu_sq0)) -v t=$((cpu_t1 - cpu_t0)) -v n="$ncpu" \
    'BEGIN { printf "%.4f", (t > 0 ? d / t * n : 0) }')
dut_loader=$(awk -v d=$((proc1 - proc0)) -v hz="$hz" 'BEGIN { printf "%.3f", d / hz }')

# Both copies go first. A run that fails leaves the previous one's log where
# it was, and parsing that reports the last measurement again under this run's
# labels -- which is how three different modes came out with the same latency
# to the second decimal, and the same packet count, before anyone noticed.
rm -f /tmp/mpiclone-osu.log
rsh "cat /tmp/mpiclone-run/osu.log" > /tmp/mpiclone-osu.log 2>/dev/null || true
if [ ! -s /tmp/mpiclone-osu.log ]; then
    echo "the job produced no output; the run is not a measurement" >&2
fi

python3 "$here/parse.py" --mode "$mode" --algo "$algo" --ranks "$ranks" \
    --bytes "$bytes" --iters "$iters" --rep "$rep" \
    --dut-busy-cores "$dut_busy" --dut-softirq-cores "$dut_softirq" \
    --dut-loader-cpu-s "$dut_loader" \
    ${out:+--out "$out"} /tmp/mpiclone-osu.log
