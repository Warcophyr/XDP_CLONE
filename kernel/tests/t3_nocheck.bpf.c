/* EXPECT: REJECT - no xdp_md->data_meta check at all before cloning. */
#include "clone_test.h"

SEC("xdp")
int t3_nocheck(struct xdp_md *ctx)
{
	return CLONE_TX(4);
}

char LICENSE[] SEC("license") = "GPL";
