/* EXPECT: ACCEPT - inline TX mode done right: the program stamps the TX
 * descriptor into its own metadata area, asks for copies on the branch where
 * no clone metadata is present, and never touches the packet. This is the
 * shape of mellanox-clone-xdp/examples/inline-clone.
 *
 * Note what the two data_meta comparisons are for. The first one tells an
 * original from a copy, and is the one the clone action is checked against.
 * The second, after bpf_xdp_adjust_meta(), is the bounds check on the area the
 * program just widened for itself; it says nothing about the driver's
 * metadata, and the verifier must not read it as "this is a copy".
 */
#include "clone_test.h"

SEC("xdp")
int t9_inline_ok(struct xdp_md *ctx)
{
	if (ctx->data_meta + sizeof(__u32) <= ctx->data) {
		/* a copy: stamp a descriptor of its own and send it out */
		if (stamp_tx(ctx, CLONE_META_SIZE))
			return XDP_DROP;

		return XDP_TX;
	}

	if (stamp_tx(ctx, 0))
		return XDP_DROP;

	return CLONE_TX(4);
}

char LICENSE[] SEC("license") = "GPL";
