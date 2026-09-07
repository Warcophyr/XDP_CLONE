/* EXPECT: ACCEPT - copies asked for only on the branch where the program
 * proved that no clone metadata is present, i.e. on an original packet.
 */
#include "clone_test.h"

SEC("xdp")
int t1_clone_ok(struct xdp_md *ctx)
{
	if (ctx->data_meta + sizeof(__u32) <= ctx->data)
		return XDP_PASS;	/* this is a copy: do not clone again */

	return CLONE_TX(4);
}

char LICENSE[] SEC("license") = "GPL";
