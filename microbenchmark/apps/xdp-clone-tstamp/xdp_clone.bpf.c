// do not change the order of the include
#define BPF_NO_GLOBAL_DATA
#include <linux/bpf.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/if_ether.h>
#include <linux/if_xdp.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>

// #ifdef DEBUG
// #define bpf_printk(fmt, ...) \
//   ({ \
//     char ____fmt[] = fmt; \
//     bpf_trace_printk(____fmt, sizeof(____fmt), ##__VA_ARGS__); \
//   })
// #else
// #define bpf_printk(fmt, ...) ({})
// #endif

#define __XDP_CLONE_PASS 5
#define __XDP_CLONE_TX 6
#define XDP_CLONE_PASS(num_copy)                                               \
  (((int)(num_copy) << 5) | (int)__XDP_CLONE_PASS)
#define XDP_CLONE_TX(num_copy) (((int)(num_copy) << 5) | (int)__XDP_CLONE_TX)

__u64 n_clone = 4;

SEC("xdp")
int xdp_clone(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;
  void *data_meta = (void *)(long)ctx->data_meta;

  // Basic packet validation (original packet without metadata)
  struct ethhdr *eth = data;
  if ((void *)(eth + 1) > data_end) {
    bpf_printk("XDP: Ethernet header validation failed\n");
    return XDP_DROP;
  }

  // Only process IP packets
  if (bpf_ntohs(eth->h_proto) != ETH_P_IP) {
    return XDP_DROP;
  }

  struct iphdr *iph = (void *)(eth + 1);
  if ((void *)(iph + 1) > data_end) {
    bpf_printk("XDP: IP header validation failed\n");
    return XDP_DROP;
  }

  // Only process UDP packets
  if (iph->protocol != IPPROTO_UDP) {
    return XDP_DROP;
  }

  __u32 ip_hdr_len = iph->ihl * 4;
  struct udphdr *udph = (void *)iph + ip_hdr_len;
  if ((void *)(udph + 1) > data_end) {
    bpf_printk("XDP: UDP header validation failed\n");
    return XDP_DROP;
  }
  if (bpf_ntohs(udph->dest) != 8901) {
    return XDP_DROP;
  }
  void *payload = (void *)udph + sizeof(struct udphdr);

  if (payload + 18 <= data_end) {
    unsigned char *p = payload;
    // rimuove magic number latency
    if (n_clone != 0) {
      p[2] = 0x00;
    }
  }

  if (ctx->data_meta + sizeof(__u32) <= ctx->data) {
    __u32 num_copy = 0;
    num_copy = *(__u32 *)data_meta;

    /* Consider valid metadata only for actual clone copies. */
    if (num_copy > 0 && num_copy < n_clone) {
      // bpf_printk("copia num_copy: %u\n", num_copy);
      return XDP_TX;
    }
    if (num_copy == n_clone) {
      if (payload + 18 <= data_end) {
        unsigned char *p = payload;
        // riscrive magic number latency
        p[2] = 0xab;
      }
      return XDP_TX;
    }
    bpf_printk("errore num_copy: %u\n", num_copy);
  }
  

  return XDP_CLONE_TX(n_clone);
}

char LICENSE[] SEC("license") = "GPL";
