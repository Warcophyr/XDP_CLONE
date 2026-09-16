/* EXPECT: ACCEPT - the same write into the packet as t10, without the stamp.
 * Nothing then asks for the shared page, the driver gives every copy a page
 * and a byte copy of the packet, and rewriting it per copy is exactly what
 * that path is for. The inline rule must not get in the way of the programs
 * that predate it: this is the shape of examples/clone-tx.
 */
#include "clone_test.h"

SEC("xdp")
int t12_write_no_stamp(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;

	if (data + 1 > data_end)
		return XDP_DROP;

	*(__u8 *)data = 0xaa;

	if (ctx->data_meta + sizeof(__u32) <= ctx->data)
		return XDP_TX;

	return CLONE_TX(4);
}

char LICENSE[] SEC("license") = "GPL";
