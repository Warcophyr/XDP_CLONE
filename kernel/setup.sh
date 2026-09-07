#!/bin/bash
# Fetch the exact Ubuntu kernel this patch was written against and apply it.
#
# Usage:
#   ./setup.sh [SRC_DIR]        default: $HOME/linux-6.8.0-60.63
set -euo pipefail

KERNEL_TAG=Ubuntu-6.8.0-60.63
KERNEL_SHA=a7bbcbe5de91a5987a6897882871dea34eaee08b
CODENAME=noble
KERNEL_REPO=https://git.launchpad.net/~ubuntu-kernel/ubuntu/+source/linux/+git/$CODENAME

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-$HOME/linux-6.8.0-60.63}

if [ ! -d "$SRC" ]; then
	echo ">> Cloning $KERNEL_TAG into $SRC (~1.8 GB checked out)"
	git clone --depth 1 -b "$KERNEL_TAG" "$KERNEL_REPO" "$SRC"
else
	echo ">> $SRC already exists, skipping clone"
fi

cd "$SRC"

got=$(git rev-parse HEAD)
if [ "$got" != "$KERNEL_SHA" ]; then
	echo "!! $SRC is at $got, expected $KERNEL_SHA ($KERNEL_TAG)" >&2
	echo "!! The patch is only guaranteed to apply on that commit." >&2
	exit 1
fi

# A clone of a tag leaves HEAD detached; work on a branch instead.
git switch -c verifier 2>/dev/null || git switch verifier

echo ">> Applying $(ls "$HERE"/patches/*.patch | wc -l) patch(es)"
git am "$HERE"/patches/*.patch

echo
echo ">> Done. Build the kernel with:"
echo "     $HERE/build.sh $SRC"
