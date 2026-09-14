/* SPDX-License-Identifier: GPL-2.0 */
/* BPF side of the A-XDP TX descriptor: what a program stamps into its own
 * metadata area to ask the NIC for a TX offload.
 *
 * Keep in sync with the AXDP_TX_* definitions in
 * mellanox-out-of-tree-clone/mlx5/core/en/xdp.h, which is where the driver
 * reads this back (mlx5e_xdp_read_tx_desc()).
 */
#ifndef __AXDP_TX_H__
#define __AXDP_TX_H__

#include <linux/bpf.h>

#include <bpf/bpf_helpers.h>

/* Three words at data_meta:
 *
 *   word 0  flow_table_metadata tag, in network order: what TX flow-table
 *           rules match on. Inert until such a rule exists.
 *   word 1  inline_hdr_size: bytes of WQE inline header, which the caller has
 *           placed at data_meta + AXDP_TX_DESC_LEN
 *   word 2  AXDP_TX_MAGIC
 *
 * The magic word is what makes the descriptor opt-in. Without it the driver
 * cannot tell a stamped descriptor from unrelated metadata, and would read
 * whatever is in word 1 as a length -- so never open-code these three words,
 * use the helpers below.
 */
#define AXDP_TX_MAGIC 0xa7d9c0deu
#define AXDP_TX_DESC_LEN 12
#define AXDP_TX_MAX_INLINE 64

/* Top bit of word 1, alongside the length: the header *replaces* the packet's
 * first hdr_len bytes instead of being pushed in front of them. The driver
 * leaves those bytes out of the DMA, so the frame keeps its length and nothing
 * in the packet has to move.
 *
 * bpf_xdp_adjust_head(ctx, hdr_len) does the same thing from here, and is the
 * right tool where the program owns the packet. It is not allowed on the
 * shared-page clone path -- its memmove of the metadata writes over the first
 * bytes of a packet the other emissions are still queued against -- and this
 * flag is how a program asks for a replace there.
 */
#define AXDP_TX_REPLACE 0x80000000u

/* Bytes of metadata the driver puts in front of a *copy* of a packet, holding
 * the 1-based copy index. Same value as XDP_CLONE_META_SIZE in the patched
 * uapi headers, repeated here so that this builds against stock ones too.
 */
#define AXDP_CLONE_META_SIZE 4

/* @tag goes to eseg->flow_table_metadata; @hdr_len bytes of inline header must
 * already sit at data_meta + AXDP_TX_DESC_LEN, so the metadata area has to be
 * at least AXDP_TX_DESC_LEN + @hdr_len bytes wide (bpf_xdp_adjust_meta()).
 *
 * bpf_xdp_adjust_head(ctx, @hdr_len) is the other way to get a replace, and the
 * right one where the program owns the packet; AXDP_TX_REPLACE is for where it
 * does not, such as the shared-page clone path.
 */
/* Stamp the descriptor. @hdr_len bytes of inline header must already sit at
 * data_meta + AXDP_TX_DESC_LEN. Returns 0, or -1 if it does not fit.
 *
 * A stamp is the only thing that makes the driver apply a TX offload; an
 * unstamped buffer is transmitted as it is.
 *
 * One bounds check and one place that reads data_meta, on purpose: a helper
 * that re-read it to set a flag afterwards would hand the verifier a pointer
 * with no proven range.
 */
static __always_inline int __axdp_stamp(struct xdp_md *ctx, __u32 tag,
                                        __u32 hdr_len, __u32 flags) {
  __u32 *desc = (void *)(long)ctx->data_meta;
  void *data = (void *)(long)ctx->data;

  if (hdr_len > AXDP_TX_MAX_INLINE)
    return -1;
  if ((void *)desc + AXDP_TX_DESC_LEN + hdr_len > data)
    return -1;

  desc[0] = tag;
  desc[1] = hdr_len | flags;
  desc[2] = AXDP_TX_MAGIC;
  return 0;
}

/* Push: the NIC puts the header in front of the packet, which stays where it
 * is, and the frame comes out @hdr_len bytes longer.
 */
static __always_inline int axdp_stamp_tx(struct xdp_md *ctx, __u32 tag,
                                         __u32 hdr_len) {
  return __axdp_stamp(ctx, tag, hdr_len, 0);
}

/* Replace: the header stands in for the packet's first @hdr_len bytes, which
 * the driver leaves out of the DMA, so the frame keeps its length.
 */
static __always_inline int axdp_stamp_tx_replace(struct xdp_md *ctx, __u32 tag,
                                                 __u32 hdr_len) {
  return __axdp_stamp(ctx, tag, hdr_len, AXDP_TX_REPLACE);
}

/* Tag only, no inline header. */
static __always_inline int axdp_stamp_tag(struct xdp_md *ctx, __u32 tag) {
  return axdp_stamp_tx(ctx, tag, 0);
}

/* The names the programs in the A-XDP tree use (mlx5/core/xdp/axdp.h), kept as
 * aliases so that a program written against either header builds against this
 * one unchanged. The wire format is the same, so only the spelling differs.
 */
static __always_inline int stamp_metadata_hdr(struct xdp_md *ctx, __u32 tag,
                                              __u32 hdr_len) {
  return axdp_stamp_tx(ctx, tag, hdr_len);
}

static __always_inline int stamp_metadata(struct xdp_md *ctx, int tag) {
  return axdp_stamp_tx(ctx, (__u32)tag, 0);
}

#endif /* __AXDP_TX_H__ */
