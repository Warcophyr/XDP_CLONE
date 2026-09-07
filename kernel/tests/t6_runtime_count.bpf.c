/* EXPECT: ACCEPT - the number of copies may be computed at runtime, only the
 * action bits have to be known at verification time. This is the shape used by
 * mellanox-clone-xdp/examples/clone-tx.
 */
#include "clone_test.h"

__u64 n_clone = 4;

SEC("xdp")
int t6_runtime_count(struct xdp_md *ctx)
{
	if (ctx->data_meta + sizeof(__u32) <= ctx->data)
		return XDP_PASS;

	return CLONE_TX(n_clone);
}

char LICENSE[] SEC("license") = "GPL";
