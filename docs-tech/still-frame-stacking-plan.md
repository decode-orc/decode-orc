# Still-frame stacking

Issue: [#340 — Stack multiple repeating still frames](https://github.com/decode-orc/decode-orc/issues/340)

Many discs hold a still picture for several consecutive frames. Each repeat is recorded at a different place on the disc, so its noise and dropouts are independent of the other repeats', including the noise recorded into the stamper that limits cross-disc stacking (see the pressing model in [stack_measurement.h](../orc/sdk/include/orc/support/stack_measurement.h)). The Stacker can therefore treat the repeats of a picture as extra inputs: opt-in stacking of the phase-compatible repeats of a still picture with frame `f`.

The repeat detector is a measurement in the SDK support tier, measured on real discs (Phase 1) before it changes any stacked output.

## Design

### Frame content measurement

The Stacker is a plugin, so the measurement lives in the SDK support tier as `orc/support/frame_content.h` with `orc/sdk/src/frame_content.cpp`, as `stack_measurement.h` does.

- **Field signature.** Luma filtered by a 4-sample box, which nulls the 4FSC subcarrier (the same operator `SampleGridMeter` uses), then averaged into 32×18 blocks over the active picture of each field. Each block also keeps the mean of each of its lines and the fraction of its box positions that read a flagged sample. The last line of each field is left out: in PAL field 2 it is line 623, whose blanked second half varies from frame to frame. A signature is about 40 kilobytes a field and is built once per source frame.
- **Same picture.** Repeats of a still are not identical apart from noise. On Volcanoes their level drifts by a gain that changes down the field: the top of the picture moves by up to 4 % in four frames while the black border stays put. So the test models a smooth change first and judges what is left:
  1. Each row of blocks is fitted, robustly, with an offset plus a gain on the block's level above blanking.
  2. The robust spread of the residual is the pair's noise. A pair whose noise is over 1.5 times the source's reference has changed everywhere (a cut, a pan) and is different.
  3. Every judged block's residual must be within `k` = 5.5 times its expected noise, which catches a local change such as a cursor or a caption. A block over the limit is excused when the excess lies on at most 3 of its lines once those lines are left out. Dropouts the decoder did not flag cover one to three lines, while a real change covers most of a block's lines (a caption's letters span all of theirs).
  4. The median relative level change, over blocks above 10 % of white, must be within 1 %. Drift moves only some rows, while a fade moves all of them.

  A block with any flagged sample is not judged: leaving samples out of one frame's block but not the other's compares different areas, which moves the mean of a block with structure by far more than the noise.
- **Colour phase.** A repeat can be stacked into the composite (or YC chroma) of frame `f` only if its subcarrier phase is the same. Phase is measured, not read from `colour_frame_index`, because the PAL 8-field reading is unreliable (see the Stacker's sample-grid notes).
  - **Chroma band.** The band is taken by a horizontal band-pass centred on the subcarrier, `(2x[n] − x[n−2] − x[n+2]) / 4`, and then a same-field vertical comb against the line whose subcarrier is inverted: two lines on in PAL, the next line in NTSC. The comb removes the luma detail near the subcarrier, which would otherwise correlate at any phase and could make a quadrature frame read as the same.
  - **Correlation.** The two frames' bands are correlated at lag 0 over a decimated grid of active lines:
    - near +1 means the same phase;
    - near −1 means opposite phase (NTSC frame ± 1, PAL frame ± 2);
    - near 0 means quadrature (PAL frame ± 1).
  - **Relative decision.** The correlation is judged against the one expected at the same phase, read from each frame's own band one subcarrier cycle (4 samples) along the line, where chroma repeats and noise does not. So the decision follows the chroma level and needs no noise model: 0.6 of the expected or more is the same phase, −0.6 or less is opposite, and within ±0.3 is quadrature.
  - **Unknown.** Where the expected correlation is below 0.2 (blank frames), the relation is unknown, and copies are taken only at the cadence offsets (±4n PAL, ±2n NTSC). There is no chroma to cancel there, and the cadence is right unless Frame Map broke it.

  A Frame Map gap that breaks the expected 4-frame (PAL) or 2-frame (NTSC) cadence is caught by the same test, and so is a one-sample grid slip within a capture. The test assumes the two frames show the same picture, which is decided first: two different pictures read as quadrature.
- **Duplicate read.** A frame whose samples are bit-identical to another's is the same physical read (a Frame Map duplicate). It is never counted as a repeat.
- **Noise.** The reference σ is measured in the block domain, by `BlockNoiseMeter` over pairs already found to match: the robust spread of their residuals, scaled by each block's noise gain. LaserDisc noise rises with frequency, so a full-band measure does not stand in for it:
  - The within-frame anchor (the box operator between a frame and itself shifted by 8 samples, as in the NoiseReducer) reads 11.4 codes on Volcanoes black, where matched pairs give 4.6. Detail inflates it further, so it serves only as the starting bound for finding the first matches.
  - The Weighted model's σ is per sample over the whole band, so the Stacker uses `BlockNoiseMeter` too, not the Weighted σ.
  - Noise also rises with level: 4.6 codes on black and 6.1 on a mid-grey still. The pair's own noise sets the block limit whenever it is above the reference, which absorbs this.
  - The Stacker measures the reference per source and 500-frame segment, on 24 pairs of frames a cadence apart spread through the segment. The pairs are matched first against the median within-frame anchor, then three times against the `BlockNoiseMeter` σ the last round's matches gave. At least 4 matching field pairs are needed. A segment without them holds the nearest one that has them, up to 4 segments away, and otherwise its frames take no copies. A reference that grew with the matches found would make a frame's copies depend on which frames were stacked first.

Every decision is made per pair `(f, f')`, from the two frames alone plus σ, with no chaining into runs. Results therefore do not depend on the order frames are visited, and preview and export agree.

### Repeat stacking

For output frame `f`, the copies are the frames `f'` within `still_frame_search` of `f` in the same source that are the same picture (per field) and the same colour phase and are not a duplicate read.

- **Cadence offsets only.** Only frames at a colour-cadence offset (±4n PAL, ±2n NTSC) are tried, as they are the only ones whose composite can be added. This also guards against slow motion: at ±1 frame a newsreader's slight movement passed as the same picture on Volcanoes, while at ±4 the movement is four times larger.
- **At least two copies.** A field takes copies from a source only when it has at least two, so the combine has at least three values. Every false match measured on Volcanoes was a lone pair, and with only two values the combine cannot reject an outlier between them. See *Phase 1 measurements* for the cost.
- **One input.** With still-frame stacking on, a single input is stacked too. A frame without copies passes through unchanged, as does every line that takes none, dropout flags included.

Each copy becomes an extra row in the existing per-pixel combine. Weighted rejection, differential dropout detection and low-confidence marking then apply unchanged, and with one input three or more copies give it outlier rejection and dropout recovery.

Constraints:

- **Active picture only.** Copies add samples only to active picture lines. VBI lines, the frame descriptor, audio and EFM come from frame `f` alone, because picture numbers, VITC, teletext and captions differ on every frame.
  - In NTSC, `first_active_frame_line` includes line 21 (closed captions), so lines 21 and 284 are excluded explicitly ([SMPTE 170M-2004](analogue-video-specifications/README.md); see [cvbs_signal_constants.h](../orc/sdk/include/orc/stage/cvbs_signal_constants.h)).
- **Weights.** Each copy takes its source's Weighted weight. Repeats carry independent stamper noise, so this slightly under-weights them, which is the safe side. Mean and Median simply receive more values.
- **Field agreement.** `check_field_agreement` still runs over the real inputs only. Copies are already vetted per field by the matcher.
- **Grid.** Copies use their own source's `sample_offsets(f)`. A copy on a different grid from `f` within the same capture fails the phase test and is dropped, never shifted.
- **Frame fetches.** Frames are fetched with `get_frame_copy` only; no `get_frame()` pointer is kept (see the transform3d use-after-free).

### Sample check of each copy

The block-mean test misses changes that leave a block's mean nearly where it was: a thin outline moved a few samples (the characters of `cc_test`, which move slightly over a still background), and a fade of small white text on black (the NatB credits), where too few blocks are bright enough for the level test. Each field that passes the signature tests is therefore also compared with its frame sample by sample, by `compare_field_samples` in [frame_content.h](../orc/sdk/include/orc/support/frame_content.h):

- **Residual.** The luma of both frames is low-passed by the 4-sample box, as the signatures are, and differenced. The difference is cut into blocks of 16 samples by 8 lines, and each block's mean is removed, which takes out level drift. The block's 3 lines furthest from its mean are left out: an unflagged dropout covers one to three lines, and without the excuse those dropouts gave the stills worst blocks of 10⁵ times the median.
- **Relative judgement.** Each block's residual energy is compared with the median block energy of the field pair, which is the pair's own noise, so no new reference is needed. A field fails when any block is over 5 times the median.
- **Fade.** A field also fails when the difference follows the picture's detail, `b − a = g·(detail)` fitted over every judged block, with |`g`| over 0.4 %, and more than 1 % of the blocks are over 2.5 times the median. The second condition keeps the gain drift of the stills, which moves smooth picture, from failing: it raises few blocks.
- **Edges and dropouts.** The outermost block columns are not judged: the edge between picture and blanking jitters, and on NatA it held the worst block of most pairs. Blocks touching a sample flagged in either frame are not judged. With fewer than half the blocks judged the result is undecided, and the copy is not used.

The check is a function of the two frames alone and gives the same result with them swapped, so copies still do not depend on the order frames are visited. A pair is checked once and cached for when its other frame is stacked. It runs only on the fields the signatures accepted, on worker threads, and the at-least-two-copies rule applies after it.

A gain allowance for timebase jitter on sharp detail, a block energy limit plus a share of the block's horizontal gradient energy, was measured and dropped: it barely moved NatA and let more NatB credit copies through.

### Test material

**Volcanoes, side 1 (PAL CAV), five captures:**
`~/raid/decodes/decode-script/volcanoes/DS{1..5}/ld-decode-16dc70f/Volcanoes_DD86-DS*_Side1_*_CAV_PAL_00001-54000.cvbs`. Each is about 54,003 frames (76.6 GB). Cut sections as for the other captures: `head -c`/`dd` at 1,418,758 bytes per frame, set `cvbs_file.number_of_sequential_frames`, and delete `dropout_run` rows outside the cut. Never use whole captures. DS1's picture #1 is at file frame 307. The other captures open within a frame or two of that, so cut each with a 50-frame margin and let Source Align line them up.

A 24-line luma strip survey of DS1 gave the figures below. Frame numbers are 0-based file frames in DS1. The survey had no dropout masking and saw only the strip. Its class boundaries proved wrong in places (see *Phase 1 measurements*), so they serve only to locate material.

- **Content.** About 31 % of frames are a picture matching the next frame. Over 70 holds last 100 frames or more, the longest picture holds 647 frames (9210–9856), and motion sections run up to 5,600 frames (16812–22425). The disc ends with 4,827 frames of black (49176–54002) and opens with lead-in and a white card (161–632, level ≈ 830).
- **Phase.** On held stills, the chroma correlation is about 0 at ±1 and ±3 frames, −0.6 to −0.96 at ±2, and +0.86 to +0.99 at ±4. That is the PAL 4-frame cadence the phase test relies on.
- **Noise vs slow motion.** On the worst block of the strip, the noise of a held still overlapped slow motion. That is why the same-picture test was measured on the cuts (*Phase 1 measurements*).

| Cut (DS1 file frames) | Frames | Contents |
|-----------------------|--------|----------|
| 9000–10999 | 2,000 | A flat card; held stills 9472–9854 (saturated) and 10184–10334; motion 9855–10183 and 10398–10532 between them; short blank runs |
| 22000–23999 | 2,000 | Motion into a held still at 22426–23020 (pale, chroma rms ≈ 40, so a weak phase signal), then motion |
| 48500–49499 | 1,000 | Motion into the black run at the end of the side |

**Phase 1 measurements.** `frame_content` was run on the three cuts, with dropouts masked and at a reference noise of 6.0 codes. Every class below was checked by eye on rendered frames.

- **What the cuts hold.**
  - Map graphics are held across most of 9128–10330. The survey's "motion 9855–10183" is two more map stills.
  - 22180–22400 is a map still, and 22426–23015 is a photograph of a mountain.
  - The "black" run at the end carries a small overlay near the top that changes every frame, so its frames are correctly not repeats.
- **Same picture at ±4 frames, blank frames excluded.**

  | Material | Matched |
  |----------|---------|
  | Map graphics, 9128–10330 | 94.0 % of 1,126 pairs |
  | Map still, 22180–22400 | 84.3 % of 216 pairs |
  | Mountain photograph, 22426–23015 | 88.7 % of 585 pairs |
  | Motion (9000–9120, 10340–10995, 22000–22170, 23020–23995, 48500–49170) | 1 of 2,491 pairs (0.04 %) |

  The motion match is an animated eruption plume whose change is below the block limit. Most of the missed still pairs carry a disturbance taller than 3 lines or a picture change inside the range.
- **Why the test models drift.** Without the row fit, 31 % of the held-still pairs failed. Worst blocks reached 8–18 times the noise, and the fault was the gain drift at the top of the field, not noise. With the fit, true repeats have a pair noise of 5.6–6.5 codes (10th–90th percentile) and a median worst block of 4.0 times it. The median level change is 0.1 %.
- **Unflagged dropouts.** Before the 3-line excuse, the photograph matched about 30 % of the time. The failures were short streaks the decoder had not flagged.
- **Colour phase.** On every non-blank pair found to be the same picture, ±4 frames read *same* (1,789 pairs), ±2 *opposite* (1,530 of 1,531) and ±1 and ±3 *quadrature*. The one exception is a nearly colourless frame (expected correlation 0.08), which reads *unknown*, as it should. The band-pass and comb take out luma detail near the subcarrier; raw minus the box would let it correlate at any phase.

- **Minimum copies.** Copies were searched at ±4 and ±8 frames. Requiring at least two copies left 96.5 % of still frames with copies (97.8 % with one), and no motion frame (8 with one: the eruption plume's two frames, which matched only each other, and a flat white card). Also requiring the copies to match each other cost another 2 % and caught nothing more.

**Phase 2 smoke run.** DS1 frames 9400–9599 (map stills), one input, `still_frame_search` 8: the reference σ measured 5.9 codes, and 184 frames took 2 to 4 copies (147 took 4). The 16 frames that took none, 9458–9473, are a fade-in of the plate boundaries, checked by eye. Every frame without copies was byte-identical to the input.

**Phase 3 measurements.** Release build, `still_frame_search` 8, the Weighted mode.

- **Noise gain (task 1).** Two 200-frame cuts of all five captures, DS1 9400–9599 (map stills) and 22500–22699 (the mountain photograph). The captures line up frame for frame by picture content, DS3 four frames later than the others. Noise is the trimmed RMS of active picture, after removing each line's offset, in the full band and in the box-filtered luma below the subcarrier.

  | Measure | Map stills, full / low band | Photograph, full / low band |
  |---------|-----------------------------|-----------------------------|
  | Repeats on one disc, frame `f` against `f` + 20 | 6.9 / 6.9 dB | 6.7 / 6.8 dB |
  | Repeats on five discs, frame `f` against `f` + 20 | 6.8 / 6.9 dB | 6.7 / 6.8 dB |
  | Five discs against one, without repeats | 1.3 / 1.1 dB | 1.4 / 1.3 dB |
  | Hold-one-out, reference 4 discs without repeats | −0.8 / −1.6 dB | −1.0 / −1.5 dB |
  | Hold-one-out, both sides with repeats | 5.5 / 6.4 dB | 5.9 / 6.3 dB |

  - **Within one output.** Frames `f` and `f` + 20 of one output share no copies, so their difference measures the output's own noise. Repeats give about 6.8 dB, close to the 7.0 dB of five values. Five discs give only about 1.3 dB, because the discs share most of their noise: it was recorded into the stamper.
  - **Hold-one-out.** A reference stacked from the other discs carries that same stamper noise, so against it, taking the stamper noise out of the held-out disc reads as a loss. Stacked with repeats on both sides, the two carry the same averaged stamper noise, and the residual falls by 5.5–6.4 dB on every disc. Repeat stacking therefore takes noise out without changing the picture.
- **Where copies are used (task 2).** Single inputs, with each copy checked by comparing frame and copy in 16 × 8 blocks, on the median of each block's lines.

  | Material | Frames with copies | Finding |
  |----------|--------------------|---------|
  | Volcanoes DS1 9000–10999, labelled by eye | 1,079 | Copies on every held frame of 9215–9456 and 9475–10314 but three (9218, 9478, 9482); none on the zoom (9000–9110), the fades (9200–9214, 9457–9474, 10315–10336), the blank frames or the eruption footage. No copy shows a picture change. |
  | ComN 20000–20399, a different map sheet every frame | 0 of 400 | No reference noise can be measured; the output is byte-identical to the input. |
  | ComN 1600–1999, one map held | 400 of 400 | Correct: the frames differ only by noise. |
  | NatA 1200–1599, photo stills | 377 of 400 | No copy shows a picture change. |
  | NatB 10400–10699, credits and a title sequence | 100 of 300 | The held steps are right. Frames 0–10 (the credits) took copies across a fade of white text on black, a false match: few blocks are above 10 % of white, so the level test does not see the fade. |
  | `cc_test` 9000–9399, NTSC animation | 254 of 400 | Mostly false matches. The characters move slightly over a still background, and the stacked frames redraw their outlines where the copies have them. Line 21/284 and every other VBI line are byte-identical to the input. |

  The block means `same_picture` compares barely change when a thin outline moves a few samples, so limited animation passes as a still. In the forest scene of `cc_test` (frames 4–217, 765 copy pairs) most copies are false; the few true ones hold a drawing for two frames or more (78–85). The snow scene (288–399, 276 pairs) is a held painting. Phase 4 adds a sample check for these (see *Sample check of each copy*).
- **Cost (task 3).** 200 frames, written by CVBS Sink, on 16 threads; the least of five runs.

  | Case | Off | On | Peak memory, off → on |
  |------|-----|----|-----------------------|
  | Five discs, map stills | 2.8 s | 10.1 s | 1,336 → 1,514 MB (+13 %) |
  | Five discs, motion (DS1 23100–23299) | 2.8 s | 3.4 s (+22 %) | 1,336 → 1,482 MB (+11 %) |
  | One disc, map stills | 0.27 s (pass-through) | 2.3 s | 245 → 575 MB |
  | One disc, motion | 0.26 s (pass-through) | 0.60 s | 245 → 561 MB |

  - **Off.** Output is byte-identical to the Phase 1 Stacker (`ae7fd5f5`) and costs the same: 2.9 s against 3.1 s and 1,336 MB against 1,337 MB on five discs.
  - **Stills.** The combine takes about 87 % of the time. It has five times as many values a sample, each disc and its four copies, and its cost goes with the values.
  - **Motion.** Each source frame's signature costs about 4 ms of CPU, now built in parallel. What remains is the fetching and comparing on the frame-fetch thread.
  - **Memory.** The signatures (about 550 kB each, mostly the chroma band) of the search window and an 8-frame lookahead, for every source.
  - **Refetching.** Fetching frames is not what costs, so no frame-copy cache was added.

  Every case is over the +5 % time limit, and both five-disc cases over the +10 % memory limit, for design review.

**Phase 4 measurements.** Release build, `still_frame_search` 8. The block size and limits (task 2) were set on every field pair the Phase 3 runs accepted, first in a prototype and then with `compare_field_samples` itself; the two agree.

- **Field pairs passing the sample check.**

  | Material | Field pairs | Pass | Worst block, median / 99th percentile |
  |----------|-------------|------|----------------------------------------|
  | Volcanoes DS1 9000–10999 (labelled) | 8,351 | 100 % | 2.2 / 2.9 × the median |
  | ComN 1600–1999, one map held | 3,096 | 100 % | 2.1 / 2.6 |
  | NatA 1200–1599, photo stills | 2,778 | 99.5 % | 2.7 / 4.6 |
  | NatB 10400–10699, held steps (frames 11–299) | 470 | 99.6 % | 2.3 / 4.5 |
  | NatB credits (frames 0–10) | 18 | 0 % (13 on a block, 5 as a fade) | 6.7 / 17 |
  | `cc_test` forest (frames 4–217) | 1,231 | 6.3 % | 29 / 275 |
  | `cc_test` snow (frames 288–399) | 414 | 90.8 % | 3.0 / 6.7 |

  - **NatA.** The worst stills are Test Card F, whose frequency gratings and sharp edges leave residual from timebase jitter. The 5 × limit sits just above them.
  - **NatB credits.** The fade is 0.6–2.2 % between copies (detail gain), with 1.8–8.5 % of the blocks raised. On Volcanoes and ComN no field pair has more than 0.25 % of its blocks raised, and on the NatB held steps 1.5 %. NatA has up to 7 % raised (99th percentile 2.3 %), but its detail gain stays under 0.6 %, and only 0.1 % of its pairs fail as a fade.
  - **The forest.** Every forest pair that passes shows the same drawing held, checked by eye: the difference is noise, with at most a faint trace at a highlight. The target of under 1 % assumed every forest copy false; the copies that remain are true.
  - **The snow scene** is a held painting, checked by eye at full resolution: the differences are horizontal noise streaks, and the fields that fail do so on faint residual in the grass detail. That errs on the safe side.
- **Copies used (task 4), against Phase 3.**

  | Material | Frames with copies, Phase 3 → 4 | Finding |
  |----------|-------------------------------|---------|
  | Volcanoes DS1 9000–10999 | 1,079 → 1,079 | Every labelled held frame keeps its copies. |
  | Volcanoes, five captures, 9400–9599 and 22500–22699 | unchanged | Every output, one disc or five, is byte-identical to Phase 3's, so the noise gain is unchanged: 6.8 dB from repeats. |
  | ComN 1600–1999 and 20000–20399 | 400 → 400, 0 → 0 | Unchanged. |
  | NatA 1200–1599 | 377 → 377 | Unchanged. |
  | NatB 10400–10699 | 100 → 92 | The credits (0–9) take none. Frame 167, a true still, loses its copies: one of its two fails on two blocks at 5.6 × along vertical edges, which leaves one. |
  | `cc_test` 9000–9399 | 254 → 83 | The forest keeps copies only on 47–52 and 78–85, where a drawing is held; the snow scene keeps them on 72 of the 78 frames it had. |
- **Cost (task 4).** As in Phase 3, against the Phase 3 stacker (`723d999c`) run alternately on the same machine; the median of three runs.

  | Case | Phase 3 | Phase 4 | Peak memory |
  |------|---------|---------|-------------|
  | Five discs, map stills | 10.2 s | 11.0 s (+8 %) | unchanged (1,549 MB) |
  | Five discs, motion | 3.4 s | 3.4 s (+2 %) | unchanged |
  | One disc, map stills | 2.32 s | 2.49 s (+7 %) | unchanged |
  | One disc, motion | 0.59 s | 0.59 s | unchanged |

  - A field comparison takes about 0.46 ms of CPU. Each pair is checked once, when its first frame is stacked, and a frame's checks run in parallel; what is left on stills is about one comparison and the fetching of the frames compared, on the frame-fetch path, ahead of the combine.
  - Motion costs nothing: the signatures reject its frames before any sample is compared.
  - Both still cases are over the +5 % time limit, for design review. Checking a batch of frames ahead in parallel, as the signatures are built, would take most of the remaining time off the frame-fetch path.
- **Functional test (task 5).** `cc-test1.tbc` file frames 9100–9147, from the forest scene, at `test-data/still-stacking/cc-test1-forest-9100`: the Phase 3 stacker took copies on 29 of the 48 frames, the Phase 4 stacker on none, against a measured reference noise (8.9 codes), and the output is byte-identical to the input.

**Further sources.** A survey sampled every 100th frame against the frames 4 and 8 later, without dropout masking and not checked by eye. Its blank and repeat shares are reliable; it cannot tell slides from motion.

| Source | Format | Repeats | Repeat run (0-based frames) | Use |
|--------|--------|---------|-----------------------------|-----|
| `~/raid/decodes/AIV_Domesday_20250115/{ComN,ComS}/DS{2,4,6,7,8,10}_*.tbc` | PAL `.tbc`, 6 captures | 3.3 % | 1600–2000 (one map, held) | Map sheets at 19600–31600, every frame a different sheet: no copies may be used there |
| `~/raid/decodes/AIV_Domesday_20250115/NatA/*.tbc` | PAL `.tbc`, 6 captures | 3.7 % | 1200–1600 | Photo stills among video |
| `~/raid/decodes/AIV_Domesday_20250115/NatB/*.tbc` (CLV) | PAL `.tbc`, 6 captures | 3.3 % | 10400–10700 | End credits and a title sequence built up in held steps, not film |
| `~/raid/decodes/cc_test/cc-test1.tbc` | NTSC `.tbc`, 10,000 frames | 2 % | 9100–9200 | NTSC cadence; real captions on line 21 (`cc.txt`), which copies must never touch |
| `~/raid/decodes/bgbirds/set1_side1.tbc` | PAL `.tbc` | 0.9 % | 800–1000 | A noisy disc (reference σ about 18 codes) |

`.tbc` sources are cut by whole fields (1135 × 313 PAL or 910 × 263 NTSC 16-bit samples a field) and loaded through the TBC source. RF originals that would need decoding first are in `~/raid/library`: Apple Visual Almanac (an NTSC stills disc, 3 captures), EcoDisc (a 700-frame hold at 15500–16200 on side 1), City-Disc and the GGV1069/GGV1011 test discs.

## Phase 1 — Frame content measurement in the SDK support tier

| # | Task | Acceptance criteria |
|---|------|---------------------|
| 1 | Add `orc/support/frame_content.h` and `orc/sdk/src/frame_content.cpp`: `FieldSignature` and `build_field_signatures(...)` (composite and YC luma, with dropout runs). | Unit tests on synthetic frames: block means match the synthetic level; chroma does not move them. |
| 2 | Add `same_picture(a, b, sigma)`, a per-field decision with the counts of judged, failed and disturbed blocks, the pair's noise and its level change (see *Same picture*). Set the limits from the Volcanoes cuts: held stills against motion, with dropouts masked. | Tests: same picture with independent noise matches; a gain drift down the field still matches; a change in one block is rejected; a 2-line unflagged spike is excused; a 2 % fade is rejected; a change everywhere is rejected on its noise; blocks reading a flagged sample are not judged; too few judged blocks gives "undecided", not a match. On the Volcanoes cuts, under 0.1 % of motion frames match at ±4, and the measured margins are recorded under *Test material*. |
| 3 | Add `build_chroma_signature(...)` and `colour_phase_relation(a, b)` returning same, opposite, quadrature or unknown, decided relative to the chroma level, and `is_duplicate_read(...)`. | Tests on synthetic PAL and NTSC composite: frames 4 apart (PAL) and 2 apart (NTSC) are the same phase; 1 apart are quadrature (PAL) or opposite (NTSC); a 1-sample grid slip is not the same phase; a chroma-free frame gives unknown; bit-identical frames are duplicates. On the Volcanoes held stills, ±4 frames reads as same phase and ±2 as opposite on every frame. |
| 4 | Add the noise estimate: `BlockNoiseMeter` over matched pairs' residuals, and the within-frame anchor as a starting bound. | Tests: both track the synthetic σ within 10 % at σ = 5 and σ = 20 codes. The anchor's bias on real noise is recorded under *Noise*. |
| 5 | Register the header in [sdk_headers.yaml](../orc/sdk/sdk_headers.yaml), then regenerate the allowlist (`tools/gen_sdk_header_allowlist.sh`) and the [plugin-sdk.md](../docs/technical/plugin-sdk.md) tables (`tools/gen_sdk_header_docs.sh`). | `ctest -L sdk` passes, including `SdkHeaderManifestSync` and `SdkHeaderDocsSync`. |

## Phase 2 — Repeat stacking in the Stacker

| # | Task | Acceptance criteria |
|---|------|---------------------|
| 1 | Add the parameters `still_frame_stacking` (bool, default `false`) and `still_frame_search` (frames either side, default 8, range 2–50). | Parameter round-trip and default-parity tests. With the option off, output is bit-identical to the current Stacker on the existing stacker test fixtures. |
| 2 | With the option on, `StackerStage::process()` builds `StackedVideoFrameRepresentation` even for one input, and `execute()` stops returning `inputs[0]`. | Single-input test: output differs from the input only on frames that have copies. |
| 3 | `StackedVideoFrameRepresentation` finds copies per source with the Phase 1 matcher at cadence offsets, keeps them only when there are at least two (see *Repeat stacking*), caches signatures in an LRU keyed by (source, frame), and measures a per-source reference σ per segment with `BlockNoiseMeter`, starting from the within-frame anchor (see *Noise*). | Tests: the copies found for `f` are the same whether frames are visited forwards, backwards or at random; a frame with one matching copy takes none; frames off the cadence are never tried. |
| 4 | Extend `stack_frame` and `stack_frame_yc` to take copies as extra rows of `all_frames`, with per-field validity from the matcher and weights from their source. Only active lines take copies; NTSC lines 21 and 284 never do. | Tests: noise falls by about √N on a synthetic still run; a dropout present in one copy is filled from the others; VBI lines are byte-identical to frame `f`'s; audio and EFM are unchanged; a copy with a different picture in one field adds only the other field. |
| 5 | Keep the field agreement check over real inputs only. Drop copies on a different grid from `f` rather than shifting them. | Test: a copy one sample out is not used. |
| 6 | Log each source's reference σ at INFO when it changes by more than a quarter, and the copies used per frame at DEBUG. The Stacker has no stage report to add counts to. | Shown in the log. |
| 7 | Update the Stacker `instructions.md`: the new parameters, when to use them, that copies never touch VBI, audio or EFM, and that the DEBUG log shows where copies were used. | Docs match the behaviour. |

## Phase 3 — Validation and performance

| # | Task | Acceptance criteria |
|---|------|---------------------|
| 1 | Run a hold-one-out test on the Volcanoes held stills with its 5 captures (DS1–DS5): the reference is a stack of 4 discs without repeats; compare the 5th disc alone with and without repeat stacking, and all 5 with and without. | A measurable dB gain on held stills; no change on film and map stills (zero copies used). |
| 2 | Measure where copies are used on real discs: the Volcanoes cuts, the AIV Domesday ComN map stills and NatB film, and `cc_test` (see *Test material*). Label the frames of one Volcanoes cut by eye as held or not. | Copies used on the held frames of the labelled cut and on none of the others; none on the map stills or the film; `cc_test`'s caption lines byte-identical to the input. Results recorded under *Test material*. |
| 3 | Measure export time and memory (Release build) with the option off and on, on a stills disc and on film, per [AGENTS.md §6](../AGENTS.md). Add a per-source frame-copy cache bounded by the window only if refetching dominates. | Off: no measurable change. On: cost recorded here; anything over +10 % memory or +5 % time is raised for design review. |
| 4 | Add a functional test, `orc-core-functional-tests-still-stacking`, on a short real cut, skipped when the sample is absent. | Labelled `functional`, not in the unit lane. |

## Phase 4 — Sample check of each copy

| # | Task | Acceptance criteria |
|---|------|---------------------|
| 1 | Add the sample check to [frame_content.h](../orc/sdk/include/orc/support/frame_content.h): a per-field comparison of two frames (composite or YC luma, with both frames' dropout runs) returning the worst block's residual energy relative to the field pair's median, the number of blocks over the limit and the decision (see *Sample check of each copy*). | Unit tests on synthetic frames: the same picture with independent noise passes; a gain drift down the field passes; a 1-pixel outline moved 2 samples in one area fails; a 1 % contrast fade of small bright text on black fails; blocks reading a flagged sample are not judged; the result is the same with the frames swapped. |
| 2 | Measure the check's block size and limits on real pairs: copies the Phase 3 runs accepted on the labelled Volcanoes cut, the ComN held map and the NatA photographs (to keep), against the `cc_test` forest scene (frames 4–217) and the NatB credits (frames 0–10, to reject). Decide the `cc_test` snow scene by eye at full resolution. Record the margins under *Test material*. | Over 99 % of the copies on the Volcanoes, ComN and NatA stills pass; under 1 % of the `cc_test` forest copies and none of the NatB credit copies pass. |
| 3 | Apply the check in `StackedVideoFrameRepresentation::find_still_copies` to every candidate field the signatures accept, before the at-least-two-copies rule. Fetch the planes with `get_frame_copy` and run the checks of a frame on worker threads, as the signatures are built. | Unit tests: a synthetic still with one copy whose outline moved 2 samples does not use that copy; a field left with one copy takes none; copies are the same whether frames are visited forwards, backwards or at random; every existing still-frame test passes. |
| 4 | Re-run the Phase 3 measurements with the check: the noise gain on the two Volcanoes cuts, copies on the labelled cut, ComN, NatA, NatB and `cc_test`, and the Release cost. | Noise gain within 0.3 dB of Phase 3's; at least 99 % of the labelled held frames still take copies; no false copies left on NatB or the `cc_test` forest scene. Results and cost recorded under *Test material*; anything over Phase 3's cost by more than 5 % time or 10 % memory is raised for design review. |
| 5 | Extend `orc-core-functional-tests-still-stacking` with a 48-frame cut of the `cc_test` forest scene, skipped when the sample is absent. | The forest frames take no copies; the output is byte-identical to the input; the Volcanoes tests still pass. |
| 6 | Update the Stacker [instructions.md](../orc/plugins/stages/stacker/instructions.md): describe the sample check under the copy criteria and replace *Limits* with what the check still misses. | Docs match the behaviour. |

## Out of scope

- **Luma from every repeat.** Below the chroma band, every repeat could add to luma whatever its phase, which would quadruple the usable PAL copies. In the composite domain that needs a band split, which is not attempted here.
- **Partial repeats.** Stacking only the static part of a picture (a menu with a moving cursor) needs block-level copy decisions.
- **NTSC 3:2 pulldown fields.** Repeated film fields are field repeats, not frame repeats, and are not used.
