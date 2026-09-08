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

/* Latency counterpart of ../xdp-clone-tstamp, with the WQE inline header.
 *
 * This one stays on the copy path -- a page and a byte copy per copy -- and it
 * cannot be moved to the driver's shared-page mode the way ../inline-xdp-clone
 * was. The reason is the latency magic below: the whole trick that makes TRex
 * see one sample per packet sent, instead of n+1 duplicates, is *editing the
 * payload* of one copy and not the others. On a shared page all the emissions
 * are the same bytes, so writing 0xab for the last copy would change the frames
 * already queued for the earlier ones and for the original. A per-copy page is
 * exactly what this needs.
 *
 * So shared mode is measurable on throughput and NDR, not on latency. Moving
 * the magic into the inline header is not a way out either: it sits at payload
 * offset 2, far past anything an inline header can reach.
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

/* The latency profile (profiles/clonlat.py) sends to this port only. */
#define UDP_PORT 8901

/* Offset of TRex's latency magic inside the UDP payload, and the byte it
 * expects there. Cloning a latency packet would hand the generator n+1 samples
 * carrying the same sequence number, which it counts as duplicates rather than
 * as latency, so the magic is stripped from the original and put back on
 * exactly one of the copies -- the last one. Same trick as
 * ../xdp-clone-tstamp, and the reason that app exists next to ../xdp-clone.
 */
#define TSTAMP_MAGIC_OFF 2
#define TSTAMP_MAGIC 0xab
#define TSTAMP_MIN_PAYLOAD 18

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
  unsigned char *payload;
  __u32 ip_hdr_len;

  /* Parsed up front, unlike ../inline-xdp-clone: both branches below need the
   * payload to get at the latency magic.
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

  if (bpf_ntohs(udph->dest) != UDP_PORT)
    return XDP_DROP;

  payload = (void *)udph + sizeof(struct udphdr);

  /* A copy carries its index in the four bytes in front of the data. Every exit
   * of this block is a plain action: the clone action at the bottom has to stay
   * reachable only from the branch where that metadata is *absent*, or the
   * patched verifier turns the program down as a nested clone.
   */
  if (data_meta + AXDP_CLONE_META_SIZE <= data) {
    __u32 num_copy = *(__u32 *)data_meta;

    if (num_copy == 0 || num_copy > n_clone)
      return XDP_DROP;

    /* The last copy is the one the generator gets to time. */
    if (num_copy == n_clone &&
        (void *)(payload + TSTAMP_MIN_PAYLOAD) <= data_end)
      payload[TSTAMP_MAGIC_OFF] = TSTAMP_MAGIC;

    if (push_inline_header(ctx, AXDP_CLONE_META_SIZE))
      return XDP_DROP;
    return XDP_TX;
  }

  /* Original packet: strip the magic, so that only the copy above carries it.
   * The copies are taken from this buffer after the run, so the write lands in
   * all of them.
   */
  if (n_clone != 0 && (void *)(payload + TSTAMP_MIN_PAYLOAD) <= data_end)
    payload[TSTAMP_MAGIC_OFF] = 0x00;

  /* No copies asked for: a plain XDP_TX, not XDP_CLONE_TX(0). It is the same
   * one frame out either way, but the clone action costs the copy-count write
   * and the whole XDP_CLONE_TX tail in the driver, and measurably so. No
   * inline header either: there are no copies for one to ride on, so this row
   * is the same code path as ../xdp-clone-tstamp at copies=0.
   */
  if (n_clone == 0)
    return XDP_TX;

  return XDP_CLONE_TX(n_clone);
}

char LICENSE[] SEC("license") = "GPL";
