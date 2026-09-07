#!/bin/bash
# Load every test program and compare the outcome with its "EXPECT:" line.
# Meaningful only on a kernel built with the patch in ../patches: on a stock
# kernel every program loads and the four REJECT cases fail.
#
# Needs root (loading a BPF program), clang, libbpf headers and bpftool.
#
#   sudo ./run.sh          outcome only
#   sudo ./run.sh -v       also print the verifier log of the rejected ones
set -u

cd "$(dirname "$0")"
VERBOSE=${1:-}
BPFFS=/sys/fs/bpf

for tool in clang bpftool; do
	command -v $tool >/dev/null || { echo "!! $tool not found" >&2; exit 1; }
done
[ "$(id -u)" -eq 0 ] || { echo "!! run as root" >&2; exit 1; }
mountpoint -q $BPFFS || mount -t bpf bpf $BPFFS 2>/dev/null

echo "kernel: $(uname -r)"
fail=0

for src in t*.bpf.c; do
	name=${src%%.*}
	obj=${src%.c}.o
	expect=$(sed -n 's/.*EXPECT: \([A-Z]*\).*/\1/p' "$src" | head -1)

	if ! clang -g -O2 --target=bpf -c "$src" -o "$obj" 2>"$name.build.log"; then
		printf 'BUILD-ERR %-20s\n' "$name"
		cat "$name.build.log"
		fail=1
		continue
	fi
	rm -f "$name.build.log"

	rm -f $BPFFS/$name
	if log=$(bpftool prog load "$obj" $BPFFS/$name type xdp 2>&1); then
		got=ACCEPT
	else
		got=REJECT
	fi
	rm -f $BPFFS/$name

	if [ "$got" = "$expect" ]; then
		printf 'ok        %-20s %s\n' "$name" "$got"
	else
		printf 'FAILED    %-20s expected %s, got %s\n' "$name" "$expect" "$got"
		fail=1
	fi

	if [ "$VERBOSE" = "-v" ] && [ "$got" = REJECT ]; then
		echo "$log" | sed 's/^/          | /'
	fi
done

[ $fail -eq 0 ] && echo "all tests passed"
exit $fail
