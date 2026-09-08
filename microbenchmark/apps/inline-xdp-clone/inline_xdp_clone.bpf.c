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

/* Throughput / NDR counterpart of ../xdp-clone, with the WQE inline header.
 *
 * The header is HDR_LEN bytes -- an Ethernet destination + source MAC pair --
 * and it is a *byte-identical copy* of the packet's own MAC pair, with the
 * packet shortened by as much (bpf_xdp_adjust_head). The frame that leaves is
 * therefore exactly the frame ../xdp-clone would have sent, produced through
 * the hardware inline header instead of straight out of the DMA. Same bytes on
 * the wire, same fanout: whatever difference shows up in the numbers is the
 * cost of the mechanism and nothing else.
 *
 * Changing those bytes would confound the comparison twice over -- a different
 * frame length, and a destination MAC the generator's port may not accept.
 */
#define HDR_LEN 12
#define META_NEED (AXDP_TX_DESC_LEN + HDR_LEN)

/* flow_table_metadata tag. Zero: nothing here installs TX flow rules, and a
 * non-zero tag with no rule to match it would only add a write to the eseg.
 */
#define TX_TAG 0

__u64 n_clone = 4;

/* Hand the first HDR_LEN bytes of the packet to the NIC as the WQE inline
 * header and drop them from the DMA, so the frame comes out unchanged.
 *
 * @cur_meta is how wide the metadata area is on entry: 4 bytes on a copy (the
 * driver put the copy index there), 0 on an original. It is a constant at every
 * call site on purpose -- bpf_xdp_adjust_meta() wants a constant delta to stay
 * out of the verifier's way.
 *
 * Returns 0, or -1 if the packet cannot take the header.
 */
static __always_inline int push_inline_header(struct xdp_md *ctx,
                                              __u32 cur_meta) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;
  __u8 hdr[HDR_LEN];
  void *meta;

  if (data + HDR_LEN > data_end)
    return -1;
  __builtin_memcpy(hdr, data, HDR_LEN);

  if (bpf_xdp_adjust_meta(ctx, -(int)(META_NEED - cur_meta)))
    return -1;

  /* Every pointer taken before that call is stale now. */
  data = (void *)(long)ctx->data;
  meta = (void *)(long)ctx->data_meta;
  if (meta + META_NEED > data)
    return -1;

  /* The header goes after the descriptor, never on top of it. */
  __builtin_memcpy(meta + AXDP_TX_DESC_LEN, hdr, HDR_LEN);

  if (axdp_stamp_tx(ctx, TX_TAG, HDR_LEN))
    return -1;

  /* Replace rather than push: the NIC prepends the header, and these HDR_LEN
   * bytes are the ones it stands in for.
   */
  if (bpf_xdp_adjust_head(ctx, HDR_LEN))
    return -1;

  return 0;
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

  /* A copy carries its index in the four bytes in front of the data. Every
   * exit of this block is a plain action: the clone action at the bottom has to
   * stay reachable only from the branch where that metadata is *absent*, or the
   * patched verifier turns the program down as a nested clone.
   */
  if (data_meta + AXDP_CLONE_META_SIZE <= data) {
    __u32 num_copy = *(__u32 *)data_meta;

    if (num_copy == 0 || num_copy > n_clone)
      return XDP_DROP;
    if (push_inline_header(ctx, AXDP_CLONE_META_SIZE))
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

  /* No copies asked for: a standard XDP_TX and nothing else, so that the
   * copies=0 point is a plain transmission and the same reference in every
   * application. No inline header either -- the original of a clone batch
   * could not carry a descriptor anyway, since the driver writes the copy count
   * over it after this run, so the header only ever rides on the copies.
   */
  if (n_clone == 0)
    return XDP_TX;

  return XDP_CLONE_TX(n_clone);
}

char LICENSE[] SEC("license") = "GPL";
