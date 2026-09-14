/* SPDX-License-Identifier: GPL-2.0 */
/* The duplication point for offloaded MPI broadcasts.
 *
 * A rank sends one packet here and says, in the header's destination bitmask,
 * which ranks the copies are for. This turns that one packet into one per set
 * bit with XDP_CLONE_TX, rewriting only the Ethernet/IP/UDP header of each.
 * The same program is also the plain router for every other packet in the job,
 * so that the four points of the comparison cross the same hops and only the
 * number of packets on the wire differs.
 *
 * The bitmask is *read* and never written, which is what lets the inline build
 * exist: on the driver's shared-page path the program may not touch the packet,
 * and it does not need to -- a broadcast payload is identical for every
 * recipient, so everything that differs is in the 42 bytes handed to the NIC as
 * the WQE inline header.
 *
 * Two builds:
 *   (default)      a page and a header rewrite per copy
 *   -DAXDP_INLINE  descriptor stamped on the original, every frame out of the
 *                  one RX page, each header inline in its WQE
 *
 * Structure follows electrode/xdp-fanout/fanout.bpf.c, which does the same job
 * for a fixed destination set.
 */
#define BPF_NO_GLOBAL_DATA
#include <linux/bpf.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>

#include "axdp_tx.h"
#include "../src/wire.h"
#include "fanout_common.h"

#define __XDP_CLONE_TX 6
#define XDP_CLONE_TX(num_copy) (((int)(num_copy) << 5) | (int)__XDP_CLONE_TX)

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __type(key, __u32);
  __type(value, struct mpiclone_peer);
  __uint(max_entries, MPICLONE_MAX_RANKS);
} ranks SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __type(key, __u32);
  __type(value, struct mpiclone_peer);
  __uint(max_entries, MPICLONE_MAX_RANKS * 4);
} peer_by_ip SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __type(key, __u32);
  __type(value, struct mpiclone_cfg);
  __uint(max_entries, 1);
} cfg SEC(".maps");

static __always_inline __u16 ip_checksum(struct iphdr *ip) {
  __u32 sum = 0;
  __u16 *w = (__u16 *)ip;

#pragma unroll
  for (int i = 0; i < 10; i++) {
    if (i == 5) continue; /* the checksum field itself */
    sum += bpf_ntohs(w[i]);
  }
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);

  return bpf_htons(~sum);
}

struct hdrs {
  struct ethhdr *eth;
  struct iphdr *ip;
  struct udphdr *udp;
  struct mpiclone_hdr *mh;
};

static __always_inline int parse_ip(struct xdp_md *ctx, struct hdrs *h) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;

  h->eth = data;
  if ((void *)(h->eth + 1) > data_end) return -1;
  if (h->eth->h_proto != bpf_htons(ETH_P_IP)) return -1;

  h->ip = (void *)(h->eth + 1);
  if ((void *)(h->ip + 1) > data_end) return -1;
  /* The 42-byte header the inline build replaces assumes no IP options. */
  if (h->ip->ihl != 5) return -1;

  h->udp = 0;
  h->mh = 0;
  return 0;
}

static __always_inline int parse_udp(struct xdp_md *ctx, struct hdrs *h) {
  void *data_end = (void *)(long)ctx->data_end;

  if (h->ip->protocol != IPPROTO_UDP) return -1;
  h->udp = (void *)h->ip + sizeof(struct iphdr);
  if ((void *)(h->udp + 1) > data_end) return -1;
  return 0;
}

static __always_inline int parse_mpiclone(struct xdp_md *ctx, struct hdrs *h) {
  void *data_end = (void *)(long)ctx->data_end;

  h->mh = (void *)(h->udp + 1);
  if ((void *)(h->mh + 1) > data_end) return -1;
  if (h->mh->magic != bpf_htonl(MPICLONE_MAGIC)) return -1;
  return 0;
}

/* Which rank the n-th copy of this batch is for. The bitmask is small and the
 * loop is unrolled, so this is a handful of instructions rather than a lookup
 * table that would have to be kept in step with the sender.
 */
static __always_inline int nth_destination(__u64 mask, int n) {
/* Not unrolled: sixty-four copies of the body spill enough registers to put
 * the inline build over the 512-byte BPF stack. A bounded loop is fine here --
 * the verifier has handled these since 5.3 -- and it is a handful of
 * instructions per packet either way.
 */
#pragma clang loop unroll(disable)
  for (int i = 0; i < MPICLONE_MAX_RANKS; i++) {
    if (mask & (1ULL << i)) {
      if (n == 0) return i;
      n--;
    }
  }
  return -1;
}

static __always_inline int count_destinations(__u64 mask) {
  int c = 0;

#pragma clang loop unroll(disable)
  for (int i = 0; i < MPICLONE_MAX_RANKS; i++)
    if (mask & (1ULL << i)) c++;
  return c;
}

#ifndef AXDP_INLINE

/* Rewrite the frame's first 42 bytes in place and transmit. The source address
 * and port are left alone: the receiver has to see which rank sent this.
 */
static __always_inline int send_to(struct hdrs *h, const struct mpiclone_peer *p,
                                   const struct mpiclone_cfg *c) {
  __builtin_memcpy(h->eth->h_source, c->self_mac, ETH_ALEN);
  __builtin_memcpy(h->eth->h_dest, p->eth, ETH_ALEN);

  h->udp->dest = p->port;
  h->udp->check = 0;

  h->ip->daddr = p->addr;
  h->ip->check = ip_checksum(h->ip);

  return XDP_TX;
}

#else /* AXDP_INLINE */

#define FANOUT_HDR_LEN (ETH_HLEN + sizeof(struct iphdr) + sizeof(struct udphdr))
#define FANOUT_META_NEED (((AXDP_TX_DESC_LEN + FANOUT_HDR_LEN) + 3) & ~3U)

/* Build this copy's 42-byte header in its own metadata and hand it to the NIC,
 * replacing the packet's first 42 bytes, which the driver leaves out of the
 * DMA. The packet is never written to -- the rule that comes with the shared
 * page, since every frame of the batch points at it and the DMA is
 * asynchronous.
 *
 * @cur_meta is constant at both call sites on purpose: bpf_xdp_adjust_meta()
 * with a variable delta leaves the metadata unknown to the patched verifier,
 * which then refuses the clone action.
 */
static __always_inline int send_to_inline(struct xdp_md *ctx, struct hdrs *h,
                                          const struct mpiclone_peer *p,
                                          const struct mpiclone_cfg *c,
                                          __u32 cur_meta) {
  /* Separately aligned locals: overlaying them puts the IP header at an odd
   * stack offset and the verifier rejects the misaligned access.
   */
  struct ethhdr e;
  struct iphdr ip;
  struct udphdr udp;
  void *meta;

  __builtin_memcpy(&e, h->eth, sizeof(e));
  __builtin_memcpy(&ip, h->ip, sizeof(ip));
  __builtin_memcpy(&udp, h->udp, sizeof(udp));

  __builtin_memcpy(e.h_source, c->self_mac, ETH_ALEN);
  __builtin_memcpy(e.h_dest, p->eth, ETH_ALEN);

  udp.dest = p->port;
  udp.check = 0;

  ip.daddr = p->addr;
  ip.check = ip_checksum(&ip);

  if (bpf_xdp_adjust_meta(ctx, -(int)(FANOUT_META_NEED - cur_meta)))
    return XDP_DROP;

  meta = (void *)(long)ctx->data_meta;
  if (meta + FANOUT_META_NEED > (void *)(long)ctx->data) return XDP_DROP;

  meta += AXDP_TX_DESC_LEN;
  __builtin_memcpy(meta, &e, sizeof(e));
  __builtin_memcpy(meta + ETH_HLEN, &ip, sizeof(ip));
  __builtin_memcpy(meta + ETH_HLEN + sizeof(struct iphdr), &udp, sizeof(udp));

  if (axdp_stamp_tx_replace(ctx, 0, FANOUT_HDR_LEN)) return XDP_DROP;

  return XDP_TX;
}

#endif /* AXDP_INLINE */

SEC("xdp")
int mpi_fanout(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data;
  void *data_meta = (void *)(long)ctx->data_meta;
  struct mpiclone_peer *p;
  struct mpiclone_cfg *c;
  struct hdrs h;
  __u32 zero = 0;
  int dst;

  c = bpf_map_lookup_elem(&cfg, &zero);
  if (!c) return XDP_ABORTED;

  /* A copy carries its 1-based index in the four bytes in front of the data.
   * Every exit of this block is a plain action: the clone action below has to
   * stay reachable only from the branch where that metadata is absent, or the
   * patched verifier turns the program down as a nested clone.
   */
  if (data_meta + AXDP_CLONE_META_SIZE <= data) {
    __u32 idx = *(__u32 *)data_meta;

    if (idx == 0 || idx >= MPICLONE_MAX_RANKS) return XDP_DROP;
    if (parse_ip(ctx, &h) || parse_udp(ctx, &h) || parse_mpiclone(ctx, &h))
      return XDP_DROP;

    dst = nth_destination(h.mh->dests, (int)idx);
    if (dst < 0) return XDP_DROP;
    p = bpf_map_lookup_elem(&ranks, (__u32 *)&dst);
    if (!p) return XDP_DROP;

#ifndef AXDP_INLINE
    return send_to(&h, p, c);
#else
    return send_to_inline(ctx, &h, p, c, AXDP_CLONE_META_SIZE);
#endif
  }

  /* An original. Anything that is not IPv4 for a host this node routes for
   * goes up its own stack untouched -- ARP, ssh, whatever shares the link.
   */
  if (parse_ip(ctx, &h)) return XDP_PASS;

  if (h.ip->daddr == c->fanout_ip && !parse_udp(ctx, &h) &&
      h.udp->dest == c->fanout_port && !parse_mpiclone(ctx, &h)) {
    __u64 dests = h.mh->dests;
    int count = count_destinations(dests);

    if (count == 0) return XDP_DROP;

    dst = nth_destination(dests, 0);
    if (dst < 0) return XDP_DROP;
    p = bpf_map_lookup_elem(&ranks, (__u32 *)&dst);
    if (!p) return XDP_DROP;

#ifndef AXDP_INLINE
    if (send_to(&h, p, c) != XDP_TX) return XDP_DROP;
#else
    if (send_to_inline(ctx, &h, p, c, 0) != XDP_TX) return XDP_DROP;
#endif

    /* One destination is a plain transmission: XDP_CLONE_TX(0) puts the driver
     * through the whole clone tail for no copy at all. A ring lives here.
     */
    if (count == 1) return XDP_TX;

    return XDP_CLONE_TX(count - 1);
  }

  /* Everything else in the job: route it on, so that the baseline and the TC
   * point cross this node exactly like the two XDP ones do.
   */
  p = bpf_map_lookup_elem(&peer_by_ip, &h.ip->daddr);
  if (!p) return XDP_PASS;

  __builtin_memcpy(h.eth->h_source, c->self_mac, ETH_ALEN);
  __builtin_memcpy(h.eth->h_dest, p->eth, ETH_ALEN);
  return XDP_TX;
}

char LICENSE[] SEC("license") = "GPL";
