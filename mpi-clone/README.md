# MPI broadcast on the XDP_CLONE driver

Accelerating `MPI_Bcast` in a real MPI, with the duplication done by the NIC
driver rather than by the sending rank.

## Why a wrapper and not the MPI transport

Open MPI carries messages over TCP on this cluster — there is no verbs device
(`/sys/class/infiniband` does not exist), no RoCE, and no UDP fabric provider.
A TCP segment cannot be cloned to several peers: the sequence numbers, ports,
checksums and connection state are all per-peer. **The driver cannot act on
MPI's own transport at all**, and no amount of work on the BPF side changes
that.

What it can act on is a datagram. The MPI profiling interface lets one
collective move onto datagrams without touching the application: `MPI_Bcast` is
intercepted, everything else goes straight to `PMPI_*`, and the benchmark on
top is stock `osu_bcast`, unmodified and unaware.

## The five points

| `--mode` | where the broadcast is duplicated | packets the root sends |
|---|---|---|
| `mpi` | nowhere — Open MPI's own `MPI_Bcast`, over TCP | its own business |
| `udp` | nowhere — our schedule, one datagram per destination | one per destination |
| `tc` | the sending rank's own TC egress hook | 1 |
| `xdp` | the fan-out node, `XDP_CLONE_TX` | 1 |
| `xdp-inline` | the same, on the driver's shared-page path with each copy's header inline in its WQE | 1 |

`udp`, `tc` and `xdp` run the **same schedule** and put the same bytes on the
wire; only the duplication moves. `mpi` is there to say what a real MPI does
today, and to show that the wrapper itself costs nothing when it is not used.

## The destination bitmask

The sender does not address a packet to a peer. It addresses one packet to the
duplication point and says, in `struct mpiclone_hdr`, which ranks the copies are
for. One mechanism then serves every schedule — a flat broadcast sets every
bit, a binomial tree sets the children of that step, a ring sets one — and what
differs between them is only which bits the sender puts in, which is where a
broadcast algorithm lives anyway.

The duplication point **reads** the bitmask and never writes the packet. That
is what lets the inline build exist: on the shared-page path the program may not
touch the packet, and it does not need to, because a broadcast payload is
identical for every recipient. Everything that differs between copies is in the
42 bytes of Ethernet/IP/UDP handed to the NIC as the WQE inline header.

## Which schedules the clone can help

The clone turns one send into N. A **ring** hop sends exactly one packet, so
there is nothing to duplicate and nothing to gain — it is here as the control
that says so. A **flat** broadcast is the opposite extreme: the root's N-1 sends
become one. A **binomial tree** sits in between, and is the interesting case
because it is what Open MPI actually does for small messages: each internal node
sends to its children, and those sends become one.

`--algo ring | linear | binomial`.

## Running it

The ranks live one per network namespace on grecale, with every packet routed
through maestrale — the same topology the Electrode experiment uses, and for the
same reason: put the ranks side by side in one namespace and their packets never
reach a wire.

```bash
# grecale, once
sudo ~/XDP_CLONE/electrode/scripts/cluster.sh up 7
make                                    # wrapper and BPF objects

# maestrale
make
scripts/run.sh --mode xdp --algo binomial --ranks 7 --bytes 8
scripts/sweep.sh --ranks 3 5 7 --algos linear binomial ring --reps 3 \
                 --out results/e1.csv
scripts/report.py results/e1.csv
```

`osu_bcast` comes from the OSU micro-benchmarks, built into `/opt/osu` on
grecale. The fan-out needs `rx_striding_rq off` and `xdp_tx_mpwqe off`;
`run.sh` refuses to start otherwise.

## Four things that silently measure something else

Each of these produces a full set of plausible numbers, which is why they are
worth writing down.

1. **Open MPI hangs inside a network namespace with no output at all.** hwloc
   probes the X displays looking for GPUs; abstract `AF_UNIX` sockets are scoped
   to the network namespace, so the probe falls through to the filesystem socket
   and blocks there for good. `HWLOC_COMPONENTS=-gl`.
2. **Every rank reports the same hostname**, so Open MPI puts them on one node
   and moves messages through shared memory: a three-rank broadcast came out at
   1.78 µs with 27 packets on the wire for the entire run. Each namespace needs
   a UTS namespace and a hostname of its own — `scripts/nsagent.sh` does both.
3. **IPv6 link-local between two macvlans on one parent** is resolved by NDP to
   the neighbour's MAC and short-circuited in software, never reaching the wire.
   Same symptom, 1.5 µs broadcasts. `--mca btl_tcp_disable_family 6`.
4. **`MPI_Allgather` is not a barrier.** Binding the UDP socket after the
   address exchange let a rank send while another had not yet bound; those
   datagrams hit a closed port and vanished, with no `UdpInErrors` to show for
   it because the kernel counts that as `UdpNoPorts`. It looked exactly like a
   0.05% packet loss. The socket is bound first now.

A fifth, from the measurement rather than the setup: **`MPI_Bcast` does not
synchronise**. Timing a loop of them at the root measures how fast it can
enqueue sends, not how long a broadcast takes — a hand-written loop reported
0.57 µs where point-to-point was 25.5. `osu_bcast` gets this right, which is
why it is what runs here.

## What is not handled

A lost datagram is counted, not recovered: the receive has a timeout, the count
is reported at `MPI_Finalize`, and **a run that reports any is not a
measurement**. This is the same caveat Electrode states for itself. On a direct
link with no congestion the count is zero; the point of reporting it is that you
can tell.

Only `MPI_COMM_WORLD` is offloaded — a sub-communicator has a rank numbering of
its own that the bitmask would not match — and at most 64 ranks, one bitmask
word.
