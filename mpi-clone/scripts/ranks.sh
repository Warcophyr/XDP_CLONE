#!/usr/bin/env bash
# Write ranks.conf from the namespaces that are actually up, rather than from
# whatever a config file last said they were. Runs on grecale.
#
#   sudo ./ranks.sh [prefix] > ranks.conf
#
# The topology itself comes from electrode/scripts/cluster.sh -- the same one,
# deliberately: the two experiments want exactly the same thing, one namespace
# per node with every packet routed through the DUT, and a second copy of it
# would only drift.

set -euo pipefail
prefix=${1:-elec-r}
dev=${DEV:-mv}

n=0
while ip netns list | awk '{print $1}' | grep -qx "$prefix$n"; do
    ip=$(ip netns exec "$prefix$n" ip -4 -br addr show "$dev" | awk '{print $3}' | cut -d/ -f1)
    mac=$(ip netns exec "$prefix$n" ip -br link show "$dev" | awk '{print $3}')
    [ -n "$ip" ] && [ -n "$mac" ] || { echo "$prefix$n has no address on $dev" >&2; exit 1; }
    echo "$ip $mac"
    n=$((n + 1))
done
[ "$n" -gt 0 ] || { echo "no $prefix* namespaces -- bring the topology up first" >&2; exit 1; }
echo "# $n ranks" >&2
