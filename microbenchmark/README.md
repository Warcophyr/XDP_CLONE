# microbenchmark

TRex-driven microbenchmarks for the XDP clone actions, moved here from
`~/xdp-clone/lat-tests` together with the applications they drive.

```
microbenchmark/
├── lat.py                  latency, one program instance per copy count
├── throughput.py           max no-loss rate, exponential ramp + binary search
├── no-drop-throughput.py   same search, plus a confirmation phase (NDR)
├── bench_common.py         paths, NIC preflight, app list, process handling
├── profiles/               TRex stream profiles
├── results/                every CSV the scripts write (untracked)
│   └── archive/            the CSVs from the runs before the move
└── apps/
    ├── xdp-clone/                  XDP_CLONE_TX(n), copies go out as-is
    ├── xdp-clone-tstamp/           same, for the latency test
    ├── inline-xdp-clone/           XDP_CLONE_TX(n) + WQE inline header
    ├── inline-xdp-clone-tstamp/    same, for the latency test
    ├── tc-clone/                   TC-based clone, for comparison
    └── tc-clone-tstamp/            same, for the latency test
```

## Running

```bash
export ETH=enp52s0f1np1
sudo ethtool --set-priv-flags $ETH rx_striding_rq off
sudo ethtool --set-priv-flags $ETH xdp_tx_mpwqe off

cd apps/inline-xdp-clone && make && cd -      # once per application
python3 lat.py
python3 throughput.py
python3 no-drop-throughput.py
```

The scripts check those two private flags before they start and refuse to run
otherwise — see "What the flags are for" below. They can be run from any
directory now; paths are anchored to the scripts themselves.

## How latency is measured

The probe and the load are **separate streams on separate ports**. `profiles/
clonlat.py` sends an uncloned probe to UDP 8902 at `LATENCY_PROBE_PPS` with the
latency histogram on `pg_id=1`, and the fan-out load to UDP 8901 at
`LATENCY_LOAD_PPS` with plain counters on `pg_id=2`; the `-tstamp` apps bounce
8902 straight back, untouched, and clone 8901. So the histogram holds one sample
per packet sent, of a frame that travelled alone, while the machine is busy
fanning out the load. `copies` in the CSV comes from the load stream's rx/tx
ratio and should equal the configured count.

It used to be one stream doing both jobs: the app stripped TRex's latency magic
from every frame and put it back on exactly one copy, so that the generator got
one sample per packet instead of n+1 duplicates. What that measured was not the
machine. The reported latency stepped from ~9 to ~43 microseconds between two
and three copies and then stopped growing — 43, 43, 45 at 3, 4, 8 copies —
identically for the XDP, the inline and the TC application. Three measurements
place the cost outside the datapath:

* moving the magic from the last copy to the **first** changes nothing (11, 11,
  43, 42, 43 µs at 1, 2, 3, 4, 8 copies), so it is not the time to finish the
  fan-out;
* letting the driver produce every copy but putting only **one** frame on the
  wire removes the step entirely (9, 10, 11, 12, 14 µs at 1..8 copies, with
  `rx_xdp_drop` counting the copies that were produced and dropped), so it is
  not the cost of cloning;
* the step is there at **100 packets per second**, one packet every 10 ms, and
  gone at 50 kpps — the slower case is the emptier one, which rules out
  congestion anywhere.

With the probe separated, the curve is flat across every copy count on an idle
machine and rises monotonically under load, which is what it should do.

## What each test measures

| script | profile | what TRex reports | latency? |
|---|---|---|---|
| `lat.py` | `profiles/clonlat.py` | `get_pgid_stats`: latency histogram of the uncloned probe (pg_id 1), flow_stats rx/tx of the cloned load (pg_id 2) | yes, under fan-out load |
| `throughput.py` | `profiles/zipf-profile.py` | port counters `opackets`/`ipackets` | no |
| `no-drop-throughput.py` | `profiles/zipf-profile.py` + one probe stream | port counters, and `get_pgid_stats` for latency | yes, at the NDR |

`throughput.py` measures **rate only**: the profile it loads builds plain
`STLTXCont` streams with no `flow_stats`, and the script reads nothing but the
port packet counters.

`no-drop-throughput.py` asks the same profile for **one extra timestamped
stream** (`latency_pps`, 1 kpps, `pg_id=1`) and reads its latency during the
confirmation probes — so the latency columns describe the device *at* its
no-drop rate rather than under some other load. The probe looks like the zipf
traffic, so the program under test treats it like any other packet, which means
it gets **cloned like any other packet**: at n copies the generator receives n+1
timestamped frames per one it sent, the latency distribution is over the whole
fanout, and `lat_dup` in `ndr_results.csv` is expected to be large. At 1 kpps
against several Mpps the probe is noise in the offered load, and since both
sides of it scale with the fanout it does not move the delivery ratio either.

## `inline-xdp-clone`

Same benchmark as `xdp-clone`, with the packet's first 12 bytes handed to the
NIC as the WQE inline header instead of being DMA'd
(`mellanox-clone-xdp/examples/inline-clone` documents the mechanism, and the
apps here include its `axdp_tx.h`).

It stamps the TX descriptor on the **original**, which is what puts the driver
on its shared-page clone path: all n+1 frames are emitted out of the one RX
page, with no page allocation and no 320-byte memcpy per copy. `xdp-clone`
allocates and copies. That difference is what this pair measures.

By default (`MODE 0`) it stamps **no inline header at all** — just the
descriptor. Every frame that leaves is byte-identical to what `xdp-clone` would
have sent: same length, same bytes, same fanout, so the two are directly
comparable and what shows up in the numbers is the page and the memcpy.
`MODE 1` pushes a 12-byte header as well, copied from the packet's own first
bytes; it measures the header on top of the shared page, but the frames leave 12
bytes longer, so read those rates against `MODE 0` of the same program and not
against `xdp-clone`.

The rule that comes with the shared page: the program may touch nothing but the
metadata. `bpf_xdp_adjust_head()` is out, since its memmove of the metadata
lands on the packet's first bytes and the other emissions share that page — so
this pushes and cannot replace. That is also why the header cannot be made
byte-identical *and* keep the frame length, which is what the earlier version of
this app did with a per-copy page.

**`inline-xdp-clone-tstamp` stays on the copy path** — for now only out of
inertia. It used to have no choice: the latency trick it carried, editing the
payload of one copy so that TRex saw one sample per packet instead of n+1
duplicates, needed a page per copy, because on a shared page writing the magic
for the last copy changes the frames already queued for the earlier ones. That
write is gone (see *How latency is measured* above), so nothing stops this app
from stamping the original and being measured on the shared page like the
throughput one. Switching it would make the latency and throughput columns
describe the same datapath, and is the obvious next step here.

At `copies=0` every application here returns a plain `XDP_TX` rather than
`XDP_CLONE_TX(0)`. It is the same one frame out either way, but the clone action
costs the copy-count write and the whole `XDP_CLONE_TX` tail in the driver, and
it costs it measurably: on this NIC the `copies=0` row went from 6.7 to 10.3
Mpps of output when the plain action was used instead. So that row is now the
plain-transmission reference, and the same code path in all of them.

Worth keeping in mind when reading `copies=0` against the rest of a column: at
one frame in and one frame out there is nothing to amortise the per-RX-packet
cost over, which is why `xdp-clone` emits only ~6.7 Mpps there and ~10.6 Mpps at
`copies=2`.

## What the flags are for

Both mistakes produce a complete set of plausible numbers that measure
something else, which is why the preflight is a hard failure:

- `rx_striding_rq off` — the `XDP_CLONE_*` actions only exist in
  `mlx5e_skb_from_cqe_linear()`, the legacy-RQ path. With striding RQ the RX
  path goes through `mlx5e_xdp_handle()`, which does not know the actions and
  drops the packets.
- `xdp_tx_mpwqe off` — the TX descriptor is only read on the regular-WQE path.
  An MPWQE session shares one eseg between many packets, so the inline header
  is ignored: the packet still goes out, just without the header, and
  `inline-xdp-clone` measures a plain clone. The driver logs
  `XDP TX descriptor ignored, xdp_tx_mpwqe is on` if this happens.

## Problems found in the tests

### Fixed

1. **`set_governor` only covered 9 cores** (`range(9)`) on a 16-core machine, so
   seven cores stayed on `schedutil` for the whole run. It now enumerates the
   CPUs that actually have a `cpufreq` governor.
2. **`lat.py` never pinned the governor at all.** A latency benchmark on
   `schedutil` is largely measuring the governor. It now pins `performance` and
   restores `schedutil`, like the throughput tests already did — so latency
   numbers from before the move are not comparable with new ones.
3. **Everything was relative to the working directory.** Run from anywhere but
   `lat-tests/` and the profile was not found and the CSVs landed elsewhere.
4. **`stderr` was a pipe nobody read.** A program that talked on stderr would
   block on a full pipe forever, and a program that failed to start reported
   nothing but "timeout". Output is now folded into stdout and the last 20 lines
   are printed when the ready signal never arrives.
5. **`terminate()` had no deadline.** A program that does not exit leaves its
   XDP program attached and the next measurement runs against it. There is now
   a 5 s deadline followed by `kill()`.
6. **`no-drop-throughput.py` mislabelled every `per_application` summary row.**
   The dict was `{"configured_copies": "ALL", **row_fields}`, and the expansion
   came last, so `row`'s own copy count overwrote `"ALL"`. Visible in
   `results/archive/ndr_summary.csv`: the `per_application` line says
   `configured_copies=0`.
7. **The artificial rate cap was invisible in the output.** `tx_cap =
   RX_CAP_MPPS / (copies + 1)` clamps the requested rate, and a probe that
   succeeds while clamped was written out as if it were the measured maximum.
   Both CSVs now carry a `rate_capped` column.
8. Three identical copies of `launch_program`/`stop_program`/`set_governor` had
   already drifted apart (only `throughput.py` set the governor). One copy now,
   in `bench_common.py`.
9. **The percentile columns were wrong.** Nine of the seventeen rows in
   `results/archive/latency_results.csv` have percentiles *above* the maximum,
   which cannot happen for one consistent set of samples:

   ```
   application,configured_copies,repetition,min,avg,max,p50,p90,p95,p99,...
   xdp-clone,0,2,5.0,8.0,8.0,400.0,2000.0,2000.0,2000.0,...
   ```

   The histogram was never the problem: `clear_pgid_stats()` does make it
   relative to the window, bucket by bucket. `total_max` is what lied — the
   client zeroes it at the clear and then rebuilds it from `last_max`, the
   server's most recent sampling interval, and only at the moments
   `get_pgid_stats()` is called. Clear, sleep five seconds, read once, and
   `total_max` describes the last fraction of a second while the histogram
   describes all five; the old code passed it in as the top of the range and
   clamped every bucket to it. Percentiles and the maximum now come out of the
   histogram alone, in one implementation shared by both scripts
   (`bench_common.histogram_percentiles()`).

10. **`ndr_summary.csv` was mostly noise.** A `scope` column that read
    `per_copies` on every row, a trailing `per_application` row holding
    whichever copy count happened to score highest, and eight `confirm_*`
    columns beside the numbers they summarised. It is now one row per
    (application, copy count) with `application`, `configured_copies`,
    `tx_cap_mpps`, `rate_capped`, `ndr_tx_mpps`, `ndr_rx_mpps` and their
    standard deviations, `ndr_fanout`, `ndr_lost_pkts`, and the latency
    percentiles.
    `fanout_multiplier` and `ndr_input_equiv_mpps` went too: both are functions
    of `configured_copies` alone — fanout is `copies + 1`, input-equivalent is
    `ndr_rx_mpps / fanout` — so nothing is lost. `ndr_tx_mpps` and
    `ndr_rx_mpps` are now the **mean over the confirmation probes** rather than
    the single search probe that found the rate, which is what makes a standard
    deviation next to them mean anything.

11. **The NDR was not a no-drop rate.** `SUCCESS_THRESHOLD = 0.99` accepted one
    packet in a hundred going missing and still called the rate an NDR, and two
    things in the measurement made a stricter criterion impossible anyway. The
    probe read its final counters *while the generator was still sending*, so
    every frame in the XDP SQ and on the wire counted as transmitted but not
    received — a whole pipeline's worth of systematic loss. And the criterion
    was a ratio of rates, which cannot express "not one frame".

    The measurement is fixed: the counters are read after the traffic stops and
    a `DRAIN_SECONDS = 1.0` pause lets everything in flight come back, so the
    delivered fraction is now the device's and not the pipeline's. The
    criterion stays a percentage — `MIN_DELIVERED_PCT`, default **99.9** — but
    it is computed in packets against the whole fanout, original included, and
    the frames actually lost by the accepted rate are reported as
    `ndr_lost_pkts`. So the claim is auditable rather than implied: at 99.9%
    over a 4 s window at 3.5 Mpps with three-way fanout, the threshold allows
    up to ~42.000 frames out of 42 million, and the column says how many really
    went missing. Set it to `100.0` for a strict no-drop rate.

    `throughput.py` still uses the 0.99 ratio: it asks a different question —
    the highest rate at which the device keeps up — and a tolerance there is a
    choice, not a bug. Do not read its numbers as no-drop rates.

12. **Nothing checked that the cloning happened.** The fanout was assumed
    everywhere: `expected_rx = tx * (copies + 1)`. A device that silently
    stopped cloning would return `rx ≈ tx`, fail at every rate, and report "no
    NDR found" — which is what the empty rows in
    `results/archive/ndr_summary.csv` look like, and it is indistinguishable
    from a device that simply cannot keep up. `measured_fanout` (`rx / tx`) is
    now recorded per probe and averaged into the summary as `ndr_fanout`: at
    1.0 the device never cloned, at `copies + 1` with `ndr_lost_pkts` at zero
    every frame of the fanout came back. That pair is the whole claim an NDR
    makes, and it is now in the table.

### Flagged — these need a decision, so nothing was changed

1. ~~**`lat.py` cannot tell whether the cloning happened at all.**~~ Fixed: the
   fan-out is now read from the *load* stream's own pg_id, which is the one that
   gets cloned, so `copies` reports 1, 2, 4 … and a driver that silently stopped
   cloning shows up as a zero. The old rows all read `rx_tx_ratio=1, copies=0`
   whatever the configured count, because the probe and the load were the same
   stream.

2. **The baseline apps probably will not load on the patched kernel.**
   `apps/xdp-clone` and `apps/xdp-clone-tstamp` have a metadata-check block that
   can fall through to the clone action:

   ```c
   if (ctx->data_meta + sizeof(__u32) <= ctx->data) {
       num_copy = *(__u32 *)data_meta;
       if (num_copy > 0 && num_copy <= n_clone) return XDP_TX;
   }   /* num_copy == 0, or > n_clone, falls through */
   ...
   return XDP_CLONE_TX(n_clone);
   ```

   On that fall-through the verifier state is `BPF_XDP_META_PRESENT`, and
   `check_xdp_clone_retval()` refuses it: *"returns XDP_CLONE_TX on a packet
   carrying 4 bytes of metadata, this is a copy and nested clones are not
   allowed"*. All four apps load on the stock kernel, which is what is running
   here, so this has not been observed yet. The fix is a `return XDP_DROP;` at
   the end of the block — semantically a no-op, because that fall-through is a
   nested clone the driver would abort anyway. Left alone because it touches the
   baseline; the `inline-*` apps are written so the block always returns.

3. **The throughput search saturates at high copy counts.** With
   `RX_CAP_MPPS = 30` and `START_TX_MPPS = 0.25`, `copies=64` gives a search
   range of 0.25 → 0.46 Mpps, so the reported maximum is the cap and not a
   measurement. `rate_capped` now says so, but the real fix is to raise
   `RX_CAP_MPPS` to the receiver's actual capability, or to read those points as
   lower bounds. The archived NDR run shows the neighbouring failure mode too —
   no rate met the threshold at all for `copies >= 1`, so those rows are empty
   — and with `ndr_fanout` now in the table, a repeat of that would say whether
   it was loss or a device that never cloned.

4. **`profiles/zipf-profile.py` hardcodes a destination MAC**
   (`58:a2:e1:d0:69:ce`, which is `enp52s0f0np0`) while the tests run on `$ETH`.
   It works only because `enp52s0f1np1` happens to be in `PROMISC`. Take the
   interface out of promiscuous mode and the throughput goes to zero with no
   explanation. The profile should take the MAC as a tunable.

5. **Trial length.** `WARMUP_SECONDS = 2`, `MEASURE_SECONDS = 4`. RFC 2544 asks
   for 60 s trials; 4 s is fine for a microbenchmark but the numbers carry that
   caveat: the decision is taken on a single 4 s window, and at
   `MIN_DELIVERED_PCT = 100.0` one stray drop anywhere in it fails the rate.

6. **Traffic runs across the attach/detach of every configuration.** `lat.py`
   starts TRex once and stops it at the very end, so between `stop_program` and
   the next `launch_program` there is no XDP program and the packets go up the
   stack. The 2 s sleep before `clear_pgid_stats` covers it, but only by
   accident of timing.

7. **The five latency repetitions share one program instance**, so they repeat
   the measurement and not the setup: they say nothing about run-to-run variance
   in attaching the program or in the page-pool state.
