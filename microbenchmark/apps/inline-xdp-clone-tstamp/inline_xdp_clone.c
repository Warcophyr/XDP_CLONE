#include <bpf/libbpf.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include "inline_xdp_clone.bpf.skel.h"

int if_index;
int n_clone = 4;
struct inline_xdp_clone_bpf *skel;

void sig_handler(int sig) {
  bpf_xdp_detach(if_index, 0, NULL);
  inline_xdp_clone_bpf__destroy(skel);
  exit(0);
}

void bump_memlock_rlimit(void) {
  struct rlimit rlim_new = {
      .rlim_cur = RLIM_INFINITY,
      .rlim_max = RLIM_INFINITY,
  };

  if (setrlimit(RLIMIT_MEMLOCK, &rlim_new)) {
    fprintf(stderr, "Failed to increase RLIMIT_MEMLOCK limit!\n");
    exit(1);
  }
}
int main(int argc, char **argv) {

  int err;
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <ifname> [n clones]\n", argv[0]);
    return 1;
  }

  bump_memlock_rlimit();

  skel = inline_xdp_clone_bpf__open_and_load();

  if (!skel) {
    fprintf(stderr, "Failed to open and load BPF object\n");
    return 1;
  }

  if_index = if_nametoindex(argv[1]);
  if (!if_index) {
    fprintf(stderr, "Failed to get ifindex of %s\n", argv[1]);
    return 1;
  }

  if (argc == 3)
    n_clone = atoi(argv[2]);

  skel->data->n_clone = n_clone;

  err = bpf_xdp_attach(if_index, bpf_program__fd(skel->progs.inline_xdp_clone), 0,
                       NULL);
  if (err) {
    fprintf(stderr, "Failed to attach BPF program\n");
    return 1;
  }
  printf("BPF program attached\n");
  fflush(stdout);

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  while (1)
    ;

  return 0;
}