#!/bin/bash
# Build the patched kernel out of tree, with the same release string as the
# Ubuntu kernel it is based on, so that uname -r and the module vermagic match
# the host and the out-of-tree mlx5 driver keeps loading.
#
# Usage:
#   ./build.sh [SRC_DIR] [BUILD_DIR]     defaults: $HOME/linux-6.8.0-60.63{,-build}
#   MAKE_TARGET=modules ./build.sh       build a specific target instead of all
set -euo pipefail

RELEASE=6.8.0-60-generic

SRC=${1:-$HOME/linux-6.8.0-60.63}
BUILD=${2:-$SRC-build}

[ -f "$SRC/Makefile" ] || { echo "!! $SRC is not a kernel tree" >&2; exit 1; }

mkdir -p "$BUILD"

if [ ! -f "$BUILD/.config" ]; then
	# Start from the config of the running kernel: same drivers, same features.
	config=/boot/config-$RELEASE
	[ -f "$config" ] || config=/boot/config-$(uname -r)
	[ -f "$config" ] || { echo "!! no kernel config found under /boot" >&2; exit 1; }
	echo ">> Using $config"
	cp "$config" "$BUILD/.config"

	# The Ubuntu config points at Canonical's certificates, which are not part
	# of the source package; leave the key lists empty or the build stops in
	# certs/.
	"$SRC"/scripts/config --file "$BUILD/.config" \
		--set-str SYSTEM_TRUSTED_KEYS "" \
		--set-str SYSTEM_REVOCATION_KEYS "" \
		-d DEBUG_INFO_BTF_MODULES

	make -C "$SRC" O="$BUILD" olddefconfig
fi

# KERNELRELEASE is how debian/rules.d injects the Ubuntu ABI ("-60-generic"),
# which does not exist anywhere in the kernel source. It sets UTS_RELEASE, and
# with it uname -r and the module vermagic, without touching VERSION,
# PATCHLEVEL or SUBLEVEL, so LINUX_VERSION_CODE stays 6.8.12 as in the distro
# kernel. LOCALVERSION= drops the "+" that scripts/setlocalversion appends for
# a git tree.
make -C "$SRC" O="$BUILD" -j"$(nproc)" \
	KERNELRELEASE=$RELEASE \
	LOCALVERSION= \
	${MAKE_TARGET:-}

echo
echo ">> Built $(cat "$BUILD"/include/config/kernel.release) in $BUILD"
echo ">> Run it in a VM with:  vng --run $BUILD --rw"
