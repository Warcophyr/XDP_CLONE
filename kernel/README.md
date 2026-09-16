# Verifier support for the XDP clone actions

This directory holds the kernel-side changes for the packet duplication actions
implemented by the out-of-tree mlx5 driver in
[`mellanox-clone-xdp`](../mellanox-clone-xdp): `XDP_CLONE_PASS`,
`XDP_CLONE_TX`, and the inline TX descriptor that goes with them. The BPF
verifier is taught to check, at program load time, the rules the driver used to
enforce at runtime, or could not enforce at all.

```
kernel/
├── patches/
│   ├── 0001-bpf-verify-XDP-clone-actions-against-the-data_meta-c.patch
│   └── 0002-bpf-check-the-XDP-inline-TX-descriptor-against-packe.patch
├── setup.sh     clone the base kernel and apply the patches
├── build.sh     configure and build it out of tree
├── tests/       programs the verifier must accept and reject, plus run.sh
└── README.md
```

Two patches, applied in order: `0001` is the clone actions and the
`data_meta` check, `0002` the inline TX descriptor on top of it. `setup.sh`
applies both.

## What the patches do

A program asks for copies by returning the number of copies in the upper bits
of the action:

```c
return (n_copies << XDP_CLONE_NUM_SHIFT) | XDP_CLONE_TX;   /* shift is 5 */
```

The driver then re-runs the program once per copy, with `XDP_CLONE_META_SIZE`
(4) bytes of metadata in front of `xdp->data`, so the program can tell a copy
from the original packet. The verifier now enforces:

1. **No nested clones.** A clone action is only accepted on a path where the
   program proved that the clone metadata is *not* there, so a copy cannot ask
   for more copies.
2. **The check is mandatory.** A program that never compares
   `xdp_md->data_meta` against `xdp_md->data` cannot ask for copies at all.
3. **In inline TX mode the packet is read-only.** A program that stamps the
   TX descriptor into its metadata area must not write into the packet
   anywhere — see [Inline TX mode](#inline-tx-mode) below.

Which makes this the accepted shape:

```c
SEC("xdp")
int clone(struct xdp_md *ctx)
{
        if (ctx->data_meta + sizeof(__u32) <= ctx->data)
                return XDP_PASS;                /* running on a copy */

        return (4 << XDP_CLONE_NUM_SHIFT) | XDP_CLONE_TX;
}
```

Note the `sizeof()`: the comparison has to be done in 64-bit arithmetic. With a
plain `int` (`+ 4`) clang truncates the pointer to 32 bits and the verifier
rejects the program before any of this applies.

Rejected programs get a verdict that says which rule was broken, for example:

```
At program exit R0 returns XDP_CLONE_TX without checking xdp_md->data_meta,
XDP_CLONE_TX is only allowed where 'ctx->data_meta + 4 > ctx->data' holds

At program exit R0 returns XDP_CLONE_TX on a packet carrying 4 bytes of
metadata, this is a copy and nested clones are not allowed

insn 20 stamps the XDP inline TX descriptor into xdp_md->data_meta, which
emits the packet out of a page shared with every copy of it, so insn 30 must
not write into the packet. Write the metadata area instead, or leave the
stamp out to get a page per copy
```

Every other XDP return value keeps being unchecked, exactly as before. One
limitation: a program that computes the action itself at runtime is invisible to
this check, because the low bits of `R0` are unknown at verification time. The
driver still has to reject a clone action coming from a copy.

### A metadata area the program grew itself

An inline program widens its own metadata area with `bpf_xdp_adjust_meta()` and
then bounds-checks it before writing the descriptor into it. That check is a
second `data_meta` vs `data` comparison, and reading it the way rule 1 reads the
first one — *the metadata is there, so this is a copy* — would make every inline
program a nested clone and reject it.

So a comparison made after the program grew the area says nothing about the
driver's metadata, and the path keeps what it proved *before* the grow, and
nothing more. Which cuts both ways: a program that grows the area before
comparing has proved nothing at all and cannot clone. Shrinking it with a
positive delta is unchanged — that still voids everything, as it always did.

## Inline TX mode

A program asks the driver for the inline TX path by stamping a three-word
descriptor into its metadata area, the last word being `XDP_TX_INLINE_MAGIC`
(`0xa7d9c0de`, the driver's and `axdp_tx.h`'s `AXDP_TX_MAGIC`):

```c
u32 *desc = (void *)(long)ctx->data_meta;

desc[0] = tag;                   /* TX flow table metadata */
desc[1] = inline_hdr_size;       /* header sits at desc + 12 */
desc[2] = XDP_TX_INLINE_MAGIC;   /* what makes it opt-in */
```

The NIC then prepends the header to the packet as it DMAs it, and the driver
emits the packet and every copy of it straight out of the one RX page: no page
allocation and no byte copy per copy. What pays for that is the packet itself.
All of those emissions point at the same bytes and the DMA is asynchronous, so
a write into the packet on one run of the program lands in the frames already
queued for the other ones. Nothing in the kernel is at risk — the page and the
references on it belong to the driver — but the frames go out garbage.

The verifier therefore refuses a program that stamps the descriptor *and*
writes into the packet:

- a store through a `PTR_TO_PACKET` pointer;
- a packet pointer handed to a helper to fill, e.g. `bpf_xdp_load_bytes()`;
- `bpf_xdp_adjust_head()`, which memmoves the metadata over the packet's first
  bytes — this is why an inline program pushes a header rather than replacing
  one, and asks the driver for a replace with `AXDP_TX_REPLACE` instead;
- `bpf_xdp_adjust_tail()` and `bpf_xdp_store_bytes()`.

`bpf_xdp_adjust_meta()` is none of those: it moves `data_meta` inside the
headroom and touches no packet byte.

The rule is checked over the whole program rather than per path, which is what
the hardware does: the driver takes the shared page from the stamp on the
*original*, and then re-runs the program once per copy, where it may well take
another path. A stamp on one path is a stamp for all of them. A program that
does need to rewrite the packet per copy simply leaves the stamp out, and gets
a page and a byte copy per copy — the `clone-tx` and `clone-kfunc` shape, which
this rule leaves alone.

Blind spots, the same class as the runtime action above: a program that builds
the magic at runtime or writes it through a helper is not seen as stamping, and
a write reaching the packet through an XDP dynptr is not seen as a write. This
is a correctness check on the program, not a kernel safety boundary.

Files touched by both patches: `kernel/bpf/verifier.c` (the checks),
`include/linux/bpf_verifier.h` (per-state and per-program tracking),
`include/uapi/linux/bpf.h` and its `tools/` copy (the two actions, the
`XDP_CLONE_*` constants and `XDP_TX_INLINE_MAGIC`). 337 insertions and
2 deletions for `0001`, another 220 and 3 for `0002`.

## Base kernel

The patch is written against, and must be applied to, **exactly** this tree:

| | |
|---|---|
| Ubuntu ABI | `6.8.0-60.63` (what `uname -r` reports as `6.8.0-60-generic`) |
| Upstream version | 6.8.12 |
| Git tag | `Ubuntu-6.8.0-60.63` |
| Commit | `a7bbcbe5de91a5987a6897882871dea34eaee08b` |
| Repository | `https://git.launchpad.net/~ubuntu-kernel/ubuntu/+source/linux/+git/noble` |

Why this exact ABI and not simply "6.8": the driver in `mellanox-clone-xdp` is a
modified copy of the in-tree mlx5 driver taken from `6.8.0-60`, and later Ubuntu
ABIs moved declarations it relies on (`struct mlx5_port_eth_proto`,
`mlx5_query_port_admin_status`, `struct mlx5_module_eeprom_query_params`) out of
the public `include/linux/mlx5/port.h` into the driver-private
`mlx5/core/mlx5_core.h`. Built against a newer tree it fails with ~20 errors in
`port.c`, `en/port.c` and `en_ethtool.c`.

**`apt install linux-source-6.8.0` does not give you this tree.** It always
installs the newest ABI in the archive (e.g. `6.8.0-139`), and older ABIs are
dropped from the pockets. The ABI you need is in `/proc/version_signature`:

```console
$ cat /proc/version_signature
Ubuntu 6.8.0-60.63-generic 6.8.12
       ^^^^^^^^^^^^ the tag to clone is Ubuntu-6.8.0-60.63
```

## Quick start

```bash
./setup.sh                  # clone the base kernel into ~/linux-6.8.0-60.63 and git am the patches
./build.sh                  # configure from the running kernel's config and build
```

`setup.sh` refuses to apply the patches if the tree is not at the expected commit.
Both scripts take the source directory as their first argument if you want it
somewhere else. `git am` needs `user.name` and `user.email` set.

## Doing it by hand

```bash
# 1. get the source
git clone --depth 1 -b Ubuntu-6.8.0-60.63 \
  https://git.launchpad.net/~ubuntu-kernel/ubuntu/+source/linux/+git/noble \
  ~/linux-6.8.0-60.63

# 2. apply the patches, in order
cd ~/linux-6.8.0-60.63
git switch -c verifier
git am /path/to/kernel/patches/*.patch

# 3. configure out of tree, from the running kernel's config
B=~/linux-6.8.0-60.63-build
mkdir -p $B && cp /boot/config-$(uname -r) $B/.config
./scripts/config --file $B/.config \
  --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS ""
make O=$B olddefconfig

# 4. build
make -C . O=$B -j$(nproc) KERNELRELEASE=6.8.0-60-generic LOCALVERSION=
```

Two of those steps are not obvious:

- The Ubuntu config points `SYSTEM_TRUSTED_KEYS` at `debian/canonical-certs.pem`,
  which is not shipped in the source package; leaving it set stops the build in
  `certs/`.
- `KERNELRELEASE=6.8.0-60-generic` reproduces what `debian/rules.d` does at
  package build time. The `-60-generic` suffix exists nowhere in the kernel
  source, so a plain `make` produces `6.8.12` instead. Passing `KERNELRELEASE`
  sets `UTS_RELEASE` (hence `uname -r` and the module vermagic) while leaving
  `VERSION`/`PATCHLEVEL`/`SUBLEVEL` alone, so `LINUX_VERSION_CODE` stays
  `6.8.12` like in the distro kernel. Getting the same string by editing
  `SUBLEVEL` to 0 would corrupt it and mislead the compat layers of other
  out-of-tree modules.

Nothing gets installed: `bzImage` and the modules stay in the build directory.

## Running it

The build directory is directly usable by [virtme-ng](https://github.com/arighi/virtme-ng),
which is the safe way to test a patched verifier and a patched NIC driver
without touching the host:

```bash
vng --run ~/linux-6.8.0-60.63-build --rw
```

Inside the VM, `uname -r` reports `6.8.0-60-generic` — the same as the host,
which is the point: the out-of-tree driver built against this tree loads in both.
To be sure you really are on the patched kernel, load a program that must be
rejected (a clone action with no `data_meta` check) and look for the verifier
message quoted above.

Build the driver against this tree, not against the host headers:

```bash
cd ../mellanox-clone-xdp/mellanox-out-of-tree-clone
make KDIR=~/linux-6.8.0-60.63-build
```

> **Do not run `make modules_install install`.** Since the release string is the
> same as the distro kernel's, that would overwrite
> `/boot/vmlinuz-6.8.0-60-generic` and `/lib/modules/6.8.0-60-generic`, i.e. the
> kernel the host boots from. Testing in a VM needs no installation at all.

## Tests

`tests/` holds one small program per rule. Each one carries the expected outcome
on an `EXPECT:` line, and `run.sh` compiles it, tries to load it and compares:

```console
# cd kernel/tests && sudo ./run.sh -v
kernel: 6.8.0-60-generic
ok        t1_clone_ok          ACCEPT
ok        t2_nested            REJECT
...
```

| test | expected | why |
|---|---|---|
| `t1_clone_ok` | ACCEPT | clone on the branch where the metadata is absent |
| `t2_nested` | REJECT | nested clone: asked for while running on a copy |
| `t3_nocheck` | REJECT | no `data_meta` check at all |
| `t4_bigcheck` | REJECT | the check covers 8 bytes, so it does not rule out a copy |
| `t5_adjust` | REJECT | metadata dropped with `bpf_xdp_adjust_meta()` before checking |
| `t6_runtime_count` | ACCEPT | number of copies computed at runtime is fine |
| `t7_reversed` | ACCEPT | same check with the operands swapped, `XDP_CLONE_PASS` |
| `t8_runtime_action` | ACCEPT | documented limitation: action computed at runtime |
| `t9_inline_ok` | ACCEPT | inline mode done right: stamps the descriptor, touches nothing else |
| `t10_inline_write` | REJECT | stamps the descriptor and then writes into the packet |
| `t11_inline_adjust_head` | REJECT | stamps the descriptor and calls `bpf_xdp_adjust_head()` |
| `t12_write_no_stamp` | ACCEPT | the same write without the stamp: a page per copy, nothing shared |

Run them inside the VM on the patched kernel; needs root, `clang`, libbpf
headers and `bpftool`. On a stock kernel every program loads, so the six
REJECT rows fail — which is itself a way to tell the two kernels apart.

`t9` is the one to watch when touching rule 1: it is the shape of
`examples/inline-clone`, and it only loads because a self-grown metadata area
stops counting as the driver's.

## License

The patches under `patches/` modify Linux kernel source files and are therefore
a derivative work under GPL-2.0, independently of the license of this
repository.
