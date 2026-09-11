"""Shared bits for the three microbenchmarks: where things live, what the NIC
has to look like, and the list of applications under test.

The rest of each script is left as it was, so that numbers taken before and
after this refactor stay comparable.
"""

import os
import shlex
import subprocess as sp
import sys

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
APPS_DIR = os.path.join(BASE_DIR, "apps")
PROFILES_DIR = os.path.join(BASE_DIR, "profiles")
RESULTS_DIR = os.path.join(BASE_DIR, "results")

# Paths are anchored to this file rather than to the working directory: the
# scripts used to only work when run from their own directory, and silently
# wrote their CSVs wherever they were started from.


def profile(name):
    return os.path.join(PROFILES_DIR, name)


def result(name):
    """Path of an output CSV, with results/ created if it is not there yet."""
    os.makedirs(RESULTS_DIR, exist_ok=True)
    return os.path.join(RESULTS_DIR, name)


def app(directory, binary):
    """Command line for one application under test, minus the copy count.

    $ETH is left in the string for os.path.expandvars() to fill in, the way the
    scripts have always done it.
    """
    return f"sudo {os.path.join(APPS_DIR, directory, binary)} $ETH"


# ---------------------------------------------------------------------------
# Applications under test
#
# The -tstamp builds are the ones the latency test needs: they strip TRex's
# latency magic from every copy but one, so that the generator gets a single
# sample per packet it sent instead of n+1 duplicates. Everything else uses the
# plain builds.
#
# 'inline' marks the applications that stamp an A-XDP TX descriptor, and
# therefore need xdp_tx_mpwqe off to be measuring anything at all.
#
# The two inline builds are not the same experiment. inline-xdp-clone stamps the
# original, which puts the driver on its shared-page path: one page for the
# whole batch, no memcpy per copy. inline-xdp-clone-tstamp cannot do that -- its
# latency magic is a per-copy edit of the payload -- so it stamps on the copies
# and stays on the copy path, measuring the inline header rather than the page.
# See README.md.
# ---------------------------------------------------------------------------
# Latency is measured on a probe stream that is never cloned, while the fan-out
# load runs beside it (profiles/clonlat.py). The load is a *publish* rate, so
# the frames the machine actually emits are LATENCY_LOAD_PPS * (copies + 1):
# 100k publishes is 200k frames at one copy and 6.5M at 64, which is why the
# curve bends at high copy counts -- that bend is the result, not an artefact.
# Set LATENCY_LOAD_PPS = 0 to measure an idle machine, where the curve comes
# out flat across every copy count.
LATENCY_LOAD_PPS = 100_000
LATENCY_PROBE_PPS = 1_000

LATENCY_APPS = {
    "xdp-clone": {
        "base_command": app("xdp-clone-tstamp", "xdp_clone"),
        "clones": [0, 1, 2, 4, 8, 16, 32, 64],
    },
    "inline-xdp-clone": {
        "base_command": app("inline-xdp-clone-tstamp", "inline_xdp_clone"),
        "clones": [0, 1, 2, 4, 8, 16, 32, 64],
        "inline": True,
    },
    "tc-clone": {
        "base_command": app("tc-clone-tstamp", "tc_clone"),
        "clones": [0, 1, 2, 4, 8, 16, 32, 64],
    },
}

THROUGHPUT_APPS = {
    "xdp-clone": {
        "base_command": app("xdp-clone", "xdp_clone"),
        "clones": [0, 1, 2, 4, 8, 16, 32, 64],
    },
    "inline-xdp-clone": {
        "base_command": app("inline-xdp-clone", "inline_xdp_clone"),
        "clones": [0, 1, 2, 4, 8, 16, 32, 64],
        "inline": True,
    },
    "tc-clone": {
        "base_command": app("tc-clone", "tc_clone"),
        "clones": [0, 1, 2, 4, 8, 16, 32, 64],
    },
    
    # "xdp-clone": {
    #         "base_command": app("xdp-clone", "xdp_clone"),
    #         "clones": [0],
    # },
    # "inline-xdp-clone": {
    #                 "base_command": app("inline-xdp-clone", "inline_xdp_clone"),
    #                 "clones": [0],
    #                 "inline": True,
    # },
        
}


# ---------------------------------------------------------------------------
# NIC preflight
# ---------------------------------------------------------------------------
# Two mlx5 private flags decide whether the driver features under test are
# reachable at all, and getting either wrong produces a full set of
# plausible-looking numbers that measure something else:
#
#   rx_striding_rq off  the XDP_CLONE_* actions are only implemented in
#                       mlx5e_skb_from_cqe_linear(), the legacy-RQ path. With
#                       striding RQ the RX path goes through mlx5e_xdp_handle(),
#                       which does not know the actions and drops the packets.
#   xdp_tx_mpwqe off    the A-XDP TX descriptor is only read on the regular-WQE
#                       path. An MPWQE session shares one eseg between many
#                       packets, so the inline header is ignored -- the packet
#                       still goes out, just without the header, and the
#                       benchmark measures a plain clone.
REQUIRED_PRIV_FLAGS = {
    "rx_striding_rq": "off",
    "xdp_tx_mpwqe": "off",
}


def _read_priv_flags(iface):
    out = sp.run(
        ["sudo", "ethtool", "--show-priv-flags", iface],
        capture_output=True,
        text=True,
    )
    if out.returncode != 0:
        raise RuntimeError(
            f"ethtool --show-priv-flags {iface} failed: {out.stderr.strip()}"
        )

    flags = {}
    for line in out.stdout.splitlines():
        if ":" not in line:
            continue
        name, _, value = line.partition(":")
        flags[name.strip()] = value.strip()
    return flags


def check_nic(iface=None, need_inline=False):
    """Refuse to start unless the NIC is set up the way the tests assume.

    Loud and up front: every one of these mistakes yields a complete set of
    numbers that look fine and mean nothing.
    """
    iface = iface or os.environ.get("ETH")
    if not iface:
        raise RuntimeError("ETH is not set; export ETH=<ifname> before running")

    flags = _read_priv_flags(iface)
    wanted = dict(REQUIRED_PRIV_FLAGS)
    if not need_inline:
        # Only the inline applications care about this one.
        wanted.pop("xdp_tx_mpwqe")

    wrong = {
        name: flags.get(name)
        for name, expected in wanted.items()
        if flags.get(name) != expected
    }
    if wrong:
        fixes = "\n".join(
            f"  sudo ethtool --set-priv-flags {iface} {name} {wanted[name]}"
            for name in wrong
        )
        raise RuntimeError(
            f"{iface} is not set up for these tests: "
            + ", ".join(f"{n}={v}" for n, v in wrong.items())
            + "\nFix it with:\n"
            + fixes
        )

    print(f"[preflight] {iface}: " + ", ".join(f"{n}={flags[n]}" for n in wanted))
    return iface


# ---------------------------------------------------------------------------
# CPU governor
# ---------------------------------------------------------------------------
def cpu_list():
    """Every CPU that has a cpufreq governor, not the first nine of them."""
    cpus = []
    base = "/sys/devices/system/cpu"
    for entry in sorted(os.listdir(base)):
        if not entry.startswith("cpu") or not entry[3:].isdigit():
            continue
        if os.path.exists(os.path.join(base, entry, "cpufreq", "scaling_governor")):
            cpus.append(int(entry[3:]))
    return cpus


def set_governor(governor):
    cpus = cpu_list()
    if not cpus:
        print("No cpufreq governor to set (driver missing?), skipping")
        return

    failed = []
    for core in cpus:
        path = f"/sys/devices/system/cpu/cpu{core}/cpufreq/scaling_governor"
        try:
            sp.run(
                ["sudo", "tee", path],
                input=governor,
                text=True,
                stdout=sp.DEVNULL,
                check=True,
            )
        except sp.CalledProcessError:
            failed.append(core)

    if failed:
        print(f"governor {governor}: failed on cores {failed}")
    else:
        print(f"governor {governor}: set on {len(cpus)} cores")


# ---------------------------------------------------------------------------
# Starting and stopping the application under test
# ---------------------------------------------------------------------------
# One copy of this, shared by the three scripts, which used to carry three
# identical ones. Two things are different from those:
#
#   - stderr is folded into stdout and the last lines are kept. The old code
#     left stderr on a pipe nobody read, so a program that said much on it would
#     block on a full pipe forever, and a program that failed to start reported
#     nothing but "timeout".
#   - terminate() is given a deadline and followed by kill(). A program that
#     does not go away leaves its XDP program attached to the interface, and the
#     next measurement runs against it.
import collections
import threading

from tqdm import tqdm

READY_MARK = "bpf program attached"


def launch_program(base_command, clones, ready_timeout=15):
    command = os.path.expandvars(f"{base_command} {clones}")
    tqdm.write("Running command: " + command)

    try:
        process = sp.Popen(
            shlex.split(command),
            stdout=sp.PIPE,
            stderr=sp.STDOUT,
            text=True,
            bufsize=1,
        )
    except Exception as exc:
        tqdm.write("Exception occurred while running command: " + str(exc))
        return None

    ready = threading.Event()
    tail = collections.deque(maxlen=20)

    def read_output():
        for line in process.stdout:
            tail.append(line.rstrip())
            if READY_MARK in line.lower():
                ready.set()

    threading.Thread(target=read_output, daemon=True).start()

    if ready.wait(timeout=ready_timeout):
        tqdm.write("Program ready")
        return process

    tqdm.write(f"Timeout: ready signal not received in {ready_timeout}s")
    if tail:
        tqdm.write("Last output from the program:")
        for line in tail:
            tqdm.write("  " + line)
    stop_program(process)
    return None


def stop_program(process, timeout=5):
    if process is None or not hasattr(process, "terminate"):
        return
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=timeout)
        except sp.TimeoutExpired:
            tqdm.write(
                f"Program did not exit in {timeout}s after SIGTERM, killing it "
                "(check that no XDP program is left attached)"
            )
            process.kill()
            process.wait()
    tqdm.write("Program terminated.")


# ---------------------------------------------------------------------------
# Latency out of TRex's per-pg_id stats
# ---------------------------------------------------------------------------
# One implementation, shared by lat.py and no-drop-throughput.py, because the
# one they used to carry was wrong in a way that is worth writing down.
#
# After clear_pgid_stats(clear_latency_stats=True) the *histogram* TRex hands
# back is correct: trex_stl_stats.py subtracts the reference bucket by bucket,
# so it describes exactly the window since the clear. 'total_max' is not. The
# client zeroes it at the clear and then rebuilds it from 'last_max' -- the max
# of the server's most recent sampling interval -- and only at the moments
# get_pgid_stats() is called. Clear, sleep five seconds, read once, and
# total_max describes the last fraction of a second while the histogram
# describes all five.
#
# That is where percentiles above the maximum came from: the old code passed
# total_max in as the top of the range and clamped every bucket to it, so a
# 400 us bucket was reported as a p50 of 400 next to a max of 8. The fix is not
# to involve total_max at all -- the histogram already carries the range.
#
# TRex's buckets are logarithmic and keyed by their lower bound, and the
# documented meaning of {100: 13} is "13 packets between 100 and 200 usec", so
# the upper bound of a bucket is the next key that exists in the scale, not the
# next key present in the dict.

LATENCY_PERCENTILES = (50, 90, 95, 99)


def _bucket_upper(lo):
    """Upper bound of TRex's logarithmic bucket whose lower bound is @lo.

    The scale runs 1..9, 10..90, 100..900, ... so the step at any magnitude is
    the magnitude itself: bucket 300 ends at 400, bucket 900 at 1000.

    Bucket 0 is the exception: the formula would make it one microsecond wide,
    when in the histograms TRex actually returns it is the whole [0, 10) decade
    -- 0 and 10 are adjacent keys with nothing between them. Getting this wrong
    put p50 at 0.5 us for every run whose latency sat in that first bucket,
    which is how the summary ended up reporting a median below its own minimum.
    """
    if lo <= 0:
        return 10.0
    step = 10 ** (len(str(int(lo))) - 1)
    return lo + step


def histogram_percentiles(histogram, percentiles=LATENCY_PERCENTILES):
    """Interpolate @percentiles out of a TRex latency histogram.

    Returns {percentile: usec}, plus the highest populated bucket's upper bound
    under the key 'max', or None if the histogram is empty.
    """
    buckets = sorted(
        (float(lo), int(count)) for lo, count in histogram.items() if int(count) > 0
    )
    if not buckets:
        return None

    total = sum(count for _, count in buckets)
    out = {}

    for percentile in percentiles:
        target = (percentile / 100.0) * total
        cumulative = 0
        value = buckets[-1][0]

        for lo, count in buckets:
            previous = cumulative
            cumulative += count
            if cumulative >= target:
                fraction = (target - previous) / count
                fraction = min(1.0, max(0.0, fraction))
                value = lo + fraction * (_bucket_upper(lo) - lo)
                break

        out[percentile] = value

    out["max"] = _bucket_upper(buckets[-1][0])
    return out


def latency_sample(client, pg_id=1):
    """min / avg / p50..p99 / max for @pg_id since the last clear, or None.

    'max' is the top of the highest populated bucket rather than TRex's
    total_max, for the reason above; it is therefore a bucket bound and not an
    observed value, which is also true of the percentiles.
    """
    stats = client.get_pgid_stats([pg_id])
    section = stats.get("latency") or {}
    entry = section.get(pg_id, section.get(str(pg_id)))
    if not entry:
        return None

    values = entry.get("latency", {})
    percentiles = histogram_percentiles(values.get("histogram", {}))
    if percentiles is None:
        return None

    sample = {
        "lat_min": float(values.get("total_min", float("nan"))),
        "lat_avg": float(values.get("average", float("nan"))),
        "lat_max": percentiles.pop("max"),
    }
    for percentile, usec in percentiles.items():
        sample[f"lat_p{percentile}"] = usec

    err = entry.get("err_cntrs", {}) or {}
    sample["lat_dup"] = int(err.get("dup", 0))
    sample["lat_seq_err"] = int(err.get("seq_too_high", 0)) + int(
        err.get("seq_too_low", 0)
    )
    return sample


LATENCY_FIELDS = (
    "lat_min",
    "lat_avg",
    "lat_p50",
    "lat_p90",
    "lat_p95",
    "lat_p99",
    "lat_max",
)
