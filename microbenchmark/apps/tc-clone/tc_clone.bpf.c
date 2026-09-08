#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <linux/pkt_cls.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/tcp.h>
#include <linux/udp.h>

char LICENSE[] SEC("license") = "GPL";

__u64 n_clone = 4;


SEC("tc")
int tc_clone(struct __sk_buff *skb) {
  void *data = (void *)(long)skb->data;
  void *data_end = (void *)(long)skb->data_end;

  struct ethhdr *eth = data;
  if ((void *)(eth + 1) > data_end)
    return TC_ACT_SHOT;
  if (bpf_ntohs(eth->h_proto) != ETH_P_IP)
    return TC_ACT_SHOT;

  struct iphdr *ip = data + sizeof(*eth);
  if ((void *)(ip + 1) > data_end)
    return TC_ACT_SHOT;
  if (ip->protocol != IPPROTO_UDP)
    return TC_ACT_SHOT;

  __u8 ihl = ip->ihl;
  if (ihl < 5)
    return TC_ACT_SHOT;

  void *ip_end = (void *)ip + ihl * 4;
  if (ip_end > data_end)
    return TC_ACT_SHOT;

  struct udphdr *udp = ip_end;
  if ((void *)(udp + 1) > data_end)
    return TC_ACT_SHOT;

  #define MAX_CLONE 512
  #pragma unroll
  for (int i = 0; i < MAX_CLONE; i++) {
    if (i >= n_clone)
        break;
    bpf_clone_redirect(skb, skb->ifindex, 0);


  }

  return bpf_redirect(skb->ifindex, 0);
}
