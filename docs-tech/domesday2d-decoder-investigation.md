# Domesday2D: a PAL still-frame chroma decoder

This document has two parts:

- **Part A** records what was found about the existing decoders, dp111's work, the literature and the test tooling.
- **Part B** sets out phased investigations. They test each theory in turn and narrow the field to one decoder that can be implemented.

**Target.** The decoder recovers the static PAL pictures that LaserVision discs carry, BBC Domesday in particular (photographs, OS maps, text overlays, test cards). It does this from one 4fsc composite frame, as accurately as possible. It must beat every existing 2D decoder (`pal2d`, `transform2d`) on that task when measured against a known source.

**Scope.** Domesday2D is a pure 2D decoder. Each frame is decoded from its own two fields only. It has no temporal reach and no held-still mode, and it does not use neighbouring frames, even when they carry the same picture. Repeated stills are left to the stacker, upstream.

**Status.** Every claim below is marked either:

- **[V]** verified: read in the cited source or code, or computed and shown.
- **[H]** hypothesis: still to be tested in Part B.

---

# Part A — Findings

## A1. Existing decode-orc PAL decoders

All decoders live in [`orc/plugins/stages/sinks/common/decoders/`](../orc/plugins/stages/sinks/common/decoders/). `pal2d`, `transform2d` and `transform3d` are one `PalDecoder` class with three settings of `ChromaFilterMode` ([`palcolour.h:33-58`](../orc/plugins/stages/sinks/common/decoders/palcolour.h)). The sink factory is `make_decoder()` in [`video_sink_stage.cpp`](../orc/plugins/stages/sinks/common/video_sink_stage.cpp).

### PalColour (`pal2d`) [V]

This is W.A. Steer's line-locked 2D FIR decoder, the architecture of BBC RD 1986/2 Fig. 23(c).

- **Burst.** Measured per line over samples 98–138. It averages the line with its ±1 and ±2 field-line neighbours, detects the V-switch, and normalises to a unit vector. The vector rotates fixed sin/cos reference tables, so the decoder runs no PLL.
- **Chroma filter.** 7 field lines × 15 taps with a radial raised cosine. The bandwidth is `1.1 MHz / 0.93`, where 0.93 is described as "empirical for 4fsc LaserDisc". Taps ±1 and ±3 are applied in quadrature with PAL sign structure. U and V share coefficients.
- **Luma.** A separate filter (taps 0 and ±2 only, ±2 weighted by 0.2) extracts chroma, which is re-modulated and subtracted from composite. There is no notch.
- **Geometry.** It works **within one field**. The nearest same-phase-inverted partner, ±2 field lines away, is **4 frame lines** from the current line in the picture.

### Transform PAL 2D (`transform2d`) [V]

This is a port of Easterbrook's pyctools-pal, also per field.

- **Tiles.** 32 samples × 16 field lines, Hann window, 50 % overlap-add.
- **Test.** Only horizontal bins 4–12 (fsc ± 2.2 MHz) are tested. Each bin is paired with its reflection about the carrier. The pair goes to chroma only if the magnitude ratio is ≥ `t` (default 0.4); otherwise it goes to luma.
- **What is missing.** Only the binary keep/discard mode exists; there is no amplitude-matching mode. Per-bin thresholds are supported internally (80 entries) but not exposed.
- **Known failure.** At sharp edges carrying both U and V (green), the U and V vertical spectra overlap. The magnitude test then rejects real chroma into luma, leaving about 25 codes of "||||" dots over ±4 field lines. No threshold fixes it (memory note on transform2d edge dots, audited 2026-10-30).

### Transform PAL 3D (`transform3d`) [V]

- **Tiles.** 16 samples × 32 frame lines × 8 fields.
- **Reach.** It looks 1 frame behind and 4 ahead. The tile reaches 2 frames back, and the missing fields are filled with blanking.
- **Relevance to stills.** For stills it gains from the temporal axis, but only if the picture really is held across the tile.

### Current figures [V]

These come from [`orc-tests/videosynth/baselines.json`](../orc-tests/videosynth/baselines.json): YUV RMS as a percentage of 16-bit full scale, composite decode against **the same decoder's** Y/C decode.

| Decoder | Multiburst Y | Bars U | Bars V |
|---|---|---|---|
| pal2d | 7.96 % | 0.093 % | 0.066 % |
| transform2d | 0.474 % | 0.372 % | 0.180 % |
| transform3d | 0.296 % | 0.181 % | 0.105 % |

What this shows:

- pal2d keeps flat colour clean and lets luma detail near fsc through as cross-colour.
- transform2d is the reverse.
- No decoder wins both. That gap is what Domesday2D has to close.
- These figures measure **separation only**. The reference shares the decoder's demodulation and low-pass, so no test today compares a decode with the **source** picture.

### Held stills (out of scope) [V]

The stacker already averages a held picture's same-phase copies (PAL frames 4 apart) before the sink sees it. Across frames 2 apart, PAL chroma inverts exactly, so a sum and difference of the two would separate Y and C with no filtering. That multi-frame decoding is deliberately excluded: Domesday2D works on one frame. A stacked held still is simply a less noisy single frame for it to decode.

## A2. dp111's decoder (newdecode)

Source: <https://github.com/dp111/domesday-aiv-restore> (GPL-3.0, five commits, 2026-10-26 to 11-01).

**What it is not.** It is not a PalColour or Transform PAL derivative, so there is no decoder source to diff [V]. It calls the unmodified `ld-chroma-decoder` twice:

- `transform2d` with threshold **0.7** (stock is 0.4), as a starting frame.
- `transform3d`, as a fallback.

**What it is.** `newdecode/` is a **MAP inverse-problem solver**: a GPU L-BFGS fit in PyTorch with about 100 `PALDEC_*` switches. It runs at about 1.5 fps on an RTX 3060-class card and needs about 32 GB of RAM per chunk.

- **Forward model** (`palmodel.py`): `comp = AY·Y + B + GU·U·cos(πk/2 − φ_l) + GV·s_l·V·cos(πk/2 − φ_l − cV)`. Phase advances 90° per sample and (90 − 0.576)° per line, with one burst phase `beta0` fitted per field. There is no per-line phase or timing.
- **Data term.** Residual spectrum per row, weighted by 1/noise-PSD, so 5.5–7 MHz disc noise is left to the prior.
- **Priors.**
  - Charbonnier (edge-preserving total variation) on luma gradients.
  - Anisotropic Charbonnier on Cb/Cr, horizontal weight greater than vertical.
  - **Chroma bandwidth is set by the prior, not by a filter.**
- **"Hanover stage".** A second pass penalises sustained same-field line alternation of chroma, masked at vertical chroma steps. It stands in for a PAL-D delay line.
- **Held runs.** Each repeat is solved on its own, with per-frame offsets and a 2×2 crosstalk matrix. 16-pixel blocks are rejected against the median, then the inliers are averaged.
- **Pre-processing.**
  - `deghost.py`: an exact IIR inverse of a one-pole edge smear, c = 0.16, τ = 9 samples (≈ 0.4 µs), fitted to Community South VITS.
  - `deherring.py`: FM-beat cancel.
- **Post-processing.** `lumaeq.py` divides decoded luma by the multiburst response, cut only, floor 0.75.
- **Calibration.** `model_params.json` holds GU 0.641, GV 0.8877, cU −136.7°, cV −46.8°, fitted to Community South frame 3030. It claims ld-chroma-decoder reads EBU bars **7 % under-saturated**.

**Weaknesses [V/H].**

- Calibration and every threshold come from **one capture set** (Community South).
- The quality benchmark truth is **ld-chroma-decoder output re-encoded with ld-chroma-encoder**, so absolute accuracy is circular.
- The chain compensates for itself:
  - Deghost lifts 4.43 MHz by about 1.18.
  - Fallback frames are then scaled by 0.92.
  - The luma EQ later removes the over-lift.

  This suggests the smear model over-corrects.
- His README says 90–96 % of noise above 3.5 MHz is shared by all copies from one glass master. That agrees with this project's own pressing-noise model.

**What carries over to decode-orc is the ideas, not the code:**

- A noise-weighted data term.
- Edge-preserving chroma regularisation in place of fixed chroma filters.
- A masked anti-Hanover term.
- Measuring the chain response from VITS.
- An explicit model of the channel's edge smear.

The earlier review of his work recommended against a GPU model decoder for decode-orc. That still stands, so any model-based idea below must run on the CPU at sink speed.

**Related work.** vrunk11's HVD decode-orc plugin (<https://github.com/vrunk11/hvd-cvbs-decoding>) is also variational: IRLS/CG, a joint burst + V-switch lock, and a re-encode anchor loop. No independent evaluation of it was found.

## A3. Literature and patents

The PDFs of the BBC reports were collected during research but are not stored in the repo.

### Transform PAL

**EP1175102**, Easterbrook / BBC, priority 2000, **expired**. <https://patents.google.com/patent/EP1175102A2/en> [V]

- **Two modes are claimed:**
  - Amplitude-matching: set the larger of each bin pair to the smaller, leaving phase alone.
  - Threshold reject (example value 40 %).

  decode-orc and ld-decode implement only the second.
- **Stated weaknesses:**
  - A ≈ 5.5 MHz low-pass makes the chroma sidebands asymmetric, causing "patterning" and limiting chroma to about 1.1 MHz.
  - In 2D, vertical colour transitions where U and V both sit at ±72 c/aph produce cross-luminance. This is the "||||" dot failure above.
- **Interlace.** It allows zero-line insertion or a block skewed by one line per field.

Easterbrook's page (<http://www.jim-easterbrook.me.uk/pal/>) adds that 2D "was encouraging", that luma is sometimes symmetric enough to be taken for chroma, and that 3D was "vastly better". He suggests software Transform PAL for "extracting high quality stills" [V].

**US7872689B2 / US8279350B2**, Snell & Wilcox (M. Weston), priority 2002, **expired**. This extends Transform PAL [V]:

- The keep/discard decision becomes a **lookup table** indexed by both magnitudes and by frequency, **trained on material with known RGB**.
- **Sideband averaging** removes U/V crosstalk caused by upper-sideband attenuation. **Lower-sideband substitution** restores high-frequency chroma loss.
- **Low-frequency luma biases the decision.**
- Processing is done on demodulated chroma.

### Field and frame combs for stills

**BBC RD 1988/11**, C.K.P. Clarke, "PAL decoding: multi-dimensional filter design" [V]:

- A **312-line** delay has the PAL-switch sense of a 2-line delay and suits still and moving pictures, but does **not** suppress Hanover bars. A BBC production decoder using it was shown in 1984.
- With picture (frame) delays, "still pictures can be decoded with full spatial resolution and without any significant cross-effects".
- Differential-phase distortion leaves residual subcarrier in flat colour for every PAL-modifier method.

**US5055920**, BTS/Philips 1989, expired: a frame-store still decoder [V].

- Sum and difference across a 2-frame (4-field) delay.
- A recursive weight ramps over 8 or more fields, then freezes.
- Fades to a comb decoder on motion.

**How the Domesday masters were archived** (<https://atsf.co.uk/dottext/domesday.html>) [V]:

- The 1" masters were transferred with a BBC R&D Transform PAL decoder "reprogrammed for single frames".
- The original players' RGB output used "a special two-field decoder", giving stills about 5 MHz of bandwidth.
- "The videotapes show none of the patterning and speckling that is clearly visible on the videodiscs."

So the precedent for this whole design is **two-field (single-frame) decoding of stills**.

### Carrier geometry across a still frame [V]

Carrier-phase shift between two lines n lines apart, computed with fsc = 283.75·fH + 25 Hz and the V-switch applied:

| Lines apart | U | V | Picture relationship for a still |
|---|---|---|---|
| 2 (same field) | 181.15° | 181.15° | 4 frame lines away (PalColour's ±2 partner) |
| **312 (other field)** | **179.71°** | **179.71°** | **1 frame line away** |
| 313 (other field) | 90.29° | 270.29° | 1 frame line away, on the other side |
| 1250 (2 frames) | 180.00° | 180.00° | the same pixel |

In a woven still frame, one vertical neighbour of every line is a 312-line partner. That neighbour carries U and V **both inverted** to within 0.3°, at a quarter of the vertical distance PalColour has to reach. This is the basis of a two-field decoder and the main hypothesis in Part B (H1). Which side (above or below) holds the 312 partner depends on field order and must be confirmed numerically.

### Other separation approaches

- **Clean PAL**, BBC RD 1980/1, EP0015134B1, US5434627A (all expired). An encoder-side pre-comb. It is relevant only if Domesday mastering used it, which is not known.
- **Adaptive combs.** EP0645935 (Snell, a lookup table instead of the comb subtractor), EP1263241A1 (per-pixel filter choice from H/V/T differences) and US5173768 (Thomson) are all expired. **US9432648** (STMicro adaptive PAL field comber) is **active until 2034**, so its per-region 2D/field/frame selection must not be copied.
- **Demosaicking is the same problem**: a baseband signal plus modulated chroma.
  - Leung, Jeon & Dubois, "Least-squares luma-chroma demultiplexing", IEEE TIP 20(7) 2011, designs the separation filters by least squares on training images. <https://www.site.uottawa.ca/~edubois/lslcd/article/TIP-06195-2010.R1_2col.pdf>
  - Condat, ICIP 2009, takes a variational approach. <https://hal.archives-ouvertes.fr/hal-00814067>
- **Neural decoders.** The ones found (libchromadec's nnTransform3D, ldzeug2) are **NTSC-only**. No PAL model or peer-reviewed paper was found.

### LaserVision channel specifics [V]

- **IEC 60856 §9.1.6.** The recorded video is **group-delay pre-distorted**: 0 ns at 0.5 MHz, −85 ns at 4.0 MHz, −135 ns at 4.43 MHz, −200 ns at 4.8 MHz. The player's replay low-pass filter is meant to cancel this ([spec](analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md)).
  - An RF decode has no replay filter, so this delay may still be in the TBC.
  - 135 ns is about 2.4 samples of chroma lead at 4fsc, with dispersion across the chroma band.
  - [H] It may explain part of the 15–21 % Domesday edge tail measured earlier, and dp111's deghost fit.
- **IEC 60856 §9.1.3.**
  - VITS on lines 19/20/332/333 (CCIR 473-3): multiburst at 0.5, 1.3, 2.3, 4.2, 4.8 and 5.8 MHz, and chroma bars at 20/60/100 %.
  - Lines 22 and 335 are blank, for disc-noise measurement.

  These give a per-capture reference for the response.
- **Replay bandwidth and radius loss.** Replay HF approaches 5 MHz, so the upper chroma sideband is cut to about +0.5 MHz. Player HF lift is keyed from burst level; inner CAV tracks lose HF.
  - [H] The symmetry test is biased wherever the sidebands are unequal.
  - This agrees with issue #345, where the stacker's chroma mismatch turned out to be gain.

### Decoder-stress test elements [V]

Source: F. Hendil, *PM 5544 pattern generator*, Philips 1970. <https://frank.pocnet.net/other/sos/Philips_PM5544_PM3400_Publication.pdf>

- **Left of the circle:**
  - a bar of **line-alternating ±(R−Y)**;
  - steady +(R−Y) and −(R−Y) bars;
  - G−Y rectangles.
- **Right of the circle:** the same with ±(B−Y).
- A red square on yellow tests chroma/luma delay. Grid lines are 230 ns wide to limit cross-colour. Gratings run 0.8–4.8 MHz.

What the elements reveal:

- Delay-line amplitude errors show as "Venetian blinds".
- A non-orthogonal demodulator colours the alternating bars, which should decode grey.

[H] What the alternating bars mean for 2D decoders:

- Line-alternating V times the V-switch is an *unswitched* carrier that lands on the U carrier's spectral position. A 2D decoder cannot tell it from U by position.
- So it is a phase/crosstalk trap, not a cross-colour test, and "correct" output is grey.
- FuBK's "+V/±U anti-PAL" fields work the same way.

**Licensing.** Card artwork (PM5544, Test Card F/J/W photographs) may be under copyright. The functional elements can be synthesised from the published parameters without the artwork, branding or name. The bundled videosynth `TESTCARD-F` asset is already in use.

## A4. videosynth as the reference encoder

This section describes the pin `139b31e` that the investigation started from. The pin is now `8217fec` ([`flake.lock`](../flake.lock)), which closes gaps 1, 2 and 4 below (videosynth#13, #14); Phase 1 closes gap 5, and gap 3 is partly closed (see the Phase 1 outcome).

**What videosynth does [V]:**

- **Input.** Float EXR (720×576, gamma 1) or MKV, converted to BT.601 10-bit 4:4:4.
- **Horizontal mapping.** Each 4fsc sample takes the **nearest-left source pixel** (`x·720/922`) over a 922-sample window from sample 177. There is no interpolation, so the zero-order-hold images reach near fsc. Luma is band-limited only if the section sets `luma_bandwidth`.
- **Chroma.** U on sin and V on cos, through a **33-tap Hamming-windowed sinc low-pass at 1.3 MHz** (Q30), exactly π/2 per sample. The burst is at ±135° with Bruch blanking.
- **Edges.** Hard edges at the window ends.
- **Fields.** Progressive rows are split into fields (upper field = even rows), so a still is **exactly static across both fields**.
- **Impairments that exist:**
  - `channel.timing_offset_samples` (fractional);
  - `channel.edge_smear` (one-pole on Y and C);
  - `channel.fm_beat`;
  - `chroma_error` (gain, phase, per-line jitter);
  - capture and pressing noise including `level_term` (the Domesday model in [`orc-tests/videosynth/projects/domesday/`](../orc-tests/videosynth/projects/domesday/));
  - dropouts;
  - held stills (`duration_frames`).

**Gaps [V]:**

1. **No truth output.** videosynth writes no source-referenced YUV, so it has to be re-derived from the published constants or added upstream.
2. **No interpolating resampler.** Nearest-pixel luma puts false energy at 4.43 MHz.
3. **No Domesday-like assets.** There are no photographs, maps, coloured text, static or chroma zone plates, or decoder-stress elements.
4. **No LaserVision channel model.** Missing are:
   - group-delay pre-distortion;
   - a replay response with a truncated upper sideband;
   - a radius-dependent HF loss;
   - differential gain/phase;
   - line-alternating V→U crosstalk;
   - inter-field misregistration.
5. **The harness has no source-referenced metrics.** Missing are RGB PSNR, ΔE, a U/V vector error split into magnitude and phase, cross-colour masked to luma detail, cross-luminance masked to chroma edges, and edge rise/overshoot. The window edges must also be excluded.

---

# Part B — Investigation phases

**Rules for every phase:**

- Prototypes and measurement scripts go under `tools/domesday2d-bench/` (Python with numpy/scipy, like [`tools/stack-benchmark/`](../tools/stack-benchmark/)). They do not ship.
- Generated media stays out of git.
- Synthetic acceptance runs on the videosynth suite (local only, `BUILD_VIDEOSYNTH_TESTS=ON`, not in CI).
- Real-disc checks use the Domesday captures in `~/raid/decodes`, cutting N-frame sections, never whole captures.
- Each phase ends with its measured results written into this document and a go/no-go note for the hypotheses it gates.

**Hypotheses under test:**

| Id | Hypothesis |
|---|---|
| H1 | Two-field (frame-raster) separation using the 312-line partner beats any single-field 2D method on stills. |
| H2 | Transform PAL done on the frame raster, with amplitude-matching and a fallback for rejected bins, removes "||||" dots without losing luma detail. |
| H3 | Separation filters trained by least squares on known-truth stills beat hand-designed ones (Snell lookup tables, LSLCD). |
| H4 | A CPU-bounded re-encode/residual refinement with edge-preserving priors adds measurable accuracy on maps and text. |
| H5 | Equalising the LaserVision channel (group delay, edge smear, chroma sideband/gain) before separation improves accuracy. It is an optional pre-separation step inside Domesday2D, so the work stays in one component. |
| H6 | Per-line burst-referenced phase/gain plus a masked Hanover suppression beats PAL averaging on 312-line combs. |

## Phase 1 — Known-truth still bench

**Goal:** measure any decoder against the **source picture**, on Domesday-like content.

1. **Truth output in videosynth.**
   - Requested upstream as [decode-orc/videosynth#13](https://github.com/decode-orc/videosynth/issues/13), a reference-component output: Y, U and V after pixel mapping, `luma_bandwidth` and the chroma FIR, at 4fsc active samples, as YUV444P16 in the video sink's raw geometry.
   - Until it lands, the harness re-derives truth from the published constants (33-tap Hamming at 1.3 MHz, nearest-pixel map).
   - **Acceptance:** for each pattern project, truth that is re-encoded with the videosynth chroma constants matches the `signal_type: yc` output to within 1 LSB.
2. **Proper resampling.** A band-limited (polyphase) image-to-4fsc mapping is requested in [videosynth#13](https://github.com/decode-orc/videosynth/issues/13). Until then, every still case sets `luma_bandwidth` to 5.0 MHz.
   - **Acceptance:** a luma-only still decoded with `mono` shows no energy above 5.5 MHz in the truth difference.
3. **Still-content assets**, generated offline as EXR from licence-clean sources:
   - static luma and chroma zone plates;
   - coloured text in 3 sizes;
   - a synthetic map: flat colour fills, fine contour lines, lettering, hatching;
   - two photographs (own or public-domain);
   - green/magenta horizontal edges (the "||||" case);
   - red on yellow;
   - Test Card F (already bundled).
   - **Acceptance:** the assets are committed to the videosynth assets repo with their licence recorded.
4. **Decoder-stress element** (synthetic, not the card artwork):
   - line-alternating ±V and ±U bars;
   - steady ±V and ±U bars;
   - FuBK-style anti-PAL fields;
   - 0.8–4.8 MHz gratings.

   The construction follows Hendil 1970. A bundled PM5544 source, or an equivalent PAL-killer pattern, is requested in [videosynth#13](https://github.com/decode-orc/videosynth/issues/13). This needs a videosynth feature to inject per-line chroma sign patterns, because an EXR still cannot express a line-alternating V that survives the V-switch.
   - **Acceptance:** the pattern's composite matches its definition. `pal2d` decodes the alternating bars grey, as Hendil predicts.
5. **Harness metrics** in [`orc-tests/videosynth/support/`](../orc-tests/videosynth/support/), each with region masks and a window-edge exclusion of 16 samples:
   - Y/U/V RMS;
   - RGB PSNR;
   - ΔE (BT.601 → CIELAB);
   - U/V vector error split into magnitude and phase;
   - cross-colour (U/V energy where truth chroma ≈ 0 and truth luma has detail above 3 MHz);
   - cross-luminance (Y error band-passed 3.5–5 MHz, masked to truth chroma edges);
   - edge 10–90 % rise and overshoot on bars and castellations.

   **Acceptance:** unit tests on synthetic arrays with known answers.
6. **`DecoderStillTruth` suite**, with `pal2d` and `transform2d` as the baselines to beat. `transform3d` is recorded for information only, because it reads neighbouring frames. Every still is a single frame, with different pictures before and after it:
   - clean;
   - Domesday noise model (`pal_domesday_A`).

   **Acceptance:** baselines recorded per content class, and the table is written into this document.

### Phase 1 outcome (2026-11-02)

The bench is built. The pin moved to videosynth `8217fec`, and decode-orc's videosynth projects were migrated to its `laserdisc:` blocks with the upstream `scripts/migrate_laserdisc_keys.py`. The whole suite's baselines were re-recorded: only PAL LaserDisc cases moved, by small amounts (LD bit-cell timing changed upstream).

| Step | State | Where |
|---|---|---|
| 1. Truth output | Done upstream (`output.truth`). **Acceptance met:** the truth re-encoded on the 4fsc carrier matches the Y/C rendering to 0.51 LSB in luma and 0.81 LSB in chroma (worst sample, all 8 frames) | [`picture_truth.h`](../orc-tests/videosynth/support/picture_truth.h), `DecoderStillTruth.PAL_TruthMatchesYc` |
| 2. Resampling | Done upstream (`horizontal_resample: bandlimited`); every still uses it, so `luma_bandwidth` is not needed. **Acceptance met:** `mono` on the luma zone plate leaves 0.030 % above 5.5 MHz, the 10-bit quantisation floor | `DecoderStillTruth.PAL_MonoBandLimit` |
| 3. Assets | **Partly done.** Bundled upstream: `PAL_STRESS` (which carries the red square on yellow), the static luma and chroma zone plates, Test Card F, bars, tartan. **Missing:** coloured text in 3 sizes, the synthetic map, the two photographs, green/magenta horizontal edges | — |
| 4. Stress element | Done upstream (`PAL_STRESS` with `chroma_line_sign`). **Acceptance met:** `pal2d` decodes both line-alternating strips grey (0.10 of the strips' chroma remains, a ±0.02 residual against 0.23 that alternates in sign on some rows; bound 0.15). `transform2d` also gives 0.10 | `DecoderStillTruth.PAL_StressAlternatingBars` |
| 5. Metrics | Done, with 10 unit tests on synthetic arrays (CIEDE2000 against Sharma et al. 2005's test data) | [`still_metrics.h`](../orc-tests/videosynth/support/still_metrics.h) |
| 6. Suite | Done; baselines recorded | [`still_truth_test.cpp`](../orc-tests/videosynth/decoders/still_truth_test.cpp), [`pal_stills.yaml`](../orc-tests/videosynth/projects/still_truth/pal_stills.yaml) |

**How the bench measures.**

- **The stills.** `pal_stills.yaml` has six one-frame stills, each between different pictures: the stress card, the luma and chroma zone plates, Test Card F, 75 % bars and tartan.
  - The noisy copy carries both noise layers of the Domesday model. `still-truth-clean` and `still-truth-yc` are derived from it.
  - All three share one truth.
- **The grid.** The video sink writes the cvbs_source active window unpadded (`output_padding: 1`). Its column 0 is sample 157 after 0H and its row 0 is truth row 0.
  - `PAL_Registration` confirms this on the Y/C rendering: of every offset within ±3 samples and ±2 rows, Test Card F's luma error is smallest there (0.049 %).
  - cvbs_source frames are flat (lines in transmission order), so the truth-to-Y/C check maps each truth row to its field line itself.
- **Units.** Everything is a percentage of the black-to-white excursion. U and V are the peak subcarrier amplitude on the same scale, so they compare directly with the truth.
  - Chroma gain is now a metric. Every decoder reads 75 % bars within 0.9 % of the truth's chroma gain, which confirms the units.
- **Masks.** 16 samples are left out at each end of a row, as is the tail of half-line 623. On the stress card, the three `chroma_line_sign` regions are also left out, with an 8-pixel and 8-row margin.
  - No single-frame decoder can recover a line-alternating colour, so those regions only enter `PAL_StressAlternatingBars`.
- **Thresholds.** Thresholds are in excursion units:
  - **Chroma present:** \|C\| ≥ 0.05.
  - **Cross-colour:** the truth has \|C\| < 0.005 over the 5 samples × 3 rows around the sample, and its luma above 3 MHz has a local RMS ≥ 0.02.
  - **Cross-luminance:** luma error in 3.5–5 MHz, counted within one sample or row of a truth chroma step ≥ 0.05.
  - **ΔE:** CIEDE2000 on a gamma 2.4 display with EBU primaries.
- **Edges.** Edges are scored as the decoder's **excess** 10–90 % rise and overshoot over the truth's own edge, which the band-limited mapping has already softened. Two edges are measured:
  - the 75 % bars' green/magenta boundary;
  - a black/white castellation of the stress card.

**Baselines, clean** (`pal2d` / `transform2d` / `transform3d`; `transform3d` is for information only):

| Still | Y RMS % | RGB PSNR dB | ΔE mean | Cross-colour % | Cross-luma % |
|---|---|---|---|---|---|
| Stress card | 4.87 / **1.35** / 1.95 | 25.3 / **31.6** / 29.9 | 2.11 / **1.04** / 1.80 | 2.33 / **1.57** / 1.91 | **1.67** / 1.79 / 2.10 |
| Luma zone plate | 12.74 / **3.05** / 4.00 | 15.9 / **27.7** / 25.5 | 11.99 / **1.74** / 3.89 | 12.28 / **3.53** / 4.32 | — |
| Chroma zone plate | 1.51 / **0.49** / 1.22 | 27.1 / **27.6** / 27.2 | 4.24 / **3.76** / 4.89 | — | 1.48 / 0.97 / **0.67** |
| Test Card F | 4.47 / **1.25** / 1.49 | 25.9 / **33.2** / 32.5 | 2.76 / **1.37** / 1.86 | 3.20 / **1.10** / 1.35 | 2.18 / 1.83 / **1.73** |
| 75 % bars | 0.96 / **0.62** / 0.73 | 35.1 / 35.0 / **35.2** | 0.64 / **0.42** / 0.62 | 0.22 / **0.05** / 0.45 | 0.97 / **0.77** / 0.89 |
| Tartan | 1.30 / 2.01 / **0.72** | 26.6 / 25.5 / **26.7** | 1.51 / 1.75 / **1.37** | 1.66 / 1.79 / 1.68 | 1.85 / 3.04 / **1.17** |

Edges, clean, `pal2d` / `transform2d`:

- **Castellation:** 30.8 / 5.1 ns of excess rise.
- **Bars:** 5.7 / 0.0 ns of excess rise, and 1.4 / 0.1 % of excess overshoot.

The Domesday noise model costs every decoder 0.3–2.3 dB of RGB PSNR (most on the bars and Test Card F, least on the zone plates and tartan) and leaves the ranking unchanged. The full metric set, including ΔE p95, chroma magnitude, phase and gain, is in [`baselines.json`](../orc-tests/videosynth/baselines.json) under `DecoderStillTruth`.

**What the baselines show.**

- **No 2D decoder wins every still.** That gap is the target Domesday2D has to close.
  - `transform2d` is far ahead wherever luma detail sits near the subcarrier (zone plate, Test Card F, stress card). There `pal2d` turns luma detail into cross-colour: 12.3 % against 3.5 % on the luma zone plate.
  - On tartan, `transform2d` is the worst of the three: its cross-luminance is 3.04 % against `pal2d`'s 1.85 %. Tartan's horizontal chroma edges are the "||||" case of A1.
- **The bar to beat, per still,** is the better of `pal2d` and `transform2d`.
- **Chroma detail is out of reach for every decoder.** On the chroma zone plate they read about 76 % low in chroma gain and 85–88° r.m.s. in phase. Fine chroma lies beyond any PAL decoder's chroma bandwidth, so the plate measures how gracefully a decoder fails rather than whether it recovers.

**Gate.** The bench is in place and its acceptance checks pass, so Phase 3 can measure against the source. Step 3 is still open:

- coloured text;
- the synthetic map;
- green/magenta horizontal edges;
- the two photographs, which need licence-clean images.

Maps and text are the Domesday target content, so they should be in the suite before Phase 3 draws conclusions. The missing assets are requested upstream as [decode-orc/videosynth#16](https://github.com/decode-orc/videosynth/issues/16): maps rendered from OpenStreetMap data (ODbL, attribution required) with OS Terrain 50 or synthetic contours, the text card, the edge patterns, the two photographs (supplied by the maintainer), and a per-asset licence record.

## Phase 2 — Real-signal characterisation (measurement only)

**Goal:** decide which channel effects are real and how large they are, before modelling or correcting any of them.

1. **VITS chain response** per capture (Domesday DS1–DS6 sides, Volcanoes, EcoDisc):
   - multiburst amplitude;
   - chroma-bar gain;
   - group delay of the chroma relative to the luma, from the bar edges.

   Compare inner and outer radius.

   **Acceptance:** a table per capture. It states whether the IEC §9.1.6 pre-distortion is still present (expect about −135 ns at fsc if the replay filter is absent), and how much of it the ld-decode demodulator already removes.
2. **Edge-smear decomposition.** Fit the VITS edge tail as (a) pure group delay, (b) dp111's one-pole smear and (c) both.
   - **Acceptance:** the residual of each model is reported, and one model is chosen for videosynth.
3. **Chroma sideband symmetry.** On flat colour areas and the chroma bars, measure the ratio of upper to lower sideband against frequency offset.
   - **Acceptance:** a usable symmetric band (in MHz) is stated for the transform tests in Phase 3.
4. **Per-line chroma phase and gain.**
   - Burst phase residual line to line within a field.
   - Line-alternating V→U crosstalk on the bars (dp111 claims about ±0.3; verify).
   - Differential phase against luma level.

   **Acceptance:** an r.m.s. figure for each, with the H6 go/no-go.
5. **Inter-field registration on stills.** Measure the horizontal sub-sample and vertical offset of field 2 against field 1 on maps, Test Card F (DS1 3001–3020) and photographs, using luma below 3 MHz.

   **Acceptance:** an offset distribution. H1 needs it under about 0.1 sample, or the offset must be correctable.
6. **Feed results back to videosynth.** The channel model is requested in [videosynth#14](https://github.com/decode-orc/videosynth/issues/14), with figures to follow. Post the measured values there for whichever of these the gates show matter:
   - group delay;
   - replay response with sideband truncation;
   - radius HF loss;
   - differential gain/phase;
   - line-alternating crosstalk;
   - inter-field offset.

   **Acceptance:** the issues are filed. The Domesday model projects gain a `channel` block once these land.

## Phase 3 — Separation geometry (H1, H2, H3)

**Goal:** choose the core separation method. Everything runs as a prototype on the Phase 1 bench, clean and noisy.

1. **Carrier lattice check.** Numerically confirm, on videosynth output, which frame-raster neighbour is the 312-line partner, and the residual 0.29° rotation.
   - **Acceptance:** a measured phase table that matches the one in A3.
2. **Like-for-like field versus frame.** Run one fixed FIR design twice: once in single-field geometry (PalColour taps), once in frame-raster geometry using the 312-line partner, with per-line phase from the lattice.
   - **Acceptance:** an H1 go/no-go by content class. "Go" means a lower cross-colour and cross-luminance error than the field version in every class except the stress element.
3. **Transform PAL variants.**
   - field versus frame raster, with tile heights 16/32;
   - binary versus amplitude-matching mode;
   - threshold restricted to the Phase 2 symmetric band;
   - a fallback for rejected bins that hands them to a frame-raster comb estimate instead of to luma.

   **Acceptance:** "||||" dot amplitude on the green edge falls below 5 codes, with no rise in multiburst Y error.
4. **Least-squares-trained filters** (LSLCD-style). Train 2D separation kernels on the frame raster against videosynth truth, using a training set disjoint from the evaluation assets.
   - **Acceptance:** report the gain over the best hand-designed result, and the generalisation gap between the training and evaluation sets.
5. **Funnel.** Rank every variant on the full metric set.
   - **Acceptance:** at most two candidates go forward. Each must beat the best existing decoder on every content class's primary metric (ΔE for photos and maps, cross-colour for text and gratings).

## Phase 4 — Adaptivity and refinement (H4, H6)

**Goal:** close the residual gap on the hard cases. Each addition must pay for its cost.

1. **Local selection.** Choose between the Phase 3 candidates and a fallback per region, using local spectral or comb-agreement features. Weston/Snell-style lookup tables, trained on the bench, are allowed.
   - **Licence constraint:** do not reproduce the claims of the active US9432648.
   - **Acceptance:** better than either candidate alone on every class.
2. **Anti-Hanover.** Use per-line burst-referenced correction, plus dp111-style masked suppression of same-field line alternation, against the Phase 2 phase/crosstalk figures, applied to the 312-line comb.
   - **Acceptance:** Hanover residue on flat bars under the Phase 2 impairments is no worse than pal2d, and vertical chroma edges are no softer.
3. **Residual refinement.** Up to N iterations of decode → re-encode with the videosynth-equivalent encoder → reassign the residual, with an edge-preserving (Charbonnier) chroma prior and a noise-weighted data term.
   - **Acceptance:** the ΔE gain on maps and text is reported against cost. "Go" only if the gain is at least 0.5 dB PSNR-equivalent at no more than 3× pal2d's CPU time per frame.
4. **Chroma output bandwidth.** Compare a fixed low-pass, prior-defined bandwidth and none, against truth.
   - **Acceptance:** the chosen setting and the reasoning are recorded.

## Phase 5 — Channel equalisation (H5)

**Goal:** correct the LaserVision channel as an **optional** first step inside Domesday2D, applied to the composite before separation. This keeps the work in one component. When the option is off, the output must be exactly what the decoder produces without the step.

1. **Prototype.** Build an equaliser from the Phase 2 model (group-delay all-pass and/or edge-smear inverse, with an optional chroma-band gain shelf).
   - **Acceptance:** on videosynth with the Phase 2 channel impairments, Domesday2D with equalisation beats Domesday2D without it on every content class, and is no worse on the clean bench. On real VITS, the equalised response is flat within a stated tolerance.
2. **Parameter source.** Compare three ways of setting the equaliser: fixed IEC 60856 nominal values, values measured per frame from that frame's VITS lines 19/20/332/333, and values measured once per source and smoothed.
   - **Acceptance:** the most robust choice is recorded. VITS-derived values must fall back to the nominal values when the VITS lines are missing or damaged (subtitles on lines 20/333, dropouts).
3. **Interaction with upstream stages.** Run with and without `fm_beat_cancel`, the stacker and `picture_band_limit` upstream.
   - **Acceptance:** no double correction. If a stage upstream already changes the response, the measured-VITS mode measures what that stage left behind; this is confirmed on the bench.
4. **Parameter surface.** One `channel_equalisation` option (off / nominal / measured, default to be set from task 2), plus anything the results show is needed.
   - **Acceptance:** the surface is agreed with the user before Phase 6.

## Phase 6 — Implementation

1. **The `domesday2d` decoder** in [`orc/plugins/stages/sinks/common/decoders/`](../orc/plugins/stages/sinks/common/decoders/), implementing the Phase 3–4 outcome and the optional Phase 5 equaliser. It must be SIMD-friendly, and `getLookBehind()`/`getLookAhead()` return 0.
   - **Acceptance:** bit-matches the prototype within 1 LSB on the bench.
2. **Sink registration**, at every place listed in A1: the factory, the options list, `schedulable_decoder_types()`, `downgrade_for_yc()`, the decoder parameters and the dependencies.
   - **Acceptance:** core and GUI unit tests pass with the new name.
3. **Tests.**
   - A `DecoderStillTruth.PAL_domesday2d` entry and baselines.
   - A unit test of the lattice and phase tables.
   - `ctest -R MVPArchitectureCheck` passes.
   - **Acceptance:** all green.
4. **Documentation** (AGENTS.md §9.1):
   - [`video_sink/instructions.md`](../orc/plugins/stages/sinks/video_sink/instructions.md);
   - [`sink-core-stages.md`](../docs/gui-user-guide/stages/sink-core-stages.md);
   - [`chroma-decoder-guide.md`](../docs/gui-user-guide/stages/chroma-decoder-guide.md), covering when to choose Domesday2D, and its limits on motion.
   - **Acceptance:** all three docs updated in the same change.
5. **Performance.**
   - **Acceptance:** throughput is measured against pal2d and transform2d. Anything more than 5 % slower than transform2d is raised for design review (AGENTS.md §6).

## Phase 7 — Real-disc validation

1. **Held-run reference** (measurement only). For Test Card F (DS1 3001–3020) and held map stills, the bench tooling in `tools/domesday2d-bench/` builds the reference without any chroma decoder under test:

- Average the same-phase repeats of each phase.
- Take Y = (A+B)/2 and C = (A−B)/2 across frames 2 apart, which is exact for a static picture.
- Demodulate C.

Each decoder then decodes **one** frame of the run on its own.
   - **Acceptance:** Domesday2D has the lowest error of all 2D decoders.
2. **Cross-disc agreement.** On single-frame stills, compare decodes of the same frame from different pressings. The aim is that lower disagreement means less decoder-induced error, with noise shared within a master kept separate.
   - **Acceptance:** better than every existing 2D decoder, measured across pressing groups.
3. **Visual panel.** Show side-by-side crops of map lettering, photograph skin tones, the green edges and the Test Card F castellations, and record them in this document.
   - **Acceptance:** the user signs off.
