# Chroma decoder noise reduction

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

The `luma_nr` / `chroma_nr` controls of earlier releases were one-dimensional
horizontal coring inside the decoders (`doYNR` / `doCNR` in the comb,
`PalColour`, `MonoDecoder` and `SecamDecoder`), with thresholds in units
unrelated to the actual noise. They removed high-frequency speckle only and
softened fine detail as they were raised. The filter below replaces them: its
spatial half is calibrated by the measured noise and runs on every decoder
path, including `mono`, `secam` and Y/C sources, so the coring code and its
controls are gone. The two parameter names are still accepted when a project
is loaded, and ignored, so older projects open.

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
inside the per-worker decoders. The ordered drain is the one place every
frame passes in sequence, and it feeds the filter for export: the workers
deposit decoded frames as before, a filter thread takes them in output order
and filters each with the mutex released, and a writer thread takes the
filtered frames and writes them. Filtering therefore overlaps decoding and
writing; done inside the drain itself the filter and the write would
serialise on the output mutex and add up per frame. A worker that has run
more than a bound ahead of the writer waits, so the decoders cannot buffer
frames without limit. Decoding the temporal neighbours inside each worker
instead (a non-recursive window using the engine's look-behind) would cost
one full extra decode per neighbour per frame, and is rejected for that
reason.

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
pre-pass over every 8th active row, every 2nd sample (no writes):
  for each plane Z in {Y, U, V}:
    d_Z(x,y)   = |box mean of (Z − P.Z) over 4 samples × 3 same-field lines|
    sigma_Z    = 30th percentile of d_Z, divided by 0.385 (the half-normal
                 percentile constant), clamped to [0.25, 8] codes
    delta_Z    = 10th percentile of |Z(x,y) − Z(x+2,y)| and of
                 |Z(x,y) − Z(x,y+2)| (the larger), divided by 0.1257
  a(x,y)       = |box mean of (Y − Y shifted 8 samples)|, same box
  anchor       = 30th percentile of a
  moving       = fraction of samples with d_Z / sigma_Z > t_moving, any plane

discontinuity if there is no P, or sigma_Y reached its clamp, or
  30th percentile of d_Y > 2 · max(anchor, 1 code), or moving > 0.5

main pass:
  m(x,y)     = discontinuity ? 1
             : smoothstep(t_still, t_moving, max over planes of d_Z / sigma_Z)
  t_still    = k1 · motion                     (motion = noise_reduction_motion)
  t_moving   = k2 · motion
  s_Z(x,y)   = range-weighted mean of Z over 5 samples × 3 same-field lines,
               weight 1 − (|Z(i) − Z(x,y)| / (1.8 · delta_Z))² clipped at 0
  out        = in + alpha · (1 − m) · (P − in) + m · (in + g · (s − in) − in)
  P          = out
```

- The pre-pass decides whether the frame continues the previous one before
  anything is blended, from statistics the picture change cannot corrupt.
  The temporal noise scale alone cannot tell: a run of unrelated stills (a
  disc of slides, maps or photographs, where every frame is a different
  picture) reads as a noisy continuation, its 30th percentile climbs to the
  clamp, and from then on most of the previous picture blends into the next
  one (measured on Domesday map stills before this pass existed: noise scale
  pinned at 8.00 on all planes, the old cut guard firing on 3 of 60 frames).
  So three rules, any of which marks a discontinuity: the temporal scale is
  at its clamp (no decoder output is that noisy); the temporal percentile
  exceeds a within-frame anchor by a ratio (the picture changed by more than
  noise over most of its area); more than half the picture is moving (a
  pan, which averaging over time cannot help either). The anchor is the
  same box operator applied between the frame and itself shifted by two
  subcarrier cycles: on noise it has the same distribution as the frame
  difference however the decoder has filtered the noise (a within-frame
  second difference does not: the NTSC comb's vertical filtering shrinks it
  to half the temporal scale), and only a change of picture inflates the
  frame difference. Measured margins on the sections in the project memory:
  Grosse Pointe Blank continuous frames reach a ratio of 1.37 and a moving
  fraction of 0.27 at most; the Domesday map stills are all caught by the
  clamp rule (ratio 1.2–2.4, since map detail inflates the anchor too).
- A discontinuity is filtered spatially only and becomes the new P; its
  temporal measurement is never used. The first frame of a sequence and a
  frame whose geometry differs from P are discontinuities too, so no frame
  is ever left at the input noise level.
- The spatial filter takes over wherever the temporal one withdraws: fully
  on a discontinuity, by the motion measure m elsewhere, so a moving object
  is smoothed within the frame while the still background keeps the better
  temporal result. Its range follows the measured scale of the differences
  between a sample and its window neighbours, read at the window's own lags
  so the decoder's residual noise correlation is accounted for, at a low
  percentile because picture detail inflates the differences wherever the
  picture is not flat. Edges and detail above 1.8 of that scale are left
  alone. The window mean is pulled towards the noisy centre sample by its
  own weights, so on flat white noise its power is v = 0.254 of the input and
  its covariance with the centre sample k = 0.458 (measured); the share g
  solves (1 − g)² + v g² + 2 k g (1 − g) = 10^(−dB/10), which the window
  reaches up to just under 6 dB and saturates beyond. The cost is a
  whole-frame spatial pass only on discontinuities (10 ms single-threaded on
  an NTSC frame, vectorised one tap offset at a time); rows with few moving
  samples are filtered sample by sample.
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
- `alpha` is the steady-state blend, derived from the user's `noise_reduction`
  in dB of still-picture noise reduction: `r = 10^(−dB/10)`,
  `alpha = (1 − r) / (1 + r)`. 6 dB gives alpha 0.60 (about four frames'
  worth of averaging); 12 dB gives 0.88. The range is capped at 12 dB so a
  missed motion can trail for at most a few frames.
- `sigma_Z` is measured on `d_Z` itself, which already includes the lower
  noise of the filtered previous frame, so the thresholds are
  self-consistent. 256-bin histograms with 0.25-code bins over the pre-pass
  subsample keep the percentiles cheap, and the scales measured on a frame
  are the ones applied to it. Start with k1 = 2.5 and k2 = 5.
- P is kept in single precision, far finer than the output's 16-bit
  quantisation. The filter's cost is bound by memory traffic, not
  arithmetic, and this halves the traffic for P.

The filter writes in place into the `ComponentFrame` it is given, so the
output backends and the preview carrier need no change.


## Parameters

Both apply to every `decoder_type`, in raw and FFmpeg output modes, and are
listed after `chroma_phase` in the descriptor order, where `luma_nr` and
`chroma_nr` used to be.

| Name | Type | Range | Default | Meaning |
|---|---|---|---|---|
| `noise_reduction` | double | 0.0–12.0 | 0.0 | Noise reduction on still picture, in dB. 0 disables the filter and leaves output bit-identical to today. |
| `noise_reduction_motion` | double | 0.5–4.0 | 1.0 | Multiplier on the motion thresholds. Above 1 treats more change as still (more smoothing, more risk of trails); below 1 is the reverse. |

Diagnostics stay in logs: at the end of an export, the mean measured scales
and the number of frames filtered spatially only at info level; per frame,
the decision with the ratio, moving fraction and scales behind it at debug
level, which is how the thresholds above were set and how a report on new
material can be read.

## Preview

The preview decodes single frames at random. To show the same result as the
export it runs the same filter with its own state:

- The stage keeps one `NoiseReducer` for preview beside
  `preview_decoder_cache_`, together with the last frame it returned and that
  frame's index. A request for that index again is answered from the kept
  frame without a decode; the scope payloads ask for the frame the renderer
  has just shown, and a recursive filter cannot run the same frame twice.
- A request for index `last + 1` continues the recursion with one decode. This
  is the playback case.
- Any other index warms up: decode frames `index − kPreviewWarmupFrames`
  through `index` in one call (one look-behind / look-ahead window around the
  run, as the export's workers decode their chunks) and run them in order
  through a reset filter. Frames before the start of the range are skipped,
  so frame 0 is filtered within itself, as in an export. With
  `kPreviewWarmupFrames = 4` and alpha ≤ 0.88 the preview carries at least
  60 % of the export's steady-state history; the remainder is a documented
  approximation.
- A change to `noise_reduction`, `noise_reduction_motion`, the active area, the
  decoder type or any decoder parameter discards the preview filter state,
  as does a new input.
- The preview decode is serialised by a mutex of its own. The decoder cache
  was already shared by every caller of `get_colour_preview_carrier()` (the
  frame render and the scope payloads); the filter state makes that sharing
  stateful, so the stage no longer relies on the callers to take turns.

The warm-up multiplies preview decode cost by up to five on a random seek. The
preview renderer already requests colour frames one at a time and tolerates
the 3D decoders' three-frame windows; the measurement task in Phase 3 decides
whether `kPreviewWarmupFrames` needs lowering for `transform3d`.

## Phase 1: noise reducer core

New files `noise_reducer.h` / `noise_reducer.cpp` in
[decoders/](../orc/plugins/stages/sinks/common/decoders/), added to
`DECODER_SOURCES` in that directory's `CMakeLists.txt`. No stage changes yet.

1. **Class and settings.** `NoiseReducer` with a `Settings` struct
   (`strength_db`, `motion_scale`, active picture rectangle in frame-flat
   coordinates), `reset()`, and `void filter(ComponentFrame& frame)` that
   filters in place and keeps the result as the next previous frame. A
   `Stats` accessor exposes the last frame's per-plane scales and whether it
   was a discontinuity. Pure class, no logging in the hot path.
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
3. **Blend.** The smoothstep weight and the per-plane in-place blend.
   - Acceptance: on a static noisy sequence the output noise variance
     converges to within 10 % of `(1 − alpha) / (1 + alpha)` of the input
     variance; a frame whose samples all change by more than `t_moving`
     takes nothing from the previous frame and becomes the new previous
     frame.
4. **Unit tests** in
   [orc-tests/core/unit/stages/video_sink/](../orc-tests/core/unit/stages/video_sink/)
   as `noise_reducer_test.cpp`, built like `comb_test.cpp` (small
   `SourceParameters`, in-memory frames, no filesystem). Cover the acceptance
   criteria above plus: a moving step edge is output within one code of its
   input on both sides of the edge (no trail); U and V receive the same weight
   as Y; `reset()` makes the next frame pass through; a sequence of still
   frames then a cut then still frames recovers.
   - Acceptance: the file is in `orc-tests/core/unit/CMakeLists.txt`, all new
     tests pass, `ClangFormatCheck` passes.

## Phase 2: export integration

1. **Parameters.** `noise_reduction` and `noise_reduction_motion` on `VideoSinkStage`:
   members, descriptors (after `chroma_phase`, as in the table above),
   `get_parameters()`, `set_parameters()` with range validation, and the
   instance of `NoiseReducer::Settings` built next to `DecoderParams` in
   `run_export_trigger()`.
   - Acceptance: `video_sink_stage_test.cpp` gains tests that the descriptors
     exist with the table's ranges and defaults, that set/get round-trips, and
     that out-of-range values are rejected.
2. **Batch export hook.** One `NoiseReducer` owned by
   `run_export_trigger()`, fed by the ordered drain: a filter thread takes
   the decoded frames in output order, and a writer thread writes the
   filtered ones (see "Where the filter runs"). With `noise_reduction == 0` the
   workers write straight from the drain as before. Pass the active
   rectangle from `videoParams` (`active_video_start/end`,
   `first/last_active_frame_line`), respecting the same
   `active_area_cropping_applied` offsets `doYNR` uses.
   - Acceptance: with `noise_reduction == 0` the raw export is byte-identical to
     the export before this change (compare a short raw export of an existing
     functional fixture).
3. **Streaming export hook.** The same arrangement around
   `run_streaming_export()`'s drain. The filter state must belong to the
   export, not to a worker.
   - Acceptance: `video_sink_streaming_test.cpp` gains a case with
     `noise_reduction = 6` that still writes exactly `real_frame_count` frames in
     order and ends cleanly.
4. **Throughput.** The filter is one frame deep and runs on its own thread
   between the drain and the writer, splitting each frame into line bands
   (one band per eight decode workers, at most two: the decoders already
   take most of the memory bandwidth, and more bands gain nothing). Measure
   export frames per second on a 300-frame section with `ntsc2d` and `pal2d`
   (the fastest decoders, hence the worst case) at the default thread count,
   with and without the filter, in a Release build.
   - Acceptance: no throughput loss with the filter off. With it on, a loss
     of about a fifth on continuous material is accepted, because the filter
     is optional and off by default; material where every frame takes the
     spatial filter costs more. The band split changes no
     output because every row is measured by the same arithmetic whichever
     band holds it.
5. **Logging.** Debug log of the filter settings at export start; info log of
   the mean scales and the count of spatially filtered frames at export end;
   debug log of each frame's decision (see "Parameters").
   - Acceptance: present in the trigger log at the stated levels, nothing
     per frame above debug.

## Phase 3: preview

1. **Preview filter state.** `mutable` preview `NoiseReducer` plus the
   last frame returned and its index, held next to `preview_decoder_cache_`
   under a preview mutex. Invalidate it whenever `matches_config()` fails,
   either new parameter or the active area changes, or the input is replaced.
   A repeat request for the kept index returns the kept frame.
   - Acceptance: a unit test with an in-memory `VideoFrameRepresentation`
     (like the functional test's `FakeStreamingVfr`, but without any output
     path) shows that two consecutive requests for the same index return the
     same planes, and that changing `noise_reduction` changes them.
2. **Continuation and warm-up.** In `get_colour_preview_carrier()`: if the
   request is `last + 1`, decode and filter that frame alone and advance;
   otherwise decode frames `index − kPreviewWarmupFrames` through `index` in
   one call and run them in order through a reset filter. Frames before the
   start of the range are skipped, not blanked, so a seek to frame 0 behaves
   like the export's first frame.
   - Acceptance: a unit test shows that sequential requests `n, n+1, n+2` give
     the same planes for `n+2` as a cold request for `n+2` when the source is
     a static noisy sequence longer than the warm-up (within the cold-start
     tolerance derived from alpha), and that a cold request for frame 0 is
     filtered within itself: changed from the plain decode, no noisier, and
     the same on every request.
3. **Preview cost.** Measure the time of a random-seek preview with
   `transform3d` and `ntsc3d`, filter on and off.
   - Acceptance: a seek stays under one second on the development machine;
     otherwise lower `kPreviewWarmupFrames` for decoders with look-around and
     record the chosen value in the header comment.

## Phase 4: continuity decision and spatial filter

Prompted by the Domesday Community discs, where every frame is a different
still and the filter as built in Phases 1–3 blended each map into the next
(see "The filter" for the measurement).

1. **Pre-pass.** `NoiseReducer::measure()`: one read-only pass over the
   subsample filling the temporal, anchor and pair-difference histograms,
   from which the scales, the moving fraction and the discontinuity decision
   follow before the main pass. The lagged measurement and the first-frame
   self-measurement of Phase 1 go.
   - Acceptance: a sequence of unrelated noisy pictures is filtered
     spatially throughout, each output closer to its own clean picture than
     the noisy input was (`TreatsUnrelatedStills_AsDiscontinuities`); a
     scene cut is a discontinuity and the frames after it recover
     (`FiltersSceneCutSpatially_AndRecoversAfterIt`); a 4fsc residual and a
     sub-threshold level step still read as still picture.
2. **Spatial filter.** The 5 × 3 range-weighted window mixed in by the motion
   measure, with its range from the pair-difference scales and its share
   from the measured (v, k) model; whole-row vectorised path for rows with
   many moving samples, sample-by-sample path otherwise; unfiltered copies of
   the rows each chunk's and band's windows reach after they have been
   blended.
   - Acceptance: a lone flat noisy frame comes out 3 dB and 6 dB quieter at
     those strengths within a quarter, and no quieter at 9; a 300-code edge
     in 10 codes of noise keeps both sides and still loses noise along the
     edge; a moving bar leaves no trace of its old position; one thread and
     seven bands give bit-identical frames and statistics.
3. **Decision rules.** Clamp, anchor ratio and moving fraction, with the
   thresholds and the measured margins recorded in "The filter"; per-frame
   debug log of the decision in `ExportNoiseReduction`.
   - Acceptance: with `noise_reduction` at 6 the 60-frame Domesday map section
     exports with every frame filtered spatially only and the 300-frame
     Grosse Pointe Blank section with no more than six; the log states both.
4. **Retire the coring controls.** Remove `luma_nr` / `chroma_nr` from the
   descriptors, the decoder configurations and the decoders (`doYNR`,
   `doCNR`, the NR filters in `deemp.h`, the SECAM helper decoder); accept
   the two keys in `set_parameters()` and ignore them; descriptor order test,
   stage help, both user-guide pages and this document updated.
   - Acceptance: no reference to either name outside `set_parameters()`,
     the help and this document; an older project that sets them loads.
5. **Preview.** Frame 0 and any seek target whose warm-up frames are
   discontinuities come out spatially filtered; the kept-frame, continuation
   and warm-up logic is unchanged.
   - Acceptance: `ColdRequestForFrameZero_IsFilteredWithinTheFrame` and the
     seek-tolerance test pass; the preview cost note in `video_sink_stage.h`
     accounts for the spatial pass.

## Phase 5: validation and documentation

1. **Real-material check.** Export short sections from the captures listed in
   the project memory (Grosse Pointe Blank side 1: the Hollywood Pictures
   logo around frame 450, the title card around 1100, and the dark credit
   shot around 2600; a Domesday Community map run such as frames 20000–20060)
   with `noise_reduction` at 0, 6 and 9. Measure luma sigma in a static black
   region, inspect the moving edges and the face in the credit shot, and the
   map lettering.
   - Acceptance: at 6 dB the static black region's sigma falls by at least
     4 dB; no visible trail on moving edges; no change to the shadow detail on
     the face; the map stills carry nothing of the neighbouring sheets and
     keep their lettering. Numbers go in the PR description, not in this
     file.
2. **Decoder guide limitations.** State that the filter is recursive, so a
   missed low-contrast motion can trail for a few frames; that the spatial
   filter reaches just under 6 dB, so moving areas stay noisier than still
   ones at higher settings; that heavily textured stills read as noisier
   than they are, so their spatial range widens; and that `video_filter`
   denoisers remain available in FFmpeg mode.
   - Acceptance: text present; no duplication of the parameter table.
3. **Export cost.** Re-measure the 300-frame export with `ntsc2d` and
   `pal2d`, filter off and on, on continuous film and on a run of stills, in
   a Release build with nothing else running.
   - Acceptance: numbers in the PR description; the project memory's figure
     updated.
