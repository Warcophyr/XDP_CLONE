// do not change the order of the include
#define BPF_NO_GLOBAL_DATA
#include <linux/bpf.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>

#include "axdp_tx.h"

#define __XDP_CLONE_TX 6
#define XDP_CLONE_TX(num_copy) (((int)(num_copy) << 5) | (int)__XDP_CLONE_TX)

/* Throughput / NDR counterpart of ../xdp-clone, on the driver's shared-page
 * clone path.
 *
 * Stamping the TX descriptor on the *original* is what puts the driver on that
 * path: it means "my copies differ only in what they write to data_meta", so
 * all n+1 frames are emitted out of the one RX page — no page allocation and no
 * 320-byte memcpy per copy. Against ../xdp-clone, which allocates and copies,
 * that is exactly what this benchmark is here to measure.
 *
 * The rule that comes with it: touch nothing but the metadata.
 * bpf_xdp_adjust_head() is out — its memmove of the metadata lands on the
 * packet's first bytes, and the frames already queued for the other emissions
 * share that page. So this pushes, it cannot replace.
 */

/* 0 = stamp only, no inline header at all. Every frame that leaves is
 *     byte-identical to what ../xdp-clone would have sent, same length and
 *     same fanout, so the two are directly comparable and the difference in
 *     the numbers is the page and the memcpy. This is the default for a
 *     reason: it is the only setting that keeps the comparison clean.
 * 1 = push a HDR_LEN-byte header as well, copied from the packet's own first
 *     bytes. Measures the header on top of the shared page, but the frames
 *     leave HDR_LEN bytes longer, so the rates are no longer comparable with
 *     ../xdp-clone — read them against MODE 0 of this same program instead.
 */
#define MODE 0

#if MODE
#define HDR_LEN 12
#else
#define HDR_LEN 0
#endif
#define META_NEED (AXDP_TX_DESC_LEN + HDR_LEN)

/* flow_table_metadata tag. Zero: nothing here installs TX flow rules, and a
 * non-zero tag with no rule to match it would only add a write to the eseg.
 */
#define TX_TAG 0

__u64 n_clone = 4;

/* Stamp the descriptor, and with MODE 1 the inline header before it.
 *
 * @cur_meta is how wide the metadata area is on entry: nothing on an original,
 * AXDP_CLONE_META_SIZE on a copy. A constant at both call sites on purpose --
 * bpf_xdp_adjust_meta() with a variable delta would make the patched verifier
 * treat the metadata as unknown and refuse the clone action.
 *
 * Returns 0, or -1 if it does not fit.
 */
static __always_inline int stamp(struct xdp_md *ctx, __u32 cur_meta) {
  void *data = (void *)(long)ctx->data;
  void *meta;
#if MODE
  void *data_end = (void *)(long)ctx->data_end;
  __u8 hdr[HDR_LEN];

  if (data + HDR_LEN > data_end)
    return -1;
  __builtin_memcpy(hdr, data, HDR_LEN);
#endif

  if (bpf_xdp_adjust_meta(ctx, -(int)(META_NEED - cur_meta)))
    return -1;

  /* Every pointer taken before that call is stale now. */
  data = (void *)(long)ctx->data;
  meta = (void *)(long)ctx->data_meta;
  if (meta + META_NEED > data)
    return -1;

#if MODE
  /* The header goes after the descriptor, never on top of it. */
  __builtin_memcpy(meta + AXDP_TX_DESC_LEN, hdr, HDR_LEN);
#endif

  return axdp_stamp_tx(ctx, TX_TAG, HDR_LEN);
}

SEC("xdp")
int inline_xdp_clone(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;
  void *data_meta = (void *)(long)ctx->data_meta;
  struct ethhdr *eth;
  struct iphdr *iph;
  struct udphdr *udph;
  __u32 ip_hdr_len;

  /* A copy carries its index in the four bytes in front of the data. Every exit
   * of this block is a plain action: the clone action at the bottom has to stay
   * reachable only from the branch where that metadata is *absent*, or the
   * patched verifier turns the program down as a nested clone.
   */
  if (data_meta + AXDP_CLONE_META_SIZE <= data) {
    __u32 num_copy = *(__u32 *)data_meta;

    if (num_copy == 0 || num_copy > n_clone)
      return XDP_DROP;
    if (stamp(ctx, AXDP_CLONE_META_SIZE))
      return XDP_DROP;
    return XDP_TX;
  }

  /* Original packet. Same validation as ../xdp-clone, so that the two drop the
   * same traffic and the rates are comparable.
   */
  eth = data;
  if ((void *)(eth + 1) > data_end)
    return XDP_DROP;

  if (bpf_ntohs(eth->h_proto) != ETH_P_IP)
    return XDP_DROP;

  iph = (void *)(eth + 1);
  if ((void *)(iph + 1) > data_end)
    return XDP_DROP;

  if (iph->protocol != IPPROTO_UDP)
    return XDP_DROP;

  ip_hdr_len = iph->ihl * 4;
  udph = (void *)iph + ip_hdr_len;
  if ((void *)(udph + 1) > data_end)
    return XDP_DROP;

  /* No copies asked for: a plain XDP_TX, not XDP_CLONE_TX(0). It is the same
   * one frame out either way, but the clone action costs the copy-count write
   * and the whole XDP_CLONE_TX tail in the driver, and measurably so. With
   * MODE 0 there is no header to stamp either, which makes this row the same
   * code path as ../xdp-clone at copies=0 -- a shared reference point.
   */
  if (n_clone == 0) {
#if MODE
    if (stamp(ctx, 0))
      return XDP_DROP;
#endif
    return XDP_TX;
  }

  /* The stamp on the original is the request for the shared-page path. Without
   * it the driver would give every copy a page and a byte copy of the packet,
   * which is precisely ../xdp-clone.
   */
  if (stamp(ctx, 0))
    return XDP_DROP;

  return XDP_CLONE_TX(n_clone);
}

char LICENSE[] SEC("license") = "GPL";
