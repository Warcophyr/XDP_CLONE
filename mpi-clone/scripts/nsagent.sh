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
# Rank i goes on core MPICLONE_CORE_BASE + i. Cores 0-15 are one hardware
# thread each of the sixteen physical cores on grecale; 16-31 are their
# siblings, and staying under 16 keeps the ranks off each other.
ns=$1
shift

base=${MPICLONE_CORE_BASE:-2}
n=${ns##*r}
case "$n" in
  ''|*[!0-9]*) pin= ;;
  *)           pin="taskset -c $((base + n))" ;;
esac

exec ip netns exec "$ns" unshare --uts /bin/sh -c \
    "hostname $ns; HWLOC_COMPONENTS=-gl DISPLAY= exec $pin $*"
