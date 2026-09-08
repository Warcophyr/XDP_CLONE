#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "tc_clone.bpf.skel.h"

static int if_index;
int n_clone = 4;
static struct tc_clone_bpf *skel;
static struct bpf_tc_hook tc_hook;
static struct bpf_tc_opts tc_opts;

static void sig_handler(int sig) {
  (void)sig;

  bpf_tc_detach(&tc_hook, &tc_opts);
  bpf_tc_hook_destroy(&tc_hook);

  tc_clone_bpf__destroy(skel);
  exit(0);
}

static void bump_memlock_rlimit(void) {
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
    fprintf(stderr, "Usage: %s <ifname>\n", argv[0]);
    return 1;
  }

  if_index = if_nametoindex(argv[1]);
  if (!if_index) {
    fprintf(stderr, "Failed to get ifindex of %s\n", argv[1]);
    return 1;
  }

  bump_memlock_rlimit();

  skel = tc_clone_bpf__open_and_load();
  if (!skel) {
    fprintf(stderr, "Failed to open and load BPF object\n");
    return 1;
  }

  memset(&tc_hook, 0, sizeof(tc_hook));
  tc_hook.sz = sizeof(tc_hook);
  tc_hook.ifindex = if_index;
  tc_hook.attach_point = BPF_TC_INGRESS; /* change to BPF_TC_EGRESS if needed */

  err = bpf_tc_hook_create(&tc_hook);
  if (err && err != -EEXIST) {
    fprintf(stderr, "Failed to create TC hook: %d\n", err);
    tc_clone_bpf__destroy(skel);
    return 1;
  }

  if (argc == 3)
    n_clone = atoi(argv[2]);
  
  // if (n_clone > 64) {
  //   fprintf(stdout, "[WARNING]: Tc seems to have a limit of 64 clones\n");
  //   return 1;
  // }
  skel->data->n_clone = n_clone;

  memset(&tc_opts, 0, sizeof(tc_opts));
  tc_opts.sz = sizeof(tc_opts);
  tc_opts.prog_fd = bpf_program__fd(skel->progs.tc_clone);
  tc_opts.handle = 1;
  tc_opts.priority = 1;

  tc_opts.flags = BPF_TC_F_REPLACE;
  err = bpf_tc_attach(&tc_hook, &tc_opts);
  if (err) {
    fprintf(stderr, "Failed to attach TC program: %d\n", err);
    bpf_tc_hook_destroy(&tc_hook);
    tc_clone_bpf__destroy(skel);
    return 1;
  }

  printf("TC BPF program attached on %s\n", argv[1]);
  fflush(stdout);

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  while (1)
    pause();

  return 0;
}
