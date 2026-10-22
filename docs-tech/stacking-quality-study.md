# Multi-disc stacking quality study

Measured on short sections cut from real captures, using the automated CLI
workflow (`tools refresh` → Disc Mapper → Source Alignment → Stacker). The aim
was to see how well the current stacking chain removes noise and dropouts, to
find defects, and to rank changes by measured benefit.

All sample values are 10-bit CVBS codes (PAL: black 256, white 844, so
1 IRE ≈ 5.9 codes). "ppm" is parts per million of active-picture samples.

## Method

### Material

300-frame sections were cut from the middle of each capture, so that every
section is programme content (checked: no black or low-detail frames in any
of them). Sidecars were trimmed and renumbered to match.

| Set | Title / side | Captures | Disc type | Section |
|---|---|---|---|---|
| `csouth` | Domesday Community South | DS2, DS4, DS6, DS7, DS8, DS10 | CAV | capture frames 20000–20299 |
| `cnorth` | Domesday Community North | DS2, DS4, DS6, DS8, DS10 | CAV | 30000–30299 |
| `natb` | Domesday National B | DS2, DS4, DS6, DS7, DS8, DS10 | CLV | 40000–40299 |
| `eco` | EcoDisc side 1 | DS1–DS5 | CAV | 20000–20299 |
| `volc` | Volcanoes side 1 | DS1–DS3 | CAV | 20000–20299 |

Two synthetic variants of `csouth` exercise the mapper:

- `skip`: DS4 has a 10-frame player skip spliced in at a frame boundary, and
  DS8 a 10-frame repeat.
- `skipmix`: the same skip, but landing mid-frame. One frame of DS4 has field 1
  from picture 20137 and field 2 from picture 20148.

### Pipeline

Each stack was built and refreshed entirely from the CLI:

```bash
orc-cli --source "PAL_CVBS_Source=input_path=DS2.cvbs[s0]; ..." \
        --filters "[s0] frame_map [m0]; ...; [m0][m1]... source_align, stacker" \
        --sink "CVBSSink=output_path=stack.cvbs" --export-project set.orcprj
orc-cli tools refresh-config set.orcprj <each frame_map> field_mapping
orc-cli tools refresh-config set.orcprj <source_align> source_alignment
orc-cli tools refresh set.orcprj --json
orc-cli set.orcprj --process
```

Each capture was also exported on its own through the same `frame_map` ranges
and `source_align` offset, using a 1-input Stacker as a pass-through. This
gives every disc on the stack's frame numbering, so each one can be compared
with the stack sample for sample.

### Measurements

All of these were computed offline with numpy:

- **Per-disc noise σ.** A least-squares fit to the pairwise difference
  variances, over samples that neither disc flags.
- **Marked dropouts.** The samples covered by each disc's dropout runs.
- **Unmarked errors.** Samples not flagged, but more than 40 codes (about
  7 IRE) from the per-sample median of the other discs, where at least 3 discs
  are usable.
- **Stacking policies.** Scored by leave-one-out: stack n of the other discs,
  then compare with the held-out disc wherever that disc is trusted.
  - Noise gain is reported against a single disc measured the same way. The
    ideal is 10·log₁₀ n.
  - Gross errors are residuals over 40 codes.
- **Model check.** A numpy model of `StackerStage::stack_mode` reproduces
  orc's stacked output bit for bit (0 ppm mismatch). That validates the
  policy comparisons as like-for-like.

## Headline results

| Set | Discs | Single-disc σ (codes) | Marked dropouts (ppm/disc) | Unmarked errors (ppm/disc) | Gain, current default | Gain, best policy |
|---|---|---|---|---|---|---|
| csouth | 6 | 3.9–6.5 | 16–184 | 130–289 | 5.9 dB (n=5) | 6.4 dB |
| cnorth | 5 | 8.6–14.4 | 17–168 | 158–500 † | 0.0 dB (n=4) | 2.8 dB |
| natb | 6 | 10.1–11.5 | 18–353 | 920–5600 † | 1.05 dB (n=5) | 5.0 dB |
| eco | 5 | 4.5–14.2 | 16–113 | 144–9100 | 5.2 dB (n=4) | 6.2 dB |
| volc | 3 | 8.4 | 101–220 | 231–442 | 2.9 dB (n=2) | 2.9 dB |

† Inflated by differences between pressings (see [Two kinds of disagreement](#two-kinds-of-disagreement-pressings-and-capture-chains)).

"Best policy" is `ivw_5sigma` (see [S1](#s1-noise-adaptive-threshold-with-inverse-variance-weights)).

### What the measurements show

1. **ld-decode leaves more errors unmarked than it marks.** On every set, the
   unflagged outliers equal or exceed the flagged samples.
   - 87–97 % of them are single-sample impulses (LaserDisc "sparkle").
   - They come in both polarities, are typically 50 codes (8.5 IRE) in size,
     and sit far from any flagged run.
   - They are not dropout edges and not edge misregistration: they occur at
     the same local gradient as the picture overall.
   - The flags themselves are accurate. Only 2–8 % of flagged samples actually
     agree with the other discs.
2. **The current default threshold is badly mismatched to the noise.**
   - `Auto` uses a fixed `smart_threshold` of 15. With σ ≈ 10 codes, and with
     differences between pressings on top, that rejects most of the good
     values.
   - On National B, five discs give **1 dB**. Plain Mean gives 4.8 dB.
3. **Defects in the chain cost picture content.** In order of severity:
   - mis-registered sources;
   - pictures dropped from the end of the stack;
   - corrupted CVBS exports when padding is present.

## Defects found

Ordered by impact. File references are to the code as of this study.

### D1. PAL padding frames are 4 samples short, so CVBS exports are corrupted

Both padding paths size a PAL frame as `height × nominal_width`, which is
625 × 1135 = 709,375 samples. The true frame is 709,379, because lines 312
and 624 carry two extra samples each:

- `source_align` sets `pad_samples_total_ = pad_height_ * pad_samples_per_line_`
  ([source_align_stage.cpp:196](../orc/plugins/stages/source_align/source_align_stage.cpp#L196)).
- `frame_map` sizes `black_frame_` the same way
  ([frame_map_stage.cpp:63-67](../orc/plugins/stages/frame_map/frame_map_stage.cpp#L63-L67)).
  So does its synthesised PAD descriptor's `samples_total` (around
  [frame_map_stage.cpp:119](../orc/plugins/stages/frame_map/frame_map_stage.cpp#L119)).

What this breaks:

- **CVBS Sink trusts `samples_total`.** It writes 709,375 samples for every
  pad. Every later frame in the file starts 4 samples early, and the shift
  accumulates: 4 more per pad. Measured: on an aligned export with one leading
  pad, frame *n* of the output equals the capture's frames *n−1*/*n* straddled
  at a 4-sample offset, and the file is 8 bytes short.
- **The stacker inherits it.** Where input 1 is a pad, it reports input 1's
  pad descriptor (see D3), so its own output frame is also written short, even
  though that frame carries real stacked content.
- **Out-of-bounds read.** `get_line(id, 624)` on a pad frame points past the
  end of the 709,375-sample buffer.

Fix: size pad frames with `frame_line_sample_offset(system, width, height)`,
as the stacker does for its own output. Add a regression test asserting
`samples_total == 709379` for a PAL pad from both stages.

### D2. The sample-grid search is clamped to ±4 and confuses lags 4 apart

`kMaxSampleShift = 4`
([stacker_stage.cpp:43](../orc/plugins/stages/stacker/stacker_stage.cpp#L43)),
and the correlation runs on the composite signal. Composite repeats every
4 samples because of the subcarrier, so a true offset of L also correlates
strongly at L±4.

Measured offsets of each capture against the first. These are constant
across a section, and confirmed with a ±16 luma search:

| Set | Offsets (samples) | Stacker applied |
|---|---|---|
| csouth | all 0 | all 0 |
| cnorth | DS4, DS8, DS10: +4 | +4 (at the edge of the window) |
| natb | DS4 +2, DS6 +1, DS8 +2 | same |
| volc | DS2 −1 | same |
| eco | **DS2 +12** | **+4** (correlation 0.85), so DS2 is stacked **8 samples (0.45 µs) out** |

The orc output matches a model that places EcoDisc DS2 at +4 bit for bit.
Smart mean happens to reject most of DS2 here, because it is also the noisiest
disc. With `Mean`, or with 2–3 discs, it would ghost.

The offset is also **not constant over a whole side**. The full-capture probe
found:

| Pair | Offset by capture frame |
|---|---|
| EcoDisc DS2 vs DS1 | +11 at 1,500 and 8,000; +12 at 20,100, 32,000 and 45,000 |
| Community North DS4 vs DS2 | 0 at 1,500, 45,000 and 52,000; +4 at 30,100 |

A single global measurement from 8 frames spread over the whole source cannot
be right everywhere. On a full Community North stack it would shift DS4 by the
wrong amount over most of the side.

Fix:

- Correlate low-passed luma (a 4-sample box removes the subcarrier) over
  ±32 samples.
- Measure per segment, for example per 500–1000 frames, and interpolate
  between segments.
- Keep the correlation gate.

The constant's comment, "more than that is a frame-alignment problem", is
wrong: 12 samples is 0.7 µs of line-start placement, not a frame.

### D3. Stack range and descriptors follow input 1 only

`frame_range`, `frame_count`, `has_frame` and `get_frame_descriptor` all
forward to `sources_[0]`
([stacker_stage.cpp:184-222](../orc/plugins/stages/stacker/stacker_stage.cpp#L184-L222)).
Source Align pads only the head, so the inputs end at different points.

| Set | Aligned input lengths | Stack length | Pictures lost |
|---|---|---|---|
| eco | 300, 308, 308, 303, 308 | 300 | last 8, present on 3–4 discs |
| volc | 300, 307, 301 | 300 | last 7 |
| cnorth | 305 … 300 | 305 | none, but frames 0–4 carry input 1's padding descriptor (see below) |

Where input 1 is a pad, the stacked frame is computed from the other discs,
but it is still described as `is_padding_frame = true`, with the short sample
count from D1. Downstream:

- TBC export writes blank fields;
- the renderer skips observers;
- the CVBS export is truncated.

Fix: report the union range of the inputs. Take descriptors from the first
input that has a real frame at that id. Set `is_padding_frame` only when every
input is padding.

### D4. Mid-frame player skips are stacked as a mixed frame

The Disc Mapper pairs fields strictly within a capture frame. It takes the
frame's picture number from field 1 and computes `pn_disagreement`, but never
uses it ([disc_mapper_analyzer.cpp:533-554](../orc/core/analysis/disc_mapper/disc_mapper_analyzer.cpp#L533-L554)).

In the `skipmix` test, the frame with field 1 from picture 20137 and field 2
from picture 20148 was kept as picture 20137, followed by `PAD_11`. The
stacker then blends the wrong field 2 in:

| Discs stacked | Field 2 of picture 20137 vs a clean disc |
|---|---|
| 2 | correlation 0.70, 61 codes rms, **38 % of samples off by more than 7 IRE** |
| 3 or 6 | 0.998, rejected by smart mean |

The 3+ disc rescue only worked because each Domesday picture is a different
still. With motion video, neighbouring pictures are similar and the wrong
field would partly pass the outlier test.

A clean frame-boundary skip and a 10-frame repeat were both handled correctly:
padded, and de-duplicated keeping the first copy.

Fix:

- In the mapper, treat a frame whose two fields carry different picture
  numbers as two half-frames. Emit it as padding for the field that does not
  belong, or drop it and let the gap be padded.
- Independently, the stacker should check field content per input (see S4).

### D5. Source Alignment pad mode ignores where the first picture number was found

Padding is `first_vbi[i] − global_first_vbi`
([source_alignment_analysis.cpp:490-493](../orc/core/analysis/source_alignment/source_alignment_analysis.cpp#L490-L493)).
It does not subtract the frame id at which `first_vbi[i]` was seen.

- It is correct only if frame 0 of every mapped source carries a picture
  number.
- A source whose mapped frame 0 is a kept lead-in frame (the default,
  `include_lead_in_out=true`), or has no readable number, is placed one or
  more frames out.
- A source with no picture numbers gets `+0` and is still stacked.
- `first_common_frame` mode does use the frame id.

All sections in this study start mid-programme, so this was not triggered
here. It is a code-review finding.

Fix: use the median of (picture number − frame id) over the scan. Exclude any
source that has no picture numbers.

### D6. Smaller stacker issues

- **The default `smart_threshold` (15) is too tight.**
  - In ld-disc-stacker the default is `15*256` on 16-bit samples, which is
    60 on the 10-bit scale. Orc's 15 is 4× tighter. For its effect, see
    [S1](#s1-noise-adaptive-threshold-with-inverse-variance-weights).
- **Integer truncation.** Mean and smart mean divide with C++ truncation,
  which biases output down by about 0.5 code. Rounding is worth
  0.05–0.1 dB.
- **`diff_dod` window.** `diff_dod` accepts values within ±500 codes of the
  median (half the 10-bit range). ld-disc-stacker uses ±10 % of the median.
  - All-dropout samples were rare here (0–4 ppm), so this had no measurable
    effect.
- **`passthrough` does nothing.** It is stored and saved
  ([stacker_stage.cpp:1742](../orc/plugins/stages/stacker/stacker_stage.cpp#L1742))
  but never read.
- **`Smart Neighbor` and `Neighbor`** are offered but behave as `Median`. That
  costs 1–3 dB against the best policy on these sets.
- **Unconfident output is never flagged.** The stacked output marks a dropout
  only where every input is flagged. Unflagged residual errors (13–4,000 ppm
  depending on set and n) reach `dropout_correct` unmarked.
- **Dropout lookup cost.** `is_sample_dropout` scans every dropout run of a
  source for every sample. Building one per-line mask per source would make
  it O(1) per sample. This is performance only.
- **Dropout-blind duplicate choice.** The mapper picks between duplicate
  copies by confidence, then by burst, SNR and PSNR, and never by dropout
  count ([disc_mapper_analyzer.cpp:1182-1196](../orc/core/analysis/disc_mapper/disc_mapper_analyzer.cpp#L1182-L1196)).

## Two kinds of disagreement: pressings and capture chains

Pairwise differences show that the captures of a side fall into groups:

| Set | Groups | Within-group pair σ | Cross-group pair σ |
|---|---|---|---|
| csouth | one: all six | 5.9–8.3 | — |
| natb | {DS2, DS7, DS10} and {DS4, DS6, DS8} | 8.2–11.1 | 17.2–18.2 |
| cnorth | {DS2, DS6} and {DS4, DS8, DS10} | 5.9–8.6 | 19.6–20.3 |

The excess is in both the luma and chroma bands, roughly doubling across
groups. Field, line and horizontal registration are correct between groups:

- same frame, same field, same line correlates best;
- residual luma lag is at most 0.2 samples;
- relative line jitter is 0.03–0.07 samples on CAV.

This is the signature of **different pressings**. Noise recorded into a
stamper is identical on every disc made from it, so it only averages out
across pressings.

The cnorth {DS2, DS6} group is also band-limited: response 0.58 at 5–6 MHz
against about 1.0 for the others, with −3.5° chroma phase. On EcoDisc, DS2 is
likewise soft (0.41 at 5–6 MHz) and much noisier than the rest.

Consequences for stacking:

- A fixed smart threshold below the cross-pressing spread picks one pressing
  per pixel, and the choice changes from pixel to pixel. That is why `Auto`
  collapses on natb and cnorth, below even plain Mean.
- Averaging across pressings is desirable: it also removes master noise.
  The combiner must not treat the other pressing as outliers.

Other systematic differences between captures are small: luma gain ±0.9 %,
DC offset ±3 codes, chroma gain ±3 % (EcoDisc DS2 −14 %), chroma phase ±2.5°,
response ±4 % below 5 MHz. Together these are a 0.3–0.5 dB floor at n=5 on
csouth.

## Stacking policy comparison

Leave-one-out gain in dB over a single disc, with gross-error rate in ppm.
The ideal gain is 3.0, 4.8, 6.0 and 7.0 dB for n = 2, 3, 4, 5.

| Policy | csouth n=5 | cnorth n=4 | natb n=5 | eco n=4 | volc n=2 |
|---|---|---|---|---|---|
| Current `Auto` (smart, t=15, truncating) | 5.88 / 27 | 0.02 / 8,213 | 1.05 / 15,062 | 5.18 / 437 | 2.87 / 320 |
| `Mean` | 6.22 / 37 | 1.75 / 1,726 | 4.83 / 1,536 | 3.74 / 418 | 2.87 / 320 |
| `Median` | 5.24 / 20 | 0.35 / 4,062 | 1.71 / 9,203 | 5.32 / 322 | 2.87 / 320 |
| Smart, t=30, rounding | 6.34 / 21 | 1.19 / 7,868 | 3.81 / 11,230 | 4.30 / 367 | 2.87 / 320 |
| Smart, t=60, rounding | 6.28 / 14 | 1.75 / 1,759 | 4.91 / 1,888 | 3.76 / 377 | 2.87 / 320 |
| Smart, t = 5σ̂, rounding | 6.34 / 21 | 1.75 / 1,802 | 4.90 / 1,757 | 4.42 / 364 | 2.87 / 320 |
| **Inverse-variance mean, reject beyond 5σ̂ (`ivw_5sigma`)** | **6.42 / 24** | **2.75 / 1,766** | **5.01 / 1,900** | **6.17 / 344** | 2.88 / 320 |

σ̂ is the median per-disc noise estimated from the data, times 1.2. That gives
a threshold of 28 codes on csouth and eco, and 57–62 on cnorth and natb.

The cnorth and natb gross-error rates are dominated by the held-out disc's own
pressing noise, not by stacking errors. Compare the policies with each other,
not with the other sets.

Two further tests:

- **No policy can vote with two discs.** At n=2, an unflagged sparkle
  survives at half amplitude: 130–7,500 ppm gross.
- **Neighbour arbitration did not help.** At n=2, I tried picking whichever
  disc is closer to a same-phase spatial prediction (mean of both discs at
  x±4) when the two disagree. It was no better than the mean (csouth: 2.19 dB
  / 178 ppm at T=40, against 2.16 / 125 for the mean) and worse at lower
  thresholds. A better predictor is needed before neighbour modes are worth
  building.

## Suggestions

Ordered by measured or expected benefit.

### Stacker

#### S1. Noise-adaptive threshold with inverse-variance weights

Measure each input's noise once, as `sample_offsets()` already measures grids:

- take pairwise robust variances over a spread of frames, then solve for
  per-disc σ.
- weight each usable value by 1/σᵢ²;
- reject values beyond k·σ̂ of the per-sample median, with k ≈ 5.

Measured benefit over the current default:

| Set | Gain |
|---|---|
| csouth | +0.5 dB |
| eco | +1.0 dB |
| cnorth | +2.7 dB |
| natb | +4.0 dB |

Gross errors stay at the level of the best fixed threshold. Make this the
default mode, and keep `smart_threshold` as an override.

#### S2. Fix the grid alignment (D2)

Use a luma correlation over ±32 samples, measured per segment and
interpolated. This removes a 0.45 µs misregistration on EcoDisc, and covers
drift of one sample across a side.

#### S3. Fix the frame range and descriptors (D3) and the padding size (D1)

D3 recovers pictures at the stack's ends. D1 makes CVBS and TBC exports of
padded stacks valid.

#### S4. Check field content per input before combining

For each frame and field, correlate each input's low-passed luma with the
median of the inputs. That is cheap: a decimated grid is enough. Exclude an
input from that field when it is well below the others, and log it. In this
study:

- correct inputs score 0.93 or more on any field;
- the mixed frame's wrong field scored 0.07.

This also guards against any remaining mapper or alignment error.

#### S5. Mark low-confidence output as dropout

Flag output samples where no majority exists, or where at n=2 the usable
values differ by more than k·σ̂. `dropout_correct` can then repair them.
Today they pass downstream unmarked.

#### S6. Round instead of truncating

Worth about 0.05–0.1 dB, and removes a −0.5 code bias.

#### S7. Optional per-input equalisation

Before combining, fit and remove each input's DC offset, luma gain and chroma
gain and phase against the median. This is worth an estimated 0.3–0.5 dB; the
estimate comes from the measured systematic spread and was not tested
end to end. Also report inputs whose high-frequency response is far below the
others (cnorth DS2/DS6, eco DS2), so users can drop them or down-weight them.

#### S8. Tidy the parameters

- Implement or remove `passthrough`, `Neighbor` and `Smart Neighbor`.
- Scale `diff_dod`'s window to the 10-bit range (or to σ̂).
- Use a per-line dropout mask in place of per-sample run scans.

### Disc mapping and source alignment

#### M1. Split mixed-field frames (D4)

Use `pn_disagreement`. When the two fields of a capture frame carry different
picture numbers, emit the foreign field as padding, or drop the frame and pad.

#### M2. Make pad-mode alignment frame-index aware (D5)

Take the median of (picture number − frame id) over the scan, not the first
number. Exclude sources with no picture numbers.

#### M3. Pad the tail as well as the head

Then all aligned inputs have equal length. This complements D3.

#### M4. Report pressing groups

Source Alignment already reads every disc. It could cluster the discs by
pairwise difference on a few frames and report the groups. Stacks that mix
pressings benefit most from S1. Stacks of a single pressing cannot remove
master noise, and users choosing discs should know that.

#### M5. Consider dropouts when choosing between duplicates

Add dropout counts to the duplicate-copy quality score, so a clean repeat
beats a damaged one.

#### M6. Detect sparkle before stacking (untested idea)

Unflagged single-sample impulses outnumber flagged dropouts on every set. A
per-source impulse detector would let 2-disc stacks reject sparkle and make
the dropout hints honest. Such a detector would compare each sample with its
same-subcarrier-phase neighbours at x±4, in the field's own line. This could
equally be raised with ld-decode's dropout detector.

## Not covered

- **Audio and EFM stacking.** Sidecars were not cut. The EFM best-source
  splicing problem is already documented in the stacker instructions.
- **Full-side behaviour of the mapper:** lead-in and lead-out, NTSC pulldown,
  CLV hour digit. The sections are mid-programme.
- **Y/C captures and NTSC.** All captures here are PAL composite.

## Implementation plan

Four phases, each one shippable on its own. Phase 1 fixes data corruption and
lost pictures, so it comes first. Phase 2 makes sure the stacker combines the
right samples. Phase 3 changes how the samples are combined. Phase 4 is
diagnostics and optional quality work.

Every task follows [TESTING.md](../TESTING.md) and AGENTS.md §4:

- Unit tests use mocked `VideoFrameRepresentation` inputs, with no file
  access. They go in the existing suites under
  `orc-tests/core/unit/stages/{stacker,frame_map,source_align}` and
  `orc-tests/core/unit/analysis`.
- Real-capture checks go in `orc-tests/core/functional`, labelled
  `functional`.
- Any change to a stage's behaviour or parameters updates that stage's
  `instructions.md` in the same change.

### Phase 1: Frame geometry and stack range

Fixes D1 and D3, and suggestion M3. **Status: done.** Re-running the EcoDisc
set gives a 308-frame stack instead of 300; the first 300 frames are
byte-identical to before, and the 8 recovered tail frames carry picture.

1. **Size PAL padding frames correctly in `source_align`.**
   - Change: compute `pad_samples_total_` with
     `frame_line_sample_offset(system, width, height)`
     ([source_align_stage.cpp:196](../orc/plugins/stages/source_align/source_align_stage.cpp#L196)).
   - Acceptance:
     - A PAL pad's descriptor reports `samples_total == 709379`.
     - `get_frame_copy` returns 709,379 samples.
     - `get_line(id, 624)` stays inside the buffer.
     - NTSC and PAL-M pads are unchanged.
     - New tests are in `source_align_stage_test.cpp`.
2. **Size PAL padding frames correctly in `frame_map`.**
   - Change: apply the same fix to `ensure_black_frame` and to the
     synthesised PAD descriptor
     ([frame_map_stage.cpp:63-67](../orc/plugins/stages/frame_map/frame_map_stage.cpp#L63-L67),
     around line 119).
   - Acceptance: the same assertions as task 1, for range-spec `PAD_N`
     entries and for `pad_gaps` padding, in `frame_map_stage_test.cpp`.
3. **Make the stacker's range the union of its inputs.**
   - Change: `frame_range` and `frame_count` cover the union of the inputs.
     `has_frame` is true when any input has the frame.
     `get_frame_descriptor` comes from the first input with a non-padding
     frame at that id. `is_padding_frame` is set only when every input is
     padding.
   - Acceptance: unit tests where input 1 has padding at the head, and is
     shorter at the tail, than the others:
     - the stacked range includes the tail frames;
     - those frames are stacked from the other inputs;
     - the descriptor is not marked as padding.
4. **Pad the tail in `source_align`.**
   - Change: in `pad_for_alignment` mode, append padding so that every
     output has the same length as the longest one. Update the Source
     Alignment report's coverage figures to match.
   - Acceptance:
     - Unit test: inputs of unequal length come out of `source_align` equal
       in length, with the tail frames marked as padding.
     - `instructions.md` describes tail padding.
5. **Add a functional round-trip test.**
   - Change: write two PAL captures of unequal length as real CVBS files,
     read them back with `CVBSSource`, align them so they need leading and
     trailing padding, stack them, and write the result with `CVBSSink`
     (`stack_alignment_roundtrip_test.cpp`). The captures are synthetic, so
     the test needs no capture files and runs everywhere.
   - Acceptance:
     - The file size is exactly *frames* × 1,418,758 bytes.
     - Every frame of the output starts on a line boundary (sync found at
       the expected sample).

### Phase 2: Registration and alignment correctness

Fixes D2, D4 and D5, and suggestions S4, M1 and M2. **Status: done.**
Re-running the study sets with the new stacker:

- EcoDisc DS2 is measured at +12 (correlation 0.997) where +4 was applied
  before. Community North DS4, DS8 and DS10 stay at +4.
- On the `skipmix` stacks, which still carry the old mapping with the mixed
  frame, the field check catches that frame and nothing else. With 2 discs
  it marks field 2 as dropout; with 3 and 6 it leaves the foreign input out
  of field 2 (correlation 0.37).
- On the five clean EcoDisc inputs (308 frames) it excludes nothing.

On these PAL CAV discs only field 1 carries a picture number (300 of 600
fields), so the picture-number comparison of task 3 cannot see the `skipmix`
frame. Task 5 adds the colour-phase comparison, which does: refreshing the
`skipmix` stacks drops the mixed frame and pads 12 pictures (it was 11 plus
the mixed frame), and the re-stack no longer logs a foreign field. On the 22
clean captures of the other sets (about 6,600 frames) it splits nothing and
leaves every mapping unchanged.

The colour-phase observer cannot be trusted across the full 8-field PAL
sequence. On clean frames field 2 reads 5 phases past field 1 instead of 1
on 0–100 % of frames depending on the capture: none on Community South,
about 40 % on NatB, and almost all on some EcoDisc discs. The 4-field part
never disagreed. The mapper therefore compares phases modulo 4, which
catches mid-frame skips of an odd number of frames; even skips are left to
the stacker's field check. The CVBS sink's phase-sequence check compares the
full 8-field ID and is probably affected by the same misreading; that is
not covered here.

1. **Rewrite the sample-grid measurement in the stacker.**
   - Change:
     - Correlate low-passed luma: a 4-sample box removes the subcarrier, and
       Y/C sources use the luma plane as now.
     - Search ±32 samples.
     - Measure per segment of N frames (start with N = 500).
     - Hold the offset from the nearest measured segment.
     - Keep the correlation gate.
     - Log each segment's offset when it changes.
   - Acceptance: unit tests with synthetic inputs shifted by +12, by −7, and
     by an offset that changes from 0 to +4 halfway through. Each frame
     resolves to the true offset, and composite inputs no longer lock onto
     L±4.
   - Update the `sample_align` section of `instructions.md`.
2. **Check field content per input in the stacker.**
   - Change: for each output frame, correlate each input's low-passed luma
     per field with the per-field median of the inputs, on a decimated grid.
     Drop an input from a field when its correlation is far below the
     others'; start with an absolute floor of 0.8, which can be tuned. Log
     the frame and input.
   - Acceptance:
     - A unit test with one input carrying a foreign field 2 shows that field
       excluded, and field 1 still used.
     - With 2 inputs and no agreement, the frame keeps both, but the
       disagreeing field is marked as dropout.
3. **Split mixed-field frames in the Disc Mapper.**
   - Change: use `pn_disagreement`
     ([disc_mapper_analyzer.cpp:554](../orc/core/analysis/disc_mapper/disc_mapper_analyzer.cpp#L554)).
     When the two fields of a capture frame carry different picture numbers,
     treat the frame as unmappable, so the gap is padded, instead of filing
     it under field 1's number. Count it in a new `splitFieldFrames`
     statistic.
   - Acceptance: a unit test in `orc-tests/core/unit/analysis` with a mocked
     field sequence that reproduces the `skipmix` case (field 1 = picture
     137, field 2 = picture 148) produces a padded gap with no mixed frame
     in the mapping.
4. **Make pad-mode Source Alignment frame-index aware.**
   - Change: compute each source's offset as the median of (picture number −
     frame id) over the scanned frames. Exclude a source that has no picture
     numbers, and name it in the report, instead of giving it `+0`.
   - Acceptance: unit tests for:
     - a source whose mapped frame 0 is a kept lead-in frame;
     - a source with one corrupt early picture number;
     - a source with no picture numbers.
     The first two align to the same picture as the others; the third is
     excluded.
5. **Detect mixed-field frames by colour phase in the Disc Mapper.**
   - Change: run the `colour_frame_phase` observer in the mapper's
     observation pass and fill in each field's phase. Split a frame, as in
     task 3, when field 2's phase is not field 1's plus one, compared modulo
     4.
   - Acceptance:
     - Unit tests: a mixed frame whose fields carry phases 22 fields apart
       is split; a frame read 4 phases out, or with an unmeasured phase, is
       not.
     - Real captures: `skipmix` loses the mixed frame; the clean sets map
       exactly as before.

### Phase 3: Combining policy

Suggestions S1, S5, S6 and S8.

1. **Measure per-input noise in the stacker.**
   - Change: once per segment, alongside the grid measurement, estimate each
     input's noise from pairwise robust variances over a spread of frames,
     solved by least squares. Cache the result and log it.
   - Acceptance: unit tests with synthetic inputs of known Gaussian σ (3, 5,
     10 and 15 codes) recover each σ within 10 %.
2. **Store the stacker mode in the project.**
   - Change:
     - A new Stacker node writes its `mode` to the project explicitly, so the
       mode a project was built with no longer depends on the build's
       default.
     - When a project is loaded and a Stacker node has no stored `mode`, set
       it to `Auto`, the default those projects were saved under, and keep it
       stored from then on.
     - New nodes get the new default from task 3.
   - Acceptance:
     - Unit tests: loading a project whose Stacker has no `mode` gives
       `Auto`, and saving it writes `mode: Auto`. A newly added Stacker
       saves its mode explicitly.
     - Tasks 3–5 leave `Auto`, `Mean`, `Median` and `Smart Mean`
       bit-identical. The only changes to an existing project's output come
       from Phase 2's alignment fixes, and from task 6 where every input is
       flagged.
3. **Add an inverse-variance stacking mode.**
   - Change: add a `Weighted` mode and make it the default for new Stacker
     nodes. Weight each usable value by 1/σᵢ², and
     reject values beyond k·σ̂ of the per-sample median. Add a new
     `reject_sigma` parameter for k, default 5.
   - Acceptance:
     - Unit tests: unequal-noise inputs give a lower output variance than
       `Mean`. A single outlier input beyond the threshold is rejected. Two
       groups offset by less than k·σ̂ are averaged, not chosen between.
     - `instructions.md` documents the mode, `reject_sigma`, the new default,
       and that older projects keep `Auto`.
4. **Round instead of truncating in `Weighted`.**
   - Change: `Weighted` rounds its result. The existing modes keep their
     truncating divide, so saved projects reproduce exactly.
   - Acceptance: a unit test checks that the mean of {1, 2} is 2 and not 1,
     and that there is no negative bias over random inputs.
5. **Mark low-confidence output as dropout in `Weighted`.**
   - Change: in `Weighted` mode, mark an output sample as dropout when no
     majority of usable values lies within k·σ̂ of the result, or when 2 usable values differ by
     more than k·σ̂. Severity distinguishes these marks from all-dropout
     holes.
   - Acceptance:
     - Unit tests for the 2-input disagreement case and for a split vote.
     - Agreeing inputs produce no marks.
6. **Tidy the stacker parameters.**
   - Change:
     - Make `passthrough` work: output the inputs' values, marked, where
       every input is flagged. Otherwise remove it.
     - Remove `Neighbor` and `Smart Neighbor`, or keep them, labelled, as
       aliases of `Median`.
     - Scale the `diff_dod` window to ±10 % of the median, as
       ld-disc-stacker does.
     - Replace the per-sample run scan in `is_sample_dropout` with a
       per-line mask.
   - Acceptance:
     - Parameter round-trip tests pass.
     - Stacking a 6-input PAL frame is no slower than before.
     - `instructions.md` matches the code, and says that the `diff_dod` and
       `passthrough` changes alter output only where every input is flagged
       (0–4 ppm on the study's sets).

### Phase 4: Diagnostics and optional quality work

Suggestions M4, M5, S7 and M6. Each task is independent; S7 and M6 should be
kept only if they measure as an improvement.

1. **Report pressing groups in Source Alignment.**
   - Change: cluster the aligned sources by pairwise difference on a few
     frames. Report the groups, and any source whose response above 4 MHz is
     far below the others'.
   - Acceptance: a unit test with two synthetic groups, where noise common
     within a group differs between the groups, reports the two groups.
     A source with a low-passed response is flagged.
2. **Prefer the cleaner copy when choosing between duplicates.**
   - Change: add the frame's dropout sample count to the duplicate-copy
     quality score. A cross-validated picture number still wins when quality
     is close.
   - Acceptance: a unit test with two copies of one picture, one heavily
     flagged, keeps the clean copy.
3. **Equalise each input before combining (optional).**
   - Change: fit and remove each input's DC offset, luma gain, and chroma
     gain and phase against the median, per segment, before combining.
   - Acceptance:
     - Unit tests: synthetic gain and offset errors are removed to within
       0.5 %.
     - Re-measured on the study's sets, the gain is at least 0.2 dB without
       a rise in gross errors. Otherwise drop the task.
4. **Detect sparkle in each input before stacking (optional).**
   - Change: flag single-sample impulses in each input by comparing with its
     same-phase neighbours at x±4 in its own line, before combining.
   - Acceptance:
     - Unit tests with injected impulses on flat and on detailed lines catch
       the impulses, with a low false-positive rate on detail.
     - Re-measured on 2-disc stacks, gross errors fall with no loss of noise
       gain. Otherwise drop the task.
