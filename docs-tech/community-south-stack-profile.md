# Community South stack: profile of a 1000-frame run

Measured 2026-10-31 at 271e16d7, the revision of the decode script's latest
Community South video stage (`orc-271e16d-20261031-155125/video/stack.orcprj`).
Every figure below comes from a run or a profiler; where a figure is an
estimate, it says so and shows the arithmetic.

Findings 1–6 have since been fixed, wholly or in part. The same cut now takes
**311 s instead of 598 s, with 46 % fewer CPU cycles, 47 % fewer NAS bytes
and 47 % less peak memory, and the output is bit-identical**. See
[After the fixes](#after-the-fixes). Each finding says what was done, and
what was left and why.

## Summary

| # | Finding | Measured cost | Fix | Status |
|---|---------|---------------|-----|--------|
| 1 | **Two of the eleven discs collide in the executor's cache: DS1 is never stacked, DS2 is stacked twice** | Stack quality, not time | Executor keys inputs on the producing node's cache key | **Fixed** |
| 2 | The video sink's VBI pre-pass stacks every frame in full to read 3 lines, then the export stacks it again | **46 % of all CPU cycles, 38 % of wall time, half the NAS reads** | Observe the VBI during the export; remux the metadata into the header afterwards | **Fixed** |
| 3 | FM Beat Cancel's 255-tap band-pass is a scalar, bounds-checked loop | ~34 % of all cycles in one loop | Branch-free loop over outputs (bit-identical) | **Fixed**; the chroma demodulator's loop of the same shape is not |
| 4 | `FieldBeat` allocates ~20 full-field `double`/`complex<double>` temporaries per fit, many fits at once | 5.8 GB of the 11.6 GB heap peak; memset 4 % of cycles | Fewer temporaries alive at once; per-thread buffers | **Partly**: temporaries scoped; per-thread buffers not done |
| 5 | glibc malloc arenas multiply with ~48 000 short-lived threads | +2.3 GB RSS, 29 M extra page faults, ~100 s system time | 4 arenas | **Fixed** in orc-cli |
| 6 | Pressing grouping fits FM for 16 frames × 11 inputs on one thread | ~30 s at 0.9 cores on the cut; ~3 min on the full side | Fetch the probe frames in parallel | **Fixed**: 35 s → 9 s on the cut |
| 7 | Every stage sizes its own thread pool to `hardware_concurrency()`, nested inside the read-ahead workers | up to 244 threads, ~78 runnable on 16 hardware threads | One shared pool / a CLI thread cap | **Not done**: see the finding |

## What was run

- **Project:** the script's `stack.orcprj`, unchanged except as listed
  below. It has 11 CVBS sources, then per source Frame Map and FM Beat Cancel,
  then Source Align, Stacker (Weighted, pressing repair, still-frame stacking,
  sample align), Dropout Correction (interpolate), Picture Band Limit, and a
  Video Sink writing FFV1/MKV with chapters and disc metadata.
- **Cut:** output frames 1–1000. Every Frame Map starts with `2-N`, so each one
  became `2-1001`.
  - That spelling made DS12, DS8 and DS10 collide as well as DS1 and DS2
    (finding 1). The measured runs therefore spell each range differently but
    equivalently (`2-501,502-1001`, `2-502,503-1001`, …), so all eleven discs
    are really stacked.
  - The first, colliding runs are kept only where marked "8-input".
- **Inputs:** the captures on the NAS (`~/raid/decodes`, NFSv3, rsize 128 KiB).
  Page cache evicted (`posix_fadvise(DONTNEED)`) before every run. Output
  written to local disk.
- **Machine:** AMD Ryzen 7 5800X (8 cores / 16 threads), 62 GB RAM, NixOS. The
  machine is shared, and some runs overlapped other users' Nix builds. Those
  runs are compared on user cycles and instructions, which contention barely
  moves, and the wall times quoted come from runs on a quiet machine.
- **Build:** `RelWithDebInfo` (`-O2 -g`) plus `-fno-omit-frame-pointer
  -mno-omit-leaf-frame-pointer`, no clang-tidy. The `-march=native` run used
  the same flags plus `ORC_NATIVE_ARCH=ON`, built from a clean worktree.
- **Tools** (all from the flake's dev shell):
  - `perf stat` (task-clock, cycles, instructions, page faults).
  - `perf record -e cycles:u -F 99 -g --user-callchains`. Plain `-g` is refused
    at `perf_event_paranoid=2`, and `ptrace_scope=1` stops attaching to a
    running orc-cli, so every profile starts orc-cli itself.
  - `perf annotate`.
  - `heaptrack` (opens its GUI on exit; use `--record-only` to avoid that).
  - A 1 Hz `/proc` sampler: RSS, threads by state, CPU, `rchar`/`read_bytes`.
  - NFS byte counters from `/proc/self/mountstats`.
  - A temporary counter build (FM fits, cache hits, stacked frames), removed
    afterwards.
  - A standalone microbenchmark of the FM band-pass.
- **Overhead:** `perf record` added under 1 % (569 s against 564 s, identical
  output).

## Finding 1: two discs collide in the executor's artifact cache

`DAGExecutor::get_cached_or_execute` looks up a node's outputs by a key built
from the stage name, its version, its parameters and **its inputs' artifact
IDs** (`compute_expected_artifact_id`, `orc/core/dag_executor.cpp:459`).
Wrapper stages do not put their input's ID into their own:

- Frame Map's output ID is `"frame_map_" + range_spec + "_rd…_pg…"`
  (`frame_map_stage.cpp:35`, `:638`), with no source in it.
- FM Beat Cancel's output ID is always `"beat_cancelled_frame"`.

So two Frame Maps with the same range text produce outputs with the same ID.
Two FM Beat Cancel nodes above them with the same carrier then get the same
cache key, and the second node is handed the first node's output.

In the script's stack, DS2 and DS1 both map `2-54003` and both have
`carrier_black_mhz: 7.0927`. The stack therefore contains one of them twice
and the other not at all. The script's own log of the real run shows it.
Inputs 1 (DS2) and 8 (DS1) have exactly the same σ and share in every segment
(5.90/5.90, 6.46/6.46, 8.95/8.95, 5.89/5.89), and they always form their own
"pressing" `{1,8}`.

On the cut, counters confirmed the mechanism. With every map spelled `2-1001`,
FM ran for only 8 distinct (input, frame) streams of 11. Once the ranges were
spelled apart, it ran for all 11.

The collision changes the result as well as hiding a disc. With all eleven
present, the grouping over the same 1000 frames is
`{1,5,6,7,8,9,10,11} {2} {3,4}`: the eight sharp discs of one mother found by
the Community South stacking comparison (doc removed after it closed; in git at
2611e5bb). The colliding cut gave `{1,8} {2} {3,4} {5,9,11} {6,7,10}`.

- **Fixed** in the executor rather than in each stage, since about ten
  stages give every output a fixed ID (`"beat_cancelled_frame"`,
  `"stacked_frame"`, `"corrected_frame"`, …). `DAGExecutor::gather_inputs`
  now builds each input's token from the cache key of the node that produced
  it (plus the output index and the artifact's own ID), and the cache key is
  the SHA-256 of that provenance string. Two alike branches over different
  sources therefore get different keys. Regression test:
  `DAGExecutor_AlikeBranchesOverDifferentSources_DoNotShareCache`.
- **Verified** by rerunning the original colliding cut (every map `2-1001`)
  with the fix. The grouping is now `{1,5,6,7,8,9,10,11} {2} {3,4}`, as with
  the ranges spelled apart. Inputs 1 and 8 went from 5.97/5.97 (one disc
  twice) to 10.24/10.66.
- **Older builds:** never give two Frame Maps identical range text, e.g.
  write one of them as `2-27000,27001-54003`. Stacks made before the fix that
  had such a pair should be rerun.

Every number below is from the corrected 11-input cut unless marked
"8-input".

## Where the time goes

### Run phases (quiet machine, 11 inputs)

| Phase | Wall | CPU used | Notes |
|-------|------|----------|-------|
| Project load, source open | ~0.1 s | — | |
| Stacker setup: still-frame reference noise (20 s), sample grid, pressing grouping (28 s) | 51 s | 2.5 → 4.2 → 0.9 cores | Grouping runs on one thread |
| Video sink VBI pre-pass ("Collecting VBI data") | 275 s | ~13 of 16 threads | Stacks every frame in full |
| Export | 286 s | ~14 of 16 threads | Stacks every frame again |
| **Total** | **613 s** | 7418 CPU-s (12.1 threads) | 31.7 T user cycles |

The run is **CPU-bound**. During the stacking passes the process read 50–60 MB/s
from the share. An A/B of stacker read concurrency on 2026-10-27 measured the
share at 100–145 MB/s for an 11-file interleaved read, so the reads are well
under what it can deliver (see [I/O](#io)).

### CPU by component

From `perf record`, with each sample attributed to the innermost stage frame on
its stack:

| Component | All | Setup | VBI pass | Export |
|-----------|-----|-------|----------|--------|
| **FM Beat Cancel** | **76.9 %** | 90.9 % | 81.5 % | 72.4 % |
| Stacker: stacking lines | 6.2 % | 0.4 % | 6.8 % | 5.9 % |
| Stacker: frame signatures | 5.8 % | 5.5 % | 6.0 % | 5.7 % |
| Picture Band Limit | 3.7 % | — | 2.0 % | 5.3 % |
| Stacker: still copies | 2.5 % | 0.1 % | 2.6 % | 2.5 % |
| PAL chroma decoder (Transform 2D/3D) | 1.7 % | — | — | 3.3 % |
| Unattributed (mostly sink threads) | 1.8 % | | | |
| FFmpeg (scale + FFV1) | 0.3 % | | | 0.6 % |
| CVBS source decode | 0.3 % | 1.5 % | 0.3 % | 0.2 % |
| Frame Map / Source Align | 0.3 % | | | |
| Stacker: pressing repair | 0.3 % | | | |
| Dropout Correction, biphase decode | < 0.1 % | | | |

- The share of cycles by phase is setup 1.9 %, VBI pass 45.8 % and export
  52.3 %.
- Pressing repair costs 0.3 % here, against 4.8 % in the colliding 8-input
  run: with the true grouping it has little to repair.
- Leaf functions are mostly inside FM:
  - `memset` is 6.7 % of all cycles (FM 4.1 %, signatures 2.3 %): zero-filled
    `std::vector` temporaries.
  - `libm` is 6.2 % (FM 4.0 %: `sincos` from `std::polar`, `hypot` from
    `std::abs(complex)`, `exp`; signatures 1.8 %).
  - `malloc`/`free` themselves are only 0.2 %.

### Inside FM Beat Cancel

`FieldBeat::FieldBeat` (`orc/sdk/src/fm_beat.cpp:283`) is 59 % of all cycles
on its own (8-input profile), and `perf annotate` puts the time in two loops:

- **`band_pass_at`, lines 376–381, about 57 % of the constructor.** This is a
  255-tap symmetric FIR (`kBandPassRadius = 127`) evaluated directly:
  - once per active sample, on the complex beat (`basis_`);
  - once per active sample plus 24, on the real IRE signal (`band_line`);
  - with two bounds tests inside the tap loop.

  The compiled code is scalar SSE2 (`mulsd`/`addsd`). Even at `-O3`, GCC does
  not vectorise this loop (`-fopt-info-vec` reports only the cheap loops at
  350, 365 and 420), because the bounds tests sit inside the tap loop.
- **`ChromaSplitter::demodulate`, `composite_split.h:154–161`, about 12 %.**
  The same shape: a per-sample FIR with a `std::clamp` on every tap index.

The rest is the moving averages (`box`, 4.6 %), zero-filling the temporaries,
and the libm calls.

**How often each frame is fitted** (counter build, 11 inputs):

| Counter | Value |
|---------|-------|
| `cancelled()` calls | 63,625 |
| Cache hits | 40,553 |
| Frame computations | 23,072 |
| Distinct (input, frame) | 11,000 |
| Field fits (`FieldBeat` constructed) | 30,299 |
| Stacked frames built | 2,000 for 1,000 distinct frames |

- Each input frame is fitted 2.1 times. Two of those are the two passes (the
  150-frame FM cache cannot bridge a pass over 1000 frames); the rest is
  eviction inside a pass. Within a pass there is no duplicate work: 0 lost
  races once the collision is gone.
- 98 % of FM time is first triggered by the still-frame copy search
  (`check_still_copy_samples` → `signature_input`), which reads ahead of the
  stack. That is the first reader of each frame, not extra work.
- About 0.2 CPU-seconds per field fit in production (76.9 % × 7722 CPU-s /
  30 299 fits, all 16 threads busy).

## Finding 2: the VBI pre-pass stacks everything twice

With `embed_chapter_metadata` or `embed_disc_metadata`, the video sink runs the
biphase observer over every frame before export
(`video_sink_stage.cpp:2288`). That has to happen first: Matroska writes
attachments and tags into the header, so the disc-metadata document must exist
before `avformat_write_header()` (`ffmpeg_output_backend.cpp:628`).

The observer reads 3 lines a field through `get_line`. But
`StackedVideoFrameRepresentation::get_line` calls `get_frame`, so every one of
those reads produces the whole stacked frame: FM fits on all 11 inputs,
signatures, the still-copy search, stacking, DOC and Band. The export then
produces every frame again: the stacker's 300-frame cache and FM's 150 cannot
hold a pass.

The same project with both options off:

| | Baseline | VBI pre-pass off | Change |
|---|---|---|---|
| Wall | 613 s | 378.5 s | −38 % |
| User cycles | 31.7 T | 17.2 T | −46 % |
| CPU | 7418 s | 4019 s | −46 % |
| NAS bytes read | 31.9 GB | 16.1 GB | −50 % |
| Peak RSS | 16.6 GB | 13.6 GB | −3.0 GB |

The NAS figure matters most for a full side:
- The baseline read its inputs from the share twice, because the 16 GB the
  first pass pulled in did not survive in the page cache until the export.
- On a full side (11 × ~77 GB) the second pass can never be cached. The
  pre-pass therefore costs a second ~800 GB read on top of the CPU.

Ways to keep chapters and disc metadata without a second full stack, cheapest
first:

1. **Take the VBI from where it is already decoded.** The script's map stage
   decodes every disc's VBI before the video stage. Disc metadata is per-disc
   anyway, so the sink could take it as an input (a file or an observation
   sidecar) instead of re-deriving it from the stack. This is the most
   script-friendly option and costs nothing at export time.
2. **A data-line read path.** VBI lines are outside the picture area, which FM
   Beat Cancel does not touch, and the stacker already treats data lines apart
   (`picture_area`, DOC's `is_data_line`). A `get_line` that stacks just those
   lines would skip the FM fits, signatures and still search.
   - It still reads every input frame. The biphase observer's comment
     measured whole-frame reads as faster than per-line reads over NFS. So
     this saves the CPU but not the second NAS pass.
   - It must give the same lines as the full stack. That can be checked by
     comparing the collected observations.
3. Write the export first and remux the header afterwards. This needs a
   second write of the output, but no second stack.

**Fixed with option 3**, the only one that keeps today's output exactly:

- Option 1 changes what the metadata is taken from (one disc's decode rather
  than the stack's) and needs a new input to the sink, so it is a design
  change for the script, not an optimisation.
- Option 2 is not cheap after all. The stack's VBI lines depend on decisions
  made over the whole frame: each input's level and gain matching, the
  field-agreement check, the noise weights and the still-frame copies. A
  line-only path would have to make those decisions too, which needs the FM
  fits it was meant to skip.

How it works now:

- When the output is a file, `VideoSinkStage::trigger` skips the pre-pass.
  Each export worker runs the biphase observer on its frame right after
  fetching it, so the three lines come from the input's cache. One observer
  handle is serialised behind a mutex, which costs nothing beside the decode.
- `FFmpegOutputBackend` (`Configuration::vbi_metadata_after_frames`) writes
  the frames to `<output>.partial`. At `finalize()` it remuxes every stream,
  unchanged and in the same order, into the output with the chapters and the
  disc-metadata attachment in the header, then deletes the partial file. If
  the remux fails, the partial file is kept and the error names it.
- A pipe or network URL cannot be remuxed, so it keeps the pre-pass.
- The remux copies the output once more on local disk: 326 MB per 1000
  frames here, ~18 GB for a full side, against a second stack of every frame.
  The partial file needs that much free space next to the output for the
  length of the export.
- A cancelled export is still remuxed, with the metadata of the frames
  written, as before.
- Closed captions have the same kind of pre-pass (NTSC). That one was left
  alone: captions are packets interleaved with the frames, not header data, so
  they cannot be added by the same remux without re-timing the subtitle
  stream.
- Tests: `SinkTruth.PAL_DiscMetadata` (CAV, no chapters) and
  `SinkTruth.NTSC_DiscMetadata` (with a chapter). Each compares the frames
  against a plain export, checks the attachment, the tags and the chapters,
  and checks that no partial file is left.

## Finding 3: the FM band-pass loop

[A microbenchmark](#reproducing) runs the constructor's two `band_pass_at`
loops over one PAL field (288 picture lines × 922 samples, plus the 24-sample
band-power margin). It compares the current code against a branch-free form:
the input padded with 127 zeros each side, and the loop over output samples
innermost, so each output's sum is still `taps[0]·x[c]` followed by
`taps[k]·(x[c−k]+x[c+k])` for k = 1…127 in the same order. Single thread,
quiet machine, best of 7:

| Flags | Current | Branch-free | Speed-up | Output |
|-------|---------|-------------|----------|--------|
| `-O2` (this profile) | 44 ms | 36 ms | ×1.2 | bit-identical |
| `-O3` (Release, CI, packages) | 44 ms | 24 ms | **×1.8** | bit-identical |
| `-O3 -march=native` | 52 ms | 14.7 ms | **×3.5** | bit-identical |

- It is bit-identical because the vectoriser works across outputs and never
  reorders one output's sum.
- The complex input is split into real and imaginary arrays (padded copies made
  once per field).
- `ChromaSplitter::demodulate` takes the same treatment. Pad the line instead
  of clamping, and the quadrature phase `(n + k) & 3` splits the taps into four
  fixed sets.
- The remaining constructor cost has smaller fixes:
  - `band_pass_taps(fs)` is recomputed for every field, with Bessel windows
    (0.8 % of FM). It is a constant per sample rate.
  - The 25-sample band power is summed afresh for every sample. A running sum
    gives the same value to within rounding (so not bit-identical).
  - `std::polar` per sample could step a phasor (also not bit-identical).

**Fixed** in `orc/sdk/src/fm_beat.cpp`. The field's IRE and the real and
imaginary beat are held with 127 zeros either side, and `band_pass()` loops
over the outputs innermost. Where the band-power margin runs past the field,
the end sample's value is repeated, as the clamped centre did. The taps are
computed once per sample rate. The 1000-frame output is bit-identical, frame
for frame (see [After the fixes](#after-the-fixes)).

Not done:

- `ChromaSplitter::demodulate` (~7 % of all cycles). Its phase pattern
  `(n + k) & 3` changes with the output sample, so the same trick needs the
  outputs split into four phases with their own tap sets. That is a bigger
  change in a header that Dropout Correction and Picture Band Limit use too,
  for about 3 % of the run.
- The running band-power sum and the stepped phasor: neither is bit-identical,
  and together they are a few % of FM.

## Finding 4: memory

### Where the heap goes

heaptrack, 11 inputs. Peak heap was 11.6 GB, with peak RSS 15.8 GB including
heaptrack's own overhead. Without heaptrack the peak RSS is 16.6 GB.

| Holder | Peak | Why |
|--------|------|-----|
| `FieldBeat` working buffers (vectors in the constructor and in `cancelled()`) | ~5.8 GB | ~20 field-length `double` / `complex<double>` vectors per fit (`ire`, `chroma`, `carrier`, `beat`, `luma`, `envelope`, each `box()` result, `basis_`, `band_`, `weight_`, `mask_`, `input_` …), 50–80 MB per fit, times the number of fits running at once |
| CVBS source frame caches | 2.4 GB | 150 frames × 11 inputs × 1.42 MB |
| FM cancelled-frame caches | 1.6 GB | 150 per input, only frames that were corrected |
| Stacker signatures | 0.66 GB | |
| Stacked frames, chroma gain, DOC | 0.6 GB | stacker's 300-frame cache, DOC's 150 |
| Dropout sidecars | 0.17 GB | each input's whole `.dropouts.meta` loaded at open |

- 205 M allocation calls (272 000/s), 46 M of them temporary.
- The biggest single holder is transient FM working memory, and it scales with
  concurrency, not with the stack. Two changes would cut it:
  - Hold fewer and narrower buffers per fit. Several are consumed once and
    could be fused into the loop that reads them, and `float` would do for the
    weights and mask.
  - Keep one set of buffers per thread (`thread_local`) instead of
    allocating ~20 vectors per field. This also removes most of the 4 % spent
    in `memset`.

**Partly fixed.** The constructor now frees each stage's temporaries when the
stage ends: the carrier and beat once they are split into the band-pass's
padded arrays, the luma and envelope once the edge measure is made, and the
analytic chroma after both. At the band-pass, which is the constructor's peak,
it holds the padded IRE, the padded beat and the edge measure, about 11 MB
over the members' 13 MB. Before, it held about 37 MB. Together with
findings 2 and 5, peak RSS on the cut went from 17.2 GB to 9.2 GB.

Per-thread buffers were not done:

- The stage threads are created per frame (finding 7), so a `thread_local`
  set would be allocated and freed as often as the vectors are now.
- It only pays once threads live longer, so it belongs with finding 7.

### glibc arenas and page faults

The process creates threads for every frame (`run_parallel` makes a fresh
`std::vector<std::thread>`). At least 48,327 distinct thread IDs appear in a
99 Hz sample of the run, and up to 244 exist at once. glibc gives threads
their own arenas (up to 8 × cores). Frame-sized buffers freed in one arena
are not reused by another, and large blocks fall back to `mmap`/`munmap`,
which costs a page fault per 4 KiB on reuse.

| Allocator | User cycles | System time | Page faults | Peak RSS |
|-----------|-------------|-------------|-------------|----------|
| glibc default | 30.1 T | 148 s | 34.8 M | 16.4 GB |
| `MALLOC_ARENA_MAX=4` | 31.3 T | 51 s | 5.9 M | 14.1 GB |
| mimalloc (`LD_PRELOAD`) | 30.3 T | 549 s | 158.8 M | 13.2 GB |

These runs overlapped other load, so compare cycles, not wall time.

- `MALLOC_ARENA_MAX=4` saves 2.3 GB and two thirds of the system time, but no
  user time: the faults were never the bottleneck.
- mimalloc decommits so eagerly that faults and system time get far worse.
- For scripted runs, set the variable in the script, or have orc-cli call
  `mallopt(M_ARENA_MAX, 4)` at start-up.

**Fixed:** orc-cli calls `mallopt(M_ARENA_MAX, 4)` at start-up on glibc,
unless `MALLOC_ARENA_MAX` is set, which still wins. The GUI is unchanged: it
holds fewer frames in flight, and it was not measured.

### Cache sizes in a streaming run

The caches are sized for interactive use (scrubbing back and forth in the
GUI): 150 frames per source, 150 per FM node, 150 in DOC, 300 in the stacker.
A one-way CLI pass only needs the window the stacker actually reads around the
current frame: still search 24 behind and ahead, signature look-ahead 8, sink
read-ahead up to 32.

- The source caches (2.4 GB) mainly serve frames FM did not change. FM stores
  an empty vector for those and reads the source again.
- Not measured here: shrinking the source cache would need a run to confirm it
  doesn't cost extra NAS reads.
- **Not done.** After the other fixes the run peaks at 9.2 GB on a 62 GB
  machine. A CLI-only cache size is a second set of numbers to keep right,
  for memory that is no longer short.

## Finding 6: serial pressing grouping

`probe_pressings` (`stacker_stage.cpp:1615`) calls `add_noise_probe` for up to
16 frames in a plain loop. Each probe pulls the frame from all 11 inputs, so
every probe is 11 FM frame fits on one thread.

- On the cut this phase took 28–32 s at 0.9 cores and 10 MB/s.
- On the Mac's full-side run the same step took 2 min 56 s, with its probes
  spread across 54 000 frames.
- The probe frames are independent. Fetching their planes with the existing
  `run_parallel` before the serial meter update would bring this to a few
  seconds. The still-frame reference noise step (20 s at 2.5 cores) is the next
  setup cost.

**Fixed:** `probe_pressings` first fetches the first 16 candidate frames from
every input on all threads. The serial loop is unchanged, so it reads them
from the inputs' caches and adds them in the same order. The grouping is the
same, and the step took 9 s instead of 35 s on the cut. The reference-noise
step went from 22 s to 17 s through the faster FM.

Loading the grouping from the project's sidecar in orc-cli, as the GUI does,
was not done. It now saves seconds, not minutes.

## Finding 7: threads

- **Read-ahead and stage pools are nested.** The sink's `FrameReadAhead` runs
  `hardware_concurrency()` workers. Each worker's frame then runs the
  stacker's `run_parallel` pools, which spawn another `hardware_concurrency()`
  threads per call.
- **Oversubscribed.** During the VBI pass the sampler saw 78 runnable threads
  on average (up to 244 in total) for 16 hardware threads.
- **The cost is memory, not lock time.** User-space lock and futex time does
  not register in the profile, and thread creation is mostly kernel time
  (inside the 148 s of system time). The real cost is the number of FM
  working sets alive at once (finding 4).
- **Fix:** one process-wide pool (or a CLI `--threads` cap that every stage
  honours) would bound that without losing throughput, since the run already
  keeps ~13 of 16 threads busy.

**Not done.**

- It is a change to how every stage runs its work: the stacker, Source Align,
  DOC, FM and the sink's read-ahead each create their own threads today. A
  shared pool that does not deadlock when a pooled task waits on another
  stage's pooled work needs work-stealing or a nested-wait rule.
- What it would save is memory and system time, and findings 4 and 5 have
  already halved both.
- It would also be the place for finding 4's per-thread buffers. Worth doing
  only if memory becomes short again, for example on an 8 GB machine.

## I/O

- **Per pass:** 16.0–16.1 GB from the NAS per 1000 output frames, i.e.
  1.45 MB per input frame (the picture plus its audio and EFM). Two passes
  mean twice that unless the page cache holds a pass (finding 2).
- **Today the run is CPU-bound.** During the stacking passes `rchar` ran at
  50–60 MB/s, against the 100–145 MB/s the share delivered for an 11-file
  interleaved read in the 2026-10-27 measurement. The VBI pass had about 12 threads in uninterruptible
  disk wait on average; the export almost none.
- **After the CPU fixes, still not I/O-bound.** The fixed run read 17.4 GB in
  311 s, ≈ 56 MB/s, still below the share. I/O only becomes the limit at
  about twice that speed.
- **The floor.** For a full side, one pass is ~790 GB, or 1.5–2.2 hours at the
  share's sustained rate. A second pass doubles that.

## After the fixes

Measured 2026-10-31, same cut, same 11-input project, page cache evicted
before each run, the two runs back to back. Both builds were `-O2 -g` with
frame pointers: the reference is e669e741 (with finding 1's fix), and the
second is the same plus findings 2–6.

| | Before | After | Change |
|---|---|---|---|
| Wall | 597.7 s | 311.4 s | **−48 %** |
| User cycles | 33.6 T | 18.2 T | −46 % |
| Instructions | 67.3 T | 31.6 T | −53 % |
| CPU | 7853 s | 4207 s | −46 % |
| System time | 169.5 s | 38.7 s | −77 % |
| Page faults | 32.6 M | 3.5 M | −89 % |
| NAS bytes read | 33.0 GB | 17.4 GB | −47 % |
| Peak RSS | 17.2 GB | 9.2 GB | −47 % |
| Pressing grouping | 35 s | 9 s | |

**The output is unchanged:**

- `ffmpeg -f framemd5` over the decoded video: all 1000 frames identical.
- Every video and audio packet: identical.
- The disc-metadata attachment: identical, apart from the build's version
  string.
- The container tags: identical, apart from their order.
- The pressing grouping and every logged σ: identical.

The FM band-pass gain is smaller here than the microbenchmark's ×1.8, which
was at `-O3`. At `-O2` it was ×1.2 per loop. Release, CI and package builds
are `-O3`, so they gain more than the measured runs did.

### Projection

Full side, 54 000 frames, this machine. The estimate assumes the cost per
frame stays as it is on the cut. The cut includes still-frame stretches, and
the transform-2D middle of the disc may differ.

| | Per 1000 frames | Full side (est.) |
|---|---|---|
| Before (measured, 11 inputs) | 598 s | ~9 h |
| After (measured, `-O2`) | 311 s | ~4.7 h |

On a full side the NAS reads of one pass are ~790 GB, which takes 1.5–2.2 h at
the share's rate, so the run is still CPU-bound at 311 s per 1000 frames
(about 56 MB/s).

## Options specific to scripted (orc-cli) runs

| Option | Status | Effect |
|--------|--------|--------|
| `MALLOC_ARENA_MAX=4` | **done** (orc-cli sets it; the variable still overrides) | −2.3 GB peak RSS, −83 % page faults, −66 % system time, no speed change |
| Build orc-cli with `-DORC_NATIVE_ARCH=ON` on the machine that runs the script | measured | −7.6 % user cycles, −13 % instructions today; the FM band-pass gains ×3.5 instead of ×1.8 once branch-free. Output is not bit-identical to CI or package builds (FMA contraction), so record it in the pack's provenance |
| `-DORC_ENABLE_LTO=ON` | not measured | Cross-library inlining; the hot loops are inside single files, so a small gain is likely |
| No VBI pre-pass when writing a file | **done** (finding 2) | −48 % wall with findings 3–6, −47 % NAS reads |
| Spell Frame Map ranges apart | only for builds before finding 1's fix | Restores DS1 to the stack |
| A CLI-wide thread cap / smaller frame caches for one-way passes | not done (findings 4, 7) | Memory: bounds the concurrent FM working sets and the 2.4 GB of source cache |

The pressing grouping is already kept in the observation store for the GUI,
but orc-cli measures it on every run. A CLI run could load it from the
project's sidecar when one exists. Now that the probing is parallel
(finding 6), that would save seconds, so it was not done.

## Reproducing

- **Cut:** copy the project next to links to the `DSn` folders (the project
  uses `../../DSn/...`).
  - Replace each Frame Map's `ranges` with a distinct spelling of the same
    window, e.g. `2-50k,50k+1-1001` with a different `k` per map.
  - Set `output_path` to local disk.
- **Before each run:** `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)` on every
  file under the `DSn` folders.
- **Profile:**
  `perf record -e cycles:u -F 99 -g --user-callchains -o perf.data orc-cli stack.orcprj --process`.
  Then `perf annotate -l -s 'orc::fm_beat::FieldBeat::FieldBeat(...)'` for the
  hot lines.
- **Counts:** `perf stat -e task-clock,page-faults,cycles,instructions`.
- **Heap:** `heaptrack --record-only -o heap orc-cli …`, then
  `heaptrack_print -f heap.zst --print-peaks 1`.
- **Band-pass microbenchmark:**
  - Lift `band_pass_at` into a file as is. Add the padded, output-innermost
    form beside it.
  - Run both over one field's worth of random data (`ire` length 313 × 1135,
    288 lines × 922 outputs, plus the 24-sample margin for the real signal).
  - `memcmp` the results.
  - Build with `g++ -std=c++20 -O3` (and `-march=native`).
