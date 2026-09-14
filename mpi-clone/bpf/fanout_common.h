/* SPDX-License-Identifier: GPL-2.0 */
/* Shared between the fan-out program, the TC program and their loaders. */
#ifndef __MPICLONE_FANOUT_COMMON_H__
#define __MPICLONE_FANOUT_COMMON_H__

#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

/* One rank, as the duplication point needs it on the wire. */
struct mpiclone_peer {
  __u32 addr;         /* network order */
  __u16 port;         /* network order */
  __u8 eth[ETH_ALEN];
};

struct mpiclone_cfg {
  __u32 n_ranks;
  __u32 fanout_ip;    /* network order: the address the ranks send to */
  __u16 fanout_port;  /* network order */
  __u8 self_mac[ETH_ALEN];
  __u8 gw_mac[ETH_ALEN]; /* the TC program writes this as the next hop */
};

#endif /* __MPICLONE_FANOUT_COMMON_H__ */
