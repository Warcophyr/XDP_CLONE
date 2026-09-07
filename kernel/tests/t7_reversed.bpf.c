/* EXPECT: ACCEPT - same check with the operands the other way around, and with
 * XDP_CLONE_PASS instead of XDP_CLONE_TX.
 */
#include "clone_test.h"

SEC("xdp")
int t7_reversed(struct xdp_md *ctx)
{
	if (ctx->data >= ctx->data_meta + sizeof(__u32))
		return XDP_PASS;

	return CLONE_PASS(2);
}

char LICENSE[] SEC("license") = "GPL";
