/* EXPECT: REJECT - nested clone: copies asked for on the branch where the
 * clone metadata is present, i.e. while running on a copy.
 */
#include "clone_test.h"

SEC("xdp")
int t2_nested(struct xdp_md *ctx)
{
	if (ctx->data_meta + sizeof(__u32) <= ctx->data)
		return CLONE_TX(4);

	return XDP_PASS;
}

char LICENSE[] SEC("license") = "GPL";
