#!/bin/sh
# Open MPI's rsh agent, launching into a network namespace instead of over ssh.
#
# Open MPI invokes the agent the way it would invoke ssh: <agent> <host> <cmd
# string>. `ip netns exec` cannot stand in for it -- it wants a real argv and
# gets one string -- so the command goes to a shell inside the namespace, which
# is what sshd does at the far end.
#
# The UTS namespace matters as much as the network one. Every rank would
# otherwise report `grecale`, Open MPI would put them all on one node, and it
# would move the messages through shared memory: a three-rank broadcast came
# out at 1.78 us and the wire carried 27 packets for the whole run. With a
# hostname each they are separate nodes and the traffic goes where it should.
#
# HWLOC_COMPONENTS=-gl is not optional either: hwloc probes the X displays for
# GPUs, abstract AF_UNIX sockets are scoped to the network namespace, so the
# probe falls through to the filesystem socket and blocks there for good. That
# is what an Open MPI "hanging with no output" inside a netns is doing.
ns=$1
shift
exec ip netns exec "$ns" unshare --uts /bin/sh -c \
    "hostname $ns; HWLOC_COMPONENTS=-gl DISPLAY= exec $*"
