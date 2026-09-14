/* SPDX-License-Identifier: GPL-2.0 */
/* What goes on the wire when a broadcast is offloaded, shared by the MPI
 * wrapper, the TC program and the XDP fan-out node.
 *
 * The one idea here is the **destination bitmask**. The sender does not address
 * a packet to a peer; it addresses one packet to the duplication point and says
 * which ranks the copies are for. One mechanism then serves every schedule: a
 * flat broadcast sets every bit, a binomial tree sets the bits of the children
 * of that step, a ring sets one. What differs between the three is only which
 * bits the sender puts in, which is where a broadcast algorithm lives anyway.
 *
 * The header is read but never written by the duplication point. That is what
 * lets the inline build work at all: on the driver's shared-page path the
 * program may not touch the packet, so everything that differs between copies
 * has to be in the 42 bytes of Ethernet/IP/UDP it hands to the NIC as the WQE
 * inline header -- and the payload, identical for every recipient of a
 * broadcast, stays where it is.
 */
#ifndef __MPICLONE_WIRE_H__
#define __MPICLONE_WIRE_H__

#define MPICLONE_MAGIC 0x4d504943u /* "MPIC" */
#define MPICLONE_MAX_RANKS 64      /* one bitmask word */

/* Rank i listens on MPICLONE_PORT_BASE + i. Deterministic on purpose: the
 * fan-out node then needs nothing from the job at run time, only how many
 * ranks there are.
 */
#define MPICLONE_PORT_BASE 20000

/* The address the offloaded modes send their one packet to. */
#define MPICLONE_FANOUT_PORT 19999

struct mpiclone_hdr {
  __u32 magic;   /* MPICLONE_MAGIC, network order */
  __u32 root;    /* rank that started this broadcast */
  __u32 seq;     /* broadcast counter, for spotting a loss */
  __u32 frag;    /* fragment index within this broadcast */
  __u32 nfrag;   /* fragments in total */
  __u32 len;     /* payload bytes in this fragment */
  __u64 dests;   /* bit i set: this packet is for rank i */
} __attribute__((packed));

#define MPICLONE_HDR_LEN ((int)sizeof(struct mpiclone_hdr))

#endif /* __MPICLONE_WIRE_H__ */
