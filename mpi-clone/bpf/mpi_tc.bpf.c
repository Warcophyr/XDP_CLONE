/* SPDX-License-Identifier: GPL-2.0 */
/* The TC point: duplicate the broadcast on the sender's own egress hook.
 *
 * This is Electrode's offload applied to the same packet the XDP fan-out gets,
 * and it is here to answer one question: what does it buy to move the
 * duplication off the sending machine? Both points save the rank the same
 * system calls -- one sendto instead of one per destination -- but this one
 * still makes the copies on the sender's core, each a full dev_queue_xmit,
 * while the XDP one makes them on another machine.
 *
 * Upstream Electrode re-enters this hook once per follower and carries the
 * index in a payload byte it restores afterwards. That is not needed: one run
 * can clone as many times as it likes, since bpf_clone_redirect() leaves the
 * original alone. What is needed is a way for the clone not to be duplicated
 * again when it comes back round the egress path, and skb->mark does that
 * without touching the packet -- which the payload trick could not do here
 * anyway, because the same bytes have to reach every recipient unaltered.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/pkt_cls.h>
#include <linux/udp.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#include "../src/wire.h"
#include "fanout_common.h"

/* Anything already carrying this has been through here. */
#define MPICLONE_TC_MARK 0x4d504943u

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __type(key, __u32);
  __type(value, struct mpiclone_peer);
  __uint(max_entries, MPICLONE_MAX_RANKS);
} ranks SEC(".maps");

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
    if (i == 5) continue;
    sum += bpf_ntohs(w[i]);
  }
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return bpf_htons(~sum);
}

#define ETH_OFF 0
#define IP_OFF ((int)sizeof(struct ethhdr))
#define UDP_OFF (IP_OFF + (int)sizeof(struct iphdr))
#define MH_OFF (UDP_OFF + (int)sizeof(struct udphdr))
#define NEED (MH_OFF + MPICLONE_HDR_LEN)

SEC("tc")
int mpi_tc_bcast(struct __sk_buff *skb) {
  struct mpiclone_cfg *c;
  struct ethhdr *eth;
  struct iphdr *ip;
  struct udphdr *udp;
  struct mpiclone_hdr *mh;
  void *data, *data_end;
  __u32 zero = 0;
  __u64 dests;

  /* A clone of ours on its way out. */
  if (skb->mark == MPICLONE_TC_MARK) return TC_ACT_OK;

  c = bpf_map_lookup_elem(&cfg, &zero);
  if (!c) return TC_ACT_OK;

  if (bpf_skb_pull_data(skb, NEED)) return TC_ACT_OK;
  data = (void *)(long)skb->data;
  data_end = (void *)(long)skb->data_end;
  if (data + NEED > data_end) return TC_ACT_OK;

  eth = data;
  if (eth->h_proto != bpf_htons(ETH_P_IP)) return TC_ACT_OK;
  ip = data + IP_OFF;
  if (ip->ihl != 5 || ip->protocol != IPPROTO_UDP) return TC_ACT_OK;
  udp = data + UDP_OFF;
  if (ip->daddr != c->fanout_ip || udp->dest != c->fanout_port)
    return TC_ACT_OK;
  mh = data + MH_OFF;
  if (mh->magic != bpf_htonl(MPICLONE_MAGIC)) return TC_ACT_OK;

  dests = mh->dests;
  if (!dests) return TC_ACT_SHOT;

  /* Every copy is made from this one run. The original is dropped at the end:
   * it is addressed to the duplication point, which in this mode is here.
   */
#pragma clang loop unroll(disable)
  for (int i = 0; i < MPICLONE_MAX_RANKS; i++) {
    struct mpiclone_peer *p;
    __u32 key = i;

    if (!(dests & (1ULL << i))) continue;

    p = bpf_map_lookup_elem(&ranks, &key);
    if (!p) continue;

    /* bpf_clone_redirect() may have moved the buffer on the previous pass. */
    data = (void *)(long)skb->data;
    data_end = (void *)(long)skb->data_end;
    if (data + NEED > data_end) return TC_ACT_SHOT;
    eth = data;
    ip = data + IP_OFF;
    udp = data + UDP_OFF;

    /* The next hop is the router, not the rank: a macvlan handed a frame
     * addressed to another macvlan on the same parent short-circuits it in
     * software, and the packet would never reach the wire. This is what the
     * sending rank's own stack would have resolved.
     */
    __builtin_memcpy(eth->h_dest, c->gw_mac, ETH_ALEN);

    udp->dest = p->port;
    udp->check = 0; /* optional for IPv4, and the payload is unchanged */

    ip->daddr = p->addr;
    ip->check = ip_checksum(ip);

    skb->mark = MPICLONE_TC_MARK;
    bpf_clone_redirect(skb, skb->ifindex, 0);
  }

  return TC_ACT_SHOT;
}

char LICENSE[] SEC("license") = "GPL";
