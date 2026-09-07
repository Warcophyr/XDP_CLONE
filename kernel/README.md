# Verifier support for the XDP clone actions

This directory holds the kernel-side changes for the packet duplication actions
implemented by the out-of-tree mlx5 driver in
[`mellanox-clone-xdp`](../mellanox-clone-xdp): `XDP_CLONE_PASS` and
`XDP_CLONE_TX`. The BPF verifier is taught to check, at program load time, the
two rules the driver used to enforce at runtime.

```
kernel/
├── patches/0001-bpf-verify-XDP-clone-actions-against-the-data_meta-c.patch
├── setup.sh     clone the base kernel and apply the patch
├── build.sh     configure and build it out of tree
└── README.md
```

## What the patch does

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
```

Every other XDP return value keeps being unchecked, exactly as before. One
limitation: a program that computes the action itself at runtime is invisible to
this check, because the low bits of `R0` are unknown at verification time. The
driver still has to reject a clone action coming from a copy.

Files touched: `kernel/bpf/verifier.c` (the checks),
`include/linux/bpf_verifier.h` (per-state tracking),
`include/uapi/linux/bpf.h` and its `tools/` copy (the two actions and the
`XDP_CLONE_*` constants) — 337 insertions, 2 deletions.

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
./setup.sh                  # clone the base kernel into ~/linux-6.8.0-60.63 and git am the patch
./build.sh                  # configure from the running kernel's config and build
```

`setup.sh` refuses to apply the patch if the tree is not at the expected commit.
Both scripts take the source directory as their first argument if you want it
somewhere else. `git am` needs `user.name` and `user.email` set.

## Doing it by hand

```bash
# 1. get the source
git clone --depth 1 -b Ubuntu-6.8.0-60.63 \
  https://git.launchpad.net/~ubuntu-kernel/ubuntu/+source/linux/+git/noble \
  ~/linux-6.8.0-60.63

# 2. apply the patch
cd ~/linux-6.8.0-60.63
git switch -c verifier
git am /path/to/kernel/patches/0001-*.patch

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

## License

The patch under `patches/` modifies Linux kernel source files and is therefore a
derivative work under GPL-2.0, independently of the license of this repository.
