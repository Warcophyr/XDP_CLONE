#ifndef CLONE_TEST_H
#define CLONE_TEST_H

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

/* The clone actions carry the number of copies in the upper bits of the return
 * value. Defined locally so the tests build against stock kernel headers too,
 * i.e. before the patch is installed on the system.
 */
#define ACT_CLONE_PASS	5
#define ACT_CLONE_TX	6
#define CLONE_PASS(n)	(((int)(n) << 5) | ACT_CLONE_PASS)
#define CLONE_TX(n)	(((int)(n) << 5) | ACT_CLONE_TX)

/* Bytes of metadata the driver puts in front of a copy, holding its index. */
#define CLONE_META_SIZE		4

/* The A-XDP TX descriptor, three words at data_meta, is what asks the driver
 * for the inline TX path: the NIC prepends the header that follows the
 * descriptor to the packet as it DMAs it, and the original and every copy of
 * it are emitted out of the one page. Defined locally like the actions above,
 * and the same values as XDP_TX_INLINE_MAGIC in the patched uapi headers and
 * AXDP_TX_MAGIC in mellanox-clone-xdp/examples/inline-clone/axdp_tx.h.
 */
#define AXDP_TX_MAGIC		0xa7d9c0deu
#define AXDP_TX_DESC_LEN	12

/* Widen the metadata area to hold the descriptor and stamp it, which is what
 * axdp_stamp_tx() does. @cur_meta is how wide the area is on entry: 0 on an
 * original packet, CLONE_META_SIZE on a copy, and a constant at every call
 * site so that the delta stays known to the verifier. No inline header: these
 * tests are about the stamp. Returns 0, or -1 if it does not fit.
 */
static __always_inline int stamp_tx(struct xdp_md *ctx, __u32 cur_meta)
{
	__u32 *desc;
	void *data;

	if (bpf_xdp_adjust_meta(ctx, -(int)(AXDP_TX_DESC_LEN - cur_meta)))
		return -1;

	/* every pointer taken before that call is stale now */
	data = (void *)(long)ctx->data;
	desc = (void *)(long)ctx->data_meta;
	if ((void *)desc + AXDP_TX_DESC_LEN > data)
		return -1;

	desc[0] = 0;			/* flow_table_metadata tag */
	desc[1] = 0;			/* inline_hdr_size: no header */
	desc[2] = AXDP_TX_MAGIC;	/* what makes it opt-in */
	return 0;
}

#endif
