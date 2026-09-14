/* SPDX-License-Identifier: GPL-2.0 */
/* MPI_Bcast over a UDP side channel, so that the duplication can be offloaded.
 *
 * Why a wrapper. Open MPI carries messages over TCP here -- there is no verbs
 * device and no UDP provider -- and a TCP segment cannot be cloned to several
 * peers: the sequence numbers, ports, checksums and connection state are all
 * per-peer. So the driver cannot act on MPI's own transport at all. What it can
 * act on is a datagram, and the MPI profiling interface lets one collective be
 * moved onto datagrams without touching the application: everything except
 * MPI_Bcast goes straight to PMPI_*, and the benchmark on top is stock
 * osu_bcast or IMB, unmodified and unaware.
 *
 *   MPICLONE_MODE=mpi     call PMPI_Bcast -- the baseline, and the control that
 *                         says the wrapper itself costs nothing
 *              =udp       our schedule over UDP, one packet per destination
 *              =tc        one packet, duplicated on the sender's TC egress hook
 *              =xdp       one packet, duplicated on the fan-out node by
 *                         XDP_CLONE_TX
 *
 *   MPICLONE_ALGO=linear   root reaches everyone in one step
 *                =binomial standard binomial tree, log2(N) steps
 *                =ring     each rank forwards to the next
 *
 * `tc` and `xdp` run the *same* sender code and put the same bytes on the wire.
 * The only difference is which program duplicates them and where, which is
 * exactly the comparison this is for.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <mpi.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef unsigned int __u32;
typedef unsigned long long __u64;
#include "wire.h"

#define MAX_PAYLOAD 1400 /* keeps a fragment inside a 1500-byte MTU */

enum { MODE_MPI, MODE_UDP, MODE_TC, MODE_XDP };
enum { ALGO_LINEAR, ALGO_BINOMIAL, ALGO_RING };

static int mode = MODE_MPI;
static int algo = ALGO_LINEAR;
static int world_rank, world_size;
static int sock = -1;
static struct sockaddr_in peer[MPICLONE_MAX_RANKS];
static struct sockaddr_in fanout;
static unsigned int seq_counter;
static unsigned long long stat_bcasts, stat_packets, stat_timeouts;
static int verbose;

static int parse_mode(const char *s) {
  if (!s || !strcmp(s, "mpi")) return MODE_MPI;
  if (!strcmp(s, "udp")) return MODE_UDP;
  if (!strcmp(s, "tc")) return MODE_TC;
  if (!strcmp(s, "xdp")) return MODE_XDP;
  fprintf(stderr, "mpi-clone: unknown MPICLONE_MODE '%s'\n", s);
  exit(1);
}

static int parse_algo(const char *s) {
  if (!s || !strcmp(s, "linear")) return ALGO_LINEAR;
  if (!strcmp(s, "binomial")) return ALGO_BINOMIAL;
  if (!strcmp(s, "ring")) return ALGO_RING;
  fprintf(stderr, "mpi-clone: unknown MPICLONE_ALGO '%s'\n", s);
  exit(1);
}

/* Every rank's address, learnt rather than assumed: each one asks the kernel
 * which source address it would use to reach the fan-out node, and they
 * exchange the answers. The fan-out node's own table is built from the same
 * rule (rank i at MPICLONE_PORT_BASE + i), and setup() prints both under
 * MPICLONE_VERBOSE so a disagreement is visible instead of silent.
 */
static void discover_addresses(const char *fanout_ip) {
  struct sockaddr_in probe, mine;
  socklen_t len = sizeof(mine);
  __u32 *all;
  int p;

  memset(&fanout, 0, sizeof(fanout));
  fanout.sin_family = AF_INET;
  fanout.sin_port = htons(MPICLONE_FANOUT_PORT);
  if (inet_pton(AF_INET, fanout_ip, &fanout.sin_addr) != 1) {
    fprintf(stderr, "mpi-clone: MPICLONE_FANOUT is not an IPv4 address: %s\n",
            fanout_ip);
    exit(1);
  }

  p = socket(AF_INET, SOCK_DGRAM, 0);
  probe = fanout;
  if (connect(p, (struct sockaddr *)&probe, sizeof(probe)) ||
      getsockname(p, (struct sockaddr *)&mine, &len)) {
    perror("mpi-clone: cannot work out my own address");
    exit(1);
  }
  close(p);

  /* This exchange is also the barrier that makes the bind above safe: no rank
   * learns where to send until every rank has answered, and a rank can only
   * answer once it is listening.
   */
  all = calloc(world_size, sizeof(*all));
  PMPI_Allgather(&mine.sin_addr.s_addr, 1, MPI_UNSIGNED, all, 1, MPI_UNSIGNED,
                 MPI_COMM_WORLD);
  for (int i = 0; i < world_size; i++) {
    memset(&peer[i], 0, sizeof(peer[i]));
    peer[i].sin_family = AF_INET;
    peer[i].sin_addr.s_addr = all[i];
    peer[i].sin_port = htons(MPICLONE_PORT_BASE + i);
  }
  free(all);
}

/* Bind first, exchange addresses second.
 *
 * The other order loses a broadcast at startup, once, and only sometimes:
 * MPI_Allgather is not a barrier, so a rank that has returned from it can be
 * sending while another has not yet bound. Those datagrams hit a closed port
 * and are gone -- with no UdpInErrors to show for it, because the kernel counts
 * that as UdpNoPorts. It looked exactly like a 0.05% packet loss.
 */
static void open_socket(void) {
  struct sockaddr_in me;
  struct timeval tv;
  int on = 1, bufsz = 4 << 20;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("mpi-clone: socket");
    exit(1);
  }
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
  setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

  memset(&me, 0, sizeof(me));
  me.sin_family = AF_INET;
  me.sin_addr.s_addr = INADDR_ANY;
  me.sin_port = htons(MPICLONE_PORT_BASE + world_rank);
  if (bind(sock, (struct sockaddr *)&me, sizeof(me))) {
    perror("mpi-clone: bind");
    exit(1);
  }

  /* A lost datagram must not hang the job. It is counted and reported at
   * MPI_Finalize instead; a run that reports any is not a measurement.
   */
  tv.tv_sec = 0;
  tv.tv_usec = 200000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void setup(void) {
  const char *fanout_ip = getenv("MPICLONE_FANOUT");

  mode = parse_mode(getenv("MPICLONE_MODE"));
  algo = parse_algo(getenv("MPICLONE_ALGO"));
  verbose = getenv("MPICLONE_VERBOSE") != NULL;

  PMPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
  PMPI_Comm_size(MPI_COMM_WORLD, &world_size);

  if (mode == MODE_MPI) return;

  if (world_size > MPICLONE_MAX_RANKS) {
    fprintf(stderr, "mpi-clone: %d ranks, the destination bitmask holds %d\n",
            world_size, MPICLONE_MAX_RANKS);
    exit(1);
  }

  open_socket();
  discover_addresses(fanout_ip ? fanout_ip : "192.168.101.1");

  if (verbose && world_rank == 0) {
    char b[INET_ADDRSTRLEN];
    fprintf(stderr, "mpi-clone: mode=%s algo=%s ranks=%d fan-out=%s:%d\n",
            getenv("MPICLONE_MODE") ?: "mpi", getenv("MPICLONE_ALGO") ?: "linear",
            world_size, inet_ntop(AF_INET, &fanout.sin_addr, b, sizeof(b)),
            MPICLONE_FANOUT_PORT);
    for (int i = 0; i < world_size; i++)
      fprintf(stderr, "mpi-clone:   rank %2d %s:%d\n", i,
              inet_ntop(AF_INET, &peer[i].sin_addr, b, sizeof(b)),
              MPICLONE_PORT_BASE + i);
  }
}

/* Where this rank's copies go for this step, as a bitmask over ranks. The
 * schedule is the only thing that differs between the algorithms; everything
 * below treats the answer the same way.
 */
static __u64 children_of(int rank, int root, int *parent) {
  int vr = (rank - root + world_size) % world_size;
  __u64 dests = 0;
  int mask;

  *parent = -1;

  if (algo == ALGO_LINEAR) {
    if (rank == root) {
      for (int i = 0; i < world_size; i++)
        if (i != root) dests |= 1ULL << i;
    } else {
      *parent = root;
    }
    return dests;
  }

  if (algo == ALGO_RING) {
    if (vr + 1 < world_size)
      dests = 1ULL << ((vr + 1 + root) % world_size);
    if (vr != 0) *parent = (vr - 1 + root) % world_size;
    return dests;
  }

  /* Binomial: a rank waits for the peer whose relative rank differs in its
   * lowest set bit, then hands the data on to vr + 2^k for every k below it.
   */
  mask = 1;
  while (mask < world_size) {
    if (vr & mask) {
      *parent = ((vr - mask) + root) % world_size;
      break;
    }
    mask <<= 1;
  }
  mask >>= 1;
  while (mask > 0) {
    if (vr + mask < world_size)
      dests |= 1ULL << ((vr + mask + root) % world_size);
    mask >>= 1;
  }
  return dests;
}

static int send_fragments(const char *buf, size_t bytes, int root, __u64 dests,
                          unsigned int seq) {
  unsigned int nfrag = bytes ? (unsigned int)((bytes + MAX_PAYLOAD - 1) / MAX_PAYLOAD) : 1;
  char pkt[MPICLONE_HDR_LEN + MAX_PAYLOAD];
  struct mpiclone_hdr *h = (struct mpiclone_hdr *)pkt;

  if (!dests) return 0;

  h->magic = htonl(MPICLONE_MAGIC);
  h->root = htonl((__u32)root);
  h->seq = htonl(seq);
  h->nfrag = htonl(nfrag);
  h->dests = dests; /* host order: only our own programs read it */

  for (unsigned int f = 0; f < nfrag; f++) {
    size_t off = (size_t)f * MAX_PAYLOAD;
    size_t n = bytes - off < MAX_PAYLOAD ? bytes - off : MAX_PAYLOAD;

    h->frag = htonl(f);
    h->len = htonl((__u32)n);
    if (n) memcpy(pkt + MPICLONE_HDR_LEN, buf + off, n);

    if (mode == MODE_UDP) {
      /* One packet per destination, from here. The point of comparison. */
      for (int i = 0; i < world_size; i++) {
        if (!(dests & (1ULL << i))) continue;
        if (sendto(sock, pkt, MPICLONE_HDR_LEN + n, 0,
                   (struct sockaddr *)&peer[i], sizeof(peer[i])) < 0)
          return -1;
        stat_packets++;
      }
    } else {
      /* One packet, whatever the fan-out. The duplication point reads
       * h->dests and makes the copies.
       */
      if (sendto(sock, pkt, MPICLONE_HDR_LEN + n, 0,
                 (struct sockaddr *)&fanout, sizeof(fanout)) < 0)
        return -1;
      stat_packets++;
    }
  }
  return 0;
}

static int recv_fragments(char *buf, size_t bytes, int root, unsigned int seq) {
  unsigned int nfrag = bytes ? (unsigned int)((bytes + MAX_PAYLOAD - 1) / MAX_PAYLOAD) : 1;
  char pkt[MPICLONE_HDR_LEN + MAX_PAYLOAD + 64];
  unsigned int got = 0;

  while (got < nfrag) {
    struct mpiclone_hdr *h = (struct mpiclone_hdr *)pkt;
    ssize_t n = recv(sock, pkt, sizeof(pkt), 0);
    size_t off, len;

    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        stat_timeouts++;
        return -1;
      }
      return -1;
    }
    if (n < MPICLONE_HDR_LEN || ntohl(h->magic) != MPICLONE_MAGIC) continue;
    if (ntohl(h->seq) != seq || (int)ntohl(h->root) != root) continue;

    off = (size_t)ntohl(h->frag) * MAX_PAYLOAD;
    len = ntohl(h->len);
    if (off + len > bytes) continue;
    if (len) memcpy(buf + off, pkt + MPICLONE_HDR_LEN, len);
    got++;
  }
  return 0;
}

int MPI_Init(int *argc, char ***argv) {
  int rc = PMPI_Init(argc, argv);
  if (rc == MPI_SUCCESS) setup();
  return rc;
}

int MPI_Init_thread(int *argc, char ***argv, int required, int *provided) {
  int rc = PMPI_Init_thread(argc, argv, required, provided);
  if (rc == MPI_SUCCESS) setup();
  return rc;
}

int MPI_Finalize(void) {
  if (mode != MODE_MPI && verbose) {
    unsigned long long t = stat_timeouts;
    if (world_rank == 0 || t)
      fprintf(stderr,
              "mpi-clone: rank %d  %llu broadcasts, %llu packets sent, "
              "%llu timeouts%s\n",
              world_rank, stat_bcasts, stat_packets, t,
              t ? "  <-- LOST DATA, this run is not a measurement" : "");
  }
  if (sock >= 0) close(sock);
  return PMPI_Finalize();
}

int MPI_Bcast(void *buffer, int count, MPI_Datatype datatype, int root,
              MPI_Comm comm) {
  __u64 dests;
  size_t bytes;
  int tsize, parent, cmp;

  if (mode == MODE_MPI)
    return PMPI_Bcast(buffer, count, datatype, root, comm);

  /* Only the world communicator is offloaded; a sub-communicator has a rank
   * numbering of its own that the bitmask would not match.
   */
  PMPI_Comm_compare(comm, MPI_COMM_WORLD, &cmp);
  if (cmp != MPI_IDENT || world_size < 2)
    return PMPI_Bcast(buffer, count, datatype, root, comm);

  PMPI_Type_size(datatype, &tsize);
  bytes = (size_t)count * (size_t)tsize;

  seq_counter++;
  stat_bcasts++;
  dests = children_of(world_rank, root, &parent);

  if (parent >= 0 && recv_fragments(buffer, bytes, root, seq_counter))
    return MPI_SUCCESS; /* counted as a timeout; see MPI_Finalize */

  if (send_fragments(buffer, bytes, root, dests, seq_counter))
    return MPI_ERR_OTHER;

  return MPI_SUCCESS;
}
