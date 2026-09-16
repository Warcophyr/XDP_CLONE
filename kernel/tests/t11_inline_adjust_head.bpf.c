/* EXPECT: REJECT - bpf_xdp_adjust_head() counts as writing the packet: it
 * memmoves the metadata area over the packet's first bytes, which on the
 * shared page belong to the frames queued for the other copies. A program that
 * wants the header to replace those bytes rather than to be pushed in front of
 * them asks the driver for it with AXDP_TX_REPLACE in the descriptor, where it
 * costs one addition to a descriptor and touches nothing.
 */
#include "clone_test.h"

SEC("xdp")
int t11_inline_adjust_head(struct xdp_md *ctx)
{
	if (ctx->data_meta + sizeof(__u32) <= ctx->data)
		return XDP_TX;

	if (stamp_tx(ctx, 0))
		return XDP_DROP;

	if (bpf_xdp_adjust_head(ctx, 4))
		return XDP_DROP;

	return CLONE_TX(4);
}

char LICENSE[] SEC("license") = "GPL";
