# Chroma decoder temporal denoising

Noise reduction for the Video Sink's decoded output, added where the signal is
finally in component form: after Y/C separation and demodulation, before the
component frame is handed to the output backend.

## Why here, and why temporal

A noise gate on the composite signal was tried first (branch
`20261023-Noise-Gate`, removed). It only worked on entirely blank frames. The
reasons are structural, and they decide the design below:

- A gate makes a yes/no decision per sample. Any threshold wide enough to catch
  the noise also catches deep shadows and dark gradients, and the boundary
  between a cleaned area and an untouched one is more visible than the noise.
- In the composite domain every sample carries subcarrier, so any nonlinear
  operation corrupts chroma. There is no clean luma until the chroma decoder
  has run.
- Capture noise is random from frame to frame and picture content mostly is
  not. A motion-adaptive temporal average attenuates noise by averaging
  without ever classifying what the picture is, which is the property the gate
  lacked. On still picture it also averages out residual dot crawl, because the
  residual subcarrier in Y alternates phase between consecutive NTSC frames.

The existing `luma_nr` / `chroma_nr` controls are one-dimensional horizontal
coring (`Comb::FrameBuffer::doYNR` at
[comb.cpp:1234](../orc/plugins/stages/sinks/common/decoders/comb.cpp#L1234),
the same filter in `PalColour`, `MonoDecoder` and `SecamDecoder`). They remove
high-frequency speckle only and soften fine detail as they are raised. They
stay as they are; the temporal filter is a separate control that works on
every decoder path, including `mono`, `secam` and Y/C sources.

The Video Sink's `video_filter` parameter can already run libavfilter
denoisers (`atadenoise`, `hqdn3d`, `nlmeans`) in FFmpeg mode. The in-decoder
filter is still worth having because it runs in the decoder's double-precision
domain before 16-bit conversion and padding, works in raw output mode and in
the GUI preview, and can use the decoder's knowledge of the active picture
area and of the subcarrier period for its motion measurement.

## Where the frames flow

The decode engine in
[video_sink_stage.cpp](../orc/plugins/stages/sinks/common/video_sink_stage.cpp)
decodes every frame independently: each worker owns a decoder from
`make_decoder()`
([video_sink_stage.cpp:105](../orc/plugins/stages/sinks/common/video_sink_stage.cpp#L105)),
builds its own look-behind / look-ahead window of `SourceField`s, and decodes
one frame. Finished frames are written to the backend strictly in order from
under an output mutex:

- batch export: the drain loop at
  [video_sink_stage.cpp:2343-2360](../orc/plugins/stages/sinks/common/video_sink_stage.cpp#L2343-L2360);
- streaming export: the drain loop at
  [video_sink_stage.cpp:2635-2651](../orc/plugins/stages/sinks/common/video_sink_stage.cpp#L2635-L2651);
- preview: one frame decoded on demand at
  [video_sink_stage.cpp:3231](../orc/plugins/stages/sinks/common/video_sink_stage.cpp#L3231).

A recursive temporal filter needs its own previous output, so it cannot live
inside the per-worker decoders. The ordered drain loop is the one place every
frame passes in sequence, and it is where the filter runs for export. Decoding
the temporal neighbours inside each worker instead (a non-recursive window
using the engine's look-behind) would cost one full extra decode per
neighbour per frame, and is rejected for that reason.

`ComponentFrame`
([componentframe.h](../orc/plugins/stages/sinks/common/decoders/componentframe.h))
holds Y, U and V as doubles in composite units (10-bit CVBS codes), one
frame-flat line per row, so the filter needs no knowledge of the decoder that
produced it. Adjacent rows belong to opposite fields; vertical neighbours
within a field are two rows apart.

An in-tree precedent for the motion measurement is the SECAM decoder's
temporal chroma blend
([secamdecoder.cpp:816-872](../orc/plugins/stages/sinks/common/decoders/secamdecoder.cpp#L816-L872)):
a windowed mean of |luma now − luma then| drives a weight between the previous
frame's value and the spatial estimate.

## The filter

Per output frame, per active sample, with P the previous *filtered* frame:

```
for each plane Z in {Y, U, V}:
  d_Z(x,y)  = |box mean of (Z − P.Z) over 4 samples × 3 same-field lines|
  sigma_Z   = 30th percentile of d_Z over the active picture (histogram),
              divided by 0.385 (the half-normal percentile constant)
m(x,y)      = max over planes of d_Z(x,y) / sigma_Z
t_still     = k1 · motion                    (motion = temporal_nr_motion)
t_moving    = k2 · motion
w(x,y)      = alpha · (1 − smoothstep(t_still, t_moving, m(x,y)))
out         = in + w · (P − in)              (all three planes, same w)
P           = out
```

- The box averages the *signed* frame difference before the magnitude is
  taken. The 4-sample box is one subcarrier cycle at 4fsc, so residual
  subcarrier in Y cancels and does not register as motion. Three same-field
  lines are rows y−2, y, y+2. At the edges of the active area the window
  slides inward rather than shrinking, so the cancellation holds everywhere.
- On still noisy picture the box-mean difference is close to a zero-mean
  normal, so dividing its 30th percentile magnitude by 0.385 recovers that
  normal's scale. Motion is then dimensionless, in units of each plane's own
  noise, and k1 and k2 mean the same thing in mono and colour: at k1 = 2.5
  about one still sample in a hundred per plane loses blend weight to noise
  alone. Taking the maximum over planes makes a colour change without a luma
  change (a colour wipe, a chroma-only fade) count as motion, while a noisier
  chroma plane does not swamp the luma measurement.
- `alpha` is the steady-state blend, derived from the user's `temporal_nr`
  in dB of still-picture noise reduction: `r = 10^(−dB/10)`,
  `alpha = (1 − r) / (1 + r)`. 6 dB gives alpha 0.60 (about four frames'
  worth of averaging); 12 dB gives 0.88. The range is capped at 12 dB so a
  missed motion can trail for at most a few frames.
- `sigma_Z` is measured on `d_Z` itself, which already includes the lower
  noise of the filtered previous frame, so the thresholds are
  self-consistent. A 256-bin histogram with 0.25-code bins keeps this at one
  pass per plane. Clamp each sigma to [0.25, 8] codes. Start with k1 = 2.5
  and k2 = 5.
- Scene-cut guard: if more than half the active samples have `m > t_moving`,
  the frame is passed through untouched and becomes the new P. This covers
  cuts, `frame_map` padding frames, and the first frame.
- A frame whose geometry differs from P (width, height) resets the filter.

The filter writes in place into the `ComponentFrame` it is given, so the
output backends and the preview carrier need no change.

## Parameters

Both apply to every `decoder_type`, in raw and FFmpeg output modes, and are
listed after `chroma_nr` in the descriptor order.

| Name | Type | Range | Default | Meaning |
|---|---|---|---|---|
| `temporal_nr` | double | 0.0–12.0 | 0.0 | Noise reduction on still picture, in dB. 0 disables the filter and leaves output bit-identical to today. |
| `temporal_nr_motion` | double | 0.5–4.0 | 1.0 | Multiplier on the motion thresholds. Above 1 treats more change as still (more smoothing, more risk of trails); below 1 is the reverse. |

Diagnostics stay in logs: at the end of an export, log the mean measured sigma
and the fraction of frames the cut guard passed through.

## Preview

The preview decodes single frames at random. To show the same result as the
export it runs the same filter with its own state:

- The stage keeps one `TemporalDenoiser` for preview beside
  `preview_decoder_cache_`, together with the frame index its state belongs to.
- A request for index `last + 1` continues the recursion with one decode. This
  is the playback case.
- Any other index warms up: decode frames `index − kPreviewWarmupFrames`
  through `index` in order through a fresh filter, each with the decoder's own
  look-behind / look-ahead window. With `kPreviewWarmupFrames = 4` and
  alpha ≤ 0.88 the preview carries at least 60 % of the export's steady-state
  history; the remainder is a documented approximation.
- A change to `temporal_nr`, `temporal_nr_motion`, the decoder type or any
  decoder parameter discards the preview filter state.

The warm-up multiplies preview decode cost by up to five on a random seek. The
preview renderer already requests colour frames one at a time and tolerates
the 3D decoders' three-frame windows; the measurement task in Phase 3 decides
whether `kPreviewWarmupFrames` needs lowering for `transform3d`.

## Phase 1: temporal denoiser core

New files `temporal_denoiser.h` / `temporal_denoiser.cpp` in
[decoders/](../orc/plugins/stages/sinks/common/decoders/), added to
`DECODER_SOURCES` in that directory's `CMakeLists.txt`. No stage changes yet.

1. **Class and settings.** `TemporalDenoiser` with a `Settings` struct
   (`strength_db`, `motion_scale`, active picture rectangle in frame-flat
   coordinates), `reset()`, and `void filter(ComponentFrame& frame)` that
   filters in place and keeps the result as the next previous frame. A
   `Stats` accessor exposes the last frame's per-plane sigma and whether the
   cut guard fired. Pure class, no logging in the hot path.
   - Acceptance: `strength_db == 0` returns without touching the frame or the
     state; a frame of different size resets and passes through.
2. **Motion measure and noise estimate.** The per-plane 4 × 3 same-field box
   mean of the signed difference (separable running sums, no per-sample
   division), the histogram percentile with the sigma clamp, and the
   noise-normalised maximum over planes.
   - Acceptance: on synthetic frames of independent Gaussian noise the
     estimated sigma is within 20 % of the analytic value for the box; a
     ±pattern repeating every 4 samples (residual subcarrier) produces
     `d == 0`.
3. **Blend and cut guard.** The smoothstep weight, the per-plane in-place
   blend, and the pass-through on cuts.
   - Acceptance: on a static noisy sequence the output noise variance
     converges to within 10 % of `(1 − alpha) / (1 + alpha)` of the input
     variance; a frame whose samples all change by more than `t_moving` is
     output unchanged and becomes the new previous frame.
4. **Unit tests** in
   [orc-tests/core/unit/stages/video_sink/](../orc-tests/core/unit/stages/video_sink/)
   as `temporal_denoiser_test.cpp`, built like `comb_test.cpp` (small
   `SourceParameters`, in-memory frames, no filesystem). Cover the acceptance
   criteria above plus: a moving step edge is output within one code of its
   input on both sides of the edge (no trail); U and V receive the same weight
   as Y; `reset()` makes the next frame pass through; a sequence of still
   frames then a cut then still frames recovers.
   - Acceptance: the file is in `orc-tests/core/unit/CMakeLists.txt`, all new
     tests pass, `ClangFormatCheck` passes.

## Phase 2: export integration

1. **Parameters.** `temporal_nr` and `temporal_nr_motion` on `VideoSinkStage`:
   members, descriptors (after `chroma_nr`, as in the table above),
   `get_parameters()`, `set_parameters()` with range validation, and the
   instance of `TemporalDenoiser::Settings` built next to `DecoderParams` in
   `run_export_trigger()`.
   - Acceptance: `video_sink_stage_test.cpp` gains tests that the descriptors
     exist with the table's ranges and defaults, that set/get round-trips, and
     that out-of-range values are rejected.
2. **Batch export hook.** One `TemporalDenoiser` owned by
   `run_export_trigger()`, called on each frame inside the ordered drain loop
   immediately before `backend->writeFrame()`. Pass the active rectangle from
   `videoParams` (`active_video_start/end`,
   `first/last_active_frame_line`), respecting the same
   `active_area_cropping_applied` offsets `doYNR` uses.
   - Acceptance: with `temporal_nr == 0` the raw export is byte-identical to
     the export before this change (compare a short raw export of an existing
     functional fixture).
3. **Streaming export hook.** The same call in `run_streaming_export()`'s drain
   loop. The filter state must belong to the export, not to a worker.
   - Acceptance: `video_sink_streaming_test.cpp` gains a case with
     `temporal_nr = 6` that still writes exactly `real_frame_count` frames in
     order and ends cleanly.
4. **Throughput.** The filter runs serially under the output mutex. Measure
   export frames per second on a 300-frame section with `ntsc2d` and `pal2d`
   (the fastest decoders, hence the worst case) at the default thread count,
   with and without the filter.
   - Acceptance: throughput loss under 10 %. If it is more, split
     `filter()` across line bands with the stage's existing worker count and
     re-measure; the band split changes no output because the box and the
     blend are both local.
5. **Logging.** Debug log of the filter settings at export start; info log of
   mean sigma and cut-guard count at export end.
   - Acceptance: present in the trigger log at the stated levels, nothing
     logged per frame.

## Phase 3: preview

1. **Preview filter state.** `mutable` preview `TemporalDenoiser` plus the
   index of the frame its state follows, held next to
   `preview_decoder_cache_` and guarded by the same access pattern.
   Invalidate it whenever `matches_config()` fails or either new parameter
   changes.
   - Acceptance: a unit test with an in-memory `VideoFrameRepresentation`
     (like the functional test's `FakeStreamingVfr`, but without any output
     path) shows that two consecutive requests for the same index return the
     same planes, and that changing `temporal_nr` changes them.
2. **Continuation and warm-up.** In `get_colour_preview_carrier()`, after the
   existing single-frame decode: if the request is `last + 1`, filter the
   decoded frame and advance; otherwise decode `kPreviewWarmupFrames` earlier
   frames in order through a reset filter first. Frames before the start of
   the range are skipped, not blanked, so a seek to frame 0 behaves like the
   export's first frame.
   - Acceptance: a unit test shows that sequential requests `n, n+1, n+2` give
     the same planes for `n+2` as a cold request for `n+2` when the source is
     a static noisy sequence longer than the warm-up (within the cold-start
     tolerance derived from alpha), and that a cold request for frame 0 equals
     the unfiltered decode.
3. **Preview cost.** Measure the time of a random-seek preview with
   `transform3d` and `ntsc3d`, filter on and off.
   - Acceptance: a seek stays under one second on the development machine;
     otherwise lower `kPreviewWarmupFrames` for decoders with look-around and
     record the chosen value in the header comment.

## Phase 4: validation and documentation

1. **Real-material check.** Export short sections from the captures listed in
   the project memory (Grosse Pointe Blank side 1: the Hollywood Pictures
   logo around frame 450, the title card around 1100, and the dark credit
   shot around 2600) with `temporal_nr` at 0, 6 and 9. Measure luma sigma in a
   static black region and inspect the moving edges and the face in the credit
   shot.
   - Acceptance: at 6 dB the static black region's sigma falls by at least
     4 dB; no visible trail on moving edges; no change to the shadow detail on
     the face. Numbers go in the PR description, not in this file.
2. **Stage help.** `temporal_nr` and `temporal_nr_motion` sections in
   [instructions.md](../orc/plugins/stages/sinks/video_sink/instructions.md)
   after `chroma_nr`, plus a Notes bullet that the preview approximates the
   export's history after a seek.
   - Acceptance: the sections follow the file's `### name (type)` pattern and
     state the default and range.
3. **User guide.** Rows in the shared-controls table of
   [chroma-decoder-guide.md](../docs/gui-user-guide/stages/chroma-decoder-guide.md#L180),
   a step in "A repeatable comparison" placing the temporal control before
   the coring controls (it costs no detail on still picture, so it should be
   tried first), and the parameter list in
   [sink-core-stages.md](../docs/gui-user-guide/stages/sink-core-stages.md#L634).
   - Acceptance: both pages describe when a trail can appear and which
     control reduces it.
4. **Decoder guide limitations.** State that the filter is recursive, so a
   missed low-contrast motion can trail for a few frames; that it cannot
   reduce noise on a moving area; and that `video_filter` denoisers remain
   available for spatial denoising in FFmpeg mode.
   - Acceptance: text present; no duplication of the parameter table.

## Phase 5: chroma coring for PAL and SECAM

Independent of the phases above. `chroma_nr` is NTSC-only because only the
comb has `doCNR`
([comb.cpp:1174](../orc/plugins/stages/sinks/common/decoders/comb.cpp#L1174)).
PAL and SECAM U/V get the same horizontal coring.

1. **PalColour.** `cNRLevel` in `PalColour::Configuration`, applied to U and V
   after demodulation with the comb's `f_nrc` filter and clip, and set from
   `DecoderParams::chromaNr` in `make_decoder()`.
   - Acceptance: `palcolour_test.cpp` shows a sub-threshold high-frequency
     U/V ripple is removed and a large step is preserved; `cNRLevel == 0` is
     bit-identical to today.
2. **SecamDecoder.** The same on its U/V output after the temporal chroma
   blend.
   - Acceptance: equivalent test in `secamdecoder_test.cpp`.
3. **Descriptors and docs.** Drop "(NTSC only)" from the `chroma_nr`
   description and the three documentation pages.
   - Acceptance: `video_sink_stage_test.cpp` checks the description text no
     longer claims NTSC only.
