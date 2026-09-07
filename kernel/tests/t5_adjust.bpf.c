/* EXPECT: REJECT - the program drops the metadata with bpf_xdp_adjust_meta()
 * and only then "proves" it is not there: a copy would pass the check.
 */
#include "clone_test.h"

SEC("xdp")
int t5_adjust(struct xdp_md *ctx)
{
	bpf_xdp_adjust_meta(ctx, 4);

	if (ctx->data_meta + sizeof(__u32) <= ctx->data)
		return XDP_PASS;

	return CLONE_TX(4);
}

char LICENSE[] SEC("license") = "GPL";
