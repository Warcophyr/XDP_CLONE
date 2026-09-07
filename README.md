# XDP_CLONE

Three related eBPF/XDP networking experiments, pulled together as one repo: MPI-style collective communication, pub/sub messaging, and the NIC driver feature both of them lean on for in-kernel packet cloning.

This repository has no code of its own. It's a set of three git submodules meant to be read and used together:

| Submodule | Path | What it is |
|---|---|---|
| [XDP-MPI-Collectives](https://github.com/Warcophyr/XDP-MPI-Collectives) | `XuDP/` | An MPI-like broadcast benchmark: forked "ranks" exchange data over UDP, with ring/linear collective algorithms accelerated by an XDP/TC BPF program, and a TCP ACK/NACK fallback path. |
| [bpf-broker](https://github.com/VladimiroPaschali/bpf-broker) (`ebpf-pubsub`) | `bpf-broker/` | A research project on using eBPF to accelerate pub/sub systems. Includes a BPF-accelerated broker, a minimal Rust MQTT-SN-over-UDP implementation, and benchmarking scripts. |
| [mellanox-clone-xdp](https://github.com/Warcophyr/mellanox-clone-xdp) | `mellanox-clone-xdp/` | A modified out-of-tree Mellanox `mlx5` driver (based on the Ubuntu `linux-headers-6.8.0-60-generic` kernel sources) that adds two new XDP verdicts, `XDP_CLONE_PASS` and `XDP_CLONE_TX`, letting a single BPF program instruct the NIC to emit N in-kernel copies of a packet. |

The dependency runs one direction: `XDP-MPI-Collectives` is built on top of the cloning verdicts implemented in `mellanox-clone-xdp`. `bpf-broker` is a separate, related experiment in the same space (eBPF-accelerated networking) and does not depend on the custom driver.

## Getting the code

A plain clone won't pull the submodules. Use `--recursive`, or run `git submodule update --init` afterwards:

```bash
git clone --recursive https://github.com/Warcophyr/XDP_CLONE.git
cd XDP_CLONE

# or, if you already cloned without --recursive:
git submodule update --init --recursive
```

## The core idea: packet cloning in the driver

Everything here builds on one trick, implemented in `mellanox-clone-xdp`: a modified `mlx5` driver that lets an XDP program return a *cloning* verdict instead of the usual `XDP_PASS` / `XDP_DROP` / `XDP_TX`.

```c
#define __XDP_CLONE_PASS 5
#define XDP_CLONE_PASS(num_copy) (((int)(num_copy) << 5) | (int)__XDP_CLONE_PASS)

#define __XDP_CLONE_TX 5
#define XDP_CLONE_TX(num_copy) (((int)(num_copy) << 5) | (int)__XDP_CLONE_TX)
```

- The low 5 bits carry the underlying XDP action; the remaining bits encode how many copies to make.
- `XDP_CLONE_PASS(10)` makes 10 copies and passes the original up the stack (11 packets total, behaves like `XDP_PASS`); `XDP_CLONE_TX(10)` does the same but retransmits (behaves like `XDP_TX`).
- Copies are chained: each copy is derived from the previous one, so modifications to copy *i* are visible in copy *j* for *j > i*.
- Every packet carries an ID in its metadata (`0` for the original, `>0` for copies, in strict sequence) so a BPF program can tell which iteration it's looking at.
- `XDP_REDIRECT` after a clone verdict is not fully supported; any return code other than the plain defaults on an already-cloned packet is treated as `XDP_DROP`.

See `mellanox-clone-xdp/README.md` for the full driver build/load/reset instructions and the complete semantics.

`XDP-MPI-Collectives` uses this so a broadcast root sends one UDP packet into the clone path instead of N-1 separate packets. The NIC and BPF program handle fan-out in kernel space, without bouncing back to userspace per recipient.

## Repository layout

```
XDP_CLONE/
├── LICENSE
├── XuDP/                      # XDP-MPI-Collectives (submodule)
│   ├── MPI.c                  # entry point: CLI parsing, BPF load, forking ranks
│   ├── mpi_collective.c        # ring / linear broadcast + ACK/NACK fallback
│   ├── my_ebpf.c / .h          # BPF program load/attach helpers
│   ├── bpf/xdp, bpf/tc         # BPF programs for each execution mode
│   └── AGENT.md                # detailed internals reference
├── bpf-broker/                 # ebpf-pubsub (submodule)
│   ├── bpf_broker/             # XDP-hooked broker (kernel + userspace)
│   ├── bpf_broker_xdp/         # variant using the XDP path
│   ├── mqttsn-udp-sr/          # minimal Rust MQTT-SN-over-UDP broker/pub/sub
│   ├── pubsub-benchmark/       # Rust benchmarking harness
│   └── script/                 # setup, tc, CPU-pinning and profiling scripts
└── mellanox-clone-xdp/         # mellanox-clone-xdp (submodule)
    ├── examples/clone-pass/    # XDP_CLONE_PASS example program
    ├── examples/clone-tx/      # XDP_CLONE_TX example program
    └── mellanox-out-of-tree-clone/  # the modified mlx5/mlx4 driver sources
```

## Quick start per component

Each submodule builds independently; see its own README for full details.

### 1. `mellanox-clone-xdp`: load the modified driver first

```bash
cd mellanox-clone-xdp/mellanox-out-of-tree-clone
make            # compile the driver
make load       # load it after a reboot
make reload     # or: load it again if already loaded
make reset      # unload and go back to the stock driver
```

By default the driver attaches to `lo`; edit the `ETH` variable in the Makefile under `mlx5/core` to point at your real interface first.

### 2. `XuDP` (XDP-MPI-Collectives): run the broadcast benchmark

```bash
cd XuDP
make
sudo ./MPI -n 4 -i enp52s0f1np1              # 4 ranks, default ring/XDP broadcast
sudo ./MPI -n 8 -a linear -s 4096 -i enp52s0f1np1 -o results.csv
```

Requires clang, gcc, make, libbpf, libelf, zlib, xdp-tools, and sudo privileges. Key flags: `-n/--np` (ranks), `-a/--algo` (`ring`, `linear`, `ring_eager`), `-s/--size` (payload bytes), `-t/--tc` (use TC instead of XDP), `-z/--naive` (userspace fallback, no BPF).

### 3. `bpf-broker` (ebpf-pubsub): BPF-accelerated pub/sub

```bash
cd bpf-broker
sudo apt update && sudo apt install -y gpg curl tar xz-utils make gcc flex bison libssl-dev libelf-dev llvm clang

./kernel-src-download.sh
./kernel-src-prepare.sh
cd bpf_broker && chmod +x ./setup-bpf-headers.sh && ./setup-bpf-headers.sh && make

# terminal 1
sudo ./bpf_broker <interface_idx>
# terminal 2
sudo ./bpf_broker_proto
# terminal 3
cd ../script && sudo ./load_tc_ingress_logger.sh
```

Tested on Ubuntu 24.04 / Linux 6.8.x. The `mqttsn-udp-sr` subfolder is a separate, dependency-light Rust reference implementation (`cargo run --bin broker|publisher|subscriber`) useful for comparing against the BPF-accelerated path without any kernel setup.
