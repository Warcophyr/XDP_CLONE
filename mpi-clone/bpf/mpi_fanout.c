/* SPDX-License-Identifier: GPL-2.0 */
/* Loader for the MPI broadcast duplication point.
 *
 * One binary drives both builds of the object -- the copy path and the
 * shared-page inline one -- because everything it configures lives in maps.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

static const char *ifname;
static const char *objpath = "mpi_fanout.bpf.o";
static const char *ranks_path = "ranks.conf";
static const char *fanout_ip = "192.168.101.1";

static int ifindex;
static struct bpf_object *obj;
static int attached;

static void usage(const char *a0) {
  fprintf(stderr,
          "usage: %s <ifname> [-o object.o] [-r ranks.conf] [-f fanout-ip]\n"
          "\n"
          "  -o  mpi_fanout.bpf.o (copy path) or mpi_fanout_inline.bpf.o\n"
          "  -r  one '<ipv4> <mac>' line per rank, in rank order\n"
          "  -f  the address the ranks send their one packet to\n"
          "\n"
          "Rank i is expected on UDP port %d + i, which is how the ranks bind.\n",
          a0, MPICLONE_PORT_BASE);
  exit(EXIT_FAILURE);
}

static int parse_mac(const char *s, __u8 out[ETH_ALEN]) {
  unsigned int v[ETH_ALEN];

  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return -1;
  for (int i = 0; i < ETH_ALEN; i++) out[i] = (__u8)v[i];
  return 0;
}

static void read_self_mac(__u8 out[ETH_ALEN]) {
  struct ifreq ifr;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);

  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
  if (fd < 0 || ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
    perror("SIOCGIFHWADDR");
    exit(EXIT_FAILURE);
  }
  close(fd);
  memcpy(out, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
}

static void fill_maps(void) {
  int fd_ranks = bpf_object__find_map_fd_by_name(obj, "ranks");
  int fd_peers = bpf_object__find_map_fd_by_name(obj, "peer_by_ip");
  int fd_cfg = bpf_object__find_map_fd_by_name(obj, "cfg");
  struct mpiclone_cfg c;
  char line[256], ip[64], mac[64];
  __u32 zero = 0, n = 0;
  FILE *fp;

  if (fd_ranks < 0 || fd_peers < 0 || fd_cfg < 0) {
    fprintf(stderr, "Error: the object is missing one of its maps\n");
    exit(EXIT_FAILURE);
  }

  fp = fopen(ranks_path, "r");
  if (!fp) {
    fprintf(stderr, "Error: cannot open %s: %s\n", ranks_path, strerror(errno));
    exit(EXIT_FAILURE);
  }

  while (fgets(line, sizeof(line), fp)) {
    struct mpiclone_peer p;
    struct in_addr in;

    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%63s %63s", ip, mac) != 2) continue;
    if (n >= MPICLONE_MAX_RANKS) {
      fprintf(stderr, "Error: more than %d ranks\n", MPICLONE_MAX_RANKS);
      exit(EXIT_FAILURE);
    }
    if (inet_pton(AF_INET, ip, &in) != 1 || parse_mac(mac, p.eth)) {
      fprintf(stderr, "Error: %s line %u is not '<ipv4> <mac>'\n", ranks_path, n + 1);
      exit(EXIT_FAILURE);
    }

    p.addr = in.s_addr;
    p.port = htons(MPICLONE_PORT_BASE + n);

    if (bpf_map_update_elem(fd_ranks, &n, &p, BPF_ANY) ||
        bpf_map_update_elem(fd_peers, &p.addr, &p, BPF_ANY)) {
      fprintf(stderr, "Error: rank %u: %s\n", n, strerror(errno));
      exit(EXIT_FAILURE);
    }
    printf("rank %2u  %s:%d  %s\n", n, ip, MPICLONE_PORT_BASE + n, mac);
    n++;
  }
  fclose(fp);

  if (!n) {
    fprintf(stderr, "Error: %s lists no ranks\n", ranks_path);
    exit(EXIT_FAILURE);
  }

  memset(&c, 0, sizeof(c));
  c.n_ranks = n;
  if (inet_pton(AF_INET, fanout_ip, &c.fanout_ip) != 1) usage("mpi_fanout");
  c.fanout_port = htons(MPICLONE_FANOUT_PORT);
  read_self_mac(c.self_mac);

  if (bpf_map_update_elem(fd_cfg, &zero, &c, BPF_ANY)) {
    fprintf(stderr, "Error: cfg: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  printf("fan-out %s:%d over %u ranks, out of %s "
         "(%02x:%02x:%02x:%02x:%02x:%02x)\n",
         fanout_ip, MPICLONE_FANOUT_PORT, n, ifname, c.self_mac[0],
         c.self_mac[1], c.self_mac[2], c.self_mac[3], c.self_mac[4],
         c.self_mac[5]);
}

static void on_signal(int sig) {
  (void)sig;
  if (attached) bpf_xdp_detach(ifindex, 0, NULL);
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
  while ((opt = getopt(argc, argv, "o:r:f:")) != -1) {
    switch (opt) {
    case 'o': objpath = optarg; break;
    case 'r': ranks_path = optarg; break;
    case 'f': fanout_ip = optarg; break;
    default: usage(argv[0]);
    }
  }

  ifindex = if_nametoindex(ifname);
  if (!ifindex) {
    fprintf(stderr, "Error: no interface %s\n", ifname);
    return 1;
  }

  setrlimit(RLIMIT_MEMLOCK, &r);

  obj = bpf_object__open_file(objpath, NULL);
  if (!obj) {
    fprintf(stderr, "Error: cannot open %s: %s\n", objpath, strerror(errno));
    return 1;
  }
  if ((err = bpf_object__load(obj))) {
    fprintf(stderr, "Error: load %s failed: %s\n", objpath, strerror(-err));
    return 1;
  }
  prog = bpf_object__find_program_by_name(obj, "mpi_fanout");
  if (!prog) {
    fprintf(stderr, "Error: no program 'mpi_fanout' in %s\n", objpath);
    return 1;
  }

  fill_maps();

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  if (bpf_xdp_attach(ifindex, bpf_program__fd(prog), 0, NULL)) {
    fprintf(stderr, "Error: XDP attach on %s: %s\n", ifname, strerror(errno));
    return 1;
  }
  attached = 1;
  printf("pid %d\nready\n", (int)getpid());
  fflush(stdout);

  pause();
  return 0;
}
