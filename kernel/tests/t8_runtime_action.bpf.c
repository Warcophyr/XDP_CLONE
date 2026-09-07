/* EXPECT: ACCEPT - known limitation: when the action itself is computed at
 * runtime the verifier cannot tell whether it is a clone, so it lets the
 * program through. The driver still has to reject a clone action coming from a
 * copy.
 */
#include "clone_test.h"

__u64 act = ACT_CLONE_TX;

SEC("xdp")
int t8_runtime_action(struct xdp_md *ctx)
{
	return (4 << 5) | (int)act;
}

char LICENSE[] SEC("license") = "GPL";
