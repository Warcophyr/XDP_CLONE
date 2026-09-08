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

## What each test measures

| script | profile | what TRex reports | latency? |
|---|---|---|---|
| `lat.py` | `profiles/clonlat.py` | `get_pgid_stats`: latency counters + histogram, flow_stats rx/tx | yes |
| `throughput.py` | `profiles/zipf-profile.py` | port counters `opackets`/`ipackets` | no |
| `no-drop-throughput.py` | `profiles/zipf-profile.py` | port counters `opackets`/`ipackets` | no |

The two throughput tests measure **rate only**. Their profile builds plain
`STLTXCont` streams with no `flow_stats` and no `STLFlowLatencyStats`, and the
scripts read nothing but the port packet counters, so there is no latency and no
percentile anywhere in `throughput_*.csv` or `ndr_*.csv`. `STLFlowLatencyStats`
appears in exactly one place in this tree, `profiles/clonlat.py`, which only
`lat.py` loads — so the broken percentile code below is confined to `lat.py`
and cannot have touched the NDR numbers.

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

**`inline-xdp-clone-tstamp` deliberately stays on the copy path.** The latency
trick — editing the payload of one copy so that TRex sees one sample per packet
sent instead of n+1 duplicates — needs a page per copy by construction: on a
shared page, writing the magic for the last copy would change the frames already
queued for the earlier ones. So shared mode is measurable on throughput and NDR,
not on latency, and the latency column measures the inline header on the copy
path instead.

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

### Flagged — these need a decision, so nothing was changed

1. **The percentile columns in `lat.py` are wrong.** Nine of the seventeen rows
   in `results/archive/latency_results.csv` have percentiles *above* the
   maximum, which cannot happen for one consistent set of samples:

   ```
   application,configured_copies,repetition,min,avg,max,p50,p90,p95,p99,...
   xdp-clone,0,2,5.0,8.0,8.0,400.0,2000.0,2000.0,2000.0,...
   ```

   `min`/`avg`/`max` come from TRex's counters and `p*` from its histogram, so
   one of the two is not being reset by
   `clear_pgid_stats(clear_latency_stats=True)` and the percentiles describe the
   whole session rather than the 5 s window. `save_latency_summary_csv` then
   drops `nan`s silently, which hides it. One debug run printing the raw
   histogram next to `total_min`/`total_max` would settle which side is stale.

2. **`lat.py` cannot tell whether the cloning happened at all.** The `copies`
   column is `rx_pkts/tx_pkts` from TRex's *flow_stats*, and the `-tstamp` apps
   deliberately leave the latency magic on only one copy — so flow_stats sees
   one rx per tx and `copies` reads 0 for every configured count. Every archived
   row confirms it: `rx_tx_ratio=1, copies=0` at `configured_copies` 0, 1, 2 and
   4 alike. If the driver silently stopped cloning, the latency numbers would
   look perfectly healthy. A fanout check has to come from elsewhere, e.g. the
   interface's own counters via `ethtool -S $ETH`.

3. **The baseline apps probably will not load on the patched kernel.**
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

4. **The throughput search saturates at high copy counts.** With
   `RX_CAP_MPPS = 30` and `START_TX_MPPS = 0.25`, `copies=64` gives a search
   range of 0.25 → 0.46 Mpps, so the reported maximum is the cap and not a
   measurement. `rate_capped` now says so, but the real fix is to raise
   `RX_CAP_MPPS` to the receiver's actual capability, or to read those points as
   lower bounds. The archived NDR run shows the neighbouring failure mode too —
   no rate met the 0.99 threshold at all for `copies >= 1`, so those rows are
   empty.

5. **`profiles/zipf-profile.py` hardcodes a destination MAC**
   (`58:a2:e1:d0:69:ce`, which is `enp52s0f0np0`) while the tests run on `$ETH`.
   It works only because `enp52s0f1np1` happens to be in `PROMISC`. Take the
   interface out of promiscuous mode and the throughput goes to zero with no
   explanation. The profile should take the MAC as a tunable.

6. **Trial length.** `WARMUP_SECONDS = 2`, `MEASURE_SECONDS = 4`. RFC 2544 asks
   for 60 s trials; 4 s is fine for a microbenchmark but the numbers carry that
   caveat, and the `SUCCESS_THRESHOLD = 0.99` decision is taken on ~4 million
   packets at the low rates.

7. **Traffic runs across the attach/detach of every configuration.** `lat.py`
   starts TRex once and stops it at the very end, so between `stop_program` and
   the next `launch_program` there is no XDP program and the packets go up the
   stack. The 2 s sleep before `clear_pgid_stats` covers it, but only by
   accident of timing.

8. **The five latency repetitions share one program instance**, so they repeat
   the measurement and not the setup: they say nothing about run-to-run variance
   in attaching the program or in the page-pool state.
