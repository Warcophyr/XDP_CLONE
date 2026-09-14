/* SPDX-License-Identifier: GPL-2.0 */
/* Loader for the TC duplication point. Run once inside each rank's namespace:
 * every rank that forwards a broadcast needs the program on its own egress
 * hook, which for a binomial tree is most of them.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

typedef unsigned int __u32;
typedef unsigned short __u16;
typedef unsigned char __u8;
typedef unsigned long long __u64;
#include "../src/wire.h"
#include "fanout_common.h"

static const char *ifname = "mv";
static const char *objpath = "mpi_tc.bpf.o";
static const char *ranks_path = "ranks.conf";
static const char *fanout_ip = "192.168.101.1";
static const char *gw_mac_s;

static int ifindex;
static struct bpf_object *obj;
static struct bpf_tc_hook hook;
static struct bpf_tc_opts opts;
static int attached;

static void usage(const char *a0) {
  fprintf(stderr,
          "usage: %s <ifname> -g <gateway-mac> [-o object.o] [-r ranks.conf]\n"
          "                   [-f fanout-ip]\n"
          "\n"
          "  -g  MAC of the next hop, which every copy is addressed to\n",
          a0);
  exit(EXIT_FAILURE);
}

static int parse_mac(const char *s, __u8 out[ETH_ALEN]) {
  unsigned int v[ETH_ALEN];

  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return -1;
  for (int i = 0; i < ETH_ALEN; i++) out[i] = (__u8)v[i];
  return 0;
}

static void fill_maps(void) {
  int fd_ranks = bpf_object__find_map_fd_by_name(obj, "ranks");
  int fd_cfg = bpf_object__find_map_fd_by_name(obj, "cfg");
  struct mpiclone_cfg c;
  char line[256], ip[64], mac[64];
  __u32 zero = 0, n = 0;
  FILE *fp;

  fp = fopen(ranks_path, "r");
  if (!fp || fd_ranks < 0 || fd_cfg < 0) {
    fprintf(stderr, "Error: %s: %s\n", ranks_path, strerror(errno));
    exit(EXIT_FAILURE);
  }
  while (fgets(line, sizeof(line), fp)) {
    struct mpiclone_peer p;
    struct in_addr in;

    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%63s %63s", ip, mac) != 2) continue;
    if (n >= MPICLONE_MAX_RANKS) break;
    if (inet_pton(AF_INET, ip, &in) != 1 || parse_mac(mac, p.eth)) {
      fprintf(stderr, "Error: %s line %u\n", ranks_path, n + 1);
      exit(EXIT_FAILURE);
    }
    p.addr = in.s_addr;
    p.port = htons(MPICLONE_PORT_BASE + n);
    if (bpf_map_update_elem(fd_ranks, &n, &p, BPF_ANY)) {
      fprintf(stderr, "Error: rank %u: %s\n", n, strerror(errno));
      exit(EXIT_FAILURE);
    }
    n++;
  }
  fclose(fp);

  memset(&c, 0, sizeof(c));
  c.n_ranks = n;
  if (inet_pton(AF_INET, fanout_ip, &c.fanout_ip) != 1) usage("mpi_tc");
  c.fanout_port = htons(MPICLONE_FANOUT_PORT);
  if (!gw_mac_s || parse_mac(gw_mac_s, c.gw_mac)) usage("mpi_tc");

  if (bpf_map_update_elem(fd_cfg, &zero, &c, BPF_ANY)) {
    fprintf(stderr, "Error: cfg: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }
  printf("%u ranks, next hop %s, on %s\n", n, gw_mac_s, ifname);
}

static void detach(void) {
  if (!attached) return;
  opts.flags = opts.prog_fd = opts.prog_id = 0;
  bpf_tc_detach(&hook, &opts);
  bpf_tc_hook_destroy(&hook);
  attached = 0;
}

static void on_signal(int sig) {
  (void)sig;
  detach();
  printf("\ndetached\n");
  exit(0);
}

int main(int argc, char **argv) {
  struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
  struct bpf_program *prog;
  int opt, err;

  if (argc < 2 || argv[1][0] == '-') usage(argv[0]);
  ifname = argv[1];
  optind = 2;
  while ((opt = getopt(argc, argv, "g:o:r:f:")) != -1) {
    switch (opt) {
    case 'g': gw_mac_s = optarg; break;
    case 'o': objpath = optarg; break;
    case 'r': ranks_path = optarg; break;
    case 'f': fanout_ip = optarg; break;
    default: usage(argv[0]);
    }
  }
  if (!gw_mac_s) usage(argv[0]);

  ifindex = if_nametoindex(ifname);
  if (!ifindex) {
    fprintf(stderr, "Error: no interface %s\n", ifname);
    return 1;
  }
  setrlimit(RLIMIT_MEMLOCK, &r);

  obj = bpf_object__open_file(objpath, NULL);
  if (!obj || (err = bpf_object__load(obj))) {
    fprintf(stderr, "Error: cannot load %s\n", objpath);
    return 1;
  }
  prog = bpf_object__find_program_by_name(obj, "mpi_tc_bcast");
  if (!prog) {
    fprintf(stderr, "Error: no program 'mpi_tc_bcast' in %s\n", objpath);
    return 1;
  }

  fill_maps();
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  memset(&hook, 0, sizeof(hook));
  hook.sz = sizeof(hook);
  hook.ifindex = ifindex;
  hook.attach_point = BPF_TC_EGRESS;
  err = bpf_tc_hook_create(&hook);
  if (err && err != -EEXIST) {
    fprintf(stderr, "Error: clsact on %s: %s\n", ifname, strerror(-err));
    return 1;
  }
  memset(&opts, 0, sizeof(opts));
  opts.sz = sizeof(opts);
  opts.prog_fd = bpf_program__fd(prog);
  if ((err = bpf_tc_attach(&hook, &opts))) {
    fprintf(stderr, "Error: TC egress attach on %s: %s\n", ifname, strerror(-err));
    return 1;
  }
  attached = 1;
  printf("pid %d\nready\n", (int)getpid());
  fflush(stdout);
  pause();
  detach();
  return 0;
}
