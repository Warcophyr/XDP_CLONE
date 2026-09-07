/* EXPECT: REJECT - the check only rules out 8 or more bytes of metadata, so
 * the else branch still includes the 4 bytes the driver writes on a copy.
 *
 * Note the sizeof(): the comparison must be done in 64-bit arithmetic. With a
 * plain int the compiler truncates the pointer to 32 bits and the verifier
 * rejects the program for pointer arithmetic, before this check applies.
 */
#include "clone_test.h"

SEC("xdp")
int t4_bigcheck(struct xdp_md *ctx)
{
	if (ctx->data_meta + sizeof(__u64) <= ctx->data)
		return XDP_PASS;

	return CLONE_TX(4);
}

char LICENSE[] SEC("license") = "GPL";
