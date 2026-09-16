/* EXPECT: REJECT - the descriptor is stamped, so the driver emits the original
 * and every copy out of the one RX page, and then the program writes into the
 * packet. Those bytes are the frames already queued for the other emissions,
 * and the DMA is asynchronous: the copies would go out changed under the
 * hardware's feet. In inline mode the metadata area is the only thing a
 * program may write.
 */
#include "clone_test.h"

SEC("xdp")
int t10_inline_write(struct xdp_md *ctx)
{
	void *data, *data_end;

	if (ctx->data_meta + sizeof(__u32) <= ctx->data)
		return XDP_TX;

	if (stamp_tx(ctx, 0))
		return XDP_DROP;

	/* pointers taken after the stamp, the metadata area moved */
	data = (void *)(long)ctx->data;
	data_end = (void *)(long)ctx->data_end;
	if (data + 1 > data_end)
		return XDP_DROP;

	*(__u8 *)data = 0xaa;		/* this is what the verifier refuses */

	return CLONE_TX(4);
}

char LICENSE[] SEC("license") = "GPL";
