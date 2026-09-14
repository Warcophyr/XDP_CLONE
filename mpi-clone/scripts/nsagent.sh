#!/bin/sh
# Open MPI's rsh agent, launching into a network namespace instead of over ssh.
#
# Open MPI invokes the agent the way it would invoke ssh: <agent> <host> <cmd
# string>. `ip netns exec` cannot stand in for it -- it wants a real argv and
# gets one string -- so the command goes to a shell inside the namespace, which
# is what sshd does at the far end.
#
# Three things happen here that each, left out, produce a full set of plausible
# numbers measuring something else:
#
#   the UTS namespace   every rank would otherwise report the host's name, Open
#                       MPI would put them all on one node, and it would move
#                       the messages through shared memory: a three-rank
#                       broadcast came out at 1.78 us with 27 packets on the
#                       wire for the whole run.
#   HWLOC_COMPONENTS=-gl
#                       hwloc probes the X displays for GPUs; abstract AF_UNIX
#                       sockets are scoped to the network namespace, so the
#                       probe falls through to the filesystem socket and blocks
#                       there for good. An Open MPI "hanging with no output"
#                       inside a netns is doing this.
#   taskset             Open MPI binds per node, and each rank is its own node
#                       here, so its idea of binding is no binding: all seven
#                       ranks came out with mask 0-31 on whatever core the
#                       scheduler felt like, SMT siblings included. Latency then
#                       moved by a factor of four between rank counts, in the
#                       native MPI point as much as in ours.
#
# Rank i goes on the i-th cpu of MPICLONE_CPUS, wrapping. Cores 0-15 are one
# hardware thread each of the sixteen physical cores on grecale; 16-31 are
# their siblings, and staying under 16 keeps the ranks off each other until
# there are more ranks than cores, at which point they share -- which is a
# property of running thirty-one ranks on one sixteen-core machine, not of any
# mode, and the same for all of them.
#
# Wrapping rather than CORE_BASE + i: the latter walked off the end of the
# machine at thirty-one ranks, `taskset: failed to set affinity: Invalid
# argument`, and Open MPI reported only that it could not start its daemons.
ns=$1
shift

cpus=${MPICLONE_CPUS:-2-15}
list=$(echo "$cpus" | tr ',' ' ' | while read -r spec; do
    for part in $spec; do
        case "$part" in
            *-*) lo=${part%%-*}; hi=${part##*-}
                 i=$lo; while [ "$i" -le "$hi" ]; do echo "$i"; i=$((i + 1)); done ;;
            *)   echo "$part" ;;
        esac
    done
done)
ncpu=$(echo "$list" | wc -l)

n=${ns##*r}
case "$n" in
  ''|*[!0-9]*) pin= ;;
  *)           cpu=$(echo "$list" | sed -n "$(( n % ncpu + 1 ))p")
               pin="taskset -c $cpu" ;;
esac

exec ip netns exec "$ns" unshare --uts /bin/sh -c \
    "hostname $ns; HWLOC_COMPONENTS=-gl DISPLAY= exec $pin $*"
