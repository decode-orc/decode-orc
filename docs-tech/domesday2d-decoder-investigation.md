# Domesday2D: a PAL still-frame chroma decoder

> **Renamed (2026-11-04).** The decoder this investigation produced ships as **PAL Still**, decoder type `palstill`. Nothing in it is specific to Domesday; only the tuning came from those discs. This record keeps the working name. The code names it uses map as follows:
>
> | In this document | Now |
> |---|---|
> | `domesday2d` decoder type | `palstill` |
> | `decoders/domesday2ddecoder.{h,cpp}`, `Domesday2DDecoder` | `decoders/palstilldecoder.{h,cpp}`, `PalStillDecoder` |
> | `orc/support/domesday2d.{h,cpp}`, `orc::domesday2d` | `orc/support/pal_still.{h,cpp}`, `orc::pal_still` |
> | Domesday2D Calibration (`domesday2d_calibration`), `orc/core/analysis/domesday2d/` | PAL Still Calibration (`pal_still_calibration`), `orc/core/analysis/pal_still/` |
> | `Domesday2DTruth`, `DecoderStillTruth.PAL_domesday2d_*` | `PalStillTruth`, `DecoderStillTruth.PAL_palstill_*` |
>
> The bench in `tools/domesday2d-bench/` keeps its name, but it now passes `palstill` to orc-cli.

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
- **Known failure.** At sharp edges carrying both U and V (green), the U and V vertical spectra overlap. The magnitude test then rejects real chroma into luma, leaving about 25 codes of "||||" dots over ±4 field lines. No threshold fixes it (memory note on transform2d edge dots, audited 2026-10-30). **Phase 3:** on the frame raster, where U and V each have their own reflection, the dots fall from 43 codes to 0.9 on the bench's edge card.

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

In a woven still frame, one vertical neighbour of every line is a 312-line partner. That neighbour carries U and V **both inverted** to within 0.3°, at a quarter of the vertical distance PalColour has to reach. This is the basis of a two-field decoder and the main hypothesis in Part B (H1). Which side (above or below) holds the 312 partner depends on field order and must be confirmed numerically. **Phase 3:** confirmed. It is the row above a field 1 row and below a field 2 row. On the 4fsc grids of videosynth and ld-decode, which are locked to the subcarrier, it is exactly 180°: the 0.29° (and the 1.15° at 2 lines) belong to line-locked sampling.

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
  - [H] It may explain part of the 15–21 % Domesday edge tail measured earlier, and dp111's deghost fit. **Phase 2: it does not.** Current ld-decode equalises the pre-distortion, and the tail is a one-pole smear (see the Phase 2 outcome).
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

This section describes the pin `139b31e` that the investigation started from. The pin is now `78bdc96` ([`flake.lock`](../flake.lock)), which closes gaps 1–4 below (videosynth#13, #14, #16); Phase 1 closes gap 5.

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
| 1. Truth output | Done upstream (`output.truth`). **Acceptance met:** the truth re-encoded on the 4fsc carrier matches the Y/C rendering to 0.51 LSB in luma and 1.16 LSB in chroma (worst sample, all 14 frames; chroma is allowed 1 LSB plus 0.25 % of the line's chroma peak, and the 1.16 is on the town map's 336-code chroma). The 98 luma samples where the text card's ringing passes the 10-bit range sit exactly at the rail | [`picture_truth.h`](../orc-tests/videosynth/support/picture_truth.h), `DecoderStillTruth.PAL_TruthMatchesYc` |
| 2. Resampling | Done upstream (`horizontal_resample: bandlimited`); every still uses it, so `luma_bandwidth` is not needed. **Acceptance met:** `mono` on the luma zone plate leaves 0.030 % above 5.5 MHz, the 10-bit quantisation floor | `DecoderStillTruth.PAL_MonoBandLimit` |
| 3. Assets | Done upstream ([videosynth#16](https://github.com/decode-orc/videosynth/issues/16), pin `78bdc96`): `OSM_TOWN` and `OSM_RURAL` (OS-style maps of Bradford-on-Avon from an OpenStreetMap extract, ODbL), `TEXT_CARD`, `GREEN_MAGENTA_EDGES` and two photographs (CC BY 4.0), with a licence record in `videosynth-assets/README.md`. `PAL_STRESS` carries the red square on yellow | [`pal_stills.yaml`](../orc-tests/videosynth/projects/still_truth/pal_stills.yaml) frames 7–12 |
| 4. Stress element | Done upstream (`PAL_STRESS` with `chroma_line_sign`). **Acceptance met:** `pal2d` decodes both line-alternating strips grey (0.10 of the strips' chroma remains, a ±0.02 residual against 0.23 that alternates in sign on some rows; bound 0.15). `transform2d` also gives 0.10 | `DecoderStillTruth.PAL_StressAlternatingBars` |
| 5. Metrics | Done, with 10 unit tests on synthetic arrays (CIEDE2000 against Sharma et al. 2005's test data) | [`still_metrics.h`](../orc-tests/videosynth/support/still_metrics.h) |
| 6. Suite | Done; baselines recorded | [`still_truth_test.cpp`](../orc-tests/videosynth/decoders/still_truth_test.cpp), [`pal_stills.yaml`](../orc-tests/videosynth/projects/still_truth/pal_stills.yaml) |

**How the bench measures.**

- **The stills.** `pal_stills.yaml` has twelve one-frame stills, each between different pictures: the stress card, the luma and chroma zone plates, Test Card F, 75 % bars, tartan, the town and rural maps, the text card, the green/magenta edge card and the two photographs.
  - The noisy copy carries both noise layers of the Domesday model. `still-truth-clean` and `still-truth-yc` are derived from it.
  - All three share one truth.
- **The grid.** The video sink writes the cvbs_source active window unpadded (`output_padding: 1`). Its column 0 is sample 157 after 0H and its row 0 is truth row 0.
  - `PAL_Registration` confirms this on the Y/C rendering: of every offset within ±3 samples and ±2 rows, Test Card F's luma error is smallest there (0.049 %).
  - cvbs_source frames are flat (lines in transmission order), so the truth-to-Y/C check maps each truth row to its field line itself.
- **Units.** Everything is a percentage of the black-to-white excursion. U and V are the peak subcarrier amplitude on the same scale, so they compare directly with the truth.
  - Chroma gain is now a metric. Every decoder reads 75 % bars within 0.9 % of the truth's chroma gain, which confirms the units.
- **Masks.** 16 samples are left out at each end of a row, as is the tail of half-line 623. On the stress card, the three `chroma_line_sign` regions are also left out, with an 8-pixel and 8-row margin.
  - No single-frame decoder can recover a line-alternating colour, so those regions only enter `PAL_StressAlternatingBars`.
  - Wherever the clean composite sits at a coding rail, 16 samples either side and 4 rows above and below are left out too: no decoder can recover a clipped sample, and every decode whose filter reaches it is spoilt. 100 % yellow and cyan peak at 133 %, above the 10-bit ceiling (129.8 %), and the band-limited mapping rings on hard-edged glyphs. This takes 29 % of the text card (its yellow and cyan text), 13 % of the edge card (only its 100 % red/cyan and blue/yellow control stripes, rows 442–507), and under 0.5 % of each map. The count of clipped samples is recorded per still (`*_clipped_samples`).
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
| Tartan | **1.30** / 2.01 / 2.12 | **26.6** / 25.5 / 25.7 | **1.51** / 1.75 / 2.13 | **1.66** / 1.79 / 1.96 | 1.85 / 3.04 / **1.74** |
| Town map | **6.29** / 6.55 / 7.17 | 21.5 / **22.0** / 21.2 | 7.74 / **7.20** / 8.31 | 5.03 / **4.61** / 4.71 | 5.34 / **4.63** / 4.95 |
| Rural map | **4.12** / 4.68 / 5.56 | **23.1** / 22.7 / 21.8 | **5.33** / 5.55 / 6.80 | — | 4.15 / **3.97** / 4.37 |
| Text card | **5.16** / 6.84 / 6.24 | **19.4** / 18.5 / 18.8 | **6.99** / 7.58 / 7.65 | 6.64 / **4.70** / 4.86 | **4.20** / 5.16 / 4.49 |
| Green/magenta edges | **4.41** / 14.78 / 5.32 | **11.5** / 9.8 / 11.2 | **15.42** / 20.15 / 16.90 | **2.27** / 2.83 / 2.61 | **4.99** / 17.78 / 5.66 |
| Photo (butterfly) | 0.74 / **0.47** / 1.37 | 39.1 / **40.4** / 34.5 | 1.16 / **0.94** / 2.43 | — | 0.63 / **0.58** / 1.65 |
| Photo (Galtströms) | **1.56** / 1.58 / 1.63 | 33.6 / **33.7** / 33.5 | 2.60 / **2.41** / 2.47 | 1.18 / **1.01** / 1.07 | 1.43 / 1.42 / **1.33** |

`transform3d`'s tartan figures moved when the stills were added: it reads the next frame, which is now the town map rather than the bars. Its other figures, and every `pal2d` and `transform2d` figure on the first six stills, are unchanged.

Edges, clean, `pal2d` / `transform2d`:

- **Castellation:** 30.8 / 5.1 ns of excess rise.
- **Bars:** 5.7 / 0.0 ns of excess rise, and 1.4 / 0.1 % of excess overshoot.

The Domesday noise model costs every decoder 0.1–4.8 dB of RGB PSNR (most on the cleanest pictures: the butterfly photograph, the bars and Test Card F; least where the decoders' own errors already dominate: the text card, the edge card and the zone plates) and leaves the ranking unchanged. The full metric set, including ΔE p95, chroma magnitude, phase and gain, is in [`baselines.json`](../orc-tests/videosynth/baselines.json) under `DecoderStillTruth`.

**What the baselines show.**

- **No 2D decoder wins every still.** That gap is the target Domesday2D has to close.
  - `transform2d` is far ahead wherever luma detail sits near the subcarrier (zone plate, Test Card F, stress card). There `pal2d` turns luma detail into cross-colour: 12.3 % against 3.5 % on the luma zone plate.
  - On tartan, `transform2d` is the worst of the three: its cross-luminance is 3.04 % against `pal2d`'s 1.85 %. Tartan's horizontal chroma edges are the "||||" case of A1.
- **The bar to beat, per still,** is the better of `pal2d` and `transform2d`.
- **The green/magenta edge card is the "||||" case at its worst.** `transform2d`'s cross-luminance is 17.8 % against `pal2d`'s 5.0 %, and its luma error 14.8 % against 4.4 %. Its chroma phase error is 57° r.m.s. against 22°. Every decoder loses about half the chroma on the 1-, 2- and 4-row stripes (chroma gain error 49 %): those lie beyond any 2D decoder's vertical chroma bandwidth.
- **Maps and text are where every decoder is weakest,** at 19–23 dB RGB PSNR and a mean ΔE of 5–8 against 33–40 dB and 1–3 on the photographs. This is the Domesday content, so it is where Domesday2D has the most to gain. `pal2d` leads on luma error on all three; `transform2d` leads on cross-colour and, on the town map, on cross-luminance. Chroma phase is 20–26° r.m.s. on all of them: small coloured lettering and lines lose their hue.
- **The photographs are easy for every 2D decoder** (0.5–1.6 % luma error), so they mainly guard against Domesday2D doing harm on natural pictures.
- **Chroma detail is out of reach for every decoder.** On the chroma zone plate they read about 76 % low in chroma gain and 85–88° r.m.s. in phase. Fine chroma lies beyond any PAL decoder's chroma bandwidth, so the plate measures how gracefully a decoder fails rather than whether it recovers.

**Gate.** Passed. Every step's acceptance is met, and the bench carries the Domesday content (maps, text, photographs), so Phase 3 can measure against the source.

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

### Phase 2 outcome (2026-11-02)

Measured with the tools in [`tools/domesday2d-bench/`](../tools/domesday2d-bench/). Every method was first checked on videosynth captures with a known impairment (the table is in the tools' README): group delay, smear, replay response, phase error, crosstalk and field offset are each read back within a few per cent.

**Captures.**

- **Domesday, 2026 decodes.** ld-decode `6148260`, `.cvbs`: Community South and North and National A (CAV), DS1, DS2 and DS5 of each; National B (CLV), DS1, DS2 and DS5.
- **Domesday, 2025 decodes.** The `AIV_Domesday_20250115` `.tbc` files, DS2 of each side, from an ld-decode older than both the group-delay equaliser and the inverse-MTF correction.
- **Volcanoes and EcoDisc.** Volcanoes side 1 and EcoDisc sides 1 and 2, DS1, 2026 decodes.

The VITS were measured on 48 frames at each of three positions: 1000 frames in (inner radius), the middle, and 1000 frames from the end (outer radius). The picture was measured on 40 frames spread over each CAV side.

**The VITS.** Domesday pressings carry the CCIR 473 insertion signals:

| Line | Field 1 | Field 2 |
|---|---|---|
| 19 | CCIR 17: bar, 2T, 20T, staircase | CCIR 330: staircase with subcarrier |
| 20 | CCIR 18: multiburst at 0.5, 1, 2, 4, 4.8 and 5.8 MHz | CCIR 331: chroma bars at 20/60/100 % |

The VITS chroma is **not locked to the burst**: its phase against the burst repeats only every six frames. So nothing that needs the burst as a phase reference can be taken from the VITS, and the line-alternating error is measured on the picture. Differential gain and phase compare steps within one line, so they are still valid.

**Domesday CAV, 2026 decodes** (medians over 27 measurements: 9 captures × 3 radii):

| Quantity | Value | Spread |
|---|---|---|
| Residual IEC 60856 group delay | 0.006 × the nominal curve | IQR −0.04 to 0.09 |
| Edge smear (videosynth `edge_smear`) | fraction 0.24, τ 6.1 samples (0.34 µs) | IQR 0.22–0.26, 5.9–6.6 |
| 20T delay, chroma − luma | −70 ns, all of it the smear's | — |
| 20T chroma/luma gain | 0.89 (0.97 inner, 0.85 outer) | — |
| Multiburst, against 0.5 MHz | 1 MHz −0.6, 2 MHz −0.6, 4 MHz −0.2, 4.8 MHz −0.8, 5.8 MHz −10.3 dB | 5.8 MHz: −11.5 inner, −9.8 outer |
| Sideband asymmetry (upper − lower) | −0.8 dB at ±0.2 MHz, −1.3 dB at ±0.4 MHz | beyond ±0.4 MHz the 20T's spectrum is too weak to measure |
| CCIR 331 100 % bar / burst | 2.33 (ITU nominal 2.33) | 2.28–2.48 |
| Chroma linearity, 20/60/100 % bars | 0.19 / 0.60 / 1 | — |
| Differential gain, 0–100 IRE | ≤ 1.7 % | 90th percentile 4.7 % |
| Differential phase, 0–100 IRE | ≤ 1.5° | 90th percentile 2.9° |
| Burst phase, line to line | 3.4° r.m.s. (includes about 1.5° of measurement noise) | 90th percentile 7.7° |

**Picture** (CAV sides):

| Capture | Field 2 dx (samples), median [10th, 90th percentile] | dy (field lines) | Line-alternating phase |
|---|---|---|---|
| Community South DS1/DS2/DS5 | 0.005 / −0.004 / −0.009 [−0.05, 0.06] | 0.01 [0.00, 0.19] | −4.2 / −3.5 / −4.8° |
| Community North DS1/DS2/DS5 | 0.011 / −0.013 / −0.007 [−0.27, 0.04] | 0.01 [−0.04, 0.06] | −4.5 / −5.2 / −2.9° |
| National A DS1/DS2/DS5 | 0.006 / −0.013 / 0.003 [−0.06, 0.23] | 0.00 [−0.30, 0.01] | +1.2 / +1.8 / +3.0° |
| Volcanoes side 1 DS1 | −0.004 [−0.14, 0.03] | 0.01 [−0.03, 0.08] | −2.1° |
| EcoDisc side 1 / side 2 DS1 | 0.048 / −0.165 [−0.46, 0.95] | 0.00 / 0.03 | +4.8 / −2.5° |
| 2025 decodes, Community South / North / National A DS2 | 0.031 / 0.008 / 0.031 | 0.01 / 0.01 / 0.00 | +5.5 / +5.1 / +10.5° |

The line-alternating error is almost entirely a rotation: the fitted 2×2 matrix's off-diagonal terms are equal and opposite to within about 0.02. What is left is a U/V gain alternation of up to about 4 % and a V↔U leak of 0.04–0.09. dp111's ±0.3 crosstalk is not there.

**Findings.**

1. **The IEC 60856 pre-distortion is not in any capture.** ld-decode has equalised it since `f7ddee5d` (May 2026), and every 2026 decode includes that commit. The 2025 decodes predate it, yet they do not carry the −135 ns either: their Butterworth low-pass over-corrects, leaving the chroma 30–40 ns late (−0.23 to −0.32 × the curve).
2. **The Domesday edge tail is a smear, not a group delay.** A one-pole tail of 0.24, τ 0.34 µs, is on every CAV Domesday side, in both decodes, at every radius. It is close to dp111's deghost fit (c 0.16, τ 9 samples). The National B **CLV** side has none, and has 6 dB more at 5.8 MHz. So the smear belongs to how the CAV sides were mastered, not to ld-decode or to the disc radius. Volcanoes (0.10–0.24, τ 4–5) and EcoDisc side 1 (0.07–0.21, τ 3–4) carry less; EcoDisc side 2 has a different, shorter tail (it fits only at the 0.5 bound with τ 2).
3. **A 20T delay alone cannot tell the two apart.** A smear delays the 20T's luma half and barely touches its chroma, so the smear reads as about −70 ns of chroma lead. The smear has to be fitted first, from the long tails, and its share simulated and taken off.
4. **The 2026 decodes have small differential gain and phase; the 2025 decodes do not.** The 2025 `.tbc` files show 13–25° of differential phase. The 2026 decoder fixed it.
5. **ld-decode's automatic inverse-MTF cuts chroma on the CAV sides.** It chose a negative strength (−0.67 to −0.75, −2.3 to −2.6 dB at fsc), taking the burst from about nominal to 16 IRE. On the CLV side it chose about 0. The CCIR 331 bar keeps its nominal ratio to the burst either way, so a burst-referenced chroma gain is right.

**Gates.**

| Step | Result |
|---|---|
| 1. VITS chain response | **Done.** The pre-distortion is not present: ld-decode removes all of it (2026) or more than all of it (2025). Inner and outer radius differ by less than 0.7 dB up to 4.8 MHz, and by 1.7 dB at 5.8 MHz |
| 2. Edge-smear decomposition | **Done.** Group delay alone fits worst; the smear alone fits the tails to the noise (0.43 against 0.96 IRE with no smear), and adding group delay to it moves the scale only to 0.006. **Chosen model: `edge_smear`, no `group_delay`** |
| 3. Sideband symmetry | **Done.** The upper sideband is 0.8–1.3 dB low within ±0.4 MHz, consistent with the multiburst's slope (−0.6 dB from 4.0 to 4.8 MHz). By +1.4 MHz (5.8 MHz) it is 10 dB down against about 1 dB for the lower sideband at 3.0 MHz. **Usable symmetric band for Phase 3: ±0.5 MHz around fsc**; a transform symmetry test reaching further must allow for a tilt of about −1.5 dB per 0.5 MHz, or compensate it |
| 4. Per-line chroma | **Done.** Burst jitter 3.4° r.m.s.; differential phase ≤ 1.5°; line-alternating phase a constant −5° to +3° per side (+5° to +10° on the 2025 decodes), with a V↔U leak of 0.04–0.09 |
| 5. Inter-field registration | **Done.** Median offset within ±0.013 samples and 0.01 field lines on every Domesday side. **H1's condition (< 0.1 sample) holds for most frames**; 10–20 % of frames on some sides read 0.2–0.3 samples or 0.3 field lines |
| 6. Feed back to videosynth | Figures below, for [videosynth#14](https://github.com/decode-orc/videosynth/issues/14) |

**Go/no-go.**

- **H5, group delay: no-go.** There is no pre-distortion left to equalise in a current ld-decode output.
- **H5, smear: go.** The equaliser's target is the inverse of the one-pole smear (0.24, τ 6.1 samples). That inverse is stable, and it can be fitted per capture from the VITS.
- **H6, per-line burst referencing: no-go.** A single line's burst phase carries 3.4° r.m.s. of jitter, comparable with the error it would correct.
- **H6, a per-capture rotation: go.** The line-alternating error is a constant −5° to +3° per side and per decoder, so a single chroma-to-burst rotation can be measured from the picture and removed before a one-line or 312-line comb. The masked Hanover suppression stays a Phase 4 candidate.
- **H1: go, with a per-frame guard.** Registration is good to a few hundredths of a sample on most frames. The decoder must detect and fall back on the frames that are not.

**Figures for videosynth#14** (Domesday CAV, 2026 decodes):

```yaml
channel:
  edge_smear: { fraction: 0.24, tau_samples: 6.1 }
laserdisc:
  channel:
    # No group_delay: current ld-decode removes the IEC pre-distortion.
    # The multiburst less the smear's own loss (which is about -2.2 dB above 2 MHz):
    response_by_picture:
      - {picture: 1000,  response_db: [[0, 0], [0.5, 0.6], [1.0, 1.0], [2.0, 1.4], [4.0, 2.1], [4.8, 1.0], [5.8, -9.8]]}
      - {picture: 53000, response_db: [[0, 0], [0.5, 0.5], [1.0, 0.6], [2.0, 1.0], [4.0, 1.5], [4.8, 0.8], [5.8, -8.0]]}
    # No field2_offset: the median is under 0.015 samples and 0.01 field lines.
chroma_error:
  differential:
    gain: [[0, 1.0], [20, 1.017], [40, 1.005], [60, 1.011], [80, 1.007], [100, 1.0]]
    phase_deg: [[0, 0.0], [20, 1.1], [40, 1.3], [60, 1.5], [80, 1.1], [100, 0.2]]
  phase_deg: -4.0           # Community sides; National A +2.0
  crosstalk: { v_to_u: 0.06, u_to_v: -0.02, alternate_with_switch: true }  # beyond the rotation
  line_jitter: { phase_deg: 3.0 }
```

The `response_by_picture` entries are the multiburst medians (against the CCIR 18 flag) at the inner and outer radius, less the magnitude of the smear fitted there. The middle radius reads up to 1 dB lower than both, so the radius dependence is within the spread between captures. The National B CLV side would take no smear, and a response falling only to about −4 dB at 5.8 MHz.

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

### Phase 3 outcome (2026-11-02)

Prototyped in Python in [`tools/domesday2d-bench/`](../tools/domesday2d-bench/) (`bench.py`, `proto.py`, `transform.py`, `lslcd.py`, `phase3_*.py`), on the Phase 1 bench, clean and noisy.

**The prototypes are checked against decode-orc first:**

- `bench.py` ports the still metrics. On decode-orc's own `pal2d` and `transform2d` decodes it reproduces every `DecoderStillTruth` figure to within 1 part in 10⁷.
- The prototype PalColour in field geometry reproduces `pal2d`'s cross-colour and cross-luminance exactly.
- The prototype Transform PAL reproduces `transform2d`'s to within 0.1 %, except where decode-orc's 16-bit output clips: about 1 % on the text and edge cards.

So the differences below come from the geometry, not from the port.

| Step | State |
|---|---|
| 1. Carrier lattice | **Done.** Every shift is a multiple of 90° on the 4fsc grid, within 0.1°. The 312-line partner is above a field 1 row and below a field 2 row |
| 2. Field versus frame | **Done.** The frame raster lowers cross-colour on every still. A fixed linear luma path that uses the partner raises cross-luminance at horizontal colour edges, so the acceptance is met for chroma only |
| 3. Transform PAL variants | **Done. Acceptance met** by Transform PAL on the frame raster. The "\|\|\|\|" dots fall from 43 codes to 0.9 (2.5 with noise), with no rise in the luma zone plate's or Test Card F's luma error |
| 4. Least-squares filters | **Done.** Frame windows beat field windows by 0.5–0.7 points of Y, U and V error. Against hand-designed filters the result is mixed. The generalisation gap is about 1.2 points |
| 5. Funnel | **Done.** One candidate goes forward: frame-raster Transform PAL. It beats both existing decoders on every primary metric, clean and noisy |

**1. The carrier lattice** (`phase3_lattice.py`).

Each picture line's carrier phase and V-switch are fitted from the clean composite against the truth (three stills, 575–1722 line pairs per row):

| Lines apart | U | V | A3 (line-locked) | Real capture (DS2 bursts) |
|---|---|---|---|---|
| 1 | 270.00° | 90.00° | — | — |
| 2 | 180.00° | 180.00° | 181.15° | 179.98° |
| 312 | **180.00°** | **180.00°** | 179.71° | 179.39° |
| 313 | 90.00° | 270.00° | 90.29° / 270.29° | — |
| 1250 | 180.00° | 180.00° | 180.00° | — |

The spread is under 0.1° on videosynth.

A3's table assumes sampling locked to the line. videosynth and ld-decode both sample at 4fsc locked to the subcarrier:

- Each line is 1135 samples, with 2 more on lines 312 and 624.
- So the 25 Hz offset moves the line timing, not the carrier, and every shift is a multiple of 90°.

The real capture's 2-line shift (179.98°) confirms the grid. Its 312-line 0.6° is about 0.007 samples of field 2 timing, which lies within the Phase 2 registration spread.

In the woven frame:

- Truth row r of field 1 has its 312-line partner at r − 1, above.
- Truth row r of field 2 has it at r + 1, below.
- The other neighbour is 313 lines away, in quadrature.

The lattice is fitted per field from the bursts. On an s = +1 line the burst phase is φ + 45°; on an s = −1 line it is φ + 135°. That fit is within 0.16–0.3° of the truth fit on videosynth.

**The frame raster's spectrum.** On the frame rows, U's carrier phase is −πr/4 plus 0 or 3π/4 by field. So U is not one spectral point but two lobes at the subcarrier:

- −1/8 cycle per frame row, with weight cos 3π/8 = 0.38;
- +3/8 cycle per frame row, with weight 0.92.

V is the mirror image: +1/8 and −3/8.

In one field, U and V sit at ∓1/4 cycle per field line, and both coincide with aliases of luma at 1/8 and 3/8 cycle per frame row. In the frame those cases are distinct:

- Low vertical luma near fsc meets only the weak lobes.
- Only luma of high vertical frequency, near ±3/8, meets the strong ones.

That is the mechanism behind H1.

**2. One design, field against frame** (`proto.py`).

PalColour's chroma kernel is a radial raised cosine whose vertical unit is one frame row. It is sampled once on the field's rows (offsets 0, ±2, ±4, ±6) and once on every frame row, with each row demodulated on its own carrier.

The luma path takes PalColour's rule: its own rows plus the rows whose carrier is inverted (same field ±2 lines) at weight 0.2. In the frame, the 312-line partner is added at the same weight.

Clean, field / frame:

| Still | Cross-colour % | Cross-luma % | ΔE mean |
|---|---|---|---|
| Stress card | 2.33 / **1.59** | **1.68** / 2.43 | 1.99 / **1.45** |
| Luma zone plate | 12.28 / **8.69** | — | 11.97 / **9.69** |
| Chroma zone plate | — | **1.48** / 2.24 | **4.02** / 4.56 |
| Test Card F | 3.20 / **2.47** | 2.18 / **1.74** | 2.63 / **1.98** |
| 75 % bars | 0.22 / **0.16** | 0.97 / 0.98 | 0.50 / **0.47** |
| Tartan | 1.66 / **1.47** | **1.85** / 3.57 | 1.36 / 1.37 |
| Town map | 5.04 / **3.33** | 5.34 / **4.66** | 7.68 / **6.45** |
| Rural map | — | 4.15 / **3.62** | 5.27 / **4.51** |
| Text card | 6.64 / **4.19** | **4.22** / 4.72 | 6.96 / **6.24** |
| Green/magenta edges | 2.27 / **2.01** | **4.99** / 9.83 | **15.22** / 19.33 |
| Photo (butterfly) | — | **0.63** / 0.65 | 1.01 / **0.89** |
| Photo (Galtströms) | 1.18 / **0.65** | 1.43 / **1.21** | 2.54 / **1.84** |

The noisy bench gives the same pattern, for example the town map at 5.19 / 3.49 % cross-colour.

- **Chroma path.** The frame raster lowers cross-colour on every still that has any: by 29 % on the luma zone plate, 34 % on the town map and 37 % on the text card.
- **Luma path.**
  - Adding the 312-line partner lowers cross-luminance where the chroma is vertically smooth (Test Card F, both maps, the Galtströms photograph).
  - It raises it at horizontal colour edges (tartan 1.85 → 3.57, edge card 4.99 → 9.83). The partner is one row away in the other field, and at an edge it carries different chroma: the classic comb failure. The "||||" dots rise from 14 to 43 codes.
  - Keeping the luma path in the field while taking the frame raster's chroma gives the field's luma exactly, with the frame's cross-colour.
- **The acceptance is not met as written:** cross-luminance is not lower in every class. **H1: go for the chroma path.** For the luma, only an adaptive separation can use the partner without failing at edges. Step 3 supplies one.

**3. Transform PAL variants** (`transform.py`).

transform2d's filter was ported and generalised to the frame raster. There, U's two lobes share the reflection y → 3H/4 − y and V's share y → H/4 − y, and the two reflections differ. In a field one reflection serves both. So each bin is tested as U and as V, paired under whichever reflection's magnitudes agree better, and kept if that pair is within the threshold. With "keep if either passes", amplitude matching paired U's strong lobe with its weak one (ratio 0.41, inside 0.4) and lost 15 % of the chroma.

Clean unless marked. "Dots" is the "||||" amplitude at the edge card's two single green/magenta edges: the luma error in 3.5–5 MHz, as the worst row's sine amplitude in 8-bit codes. "Multiburst Y" is the luma zone plate's luma error, with Test Card F's gratings beside it.

| Variant | Dots clean / noisy | Luma zone plate Y % | Test Card F Y % | Town map ΔE | Text cross-colour % | Edge card ΔE |
|---|---|---|---|---|---|---|
| Field, 16 rows (= transform2d) | 43.0 / 43.4 | 3.05 | 1.25 | 7.20 | 4.70 | 20.15 |
| Field, 32 rows | 54.4 / 54.0 | 3.03 | 1.30 | 7.41 | 4.76 | 19.68 |
| Frame, 16 rows | 0.9 / 2.5 | 4.23 | 1.15 | 7.35 | 4.85 | 15.13 |
| **Frame, 32 rows** | **0.9 / 2.5** | **3.04** | **1.17** | 7.19 | 4.74 | 15.47 |
| Field, amplitude matching | 72.8 / 72.9 | 2.76 | 1.16 | 6.39 | 3.76 | 20.70 |
| Frame 32, amplitude matching | 25.3 / 25.6 | 2.76 | 1.05 | 6.39 | 3.84 | 16.08 |
| Field, test within ±0.55 MHz | 43.0 / 43.4 | 3.29 | 2.15 | 7.92 | 4.86 | 20.14 |
| Frame 32, test within ±0.55 MHz | 0.9 / 2.5 | 3.26 | 2.11 | 8.02 | 4.95 | 15.51 |
| Field, rejected bins to the comb | 39.1 / 39.3 | 5.53 | 1.48 | 7.37 | 5.06 | 18.34 |
| Frame 32, rejected bins to the comb | 1.0 / 2.1 | 4.98 | 1.34 | 7.38 | 5.28 | 15.45 |
| **Frame 32, frame U/V kernel** | **0.9 / 2.5** | **3.04** | **1.17** | **6.35** | **3.21** | 18.92 |

- **The frame raster removes the dots.** In a field, U and V answer to one reflection. Where a sharp green edge spreads their vertical spectra over each other, a bin's magnitude and its reflection's no longer match, and real chroma is thrown into luma. In the frame each component has its own reflection, so such a bin passes as whichever component dominates it. The luma zone plate and Test Card F lose nothing. Frame tiles of 32 rows (16 field lines of picture) beat 16 rows on the zone plate (3.04 against 4.23 %).
- **Amplitude matching: no-go.** It is the best on maps and the zone plate, but it leaves 25 codes of dots in the frame and 73 in the field. Where luma adds to one side of a pair, matching keeps the pair at the smaller side's level instead of rejecting it.
- **Restricting the test to the symmetric band: no-go.** On the clean bench it costs Test Card F's luma (1.17 → 2.11 %), and with noise the zone plate's (3.26 → 8.08 %). On a bench copy put through the Phase 2 replay response (`bench.composite("channel", …)`), it is never better. For example, the frame decoder's town map goes from 6.74 to 7.77 ΔE and the dots stay at 1.1. At threshold 0.4 the test tolerates 8 dB of mismatch, and the measured tilt is under 3 dB within ±1.6 MHz.
- **Handing rejected bins to a frame comb: no-go.** The bins the test rightly rejects are mostly luma, and the comb's estimate of them carries the comb's cross-colour (zone plate 3.04 → 4.98 %).
- **The U/V kernel after the transform matters.** transform2d demodulates its chroma with PalColour's field kernel. The same kernel sampled on the frame rows (±7 rows) lowers cross-colour (zone plate 3.53 → 2.50 %, Test Card F 1.09 → 0.58 %, text 4.74 → 3.21 %) and map ΔE (town 7.19 → 6.35). It costs the edge card (15.5 → 18.9 ΔE), whose 1- to 4-row stripes it smears.
  - Shortening the kernel trades the two. At ±3 rows the edge card reads 14.3 ΔE and the text card 5.80, but text cross-colour rises to 4.98 %, above transform2d's 4.70.
  - The choice belongs to Phase 4 step 4 (chroma output bandwidth).
  - Threshold 0.5 is slightly better than 0.4 on maps and text, but raises the noisy dots to 1.9 codes. 0.6 brings the dots back (20.7).
- **Cost.** In the prototype the frame transform takes 1.2× the field transform's time, from the second reflection test. The tile area per frame is the same.

**4. Least-squares-trained filters** (`training.py`, `encoder.py`, `lslcd.py`, `phase3_lslcd.py`).

- **The training set.** 24 procedural pictures, six each of maps, text, natural 1/f texture and saturated graphics, drawn from a seed. The text is set in DejaVu. None of it shares an asset with the evaluation stills. The last picture of each kind is held out.
- **Encoding.** The pictures are encoded on the bench's four frame phases by an encoder that matches videosynth's composite to 1–2 codes (`encoder.check_encoder`). Each is used clean, and again with the Domesday model's noise (the noisy bench less the clean one, on its unmeasured lead and tail frames).
- **The filters.** One kernel per class of row parity × carrier quadrant × V-switch, 16 classes, which is exact for the lattice. Each estimates Y, U and V from a composite window, and is solved by ridge-regularised least squares.

Y / U / V r.m.s. error (%, mean over pictures):

| Window | Training | Held out | Evaluation stills |
|---|---|---|---|
| Frame, ±4 rows × ±8 taps, clean | 2.42 / 2.06 / 2.70 | 2.53 / 2.35 / 2.62 | 3.76 / 3.26 / 4.18 |
| Field, ±4 rows × ±8 taps, clean | 3.00 / 2.56 / 3.36 | 3.13 / 2.79 / 3.36 | 4.45 / 3.97 / 4.80 |
| Frame, ±6 rows × ±10 taps, clean | 2.34 / 2.04 / 2.57 | 2.42 / 2.30 / 2.45 | 3.63 / 3.25 / 3.95 |
| Field, ±6 rows × ±10 taps, clean | 2.84 / 2.53 / 3.11 | 2.95 / 2.74 / 3.08 | 4.15 / 3.90 / 4.30 |
| Frame, ±6 × ±10, noise-trained, on noisy | 2.83 / 2.24 / 2.84 | 2.94 / 2.52 / 2.74 | 4.11 / 3.41 / 4.17 |
| Field, ±6 × ±10, noise-trained, on noisy | 3.23 / 2.71 / 3.37 | 3.37 / 2.94 / 3.36 | 4.56 / 4.01 / 4.50 |

- **H1 holds for the optimal linear filter.** Trained the same way, frame windows beat field windows in every component, at both sizes, clean and noisy, by 0.4–0.7 points on the evaluation stills.
- **Gain over the best hand-designed linear filter** (the frame PalColour of step 2) is mixed.
  - Lower luma error on 10 of the 12 stills (town map 5.12 against 5.47 %).
  - Far better on horizontal colour edges: edge card ΔE 7.49 against 19.33, the best of every decoder measured; text ΔE 5.28 against 6.24.
  - But 2–3× the cross-colour (luma zone plate 18.0 against 8.7 %, Test Card F 6.9 against 2.5 %, text 10.0 against 4.2 %), so ΔE is worse on the maps (8.69 against 6.45) and the photographs (Galtströms 2.89 against 1.84).
- **The generalisation gap is the training set's.** Training to held out costs 0.1–0.3 points; training to evaluation costs about 1.2.
  - The evaluation stills carry far more luma detail near fsc than the procedural set.
  - On the training set the least-squares optimum spends the luma rejection on chroma detail.
  - A training set richer in luma detail would move the balance. But no linear filter reaches the transform's luma zone plate (10.2 against 3.0 %).
- **H3: no-go as the core separator.** It is beaten by the frame-raster transform on every primary metric. It is carried to Phase 4 as the local-selection fallback for horizontal colour edges, where it is the best of everything measured.

**5. Funnel** (`phase3_run.py`).

Primary metrics per class, as the acceptance names them: ΔE for photos and maps, cross-colour for text and gratings. The gratings are the luma zone plate and Test Card F.

Best existing (`pal2d` / `transform2d`) against the candidate:

| Class | Still | Metric | Clean | Noisy |
|---|---|---|---|---|
| Photos | Butterfly | ΔE | 1.16 / 0.94 → **0.86** | 2.60 / 2.44 → **2.00** |
| Photos | Galtströms | ΔE | 2.60 / 2.41 → **2.00** | 3.50 / 3.31 → **2.70** |
| Maps | Town | ΔE | 7.74 / 7.20 → **6.35** | 8.26 / 7.62 → **6.66** |
| Maps | Rural | ΔE | 5.33 / 5.55 → **4.93** | 5.87 / 6.00 → **5.26** |
| Text | Text card | Cross-colour % | 6.64 / 4.70 → **3.21** | 6.69 / 4.75 → **3.28** |
| Gratings | Luma zone plate | Cross-colour % | 12.28 / 3.53 → **2.50** | 12.29 / 3.57 → **2.54** |
| Gratings | Test Card F | Cross-colour % | 3.20 / 1.10 → **0.58** | 3.31 / 1.29 → **0.78** |

**The candidate: frame-raster Transform PAL.**

- **Tiles.** Hann-windowed tiles of 32 frame rows × 32 samples, with 50 % overlap-add.
- **The test.** Every bin of 0.5–1.5 fsc is tested as U (y → 3H/4 − y) and as V (y → H/4 − y), and paired under the better-agreeing reflection. It is kept whole (binary) at threshold 0.4.
- **Luma** is the composite less that chroma.
- **U and V** come from per-row demodulation on the burst-fitted lattice, filtered by PalColour's radial raised cosine sampled on the frame rows.

It beats both existing decoders on all fourteen entries. It also leads on the stress card, Test Card F, the bars and the tartan (ΔE 0.86, 1.08, 0.36, 1.27 against 1.04, 1.37, 0.42, 1.51 at best).

Nothing else passes, so no second candidate goes forward:

- The frame transform with transform2d's field U/V kernel misses the rural map (5.45 against 5.33).
- Amplitude matching misses the dots.
- The frame PalColour misses the gratings: zone plate cross-colour 8.69 against 3.53 %.
- The least-squares filters miss the gratings, text and maps.

**What the candidate leaves for Phase 4:**

- **The edge card.** ΔE 18.9 against `pal2d`'s 15.4. The U/V kernel smears the 1- to 4-row stripes. The least-squares frame filter reads 7.5 there, and the ±3-row U/V kernel 14.3.
- **Luma on maps and text.** Error is a little above `pal2d`'s (town 6.85 against 6.29 %, text 6.80 against 5.16 %), though both their ΔE and cross-colour are better.
- **The chroma zone plate.** ΔE 4.29 against 3.76: fine chroma smeared vertically, as on the edge card.

**Go/no-go.**

- **H1: go.** Separation on the woven frame beats the field in every form tried:
  - the fixed linear filter's chroma path;
  - the optimal linear filter (0.4–0.7 points);
  - Transform PAL ("||||" dots 43 → 0.9 codes).
- **H2: go**, with binary decisions on the frame raster's two reflections. Amplitude matching, band-restricted testing and the comb fallback for rejected bins are each no-go.
- **H3: no-go as the core.** It is carried to Phase 4 as the fallback for horizontal colour edges.

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

### Phase 4 outcome (2026-11-03)

Prototyped in [`tools/domesday2d-bench/`](../tools/domesday2d-bench/) (`adaptive.py`, `hanover.py`, `phase4_run.py`, `phase4_hanover.py`, `phase4_jitter.py`), on the Phase 1 bench, clean and noisy, after the Phase 3 separation.

| Step | State |
|---|---|
| 1. Local selection | **Done. Acceptance met.** The selection is an edge-preserving vertical chroma fit whose strength follows the local luma detail. It beats the Phase 3 candidate and every fallback on every class's primary metric. The least-squares fallback is dropped: even an oracle choosing it per block loses |
| 2. Anti-Hanover | **Done. Acceptance met** under the Phase 2 impairments as measured: a per-source 2×2 correction, plus masked field-line averaging that checks the 312-line partner. Hanover residue on the bars is 0.09 % against `pal2d`'s 0.13 %, and vertical chroma edges rise in 0.8 rows against `pal2d`'s 7.5. Per-line burst referencing: no-go, confirmed on a capture |
| 3. Residual refinement | **Done.** Handing the unexplained chroma residual back to luma: **go**, +1.6 to +4.6 dB on maps and text for 0.3× `pal2d`'s CPU time. Iterated decode → re-encode passes: **no-go**, they cost primary metrics |
| 4. Chroma output bandwidth | **Done.** Vertical: set by the edge-preserving prior, not a fixed low-pass and not none. Horizontal: PalColour's raised cosine (1.1 MHz / 0.93) is kept |

**What Phase 3 left was after separation.** The frame-raster transform separates the chroma well; the fixed U/V kernel after it (PalColour's radial raised cosine over ±7 frame rows) does the damage. Without any vertical filter the edge card falls from 18.9 to 2.1 ΔE and the chroma zone plate from 4.3 to 1.3. But the transform's residual luma leak then shows as cross-colour: text card 3.2 → 10.5 %, town map 3.0 → 9.3 %. Every fixed kernel trades one against the other (Phase 3 step 3).

**The Phase 4 chroma path** (`adaptive.py`), on the separated chroma c:

1. Each row is demodulated on its own carrier and filtered horizontally only, by PalColour's raised cosine.
2. **A vertical fit along each column of the frame raster:** minimise |W − W₀|² + Σ λ(r) · √(ε² + |W(r+1) − W(r)|²) over W = (U, V), a vector Charbonnier (smoothed total-variation) prior.
   - Solved by six reweighted least-squares iterations, each one tridiagonal solve per column.
   - ε = 0.002.
   - **The strength λ = 0.01 + 5 × the local r.m.s. of the 3.5–5 MHz luma** that the transform left (5 rows × 15 samples). Where the luma is busy, the leak is flattened; where it is quiet, chroma steps are kept.
3. **The residual c − M(U, V)** (chroma the fit does not explain) is handed back to luma:
   - after soft shrinkage against the chroma-band noise measured on the blanked lines (field lines 7–16): gain max(0, 1 − σ/envelope), envelope over 3 rows × 9 samples;
   - only where the luma has detail: gain × d / (d + 0.003), d the luma detail of step 2.

Clean / noisy bench. "Phase 4" is steps 1–3; step 2's measures are added below.

| Variant | Town ΔE | Rural ΔE | Text cross-colour % | Luma zone plate cross-colour % | Test Card F cross-colour % | Butterfly ΔE | Galtströms ΔE | Edge card ΔE | Town Y % |
|---|---|---|---|---|---|---|---|---|---|
| Phase 3 candidate (fixed ±7-row kernel) | 6.35 / 6.66 | 4.93 / 5.26 | 3.21 / 3.28 | 2.50 / 2.54 | 0.58 / 0.78 | 0.86 / 2.00 | 2.00 / 2.70 | 18.92 / 19.22 | 6.85 / 6.97 |
| Fixed kernel, ±3 rows | 6.66 / 7.09 | 4.54 / 5.04 | 4.76 / 4.86 | 2.61 / 2.69 | 0.74 / 1.05 | 0.65 / 2.22 | 1.98 / 2.97 | 14.28 / 14.67 | 6.85 / 6.97 |
| No vertical filter | 9.30 / 10.12 | 5.97 / 7.04 | 10.48 / 10.65 | 5.23 / 5.42 | 1.94 / 2.49 | 0.82 / 3.92 | 3.38 / 5.22 | 2.06 / 3.09 | 6.85 / 6.97 |
| Oracle: Phase 3 or least squares per 8×8 block | 6.42 / 6.90 | 4.45 / 5.02 | 6.98 / 7.00 | 2.64 / 2.67 | 0.60 / 0.80 | 0.72 / 1.96 | 2.22 / 2.89 | 7.38 / 7.95 | 5.37 / 5.62 |
| Fit, constant λ 0.05 | 6.69 / 7.03 | 4.26 / 4.61 | 6.80 / 6.93 | 4.00 / 4.01 | 0.85 / 0.95 | 0.90 / 1.76 | 2.01 / 2.58 | 3.06 / 3.42 | 6.85 / 6.97 |
| Fit, λ from luma detail (step 1) | 5.91 / 6.16 | 4.38 / 4.67 | 2.44 / 2.50 | 1.68 / 1.70 | 0.33 / 0.42 | 0.68 / 1.86 | 1.98 / 2.61 | 5.26 / 5.93 | 6.85 / 6.97 |
| + all of the residual to luma | 4.52 / 4.96 | 3.81 / 4.26 | 2.44 / 2.50 | 1.68 / 1.70 | 0.33 / 0.42 | 0.64 / 2.21 | 1.59 / 2.56 | 5.98 / 6.87 | 2.66 / 3.25 |
| **+ residual shrunk and detail-gated (Phase 4)** | **4.55 / 5.03** | **3.80 / 4.22** | **2.44 / 2.50** | **1.68 / 1.70** | **0.33 / 0.42** | **0.65 / 1.86** | **1.64 / 2.49** | 5.83 / 6.46 | **2.69 / 3.40** |
| Phase 4, two re-encode passes | 4.66 / 5.18 | 3.71 / 4.20 | 2.95 / 3.01 | 1.73 / 1.75 | 0.35 / 0.46 | 0.58 / 1.96 | 1.62 / 2.58 | 5.43 / 6.10 | 2.67 / 3.43 |
| Phase 4, horizontal 1.5 MHz | 4.66 / 5.18 | 3.74 / 4.22 | 3.04 / 3.11 | 1.71 / 1.73 | 0.34 / 0.45 | 0.60 / 1.95 | 1.63 / 2.58 | 5.52 / 6.17 | 2.70 / 3.46 |

The "||||" dots stay at 0.8 codes clean, 2.4 noisy.

**1. Local selection.**

- **The least-squares fallback is dropped.** An oracle that knows the truth and picks the Phase 3 candidate or the least-squares frame filter for each 8×8 block is still far behind the fit: text cross-colour 7.0 against 2.4 %, town 6.4 against 4.6 ΔE. No real selector between the two can do better than that oracle.
- **The feature is the local luma detail.** A constant strength cannot serve both cases: at λ 0.05 text cross-colour is still 6.8 %, and at λ 0.1 the edge card is already at 4.4 ΔE. Steering λ by the 3.5–5 MHz luma that the transform left lets it go high only where leakage can come from.
- **The acceptance is met.** Phase 4 beats the Phase 3 candidate and each fallback on every primary metric, clean and noisy. The only stills it does not win are the stress elements:
  - The edge card is best decoded with no vertical filter at all (2.1 against 5.8 ΔE), but that version fails everything else.
  - The bars and tartan are best with two re-encode passes.
- **Licence.** The selection is a continuous regularisation strength inside one estimator. It does not choose between line, field and frame combs per region, which is what US9432648's claims cover as noted in A3. This is an engineering reading, not a legal opinion.

**2. Anti-Hanover** (`hanover.py`, `phase4_hanover.py`, `phase4_jitter.py`).

The impairment uses the Phase 2 figures:

- a chroma-to-burst phase error of −4°;
- an alternating crosstalk of V→U 0.06 and U→V −0.02;
- line jitter, with the split measured below.

The Phase 4 fit keeps a line-alternating error that the fixed kernel averaged out. In the frame raster the 312-line partner has the same V-switch, so the error alternates in row pairs, and the fit takes a 4° step as structure. Uncorrected, the bars' Hanover residue is 3.7 % against `pal2d`'s 0.13 %.

- **Per-capture 2×2 correction.** W = (I + sM)⁻¹ W_s on each row, after demodulation and before the vertical fit.
  - M is fitted from field-adjacent rows of flat colour.
  - **It must be pooled over many frames.** A single frame's fit is fooled by fine colour stripes one field line high: on the edge card the correction then costs 1.5–1.8 ΔE with no impairment at all. Pooled over the twelve stills, with pairs whose alternating half exceeds 0.3 of their mean set aside, it reads [[0, 0.129], [−0.089, 0]] against the true [[0, 0.130], [−0.090, 0]] (noisy: 0.128, −0.087).
  - Re-encoding a corrected estimate (the residual of step 3) must go through the inverse correction. Otherwise the residual carries the error straight back.
- **Masked field-line averaging.** A [¼, ½, ¼] average over field lines (raster rows r ± 2), applied only where two conditions hold:
  - the field lines either side agree (not at a vertical chroma step);
  - the 312-line partner row deviates the same way as row r (difference under 0.04).

  A Hanover error repeats on the partner, which has the same switch; a stripe one field line high in one field does not. Without that second condition the averaging cost the edge card 5.4 → 10.9 ΔE (measured with two re-encode passes).
- **The line jitter, measured on a capture** (`phase4_jitter.py`, Community South DS1, 150 frames, 798 flat-colour line pairs). Same-switch line pairs decoded against the field-fitted lattice differ in hue by 6.38° r.m.s.; against each line's own burst, by 6.78°.
  - So the picture does not follow its burst line by line. Splitting the variance gives about 2.0° shared by picture and burst, and 2.6° of burst-reading noise.
  - National A (90 pairs) and Volcanoes (6) have too little flat colour to decide.
  - The bench impairment uses that split. "All shared" (3.4°) is the worst case for the field fit.

Hanover residue on the bars (field-line alternation of the decoded U/V on flat colour, amplitude in % of white), and the rise of the decoded chroma across the edge card's single green/magenta edges:

| Decoder | Residue, measured split, clean / noisy | Residue, all shared, clean / noisy | Edge rise (rows) | Bars ΔE (measured split, clean) |
|---|---|---|---|---|
| `pal2d` (prototype) | 0.13 / 0.15 | 0.16 / 0.18 | 7.5 | 0.82 |
| Phase 3 candidate | 0.11 / 0.12 | 0.13 / 0.14 | 6.9 | 0.63 |
| Phase 4, uncorrected | 3.72 / 2.76 | 3.55 / 2.69 | 0.8 | 2.49 |
| + matrix from the frame alone | 0.25 / 0.23 | 0.60 / 0.45 | 0.8 | 0.68 |
| + pooled matrix | 0.25 / 0.23 | 0.60 / 0.45 | 0.8 | 0.67 |
| + masked averaging only | 0.32 / 0.30 | 0.92 / 0.83 | 0.8 | 0.84 |
| **+ pooled matrix + masked averaging** | **0.09 / 0.09** | 0.29 / 0.26 | **0.8** | **0.61** |
| + pooled matrix + per-line burst | 0.41 / 0.49 | 0.05 / 0.25 | 0.8 | 0.78 |

- **The acceptance is met** for the measured impairments. The residue is below `pal2d`'s and vertical chroma edges are no softer than Phase 4 without the measures. Test Card F's residue is 0.13 against `pal2d`'s 0.40.
- **Per-line burst referencing: no-go.** It wins only if all the jitter is shared, and the capture says it is not. The decoder could make that check per source (as `phase4_jitter.py` does) and switch to it. If all the jitter were shared, the chosen measures would leave 0.29 against `pal2d`'s 0.16.
- **The pooled matrix is a per-source parameter.** Phase 6 needs a way to measure it over a capture, as Phase 5 step 2 does for the equaliser.
- With no impairment the measures cost almost nothing: butterfly 0.65 → 0.71 ΔE clean, and 1.86 → 1.79 noisy (better).

**3. Residual refinement.**

- **Reassigning the residual: go.** It is what lowers map and text luma below `pal2d`'s, the Phase 3 leftover: town 6.85 → 2.69 %, text 6.80 → 3.83 %, luma zone plate 3.04 → 0.95 %. RGB PSNR against the fit alone:

| Still | Clean | Noisy |
|---|---|---|
| Town | 22.18 → 26.81 (+4.6 dB) | 22.03 → 25.92 (+3.9 dB) |
| Rural | 24.00 → 25.83 (+1.8 dB) | 23.78 → 25.41 (+1.6 dB) |
| Text | 21.34 → 23.77 (+2.4 dB) | 21.18 → 23.42 (+2.2 dB) |

  - The noise-weighted data term matters. Handing all of the residual to luma moves chroma-band noise into luma: noisy butterfly 1.86 → 2.21 ΔE, bars luma 1.17 → 2.05 %. Shrinking it against the blanked lines' noise and gating it by luma detail keeps the gain and loses nothing on the photographs.
  - A per-frame noise estimate from the picture itself (its median envelope) mistakes map and text detail for noise. The blanked lines do not.
- **Iterated decode → re-encode: no-go.** A second pass of the fit on the re-encoded residual helps flat colour (bars ΔE 0.34 → 0.15) and the edge card. But it costs the primary metrics: text cross-colour 2.44 → 2.95 %, noisy butterfly 1.86 → 1.96, town 4.55 → 4.66. On maps and text it gains under 0.5 dB, except clean text (+0.57 dB).
- **Cost** (C++ microbenchmark of the stage, scalar double precision, one thread, against decode-orc's own decoders on a 100-frame Community South cut; pipeline overhead of 21 ms a frame taken off):

| Part | ms / frame |
|---|---|
| `pal2d` | 15.6 |
| `transform2d` | 37 |
| Luma detail (31-tap band-pass, box) | 9.5 |
| Demodulation and horizontal filter | 5.7 |
| Vertical fit, 6 iterations | 28 |
| Residual reassignment | 4.5 |
| A second re-encode pass | 30 |

  - The refinement that is kept, the reassignment, costs 0.3× `pal2d`, well inside the 3× gate. The second pass would cost 1.9×.
  - Three iterations of the fit instead of six save 11 ms but raise text cross-colour to 2.94 %.
  - The correction and masked averaging are a 2×2 multiply and a few box filters, about the reassignment's cost (not timed).

**4. Chroma output bandwidth.**

- **Vertical: prior-defined.** Every fixed vertical low-pass trades cross-colour against edges (±7 rows: text 3.21 %, edge card 18.9 ΔE; ±3 rows: 4.76 %, 14.3). None (the horizontal filter only) is the best on the edge card and the chroma zone plate and the worst everywhere else. The Charbonnier fit's bandwidth varies with the picture and beats both on every primary metric.
- **Horizontal: PalColour's raised cosine (1.1 MHz / 0.93), unchanged.** At 1.5 MHz the flat colour and the chroma zone plate improve (bars ΔE 0.34 → 0.23), but text cross-colour rises from 2.44 to 3.04 % and map ΔE by 0.1.

**The training pictures.** The Phase 4 parameters were chosen on the evaluation stills. On the procedural pictures of Phase 3 step 4 (three of each kind; none was used here), Phase 4 against the Phase 3 candidate:

| Kind | ΔE clean | ΔE noisy | RGB PSNR clean (dB) | Y % clean |
|---|---|---|---|---|
| Maps | 8.10 → 6.09 | 8.32 → 6.49 | 20.1 → 23.4 | 8.19 → 4.17 |
| Text | 3.65 → 2.33 | 4.07 → 2.92 | 25.6 → 29.0 | 1.76 → 1.96 |
| Natural texture | 3.87 → 2.83 | 4.29 → 3.51 | 30.5 → 33.9 | 1.42 → 1.26 |
| Graphics | 4.80 → 1.79 | 5.22 → 2.26 | 20.4 → 27.2 | 1.79 → 2.54 |

ΔE and PSNR improve on every kind. Luma is worse on text and graphics: their saturated colour edges carry chroma the fit does not follow, and part of it is handed to luma.

**The Phase 4 decoder.**

1. Phase 3's frame-raster Transform PAL separation.
2. Per-row demodulation and PalColour's horizontal filter.
3. The per-source 2×2 line-alternating correction.
4. Partner-checked masked field-line averaging.
5. The luma-steered vertical Charbonnier fit.
6. The noise-shrunk, detail-gated residual back to luma.

Against the best existing decoder (`pal2d` / `transform2d`), Phase 4 with the step 2 measures, no impairment:

| Class | Still | Metric | Clean | Noisy |
|---|---|---|---|---|
| Photos | Butterfly | ΔE | 1.16 / 0.94 → **0.71** | 2.60 / 2.44 → **1.79** |
| Photos | Galtströms | ΔE | 2.60 / 2.41 → **1.69** | 3.50 / 3.31 → **2.47** |
| Maps | Town | ΔE | 7.74 / 7.20 → **4.58** | 8.26 / 7.62 → **5.04** |
| Maps | Rural | ΔE | 5.33 / 5.55 → **3.83** | 5.87 / 6.00 → **4.23** |
| Text | Text card | Cross-colour % | 6.64 / 4.70 → **2.44** | 6.69 / 4.75 → **2.50** |
| Gratings | Luma zone plate | Cross-colour % | 12.28 / 3.53 → **1.68** | 12.29 / 3.57 → **1.70** |
| Gratings | Test Card F | Cross-colour % | 3.20 / 1.10 → **0.33** | 3.31 / 1.29 → **0.42** |

The Phase 3 leftovers are closed:

- The edge card is 5.8 ΔE against `pal2d`'s 15.4.
- Map and text luma are below `pal2d`'s.
- The chroma zone plate is 2.0 ΔE against 3.8.

**What Phase 4 leaves:**

- **Cost.** The separation (about 1.2 × `transform2d`, 44 ms) plus the chroma path (about 52 ms) is about 2.6× `transform2d`'s decoder time in this scalar estimate. Phase 6 step 5's design review will be triggered. Single-precision SIMD and running the vertical fit on column blocks are the obvious levers.
- **The edge card's single-row colour stripes.** They are beyond what any smooth chroma estimate follows, and the luma there is 10.3 % in error (3.7 % without reassignment). It is a stress pattern, outside PAL's vertical chroma capacity in one field.
- **The per-source Hanover matrix**, and the per-source choice of burst reference, for Phase 5/6 to source.

**Go/no-go.**

- **H4: go**, in the form of a single residual reassignment with a noise-weighted, detail-gated data term behind an edge-preserving (Charbonnier) chroma prior. Iterated re-encode refinement is no-go.
- **H6:**
  - per-line burst referencing: **no-go**, on the bench and on a capture;
  - per-source 2×2 correction: **go**;
  - masked suppression: **go**, with the 312-line partner check.
- **H3's fallback:** dropped. Local selection is done inside the chroma fit.

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

### Phase 5 outcome (2026-11-03)

Prototyped in [`tools/domesday2d-bench/`](../tools/domesday2d-bench/):

- `equalise.py` is the equaliser.
- `phase5_bench.py` writes the channel bench.
- `phase5_run.py`, `phase5_vits.py` and `phase5_upstream.py` measure it.

The decoder is the Phase 4 one (frame-raster transform, partner-checked masked averaging, vertical fit, residual reassignment). The equaliser runs on the composite before it.

| Step | State |
|---|---|
| 1. Prototype | **Done.** The inverse of the one-pole smear, followed by a burst-referenced chroma gain. It beats no equalisation on photographs, maps and the edge card, and leaves the clean bench bit-identical. Text and grating cross-colour rise by the chroma gain it restores (×1.08), no more. On real VITS the smear tail is gone and the 20T delay is within ±20 ns. The inverse replay response: **no-go** |
| 2. Parameter source | **Done.** Measured once per source, from the VITS of many frames, with guards that read "no smear" when the fit cannot tell. Per frame: **no-go** (no single frame's fit is usable). Nominal: **no-go** as a default (it costs every disc without the smear) |
| 3. Upstream stages | **Done.** No double correction. `fm_beat_cancel` and `picture_band_limit` leave the VITS and the smear as they were, and the stacker's output carries the stack's own smear. The equaliser gains the same behind each |
| 4. Parameter surface | **Done. Agreed with the user (2026-11-03):** a **Channel Equalise** transform stage of its own, not a Domesday2D option. It helps every PAL decoder |

**The channel bench.** `phase5_bench.py` renders `pal_stills` with the four LaserDisc VITS lines (CCIR 17, 18, 330, 331), in four copies:

- without and with the Phase 2 channel (edge smear 0.24, τ 6.1, and the inner-radius `response_db` of videosynth#14);
- with and without the noise.

The truth is the bench's. On these renders the VITS fit reads 0.190, τ 5.65 clean and 0.184, τ 5.79 noisy, not the 0.24 that was put in. The fit sees the smear and the replay response together, as it does on a capture, whose 0.24 is likewise the two combined.

**The equaliser** (`equalise.py`):

1. **The inverse smear.** 1/H(z) = (1 − a z⁻¹) / ((1 − c a) − (1 − c) a z⁻¹), with c the fraction and a = exp(−1/τ).
   - It runs over the composite in transmission order, sync and blanking included, as the smear did.
   - Its pole, (1 − c) a / (1 − c a), is 0.81 for c 0.24 and τ 6.1, so the filter is stable, with a tail of 4.7 samples.
   - It costs two multiply-adds a sample.
2. **The burst-referenced chroma gain.** Nominal burst amplitude over the frame's median burst amplitude on the active lines, applied to U and V after decoding.
   - **It is not optional behind the inverse smear.** The smear takes 2.2 dB off the subcarrier and the inverse puts it back, so after the inverse the bench's chroma is 11 % strong (bars chroma gain error 8.4 % → 10.7 %, in the other direction).
   - This is the same effect as dp111's chain: deghost lifts the carrier by about 1.18 and later steps take it off again.
   - With the gain, the error is 0.66 %.
   - decode-orc's PAL decoders normalise the burst's phase but not its amplitude, so today their saturation follows each capture's chroma level.

Phase 4 decoder, without and with the equaliser. Smear measured from each render's own VITS. Clean / noisy.

| Bench | Equaliser | Butterfly ΔE | Galtströms ΔE | Town ΔE | Rural ΔE | Text cross-colour % | Luma zone plate cross-colour % | Test Card F cross-colour % | Edge card ΔE | Town Y % | Bars chroma gain error % |
|---|---|---|---|---|---|---|---|---|---|---|---|
| No channel | none | 0.71 / 1.79 | 1.69 / 2.47 | 4.58 / 5.04 | 3.83 / 4.23 | 2.44 / 2.50 | 1.68 / 1.70 | 0.33 / 0.42 | 5.83 / 6.45 | 2.70 / 3.40 | 0.65 / 0.69 |
| No channel | **measured** (reads "no smear") | 0.71 / 1.80 | 1.69 / 2.47 | 4.58 / 5.04 | 3.83 / 4.23 | 2.44 / 2.50 | 1.68 / 1.70 | 0.33 / 0.42 | 5.83 / 6.46 | 2.70 / 3.40 | 0.63 / 0.91 |
| No channel | nominal 0.24 / 6.1 | 1.00 / 2.07 | 2.13 / 2.84 | 5.00 / 5.32 | 4.19 / 4.50 | 2.45 / 2.50 | 1.66 / 1.67 | 0.32 / 0.41 | 6.12 / 6.79 | 4.86 / 4.90 | 0.60 / 0.83 |
| Channel | none | 1.29 / 2.11 | 2.18 / 2.81 | 5.30 / 5.68 | 4.46 / 4.79 | 1.49 / 1.52 | 1.58 / 1.59 | 0.30 / 0.40 | 7.62 / 8.02 | 4.63 / 5.20 | 8.35 / 8.32 |
| Channel | burst gain only | 1.00 / 2.00 | 2.07 / 2.75 | 5.22 / 5.65 | 4.40 / 4.76 | 1.61 / 1.64 | 1.71 / 1.72 | 0.32 / 0.43 | **6.36 / 6.96** | 4.63 / 5.20 | 0.62 / 0.94 |
| Channel | inverse smear only | 1.29 / 2.23 | 1.94 / 2.72 | 5.21 / 5.60 | 4.58 / 4.87 | 1.79 / 1.82 | 1.88 / 1.88 | 0.35 / 0.46 | 7.17 / 7.07 | 4.39 / 4.86 | 10.70 / 10.06 |
| Channel | **inverse smear + burst gain, measured** | **0.82 / 1.99** | **1.85 / 2.64** | **4.99 / 5.42** | **4.32 / 4.68** | 1.60 / 1.64 | 1.69 / 1.70 | 0.31 / 0.42 | 6.53 / 7.18 | **4.39 / 4.86** | 0.66 / 0.87 |
| Channel | the same, nominal (the true smear) | 0.84 / 2.03 | 1.91 / 2.70 | 5.03 / 5.45 | 4.36 / 4.72 | 1.60 / 1.63 | 1.68 / 1.69 | 0.31 / 0.42 | 6.59 / 7.26 | 4.70 / 5.09 | 0.67 / 0.86 |
| Channel | the same + inverse replay response to 5 MHz | 0.95 / 2.14 | 1.87 / 2.82 | 5.07 / 5.62 | 4.22 / 4.67 | 1.85 / 2.01 | 1.70 / 1.71 | 0.32 / 0.44 | 6.43 / 7.06 | 4.01 / 4.90 | 0.66 / 0.88 |

**1. The prototype.**

- **The acceptance, read class by class:**
  - **Photos and maps:** better on every entry, clean and noisy. Town 5.30 → 4.99, butterfly 1.29 → 0.82; town luma 4.63 → 4.39 %, butterfly luma 0.90 → 0.50 %.
  - **Text and gratings:** cross-colour rises from 1.49 to 1.60 % (text) and from 1.58 to 1.69 % (luma zone plate). That is the uncorrected channel's 8 % chroma loss being restored: the channel had cut the cross-colour along with the true chroma. The inverse smear itself adds nothing (burst gain only 1.61, both 1.60).
  - **Edge card:** better than none (7.62 → 6.53), but worse than the burst gain alone (6.36). The noisy "||||" dots rise from 2.19 to 2.56 codes, because the inverse lifts the noise by up to 2.3 dB above 2 MHz.
- **No worse on the clean bench.** On both renders without the channel, the measured smear is "none", and the output is bit-identical to no equaliser.
- **The measured value beats the true one.** 0.19 / 5.65 from the VITS beats the 0.24 / 6.1 that was put in, because it includes the replay response's share of the tail.
- **The inverse replay response: no-go.** It inverts the bench's own response, capped at 6 dB and held flat above 5 MHz. It lowers map luma (town 4.39 → 4.01 %), but raises text cross-colour (1.60 → 1.85 %) and costs the photographs. On the no-channel bench it costs everything. The response is left alone.
- **Real VITS** (`phase5_vits.py`; Domesday Community South DS1, Community North DS1 and National A DS2, 2026 decodes; 48 frames at the inner, middle and outer radius):

| Quantity | Before | After the inverse smear at the source's value |
|---|---|---|
| Smear fit on the tails | 0.18–0.28, τ 5.7–7.0 | 0.003–0.028; every radius is rejected as "no smear" |
| Tail residual (IRE r.m.s.), smear fit / no smear | 0.49–0.74 / 0.98–1.19 | 0.46–0.90 / 0.47–0.92 (no difference left to fit) |
| 20T chroma − luma delay | −52 to −85 ns | −19 to +14 ns |
| 20T chroma/luma gain | 0.85–0.99 | 1.06–1.23 (the subcarrier lift that the burst gain takes off) |
| Multiburst 0.5–4.8 MHz, against the flag | −0.4 to −1.6 dB | +0.4 to +2.4 dB |

**The stated tolerance.** After the equaliser, the tail is not measurable and the 20T delay is within ±20 ns. The multiburst is within +0.4 to +2.4 dB of the flag from 0.5 to 4.8 MHz, a lift rising towards 4 MHz.

That lift is the replay response that Phase 2 reported less the smear (+0.6 dB at 0.5 MHz to +2.1 dB at 4 MHz). The bench shows it is better left in place than inverted.

**2. Parameter source.**

- **Per frame: no-go.** On the noisy bench no frame's own fit was usable, so every frame fell back to no correction.
  - On the captures, 16 single frames at each radius of six sides all read "no smear".
  - One line's tail is too noisy for the guard to accept it. A frame-by-frame measurement inside the decoder therefore cannot work.
- **Per source: go.** One fit on line 19 averaged over many frames.
  - **Guards.** It is accepted only if all three hold:
    - the fraction is below 0.45 and τ is at least 2 samples (the generator's own edge shaping fits as 0.5 at τ 1.1);
    - the smear explains the tails at least 20 % better than no smear;
    - otherwise the result is "no smear".
  - **The source's value is the median of the three radius fits.**

| Capture | Inner | Middle | Outer | Source value | Burst / nominal, inner / middle / outer |
|---|---|---|---|---|---|
| Community South DS1 | 0.243, 6.1 | 0.182, 7.0 | 0.278, 5.7 | 0.243, 6.13 | 1.08 / 1.00 / 0.95 |
| Community North DS1 | 0.236, 6.2 | 0.183, 6.9 | 0.250, 5.9 | 0.236, 6.22 | 1.11 / 0.98 / 1.20 |
| National A DS2 | 0.241, 6.3 | 0.235, 6.1 | 0.261, 5.9 | 0.241, 6.10 | 1.08 / 0.98 / 1.03 |
| National B DS2 (CLV) | none | none | none | none | 1.05 / 1.05 / 1.06 |
| Volcanoes DS1 | 0.10, 5.1 (rejected) | 0.11, 4.6 (rejected) | 0.24, 3.7 (rejected) | none | 1.23 / 1.24 / 1.44 |
| EcoDisc side 1 DS1 | 0.21, 3.1 (rejected) | 0.07, 4.0 (rejected) | 0.13, 3.0 (rejected) | none | 1.19 / 1.21 / 1.22 |

- **The Domesday CAV sides agree** to within 0.01 in fraction and 0.12 sample in τ.
  - Within a side the radius fits spread by ±0.05, with no consistent trend (the middle is lowest on both Community sides).
  - A per-radius schedule is not justified.
  - On Volcanoes and EcoDisc the smear-shaped tail is short and explains less than 20 % of the tail, so the guard leaves them alone.
- **Nominal (0.24, 6.1) as a default: no-go.** On a source without the smear (CLV, the other discs), it costs every class: butterfly 0.71 → 1.00, town luma 2.70 → 4.86 %. The decoder cannot detect that frame by frame, as shown above.
- **The fallback when the VITS cannot be measured is "off", not nominal.** That case is a source with no bar on line 19, or one whose fit is rejected.
  - The averaged fit already skips frames whose bar edge is not found (a dropout on it).
  - A rejected fit is indistinguishable from a disc without the smear.
- **The burst gain is per frame.** It is the median over about 570 lines, so it needs no source-level measurement. It also tracks the decode's MTF changes across a side: 0.95–1.20 within one Domesday side.
  - Volcanoes and EcoDisc carry 19–44 % more burst than nominal, so burst-referencing would reduce their saturation by that much.
  - On Domesday the CCIR 331 bar keeps its nominal ratio to the burst (Phase 2), so burst-referencing is right there.
  - The CCIR 331 bar reads 2.18 on Volcanoes against a nominal 2.33, so there it would leave the picture about 7 % under-saturated.

**3. Upstream stages** (`phase5_upstream.py`). The channel bench, noisy, run with `orc-cli` through each stage, then measured by the equaliser from its own VITS and decoded by Phase 4:

| Input → stage | Measured smear | Town ΔE, none → equalised | Butterfly ΔE | Bars ΔE | Edge card ΔE |
|---|---|---|---|---|---|
| Channel bench | 0.184, 5.79 | 5.68 → 5.42 | 2.11 → 1.99 | 1.71 → 1.01 | 8.02 → 7.18 |
| → `picture_band_limit` (5.4 MHz) | 0.184, 5.79 (unchanged) | 5.79 → 5.56 | 2.08 → 1.94 | 1.72 → 1.00 | 8.10 → 7.26 |
| With an FM beat (−24 dB, 7.088 MHz) | 0.184, 5.79 | 5.69 → 5.44 | 2.12 → 1.99 | 1.86 → 1.52 | 8.13 → 7.47 |
| → `fm_beat_cancel` (7.088 MHz) | 0.184, 5.79 (unchanged) | 5.69 → 5.44 | 2.12 → 1.99 | 1.71 → 1.01 | 8.02 → 7.17 |
| Two captures → `stacker` (defaults) | 0.180, 5.87 (the two read 0.184 and 0.174) | 5.66 → 5.40 | 2.06 → 1.92 | 1.70 → 0.99 | 8.00 → 7.12 |

- **There is no double correction.**
  - `picture_band_limit` and `fm_beat_cancel` never touch the VBI lines and do not change the response below 5.4 MHz, so the VITS still measure the smear the picture carries.
  - The stacker averages the VITS with the picture. Its `chroma_gain_match` scales each source's burst with its picture chroma, so the burst gain downstream still reads the stack's chroma.
  - The equaliser's gain is the same behind every stage: town −0.24 to −0.26 ΔE, butterfly −0.12 to −0.14, bars to 1.0.
- **The beat is not the equaliser's.** It is on the master before the smear, and the equaliser leaves it where it is (bars 1.52 with the beat against 1.01 without). `fm_beat_cancel` upstream removes it as it does without the equaliser.

**Cost.** Two multiply-adds a sample for the inverse, and one burst measurement and a multiply per frame for the gain: under 1 ms a frame against Phase 4's ~96 ms. The source measurement is a tool run (below), not part of the decode.

**4. Parameter surface (agreed 2026-11-03).** The plan put the equaliser inside Domesday2D, as a `channel_equalisation` option of off / nominal / measured. The results moved it out.

- **Why it is a stage of its own.** The smear is in the composite whatever decodes it. A decoder option would also have to follow the Video Sink's `multiple` schedule, which would equalise only some ranges. Colour level and edge sharpness would then change at every switch between decoders. A calibration tool would also sit on the sink for every decoder, including NTSC, that cannot use it.
- **Channel Equalise** (proposed id `channel_equalise`): a PAL composite → composite transform stage, like `fm_beat_cancel` and `picture_band_limit`.
  - **Where it goes:** after the Stacker and Dropout Correction, beside Picture Band Limit, before the sinks.
  - **Parameters:** `smear_fraction` and `smear_tau_samples`.
  - **The stage does two things:**
    1. the inverse smear;
    2. the burst-referenced chroma gain, as a (1 − cos ωT)² shelf on the subcarrier band, the shape the Stacker's `chroma_gain_match` uses. A stage cannot reach the decoder's U and V.
  - **The gain is not a separate option.** It is what keeps the inverse from leaving the chroma 11 % strong. A source without the smear leaves the stage out, gain and all.
- **The Channel Calibration tool** sets both parameters, as FM Beat Calibration sets `fm_beat_cancel`'s carrier.
  - It fits line 19 over frames at the inner, middle and outer radius and applies the guards.
  - It reports either the value or "no smear: leave the stage out". With no measurable VITS it says so.
- **No `nominal` mode.** It is wrong on every disc without the CAV Domesday smear, and the tool measures the Domesday value to within 0.01.
- **The burst gain as a general chroma level control stays out of scope.** On Domesday it would even out the decode's 0.95–1.20 level swings across a side; on Volcanoes and EcoDisc it would cut saturation by up to a third.
- **Phase 4's per-source Hanover matrix stays with Domesday2D.** It is a correction of the demodulated chroma, and the other decoders average the error away. Phase 6 decides how the decoder measures it: pooled over the frames it decodes, or set by a tool on the sink.

**The stage in front of every decoder** (`phase5_decoders.py`). The gain on U and V, as above, against the gain on the composite as the stage applies it. Channel bench, clean / noisy:

| Decoder | Equaliser | Butterfly ΔE | Galtströms ΔE | Town ΔE | Rural ΔE | Text cross-colour % | Edge card ΔE | Town Y % | Bars chroma gain error % |
|---|---|---|---|---|---|---|---|---|---|
| `pal2d` (prototype) | none | 1.49 / 2.84 | 2.85 / 3.73 | 8.08 / 8.56 | 5.69 / 6.20 | 5.94 / 5.99 | 16.36 / 16.59 | 7.38 / 7.47 | 8.54 / 8.51 |
| `pal2d` | gain on U/V | 1.08 / 2.80 | 2.61 / 3.65 | 7.80 / 8.40 | 5.49 / 6.11 | 6.43 / 6.48 | 15.59 / 16.00 | 6.82 / 6.95 | 0.86 / 1.08 |
| `pal2d` | **stage** | 1.08 / 2.74 | 2.67 / 3.64 | 7.93 / 8.50 | 5.57 / 6.16 | 6.67 / 6.70 | 15.55 / 15.94 | 7.07 / 7.17 | 0.86 / 1.08 |
| `transform2d` (prototype) | none | 1.45 / 2.73 | 2.59 / 3.52 | 7.10 / 7.52 | 5.71 / 6.17 | 3.17 / 3.29 | 20.87 / 21.02 | 6.46 / 6.67 | 8.55 / 8.52 |
| `transform2d` | gain on U/V | 1.02 / 2.67 | 2.37 / 3.43 | 6.93 / 7.41 | 5.61 / 6.12 | 3.44 / 3.56 | 21.05 / 21.25 | 6.22 / 6.47 | 0.87 / 1.09 |
| `transform2d` | **stage** | 1.03 / 2.63 | 2.37 / 3.38 | 6.96 / 7.43 | 5.67 / 6.14 | 3.33 / 3.43 | 20.88 / 21.17 | 6.26 / 6.47 | 0.86 / 1.08 |
| Phase 4 | none | 1.29 / 2.11 | 2.18 / 2.81 | 5.30 / 5.68 | 4.46 / 4.79 | 1.49 / 1.52 | 7.62 / 8.02 | 4.63 / 5.20 | 8.35 / 8.32 |
| Phase 4 | gain on U/V | 0.82 / 1.99 | 1.85 / 2.64 | 4.99 / 5.42 | 4.32 / 4.68 | 1.60 / 1.64 | 6.53 / 7.18 | 4.39 / 4.86 | 0.66 / 0.87 |
| Phase 4 | **stage** | 0.84 / 1.93 | 1.88 / 2.59 | 5.09 / 5.50 | 4.43 / 4.75 | 1.49 / 1.55 | 6.57 / 7.21 | 4.57 / 5.03 | 0.64 / 0.86 |

- **Every decoder gains on the photographs and maps,** and its chroma comes back to within about 1 % of the truth.
- **The composite shelf does the gain's job.** It costs a little of the U/V gain's map luma: Phase 4 town 4.57 against 4.39 %, because the shelf also scales luma near the subcarrier. It costs less cross-colour (Phase 4 text 1.49 against 1.60 %).
- **On the bench without the channel,** the stage changes nothing that matters with any decoder: largest change 0.01 ΔE, bars chroma error within 0.22 %. The calibration would leave it out there anyway.
- **Edges and cross-colour:** `transform2d`'s edge card barely moves (20.87 → 20.88). Cross-colour rises with the restored chroma level, as for Phase 4.

**Go/no-go.**

- **H5, smear: go**, as a **Channel Equalise** stage set by a per-source VITS calibration tool, with a burst-referenced chroma gain inside it. It is added only to sources the tool finds the smear on. Not part of Domesday2D.
- **H5, group delay:** no-go (Phase 2).
- **H5, replay response and sideband:** no-go. Inverting the measured response costs cross-colour and the photographs.
- **Per-frame VITS parameters:** no-go.
- **Nominal values:** no-go.

## Phase 6 — Implementation

1. **The `domesday2d` decoder** in [`orc/plugins/stages/sinks/common/decoders/`](../orc/plugins/stages/sinks/common/decoders/), implementing the Phase 3–4 outcome, with a source for the per-source Hanover matrix. It must be SIMD-friendly, and `getLookBehind()`/`getLookAhead()` return 0.
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
   - [`chroma-decoder-guide.md`](../docs/gui-user-guide/stages/chroma-decoder-guide.md), covering when to choose Domesday2D, and its limits on motion;
   - the Domesday workflow guide ([`decoding-domesday.md`](../docs/gui-user-guide/decoding-domesday.md)), whose decoder section names Domesday2D for single-frame stills.
   - **Acceptance:** all four docs updated in the same change.
5. **Performance.**
   - **Acceptance:** throughput is measured against pal2d and transform2d. Anything more than 5 % slower than transform2d is raised for design review (AGENTS.md §6).
6. **The Channel Equalise stage** (Phase 5), a PAL composite transform stage in [`orc/plugins/stages/`](../orc/plugins/stages/), with its Channel Calibration tool.
   - **Acceptance:** matches `equalise.py` (and `phase5_decoders.py`'s composite shelf) within 1 LSB on the channel bench. A frame passes bit-identical when the fraction is 0.
   - **Tests:** a videosynth case with the Phase 2 channel. The tool must read the smear within 0.02 and must read "no smear" on the same case without the channel.
   - **Documentation:** its `instructions.md` and its section in [`transform-stages.md`](../docs/gui-user-guide/stages/transform-stages.md). Add it to the Domesday workflow guide ([`decoding-domesday.md`](../docs/gui-user-guide/decoding-domesday.md)).

### Phase 6 outcome (2026-11-03)

| Item | State |
|---|---|
| 1. The `domesday2d` decoder | **Done. Acceptance met on the noisy bench:** Y, U and V within 1 LSB of the prototype on every still. On the noise-free bench the transform's U/V test ties exactly in about 17,000 bins a frame, and there the two implementations differ (see below). The per-source Hanover matrix comes from a new **Domesday2D Calibration** tool |
| 2. Sink registration | **Done.** Factory, options list (PAL projects), `schedulable_decoder_types()`, `downgrade_for_yc()`, the `hanover_*` parameters, `transform_threshold`'s dependency. Every test outside the videosynth suite passes (3,950), and the videosynth suite (309) |
| 3. Tests | **Done.** `DecoderStillTruth.PAL_domesday2d_Clean` and `_Domesday` with baselines; `Domesday2DTest` (the lattice, the phase tables, the matrix and its fit); `Domesday2DTruth` (the calibration and the correction); `MVPArchitectureCheck` passes |
| 4. Documentation | **Done.** `video_sink/instructions.md`, `sink-core-stages.md`, `chroma-decoder-guide.md` (a Domesday2D section: when to choose it, its limits on motion), `decoding-domesday.md` (the schedule's default for single-frame stills is now `domesday2d`) |
| 5. Performance | **Measured, profiled and halved; still raised for design review:** 2.2× `transform2d` (38 against 17.5 ms of CPU a frame on one core; 82 ms before profiling) and about 62 MB of working planes per decoder thread (100 MB before) |
| 6. Channel Equalise | **Done. Acceptance met:** within 0.5 code of `equalise.py` (rounding only) on the channel bench, bit-identical at fraction 0; Channel Calibration reads the prototype's smear within 2·10⁻⁵ and "no smear" without the channel |

**1. The decoder** (`decoders/domesday2ddecoder.{h,cpp}`, and `orc/support/domesday2d.{h,cpp}` for what the decoder and its calibration share).

It is the Phase 4 decoder as `phase5_decoders.phase4()` runs it: frame-raster Transform PAL (32 × 32 tiles, threshold `transform_threshold`), per-row demodulation and PalColour's horizontal raised cosine, the per-source correction when the matrix is non-zero, partner-checked masked averaging, the luma-steered Charbonnier fit, and the shrunk, gated residual back to luma. `chroma_gain` scales U and V at the output; `chroma_phase` turns the demodulation reference, and the re-encode with it, so the residual stays consistent.

Against the prototype (`phase6_compare.py decoder`), in 16-bit output codes:

| Bench | Y | U, V | ΔE |
|---|---|---|---|
| Noisy | ≤ 1 on every still | ≤ 1 on every still | identical to 3 decimals (text card 4.240 / 4.235, from pixels the output clips) |
| Clean | ≤ 1 on 3 stills; up to 2,456 on the edge card | ≤ 1 on 4 stills; up to 2,134 on the edge card | within 0.006 on every still |

- **The output clips Y** at codes 256 and 65216 (the half line at the foot of the picture goes to −0.49). Those samples are left out of the comparison.
- **Ties on the clean bench.** A noise-free picture with symmetric structure makes the U and V reflections agree exactly in many bins (17,467 on the edge card's frame). Which one wins then depends on the FFT's rounding: numpy's in the prototype, FFTW's in the decoder. The decoder breaks a tie within 10⁻⁹ towards U and treats bins below 10⁻¹⁸ as empty, so its result does not depend on the FFT library or the machine. The noisy bench has no ties.

**The Hanover matrix source.** The decoder takes M as four video-sink parameters (`hanover_uu`, `_uv`, `_vu`, `_vv`), default 0, which leaves the rows uncorrected.
- The **Domesday2D Calibration** tool (core analysis, `video_sink` nodes) pools the flat-colour pairs of 200 frames spread over the sink's input, as `hanover.fit_matrix()` does, and applies M.
- It demodulates the composite directly rather than the transform's chroma: on flat colour the luma has nothing at the subcarrier. On the impaired Phase 4 bench the two routes agree to 0.002 (clean: −0.0888 / 0.1290 after the transform against −0.0890 / 0.1294 from the composite; noisy: −0.0873 / 0.1278 against −0.0858 / 0.1273).
- Because of that the tool needs no FFT library in core.
- On a videosynth pressing played with a leak of V→U 0.06 and U→V −0.02 (`chroma_error.crosstalk`, alternating with the switch), it reads both within 0.0017 and reads under 0.005 without the leak.
- Decoding with the measured matrix shrinks the leak's mark against the leak-free decode on every plane: Y 0.218 → 0.130 %, U 0.095 → 0.085 %, V 0.060 → 0.058 % of full scale. With M's sign reversed it roughly doubles (Y 0.41 %).
- Much of what remains is the transform's keep-or-drop decisions, which the leak moves and no correction after the transform undoes.

**The still bench** (`DecoderStillTruth`, the C++ decoder), Domesday noise, against `pal2d` / `transform2d`. These are the Phase 4 figures, reproduced:

| Still | Metric | `pal2d` | `transform2d` | `domesday2d` |
|---|---|---|---|---|
| Butterfly | ΔE | 2.60 | 2.44 | **1.79** |
| Galtströms | ΔE | 3.50 | 3.31 | **2.47** |
| Town map | ΔE | 8.26 | 7.62 | **5.04** |
| Rural map | ΔE | 5.87 | 6.00 | **4.23** |
| Text card | Cross-colour % | 6.69 | 4.75 | **2.50** |
| Luma zone plate | Cross-colour % | 12.29 | 3.57 | **1.70** |
| Test Card F | Cross-colour % | 3.31 | 1.29 | **0.42** |
| Green/magenta edges | ΔE | 15.90 | 20.47 | **6.45** |
| Town map | Y % | 6.39 | 6.70 | **3.41** |

**5. Performance.** Release build, one CPU (`taskset`), 48 frames of Community South DS1. The pipeline's own cost (the `mono` decode) is taken off. CPU time (user); the wall time adds each worker's first touch of its working planes, a one-off that a 48-frame cut makes large (`domesday2d` 47.5 ms wall).

| Decoder | ms a frame |
|---|---|
| `pal2d` | 10.4 |
| `transform2d` | 17.5 |
| `domesday2d`, as first built | 82 |
| `domesday2d`, after profiling | **38** |

- **The cost is about 2.2× `transform2d`** (Phase 4 estimated 2.6×; the first build was 4.6×).
- **Memory.** 11 planes of 5.7 MB per instance, about 62 MB per worker thread (17 planes, 100 MB, as first built).
- **Both still exceed the AGENTS.md §6 thresholds (5 % time, 10 % memory) and stay raised for design review.** What is left would change the arithmetic (below).

**Profiling** (2026-11-03). `perf record -e cpu-clock:u` (the machine allows user-space software sampling only: no hardware counters, no call graphs), source-line attribution from a `-g` build, and a harness that decodes 24 frames of the Community South cut through `decodeRaster()` on one CPU. Each change was checked against the output before it, with FFTW's plans pinned by a wisdom file: `FFTW_MEASURE` picks plans by timing, so two runs of the same build differ by up to 10⁻¹⁵ (10⁻¹⁰ codes).

| Step | ms a frame (harness) | Output |
|---|---|---|
| As first built | 63 | — |
| No zero-fills of planes that are overwritten; no plane copies without a Hanover matrix; reflection only at the row ends; skip all-zero column blocks in the fit | 49.8 | bit-identical |
| The 129-tap detail band-pass as an FFT convolution (FFTW's codelets use AVX at run time even in a baseline build) | 43.5 | ≤ 1.4·10⁻⁹ codes |
| `filterTile()` without branches: below SSE4.1 every select on doubles is a branch, and its decisions follow the picture | 39.5 | bit-identical |
| `-fno-math-errno` on the decoder's source: otherwise each `sqrt` is a libm call and its loop does not vectorise | 36.4 | bit-identical |
| Demodulation fused with the horizontal filter (`demodulate_filtered()`) | 35.3 | bit-identical |
| Running window sums interleaved two or three rows at a time (each sum waits on the one before) | 34.6 | bit-identical |
| The vertical fit also built for AVX2 (`target_clones`, chosen at load time; GCC or Clang on x86-64 glibc) | **31.5** | bit-identical |

Every step was checked bit-identical, or within the stated difference, against the one before it. Through `phase6_compare.py` every noisy-bench still is still within 1 LSB of the prototype. Against Phase 6's first decode, its output codes are identical on the noisy bench, and on the clean bench 55 of 23 million codes differ by 1.

- **Where the time goes now** (31.5 ms): the vertical fit 8, the transform 8.3, the luma detail 4.7, the demodulation and filter 3.4, the masked averaging 3.9, the residual and reassignment 4.1.
- **The vertical fit is bound by the divider.** Each of its six iterations takes, per sample, a square root and a divide for the weight and a divide for the Thomas pivot. On Zen 3, SSE2 does those at about 9.5 cycles a sample. Without AVX2 (Windows, and x86-64 CPUs without it) the fit is 12 ms and the decode about 35.5 ms.
- **Tried, no faster, not kept:** the detail convolution in FFTW's halfcomplex form (39.5), or as two rows per complex transform (37.3); FFT lengths other than 1280; pruning the tile FFT's column passes to the nine kept columns; the fit's weights made inside the Thomas sweep; narrower fit blocks (8 or 16 columns are slower); AVX2 copies of the other stages (0.4 ms in all, for four more annotated functions).
- **Construction.** The detail plans use `FFTW_ESTIMATE`: measuring them took 0.57 s at the first construction and found nothing faster. A decoder now constructs in 46 ms.
- **What remains would change the arithmetic:**
  - single precision in the fit (the divider does twice as many floats);
  - three fit iterations instead of six (text cross-colour 2.44 → 2.94 % in Phase 4);
  - the detail band-pass decimated.

**6. Channel Equalise** (`orc/plugins/stages/channel_equalise/`, `orc/support/channel_equalise.{h,cpp}`, the tool in `orc/core/analysis/channel_equalise/`).

- **The stage.** It is `equalise.unsmear()` followed by `phase5_decoders.stage()`'s composite shelf. Each frame's burst is measured after the inverse, as the median over field lines 23–310 of both fields, and the gain brings it to 0.1702.
  - `smear_fraction` 0 (the default) passes every frame through bit-identical; the node's status is yellow until it is set.
  - On `chan_noisy` at 0.1842 / 5.787 every sample is within 0.5 code of the prototype (`phase6_compare.py equalise`), which is the output's rounding.
- **The tool.** Channel Calibration fits field 1 line 19, averaged over 48 frames at the inner, middle and outer radius (or the whole input when it is shorter than three windows), with `fit_smear()`'s model and `accept()`'s guards.
  - The model's linear part (bar height, pulse area, base) is solved exactly for each (fraction, τ), within the prototype's bounds, and (fraction, τ) is found by a grid and a pattern search.
  - **The pulse area's bound (2 IRE µs) is active on every capture measured.** The 2T pulse's own area is nearer 18. The unbounded fit reads `chan_noisy` as 0.116 / 7.7 rather than 0.184 / 5.8. The bound is kept, because the Phase 2 and Phase 5 figures were measured with it.
  - It reads `chan_noisy` 0.1842 / 5.788 (prototype 0.1842 / 5.787), `chan_clean` 0.1904 / 5.648 (0.1904 / 5.648), and "no smear" on `vits_clean` and `vits_noisy`.
  - On 48-frame cuts at the inner radius: Community South 0.2431 / 6.130 (0.2431 / 6.131), National B and Volcanoes "no smear", as in Phase 5.
- **Tests.**
  - `ChannelEqualiseTruth`: the case `projects/channel/pal_channel.yaml`, with the edge smear, the inner-radius response and the four LaserDisc VITS lines; its copy without the channel blocks comes from `drop_channel_block.py`, which now drops a top-level `channel:` block too.
    - The tool reads 0.2011 / 5.405 there, within 2·10⁻⁵ of the prototype, and "no smear" on the copy without the channel.
    - The stage at that value brings every section nearer the copy without the channel. Low-luma error on the photograph falls from 1.00 to 0.37 IRE, and chroma-band error on the bars from 2.18 to 1.07.
    - The 2–3.5 MHz band's error rises, because the replay response's lift there is not inverted, as Phase 5 found.
    - The burst ends within 0.1 % of nominal, and fraction 0 is bit-identical.
  - Unit tests of the inverse, the shelf, the parameters and the fit's guards.
- **Documentation.** The stage's `instructions.md`, its section in `transform-stages.md`, the CLI tools table, and `decoding-domesday.md`, whose in-development note is replaced.

**Left for Phase 7:**

- **The blank-line noise.** The reassignment measures the chroma-band noise on raster rows 12–31 (field lines 7–16). On a real CAV disc field line 16 carries the LaserDisc VBI code, whose edges reach the 3–5.9 MHz band, so the noise read there may be high and the reassignment too timid. The bench has no VBI on those lines. Phase 7 should compare the noise read with and without line 16 on captures. (Phase 7: line 16 raises it 5–15 %, and leaving it out is slightly worse; kept.)
- The design review of item 5 (2.2× `transform2d` after profiling).

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

### Phase 7 outcome (2026-11-03)

| Item | State |
|---|---|
| 1. Held-run reference | **Done. Met on 8 of 9 stills, on both pressings measured.** Measuring it found that the line-alternating error belongs to each still, not to the disc; the decoder now fits its rotation per frame (below). The miss is the town map, where `transform2d` is 0.14–0.17 ΔE better |
| 2. Cross-disc agreement | **Done. Met on the average and on 27 of 30 stills:** across pressing groups Domesday2D's decodes disagree least (ΔE 3.15 against 3.56 `pal2d` and 3.83 `transform2d`; U/V a third lower). The misses are a foliage photograph, a satellite picture and one OS map, all by luma, all to `pal2d` |
| 3. Visual panel | **Crops made** for the user's sign-off (below); they are of BBC pictures, so they stay beside the captures rather than in this repository |

**Tools and projects.** [`phase7_real.py`](../tools/domesday2d-bench/phase7_real.py) does all three steps on Community South. Its `setup` calibrates each disc over the whole side and writes two decode-orc projects that open in the GUI:

- **`held.orcprj`**
  - Runs DS1 and DS6, each through:
    - the disc map (from the side's stack project);
    - the held pictures;
    - Channel Equalise at the disc's measured smear.
  - Each disc then feeds three video sinks, `pal2d`, `transform2d` and `domesday2d` (with the disc's `hanover_*`), plus a CVBS sink of the equalised composite for the reference.
- **`cross.orcprj`**
  - Runs the same chain for seven discs over thirty single-frame stills.
  - The discs come from four pressing groups: DS1 and DS2; DS6 and DS9 (a second pressing of the same mother); DS3; and DS5 and DS11.

Their paths are relative, so they open wherever the side's folder is mounted. In the GUI each sink's preview shows its decoder, so the three can be compared frame by frame. For this run they are in `community-south/domesday2d-phase7/` beside the captures, with the decodes, the scores (`held.json`, `cross.json`) and the crops (`panels/`).

**Calibration**, over the whole side (Channel Calibration, then Domesday2D Calibration behind it):

| Disc | Smear fraction, τ (samples) | `hanover_uu`, `_uv`, `_vu`, `_vv` |
|---|---|---|
| DS1 | 0.236, 6.20 | −0.009, 0.051, −0.106, −0.024 |
| DS2 | 0.260, 5.97 | −0.009, 0.054, −0.111, −0.026 |
| DS3 | 0.244, 5.93 | −0.008, 0.078, −0.112, −0.012 |
| DS5 | 0.188, 7.09 | −0.007, 0.052, −0.124, −0.035 |
| DS6 | 0.232, 6.34 | −0.007, 0.044, −0.098, −0.026 |
| DS9 | 0.235, 6.20 | −0.008, 0.045, −0.105, −0.027 |
| DS11 | 0.209, 6.74 | −0.007, 0.055, −0.117, −0.028 |

**1. The held-run reference.** Nine held runs, found on the stacked side: Test Card F (pictures 3001–3020), the UK and England maps, a menu, the UK colour map, an OS map, the town map, a map key and a photograph (24 frames each).

- **The four carrier phases.** Each frame is classed by its burst-fitted carrier phase. Every run steps a quarter turn a frame, giving 5 or 6 frames per phase.
- **Leave one out.** The reference for each scored frame leaves that frame out.
- **Moving content.** A mouse pointer moves during the UK colour map; samples that change within the run are left out of the score (under 1 % of the picture).
- **Reading the composite.** The CVBS sink writes the internal samples bitwise, so an equalised sync tip below 0 is stored negative: the file is read as int16.

**The line-alternating error is the picture's.** Fitted on each run's exact chroma (no luma to confuse it), the error is a rotation of very different size from still to still. The same values appear on DS1 and DS6:

| Still | Rotation, DS1 / DS6 |
|---|---|
| Test Card F | −0.015 / −0.040 |
| UK map | 0.023 / 0.026 |
| Photograph | 0.057 / 0.044 |
| Map key, OS map | 0.11 |
| Town map | 0.17 |
| Menu | 0.19 |
| England map, UK colour map | 0.21–0.23 (about 13°) |

So it was mastered into each still. Phase 2's "constant −5° to +3° per side" was a side's average, and the side's pooled matrix (DS1: rotation 0.08) over-corrects Test Card F and corrects the coloured maps by only a third. `pal2d` and `transform2d` average it out over two lines. Domesday2D, which keeps vertical colour detail, showed it: with the pooled matrix it was the worst decoder on the England and UK colour maps (ΔE 3.45 and 3.24 against 3.08 and 2.63 for `transform2d`).

**The fix: each frame's own rotation.** The decoder now fits M on each frame's own flat colour and keeps only the rotation, (M_UV − M_VU)/2. The leak and gains come from the `hanover_*` values, as before. A frame with too little flat colour (under 1000 pairs) takes the `hanover_*` values as they are. A new sink parameter, `hanover_per_frame`, default on, turns it off.

- **Why only the rotation.** A single frame's full fit is pulled by colour stripes one field line high, as Phase 4 found. Checking the 312-line partner (`flat_pairs(partner=0.03)`: a Hanover error repeats there, a stripe does not) cleans the fit on the unimpaired bench. With an error present, though, the edge card still reads [[0.19, 0.02], [−0.20, 0.07]] against [[0, 0.13], [−0.09, 0]].
- **Accuracy of the rotation.** The rotation alone reads within 0.01 of the true 0.11 on every still of the impaired bench, the edge card included, and about 0 without the impairment.
- **The bench.** On Phase 4's impaired bench (`phase4_hanover.py`, `phase7_rotation_masked`) every still's ΔE and Hanover residue are within 0.005 of the pooled matrix.
- **A robust refit was tried and dropped.** Trimming the pairs by their residual lost the very pairs that carry the error: one frame read 0.05 where the run reads 0.21.
- **On the discs** (prototype, two frames of each held run, against the run-matched reference), mean ΔE:

| Matrix | Mean ΔE |
|---|---|
| Pooled | 3.28 |
| Full per-frame fit | 3.05 |
| Per-frame rotation + pooled terms | 3.09 |

- **Against the prototype.** Every noisy-bench still is within 1 LSB of the prototype with the same rule (`phase6_compare.py`, now using `hanover.frame_matrix()`). The clean bench's U/V ties now also move which pairs pass the flatness test, so more of its stills differ there; its ΔE agrees within 0.03.
- **The videosynth leak.** `Domesday2DTruth.PAL_Hanover` now decodes four ways. The per-frame rotation takes most of the leak's luma mark out on its own (0.217 → 0.143 %), but leaves its symmetric part (V 0.059 → 0.063 %). With the calibrated matrix it does as well as the matrix alone (Y 0.130, U 0.084, V 0.058 %).
- **Cost.** 2.2 ms a frame for the fit over a source whose `hanover_*` are set: the flat-pair scan works on squared magnitudes, since a complex `abs` per test made it 18 ms. A source with no matrix also pays the correction on every frame, so the decode is about 40 ms a frame, 2.4× `transform2d`.

**Held-run results** (`domesday2d` with the per-frame rotation; the mean over every frame of each run, ΔE / luma r.m.s. %):

| Still | `pal2d` DS1 | `transform2d` DS1 | `domesday2d` DS1 | `pal2d` DS6 | `transform2d` DS6 | `domesday2d` DS6 |
|---|---|---|---|---|---|---|
| Test Card F | 3.84 / 3.23 | 3.13 / 2.27 | **2.32 / 2.12** | 3.66 / 3.10 | 2.97 / 2.12 | **2.18 / 1.98** |
| UK map | 2.99 / 2.43 | 3.09 / 2.83 | **2.67** / 2.77 | 2.84 / 2.28 | 2.93 / 2.62 | **2.50** / 2.57 |
| England map | 3.07 / 2.52 | 3.05 / 2.55 | **2.79** / 2.79 | 2.88 / 2.36 | 2.91 / 2.45 | **2.58** / 2.59 |
| Menu | 2.69 / 2.21 | 2.68 / 2.27 | **1.88** / 2.40 | 2.63 / 2.21 | 2.63 / 2.29 | **1.83** / 2.39 |
| UK colour map | 2.66 / 2.24 | 2.59 / 2.21 | **2.45** / 2.47 | 2.45 / 2.09 | 2.42 / 2.12 | **2.25** / 2.29 |
| OS map | 5.65 / 3.44 | 5.25 / 3.21 | **5.16** / 3.39 | 5.55 / 3.32 | 5.19 / 3.10 | **5.11** / 3.29 |
| Town map | 5.01 / 3.30 | **4.50 / 2.91** | 4.64 / 3.03 | 4.93 / 3.20 | **4.45 / 2.82** | 4.62 / 2.92 |
| Map key | 3.31 / 2.18 | 3.08 / 1.95 | **2.78** / 2.01 | 3.07 / 1.99 | 2.84 / 1.77 | **2.61 / 1.74** |
| Photograph | 3.60 / 2.34 | 3.48 / 2.31 | **3.25** / 2.35 | 3.09 / 2.06 | 2.95 / 2.03 | **2.81 / 1.97** |

- **Where Domesday2D wins.** It has the lowest ΔE on every still but the town map. It is lowest by most on Test Card F (2.3 against 3.1) and the menu (1.9 against 2.7), where vertical colour edges dominate.
- **The town map.** It loses in the dense black building outlines on pale fills, where `transform2d`'s vertical averaging hides more of the leftover chroma noise. It wins on the one-line coloured grid lines and the menu bar. The rotated bench (below) also finds this picture its most phase-sensitive.
- **Luma.** On the flat-colour maps its luma error is 0.2–0.3 % above the others'. The single frame's rotation reads about 0.8 of the run's (0.17–0.19 against 0.21–0.23): noise in the fit's regressor pulls it towards 0, and what remains goes to luma through the reassignment.
- **Phase 6's open question, the blank-line noise.** Field line 16 (the VBI code) reads about twice the median blank row on every still, so it raises the reassignment's noise estimate by 5–15 %. Leaving it out made the held stills slightly worse (ΔE 3.114 → 3.122, Y 2.60 → 2.62 %), so rows 12–31 stay.

**2. Cross-disc agreement.** Thirty single-frame stills from across the side:

| Picture | Content |
|---|---|
| 8137, 8501, 9337, 10500, 11137, 12937 | Photographs: a tractor and people, a narrowboat, a market, buildings, a building with its sign, cars |
| 14137, 14511, 14812, 15337, 15937 | Photographs: people, an aerial view, runners, a harbour, a town from above |
| 16500, 17748 | Aerial and satellite pictures: foliage, a coastline |
| 18937, 22535, 23738, 25597, 27337 | OS maps |
| 31502, 32137, 32737 | Town maps and a coastal map |
| 33337, 33937, 34500, 35137, 35500, 36937 | Photographs: shops, a harbour with cars, a fire engine, terraced houses, a crowd, houses and cars |
| 38738, 39500, 39937 | Photographs: hills, a riverside, a kitchen |

Every pair of the seven discs is compared, in the picture less the edge exclusion. First, each pair's luma gain and offset, and its chroma's complex gain, are fitted per decoder: pressings differ in level (DS3's luma sits 7 % high), and every decoder would carry that alike. Mean over the pairs:

| Decoder | Within a group: ΔE / Y / U / V % | Across groups: ΔE / Y / U / V % |
|---|---|---|
| `pal2d` | 1.38 / **1.05** / 0.45 / 0.45 | 3.56 / **4.04** / 0.82 / 0.84 |
| `transform2d` | 1.81 / 1.49 / 0.52 / 0.53 | 3.83 / 4.68 / 0.79 / 0.82 |
| `domesday2d` | **1.26** / 1.34 / **0.34 / 0.35** | **3.15** / 4.67 / **0.52 / 0.56** |

- **Chroma.** Domesday2D's chroma agrees best by about a third, on every still.
- **ΔE.** It is lowest on 27 of the 30 stills. `pal2d` is lower on three, all by its softer luma:
  - the foliage (16500): 5.66 against 6.07;
  - the satellite coastline (17748): 2.52 against 2.69;
  - an OS map (18937): 5.35 against 5.69.
- **Luma across groups.** It stays at 4–5 % for every decoder after level matching, more than the decoders differ. It is the captures' sub-sample registration and the masters' own differences, and `pal2d`'s softer luma suffers from it least.

**The bench at every PAL phase.** The still bench always put each still on the same frame, so each had only ever been decoded at one place in the 4-frame (8-field) sequence, and the decoder was tuned there. [`phase7_rotation.py`](../tools/domesday2d-bench/phase7_rotation.py) renders the bench with the twelve stills moved on by 0–3 frames. Each still keeps its own noise seeds, and rotation 0 is byte-identical to the suite's render. Every decoder is then scored at every phase.

- **Rankings.** They hold at every phase: Domesday2D is lowest on every still, clean and noisy.
- **Stable stills.** Most stills move by under 0.05.
- **Phase-sensitive stills.** Three move more:
  - **The town map** moves for every decoder (cross-colour, clean: `pal2d` 4.90–5.38, `transform2d` 4.39–4.79, Domesday2D 3.21–3.90). It is the content, not the decoder.
  - **The town map and tartan** have Domesday2D's best score at the phase it was tuned on (3.21 against a mean of 3.55; 0.36 against 0.42). The single-phase figures quoted from Phases 4–6 flatter it there by about 10 %.
  - **The green/magenta edges** have their worst score at that phase (0.28 against 0.22).
- **Follow-up.** A bench figure should be the mean over the four rotations. `DecoderStillTruth` still runs rotation 0 only; adding the rotations to the suite is a follow-up.

**3. The visual panel.** One image per still: whole frames side by side at the decoders' own resolution, one pixel per decoded 4fsc sample and frame line (922 × 576 each, neither cropped nor scaled). Each is labelled, and they are in three sets under `panels/`:

- **`held/`** (`phase7_real.py panel`): the nine held stills of DS1. The held reference comes first, then the middle frame of the run from each decoder.
- **`single/`**: the thirty single-frame stills of DS1, the case Domesday2D is for. No reference exists for these.
- **`synthetic/noisy/` and `synthetic/clean/`** (`phase7_real.py panel-bench`): the twelve stills of the videosynth bench, with and without the Domesday noise model. Each starts with its picture truth.

The panels and the GUI projects are for the user's sign-off; that is the only acceptance still open.

**Left open:**

- **The user's sign-off of the panels.**
- **The town map.** Domesday2D's one loss on the held stills (Phase 10).
- **The rotation's bias.** A single frame reads about 0.8 of the run's rotation (Phase 8).
- **The design review of the cost:** about 40 ms a frame, 2.4× `transform2d` (Phase 10).
- **The suite at every phase.** `DecoderStillTruth` on the rotated bench (Phase 8).

## Review after Phase 7

A full review of the finished decoder (2026-11-03) found that it had not been tested the way it is used:

- **Not on the chain it is used in.** Every real-disc figure is from one unstacked disc with Channel Equalise only. The Domesday guide puts the decoder after Stacker, Dropout Correction, Picture Band Limit and Channel Equalise.
- **Every constant from one noise level and twelve stills.** The bench that chose the constants also judged acceptance.
- **One disc side.** Community South only.

The review also found faults in the code. Phases 8–10 close these:

- **Phase 8** fixes the code and the tests that guard it.
- **Phase 9** measures what was not measured, and changes nothing.
- **Phase 10** tunes from Phase 9's findings and re-runs the acceptance.

## Phase 8 — Hardening

**Goal:** fix what the review found in the code, and make the tests guard it on every platform.

1. **Channel Calibration's bounds.**
   - The fit searches the time constant up to 100 samples (`kFitMaxTau`), but `judge_smear_fit` checks only its lower bound. A fit between 50 and 100 is accepted, "Apply" writes a value that `ChannelEqualiseStage` refuses (`kMaxTauSamples` 50), and the DAG then fails.
   - Reject, or report as out of range, any fit outside the stage's own limits.
   - **Acceptance:** a unit test with a window that fits above 50 samples shows the tool reports it and writes nothing the stage refuses.
2. **The per-frame rotation, fitted alone.**
   - `frameMatrix()` solves all four terms by `fit_matrix()` and keeps only (M_UV − M_VU)/2.
   - On a frame whose flat colour is mostly one hue (a caption on one background, a single map fill), the 2×2 Gram matrix is nearly singular. With one hue d, only M·d is determined, so the rotation read is partly the symmetric leak and partly noise. The only guard is `det <= 0`, and 1000 pairs come easily from one flat area.
   - Fit θ as the single unknown, with the symmetric part held at the `hanover_*` values. One hue determines one unknown.
   - Then try an errors-in-variables form (noise in the pair means as well as their halves), or pairs from the masked average, against Phase 7's bias: a single frame reads about 0.8 of the run's rotation.
   - **Acceptance:**
     - single-hue synthetic frames read the bench rotation within 0.01;
     - the held runs' single-frame rotation is within 0.02 of each run's;
     - no bench still or held still gets worse.
3. **A colour killer.**
   - PalColour divides by max(|burst|, its kill threshold), so a source without burst decodes with U and V near zero.
   - Domesday2D takes only the burst's phase. On a monochrome or burstless PAL frame it fits that phase to noise, and the luma detail the transform moved into the chroma plane comes out as colour.
   - When the frame's median burst amplitude is under PalColour's threshold, output no U or V and give the separated chroma back to luma.
   - **Acceptance:**
     - a monochrome videosynth still decodes with U and V within noise of 0;
     - its luma is within 1 LSB of `pal2d`'s monochrome path, or the difference is explained.
4. **The Hanover matrix parameters.**
   - **All zero no longer means uncorrected.** With `hanover_per_frame` on (the default), an all-zero matrix still gets the frame's rotation. Correct `domesday2ddecoder.h`, the parameter help and `instructions.md` to say so.
   - **A value out of range is silently dropped.** In `video_sink_stage.cpp` a `hanover_*` value beyond ±0.5 is ignored while the other three are applied, which leaves a mixed matrix. Refuse the whole set instead.
   - **A singular matrix gives NaN.** `apply_matrix` has no guard for det(I + sM) near 0, which values within the ±0.5 range can reach (uu = vv = −0.5, uv = vu = 0.5). Bound it, and fall back to no correction.
   - **The calibration's phase reference.** It demodulates at phase 0, and the decoder at the user's `chroma_phase`. With `chroma_phase` set, the symmetric terms are applied in a turned basis. Rotate them by `chroma_phase`, or state that the calibration assumes 0.
   - **Acceptance:** unit tests for each case.
5. **Comments and shared caches.**
   - **The `add_flat_pairs` comment** in `domesday2d.h` puts the 312-line partner on the wrong side (the code is right: frame line k + 312 is field 2 line k − 1).
   - **The frame caches.** In `EqualisedVideoFrameRepresentation`, `get_frame_copy` dereferences a cache pointer after the lock is released. `get_frame` passes the unequalised frame on if another thread evicts the entry first. With more than 16 frames in flight that is a use after free, or a frame silently not equalised. `picture_band_limit` uses the same pattern; fix both.
   - **Acceptance:** a stress test that reads more frames on more threads than the cache holds, under ThreadSanitizer, with every frame equalised.
6. **Tests that run everywhere.**
   - **A CI decode test.** Only the lattice unit test (`Domesday2DTest`) runs in CI; the truth suites are local. Add a core unit test that builds a small PAL composite in the test (bars, an edge, a flat field), decodes it with `domesday2d` and checks it against tolerances. That catches drift between platforms: Windows and x86-64 without AVX2 (the fit's SSE2 path), and ARM macOS, where the compiler may contract to FMA.
   - **Reproducible output.** The tile FFTs are planned with `FFTW_MEASURE`, so two runs can differ in an occasional output code. Use `FFTW_ESTIMATE` if the time allows, or show the difference never reaches an output code.
   - **The suite at every phase.** `DecoderStillTruth` runs only rotation 0. Add the four rotations of `phase7_rotation.py`, and record each still's mean over them as its baseline.
   - **Acceptance:**
     - the CI test passes on every CI platform;
     - two decodes of the same input are byte-identical;
     - the four-rotation baselines are recorded.

### Phase 8 outcome (2026-11-03)

| Item | State |
|---|---|
| 1. Channel Calibration's bounds | **Done.** A fit beyond the stage's 50 samples is a new verdict, out of range: reported with its fit, and nothing is written |
| 2. The per-frame rotation | **Done; one acceptance not met, by design.** The rotation is fitted alone. Single-hue frames read within 0.003, and the held stills improve on average; the single frame still reads under the run's rotation, because the transform loses part of the error before any fit sees it (below) |
| 3. Colour killer | **Done.** A frame without a burst decodes exactly as `mono` does. Against `pal2d` the difference is explained: `pal2d` has no monochrome path |
| 4. Hanover parameters | **Done**, with a real fault found in `chroma_phase` handling |
| 5. Comments and caches | **Done.** The use after free is shown on the old code and gone on the new |
| 6. Tests that run everywhere | **Done.** A CI decode test; byte-identical runs (`FFTW_ESTIMATE`); `DecoderStillTruth` at all four phases, baselines re-recorded |

**1. Channel Calibration.** `judge_smear_fit` now returns `OutOfRange` for a fit that explains the tails but has τ above `kMaxTauSamples`. The source's verdict is out of range when no window is a smear and one is out of range. The report gives each window's fit and leaves the stage's values alone, and **Apply to Stage** writes nothing. The verdict logic and the report are now free functions (`combine_channel_windows`, `describe_channel_calibration`) so the unit tests can drive them: a synthetic segment smeared at τ = 70 reads out of range, and `applyToGraph` refuses it.

**2. The rotation fitted alone** (`fit_rotation` in `orc/support/domesday2d`, prototype `hanover.frame_matrix`, tool [`phase8_rotation.py`](../tools/domesday2d-bench/phase8_rotation.py)). With the symmetric part S held at the `hanover_*` values, half − S·mean = θ J·mean has one unknown, which one hue determines.

- **Single-hue frames** (the noisy bench's timing and noise, Phase 2 impairment, true rotation 0.110):

| Frame | Full fit (Phase 7) | Rotation alone |
|---|---|---|
| Whole-picture fill | 0.059 | 0.112 |
| Caption box on grey | 0.084 | 0.111 |
| Pale map fill ruled black | 0.041 | 0.109 |
| Two hues | 0.111 | 0.111 |

- **The cut.** Phase 7 dropped pairs whose alternating half exceeds 0.3 of their mean. That selects on the quantity being fitted: at a rotation of 0.25 the cut sits just above the signal, and noise pushes the pairs that carry the most rotation out. The fit now takes a first pass at 0.6, then three passes cutting at 0.3 on the residual about the last pass's rotation.
- **Where the 0.8 comes from.** On the held runs (`phase8_rotation.py held`), against the rotation of each run's exact chroma:
  - **The transform** loses a picture-dependent share of the error before any fit sees it. The run's noise-free average, put through the transform, reads 0.243 against 0.266 on the England map, and 0.078 against 0.158 on the town map. Domesday2D corrects the transform's output, so correcting by what is in that output is self-consistent. The rest is already in the luma.
  - **Noise** costs the rest, through the cut above.
  - **Not errors-in-variables.** The single frames' Σ|mean|² is not inflated against the noise-free run's, so an errors-in-variables form would not help, and it was not adopted.
  - **The run's own full fit** is no reliable reference on the maps either. Its M_UU of −0.1 to −0.18 comes from the same single-hue degeneracy.
  - "Within 0.02 of each run's" therefore cannot be met by any fit. The measure that matters is the decode.
- **Decoded** (the prototype, four frames of each of the 18 held runs, against each run's reference), mean over the runs:

| Per-frame fit | Rotation read | ΔE | Luma r.m.s. % |
|---|---|---|---|
| Full fit (Phase 7) | — | 3.022 | 2.514 |
| Alone, cut 0.3 on the half | — | 3.002 | 2.491 |
| Alone, cut 0.6 | — | 2.989 | 2.477 |
| **Alone, cut centred (adopted)** | England map 0.235 against Phase 7's 0.169 | **2.981** | **2.468** |

  - **Where it helps.** The flat-colour maps gain most: England map DS1 ΔE 2.78 → 2.57, luma 2.80 → 2.58 %; UK colour map 2.44 → 2.29. This takes much of Phase 7's "luma 0.2–0.3 % worse on the maps" away.
  - **Where it doesn't.** Two runs move the other way, by noise-sized amounts: town map DS6 +0.006, photograph DS6 +0.002.
- **Benches.** On the impaired bench (`phase4_hanover.py`, every case including no impairment), no still is worse by more than 0.005 in ΔE, Hanover residue or cross-colour. Every noisy-bench still is within 1 LSB of the prototype (`phase6_compare.py`).
- **Fallback.** A rotation beyond 0.5, or a matrix the result cannot undo, falls back to the configured matrix.

**3. Colour killer.**

- **The rule.** `fit_lattice` now gives the median burst amplitude of the picture rows. Under PalColour's kill threshold (0.054 of white − blanking, a quarter of nominal), the frame skips the transform: U = V = 0, and the composite is the luma.
- **Against the mono decoder:** identical (< 1e-9) on a synthetic monochrome frame.
- **Against `pal2d`.** `pal2d` has no monochrome path: its luma is always the composite less what its Y filter takes as chroma. So it softens luma edges (by up to 53 codes on the test frame), and on flat picture it reads 0.62 % of the sample code low, which is DC leakage in its Y filter. That leakage is worth a look in `pal2d` itself (not done here).
- **A weak burst** (half amplitude) still decodes colour.

**4. The Hanover parameters.**

- **All zero:**
  - the header, `instructions.md` and the user guide now say that with `hanover_per_frame` on, all zero still corrects each frame's rotation;
  - a value that arrives as an int (0 from YAML) is now accepted.
- **A set out of range, or one that can't be undone:**
  - the Video Sink refuses the whole set when any value is beyond ±0.5;
  - it also refuses a matrix with det(I ± M) under 0.25 (`invertible`);
  - the decoder checks again and falls back to no correction.
- **`chroma_phase` was a real fault.** Demodulating at phase φ turns a row of switch s by sφ, so `chroma_phase` itself adds an alternating rotation:
  - **Effect.** With `hanover_per_frame` off, any nonzero `chroma_phase` left Hanover bars.
  - **The exact form.** W_φ = (I + s·BA⁻¹)·A·W, with A = cI + nJM and B = cM + nJ. The decoder now corrects with BA⁻¹ (`in_phase_basis`), which leaves A·W: a desaturation by cos φ, as `pal2d`'s line average gives.
  - **Caught by its test.** The first version, A⁻¹B, left a 1e-3 alternation that the unit test caught.
  - **The calibration** still measures at phase 0, and the docs say so.

**5. Comments and caches.**

- **The comment.** `add_flat_pairs`' partner comment now has the rows the right way round.
- **The fix.** Channel Equalise and Picture Band Limit cache `shared_ptr` frames:
  - `get_frame_copy` holds a reference while it copies;
  - a frame just made is returned from its own reference, never looked up again;
  - `get_frame` / `get_line` pin the frame for the calling thread until its next call on the object (`pin_for_thread` in `lru_cache.h`), which is the SDK's pointer contract, now kept per thread.
- **Two tests per stage:**
  - many threads reading more frames than the cache holds;
  - a deterministic one: a pointer is read only after another thread has evicted its frame.
- **Results:**
  - On the committed code the deterministic test **segfaults**.
  - On the new code all pass, under ThreadSanitizer too.
  - The concurrent test alone did not catch the old code: 8 threads never evict a frame in use.
- **A blocker found on the way.** Getting TSan to run at all meant turning off the decoder's `target_clones` under a sanitizer. The ifunc resolver runs during relocation, before the sanitizer runtime is ready, and crashed every TSan binary that loads the video sink.
- **Not fixed here.** Stacker, Dropout Correction, FM Beat Cancel and TBC Source use the same `get_ptr` pattern.

**6. Tests that run everywhere.**

- **The CI decode test** (`Domesday2DDecoderTest`, unit/sinks). It builds a frame in the test: bars, a flat field, a vertical colour edge and a luma edge, with the carrier running on over the 1137-sample lines so field 2 sits where a real capture puts it.
  - Away from transitions it decodes within 1 % (luma) and 0.5 % (chroma) of the truth.
  - The colour edge is sharp to a field line.
  - Eight sampled points match values recorded here within 1e-7. A 16-bit code is 2e-5, so a platform that drifts fails.
  - It also covers the colour killer.
- **Reproducible output.**
  - The tile plans are `FFTW_ESTIMATE`.
  - Two `orc-cli` decodes of the noisy and clean bench are byte-identical.
  - Cost: 0.5 ms a frame (4.9 against 4.4 ms for a frame's tiles), about 1 % of the decode.
- **The suite at every phase.**
  - `orc-tests/videosynth/rotate_stills.py` derives `pal_stills_rot{1,2,3}` and their noise-free copies in the build.
  - `DecoderStillTruth`'s eight decoder tests score all four rotations, check each still's truth moved with it, and record each metric's mean.
  - The baselines are re-recorded. The means sit within ±0.03 ΔE of the old rotation-0 figures, and Domesday2D is lowest on every still, clean and noisy.

**Left open:**

- **The transform's loss of the Hanover error.** Part of the error goes to luma at separation (half of it on the town map). Correcting before separation (separate, fit, re-encode the error, separate again) would reach it, at a second transform's cost. This is a Phase 10 candidate.
- **The same cache pattern in other stages** (above).
- **`pal2d`'s luma DC leakage** (item 3).
- **A flaky test.** `CVBSStreamReaderTest.ConcurrentRequests_AllSucceed` failed once under `ctest -j 12` and passed three times alone. It was not touched here.

## Phase 9 — The production chain and wider material (measurement only)

**Goal:** measure the decoder where it is used and on material it never saw, before any further tuning. Real-disc work uses cut sections or sparse frames, as before.

1. **The full Domesday chain.**
   - Score the Phase 7 held runs and thirty stills on Community South's stack project: Stacker (with `still_frame_stacking`), Dropout Correction, Picture Band Limit at the set's measured cutoff, Channel Equalise, then the three decoders.
   - The held runs are scored as in Phase 7, against a reference built from the stack's own held run.
   - For cross-disc agreement, each pressing group is stacked separately and the group stacks are compared. Comparing a stack with discs that are part of it would measure their shared noise.
   - **Acceptance:** a table like Phase 7's for the stack. A ranking that changes from the per-disc one is explained.
2. **The noise estimate in that chain.** The reassignment reads the chroma-band noise on raster rows 12–31 (field lines 7–16). On the stack those lines no longer look like the picture:
   - the Stacker's still-frame copies stack only the active picture;
   - Picture Band Limit filters only the active picture;
   - field line 16 carries the LaserDisc code (Phase 7: 5–15 % of the estimate on one disc; its share grows on a stack, whose picture noise falls while the code does not).

   Measure the noise in the active picture directly (flat areas of the held runs, from the same-phase differences) and compare it with the blank-row estimate, per disc and on the stack. Try other estimators: rows 12–29 without line 16; a median; and the picture's own flat areas.
   - **Acceptance:** the ratio of blank-row to picture noise on each input, and each estimator's held-still ΔE and luma error.
3. **Other sides.**
   - Community North, National A and National B: their Channel Calibration and Domesday2D Calibration figures.
   - Held runs on each side (Test Card F, or any still held for 20 frames or more).
   - Twenty or more single-frame stills on each side. On National these include photographs with text overlays.
   - **Acceptance:** the Phase 7 step 1 and step 2 tables for each side.
4. **Dropouts.**
   - The decoder reads each line's 312-line partner from the other field. A line that Dropout Correction rebuilt from its neighbours breaks that relation, for the transform and for the partner check.
   - On the noisy bench, cut runs of 1–3 lines in one field at the Domesday model's rates, repair them with Dropout Correction (`interpolate`), and score each decoder against the truth in a band around the repairs.
   - **Acceptance:** each decoder's error near repairs, against the same stills without dropouts.
5. **Motion the schedule misses.** The schedule's default sends to `domesday2d` every frame the Stacker did not find held, so motion it misses is combed.
   - Decode a moving sequence (National's surrogate walks and film, and a videosynth pan) with all three decoders.
   - Measure each decoder's error against `transform2d` and against the truth (videosynth), and count how many frames of a National side the Stacker would leave to `domesday2d` although they move.
   - **Acceptance:** how much worse a misrouted moving frame is, and how often it happens, stated in the decoder guide.
6. **Better measures.** Every Phase 7 table gets:
   - **The 99th percentile of ΔE** beside the mean, since a local artefact hides in a mean.
   - **A confidence interval**, by bootstrap over the frames of each held run and over the stills.
   - **Luma on its own**, as the luma r.m.s. and its 3.5–5 MHz part. ΔE mostly weighs colour, and luma is what is seen first:
     - on the edge card Domesday2D's luma error is 9.8 % against `pal2d`'s 4.5 %;
     - on the real flat-colour maps it is 0.2–0.3 % above the others.
   - **Acceptance:** the Phase 7 and Phase 9 tables carry all three.
7. **Checking the held reference.**
   - **Against the card.** Register Test Card F's held reference (DS1 3001–3020) against the card artwork that videosynth bundles, with a fitted scale, offset, level and gamma, and report what remains. A reference good to within the decoders' differences validates the held-still tables.
   - **The horizontal chroma bandwidth.** The reference demodulates with PalColour's horizontal filter, the same as Domesday2D's, so it cannot show whether that filter is too narrow. Build the reference with a wider filter (1.3 MHz, the videosynth chroma FIR) and with a narrower one, and re-score.
   - **Acceptance:** the residual against the card, and each decoder's ranking under each reference filter.
8. **Synthetic against real** (added at the user's request while Phase 9 ran). On the noisy bench Domesday2D takes about 40 % off `transform2d`'s ΔE; on the real held runs, 26 % on Test Card F and under 10 % on most maps. Find why: whether the bench models the channel, the noise or the Hanover error wrongly, or whether it is the pictures.
   - **Acceptance:** the gap taken apart into its causes, with each cause's share.

### Phase 9 outcome (2026-11-03)

Nothing in the decoder was changed. Every figure is from the current decoders (Phase 8, 849cc90b), so the Phase 7 decodes were made again. Tools: `phase9_gap.py`, `phase9_stack.py`, `phase9_sides.py`, `phase9_noise.py`, `phase9_dropouts.py`, `phase9_motion.py`, `phase9_reference.py` and `phase9_tables.py` in [`tools/domesday2d-bench/`](../tools/domesday2d-bench/). `bench.measure()` now also reports `delta_e_p99` and `y_hf_rms_pct`. The decodes, projects and scores are beside the captures, in each side's `domesday2d-phase9/`.

| Item | State |
|---|---|
| 1. The full Domesday chain | **Done; the held ranking changes.** On the stack Domesday2D still has the lowest mean ΔE and 99th percentile on the held runs, but its lead over `transform2d` falls to 0.15 with an interval that takes in 0, and it loses three of nine (the OS map, the town map, the photograph). Stacking removes most of the noise, and with it most of Domesday2D's lead on maps and photographs, as item 8 predicts. Across pressing-group stacks it agrees best on 27 of 30 stills, as on single discs |
| 2. The noise estimate | **Done.** On a single disc the blank rows read 0.90–0.93 of the picture's noise, and every estimator that reads lower decodes worse. Reading higher helps: 1.5× the blank-row figure takes 0.02–0.03 off the held stills' ΔE and 3 % off their luma error, and the best lies further up. On the stack the blank rows read 2.8× the picture's noise, and a higher reading still helps |
| 3. Other sides | **Done.** On the held runs Domesday2D has the lowest ΔE and 99th percentile on all four sides (−0.23 to −1.13 against `transform2d`), and across pressing groups it agrees best on all four. Within a pressing `pal2d` agrees best on Community North and National A, by luma. Its one systematic loss is National A's colour bars, which carry a symmetric line-alternating leak the per-frame fit does not take out |
| 4. Dropouts | **Done.** Domesday2D stays lowest near repairs at every rate, but it loses the most there: ΔE +0.52 near real-rate repairs against +0.30 for the others, and +1.9 against +1.0–1.3 near whole-line repairs, where its luma ends level with `pal2d`'s |
| 5. Misrouted motion | **Done.** Interlaced motion costs Domesday2D little up to about 2 samples a field; at 8 it ties `transform2d` on a photograph, and at 24 it is worst by 0.7–0.9 ΔE. On National B 37 % of the frames are video motion that `domesday2d, held:transform3d` would comb; on Community North and National A, none beyond the test's own false-alarm rate |
| 6. Better measures | **Done.** Every table below carries the 99th percentile, the luma error and its 3.5–5 MHz part, and bootstrap intervals |
| 7. The held reference | **Done, half met.** Every ranking holds under a narrower and a wider reference filter. The card artwork is a different rendering of the card, so it cannot check the reference to the 0.1–0.3 % the decoders differ by |
| 8. Synthetic against real | **Done.** The bench's channel, noise and Hanover models account for little of the gap. It is the pictures: Domesday's carry 3–10 times less luma in the chroma band than the bench's stills, and on its dense maps Domesday2D loses at luma detail even with no noise at all |

**8. Synthetic against real.** On the noisy bench Domesday2D takes 46 % off `transform2d`'s mean ΔE (2.88 against 5.37, the four-rotation baselines); on the Community South held runs, 11 %. Each held run's frames were rebuilt between real and synthetic ([`phase9_gap.py`](../tools/domesday2d-bench/phase9_gap.py)), four frames of each of the 18 runs, and decoded by the real decoders:

- **R:** the real frame.
- **X0:** the mean of the run's frames of R's carrier phase: the real channel and picture, the noise reduced by about √6.
- **X1:** the run's reference (its Hanover error taken out) modulated exactly on R's lattice, as videosynth modulates: the real picture over an ideal channel.

Each is scored against the run's reference, the exact truth of X1; R against the reference without it, as Phase 7 scores. Mean ΔE over the runs:

| Input | `pal2d` | `transform2d` | `domesday2d` | Domesday2D against `transform2d` |
|---|---|---|---|---|
| R, the real frame | 3.55 | 3.34 | 2.98 | −10.8 % |
| X0, the real composite without its noise | 2.72 | 2.54 | 2.39 | −6.1 % |
| X1, the picture over an ideal channel | 2.44 | 2.19 | 2.05 | −6.7 % |
| X1 + the bench's noise | 3.20 | 2.93 | 2.54 | −13.3 % |
| X1 + the frame's real noise | 3.28 | 3.03 | 2.66 | −12.2 % |
| X1 + the run's Hanover error + real noise | 3.28 | 3.04 | 2.68 | −11.9 % |
| The noisy bench, for comparison | 6.03 | 5.37 | 2.88 | −46 % |

- **The channel is not it.** The real composite without noise (X0) and the ideal one (X1) give Domesday2D the same small lead, 6–7 %. What the real channel adds after Channel Equalise costs every decoder alike, about 0.3 ΔE.
- **The noise is not it.** The bench's noise and the frame's own give the same lead, 13 % and 12 %. Noise widens the lead, as Domesday2D's vertical fit removes more of it. In flat areas the two noises are alike (below 3 MHz 0.91 % real against 0.83 % bench; the chroma band 1.43 % against 1.67 %, the bench a sixth higher). Over the whole picture the real frame's below 3 MHz reads 1.40 %: frame-to-frame timing jitter at edges, which the bench does not model, and which moves the lead by about a point.
- **The Hanover error is not it.** Putting the run's error back changes Domesday2D by 0.014: the per-frame correction takes it out.
- **The pictures are.** Over an ideal channel without noise Domesday2D leads by 6.7 % on Domesday's pictures, against 53 % on the clean bench. Two things differ:
  - **Less for any separator to fix.** The share of luma energy at 3.5–5.5 MHz is 0.1–2.6 % on the Domesday stills (Test Card F 2.6 %, the town map 1.6 %, the photograph 0.1 %) against 3–12 % on the bench's (Test Card F 6.3 %, the OSM town map 12 %). Domesday's pictures came through a camera and a 1980s PAL chain and are soft; the bench's are rendered sharp, and four of its twelve stills are stress patterns where the frame-raster geometry wins 2–3 times over. Its mean is dominated by them.
  - **The maps at luma detail.** With no noise and an ideal channel, Domesday2D still loses to `transform2d` on the OS map, the town map, the map key and the photograph (by 2–13 %). It wins in their flat areas (town map 0.97 against 1.10) and loses where the luma has detail (6.64 against 5.32). Its chroma error there sits at low vertical frequencies (22 % of it below 1/8 cycle a line, against 9 % for `transform2d`), so it is broad hue and saturation error over the detail, not lost resolution. On the bench's synthetic maps it wins in the same places (OSM town, at detail: 5.56 against 8.83). Both carry about 3 % of fine static chroma at detail; what the bench lacks is whatever the real maps' detail is.
- **Not the per-frame Hanover fit.** Turning `hanover_per_frame` off on X1 changes nothing.
- **Not mastered streaks.** In strictly flat areas the reference holds no line-to-line chroma beyond its own leftover noise (0.00–0.07 %, against 0.4 % noise); the streaks seen in the town map's chroma lie at its luma detail.

So the bench overstates Domesday2D's gain because its pictures are sharp and synthetic, not because it models the disc wrongly. Two things in the model are off and matter for tuning: the blank rows read 1.08× the bench's picture noise but 0.90–0.93× a real disc's (item 2), and the bench has no frame-to-frame jitter. The held-out stills of Phase 10 item 4 should be Domesday-like (soft, camera-shot, real maps), or the bench will keep tuning for content the discs do not have.

**2. The noise estimate.** [`phase9_noise.py`](../tools/domesday2d-bench/phase9_noise.py) measures each held frame's noise in the picture's flat areas (the frame less the mean of the run's other frames of its phase, chroma band, scaled for that mean's own noise) and decodes four frames of each run with the prototype, with the reassignment's noise taken from each estimator. Community South, per disc:

| Estimator | Reads, against the picture's noise (DS1 / DS6) | Held ΔE DS1 / DS6 | Luma % DS1 / DS6 |
|---|---|---|---|
| Rows 12–31, the decoder's | 0.93 / 0.90 | 3.055 / 2.905 | 2.557 / 2.380 |
| Rows 12–29, without line 16 | 0.87 / 0.81 | 3.062 / 2.913 | 2.574 / 2.401 |
| Rows 12–31, median | 0.74 / 0.78 | 3.068 / 2.915 | 2.599 / 2.407 |
| The frame's own flat areas | 1.05 / 1.08 | 3.041 / 2.896 | 2.526 / 2.354 |
| Rows 12–31 × 1.25 | 1.16 / 1.12 | 3.039 / 2.891 | 2.509 / 2.339 |
| Rows 12–31 × 1.5 | 1.39 / 1.35 | 3.028 / 2.883 | 2.474 / 2.309 |

- **The picture is noisier than the blank rows**, by about a tenth: the noise grows with the picture level in the chroma band, which the bench's model puts only above 5.5 MHz. On the bench the blank rows read 1.08× its picture noise, so the reassignment was tuned where its estimate sits high, and on a disc it sits low.
- **Line 16 is not the problem on a single disc.** Leaving it out lowers the reading by 4–10 % and makes every still slightly worse.
- **Higher is better, past the picture's own noise.** The best scale is above 1.5; the photograph of DS1 (blank rows 2.28 %, a dropout in them) is the only run that does not gain. Where the optimum lies, and whether a level-aware estimate does better than a scale, is Phase 10 item 1's.
- **On the stack the blank rows read 2.8× the picture's noise** (1.54 % against 0.55 %; without line 16 2.25×, median 2.32×). The Stacker's still-frame copies and Picture Band Limit work on the active picture only, so the blank rows keep a single stack's noise and line 16's code while the picture loses more. The picture's figure reads low here, as the copies make a run's frames share their noise; the ratio is an upper bound. Even so, every lower estimate decodes worse there too, and 1.5× is still slightly better (ΔE 2.213 against 2.222, luma 1.588 against 1.628 %). So the reassignment is better with a higher threshold than any honest noise estimate gives, on a disc and on a stack: its threshold scale, not only its estimator, is Phase 10's to set.

**4. Dropouts.** [`phase9_dropouts.py`](../tools/domesday2d-bench/phase9_dropouts.py) cuts dropouts into the noisy bench, writes them to its dropout sidecar, repairs them with Dropout Correction (`interpolate`) and scores each decoder within 4 rows and 16 samples of a repair, against the same band of its decode of the bench without them. Real Community South dropouts (DS1, the whole side) come about 9 to a frame, median 14 samples, 99 % under 103 samples and none over 220: none is a whole line. Three cases:

| Case | `pal2d` ΔE (luma %) | `transform2d` | `domesday2d` |
|---|---|---|---|
| The real rate and lengths (9 a frame) | 4.72 → 5.02 (2.97 → 3.79) | 4.74 → 5.04 (3.32 → 4.18) | 2.20 → 2.72 (1.79 → 3.08) |
| Ten times the real rate | 5.79 → 6.08 (4.29 → 5.00) | 5.14 → 5.44 (4.05 → 4.84) | 2.77 → 3.19 (2.62 → 3.89) |
| Whole lines, 1–3 of one field, 4 a frame | 7.16 → 8.45 (4.33 → 5.97) | 7.18 → 8.15 (4.90 → 6.64) | 3.41 → 5.30 (3.27 → 6.03) |

- **Domesday2D stays best near repairs**, but its margin shrinks: a repaired run no longer matches its 312-line partner, and the frame-raster transform reads the mismatch as chroma. Its luma near repairs suffers most: +1.3 % at the real rate against +0.8 % for the field decoders.
- **The worst stills** are the ones with vertical colour detail: the green/magenta edges (2.77 → 5.85 at the real rate) and the tartan (0.78 → 1.19).
- **Whole-line repairs** do not occur on these captures; they leave Domesday2D's luma near the repair level with `pal2d`'s.
- Only rotation 0 was run.

**5. Motion the schedule misses.** [`phase9_motion.py`](../tools/domesday2d-bench/phase9_motion.py).

- **On the bench.** A photograph (Galtströms) and the OSM town map panned horizontally under the still bench's noise, rendered by videosynth with their truth: as interlaced motion (each field a different instant) at 2, 8 and 24 samples a field, and as film at 2 (both fields one instant). Ten frames each, mean ΔE (99th percentile):

| Pan | Picture | `pal2d` | `transform2d` | `domesday2d` |
|---|---|---|---|---|
| Film, 2 samples a field | photograph | 3.09 (10.6) | 2.96 (10.1) | 2.28 (7.9) |
| Interlaced, 2 | photograph | 3.09 (10.6) | 2.96 (10.2) | 2.45 (8.7) |
| Interlaced, 8 | photograph | 3.05 (10.5) | 2.92 (10.1) | 2.94 (11.9) |
| Interlaced, 24 | photograph | 2.78 (9.2) | 2.64 (9.1) | 3.35 (13.8) |
| Film, 2 | map | 7.23 (26.0) | 6.31 (22.1) | 4.31 (17.6) |
| Interlaced, 2 | map | 7.23 (26.0) | 6.31 (22.2) | 4.50 (18.2) |
| Interlaced, 8 | map | 6.95 (25.3) | 6.06 (21.5) | 5.32 (23.9) |
| Interlaced, 24 | map | 4.91 (18.6) | 4.70 (17.5) | 5.62 (23.9) |

  The 8 and 24 pans are of wider pictures, scaled down further, so they are softer than the 2-sample pans; compare decoders within a row. Up to about 2 samples a field (a pan across the picture in 9 s) Domesday2D's lead is barely touched. By 8 its 99th percentile is the worst of the three, and by 24 (3 s across) it is worst on the mean too, by 0.7–0.9 ΔE.
- **How often.** Every 25th frame of each side's stacked video, scored by how far its fields are from one instant (each line of one field against the other field's lines either side, relative to its own field's). The threshold is the 99th percentile of the frames in the side's held runs, so about 1 % of frames read as motion by chance:

| Side | Lit frames scored | Not held, two instants | Share |
|---|---|---|---|
| Community North | 1,262 | 19 | 1.5 % |
| National A | 1,945 | 12 | 0.6 % |
| National B (CLV) | 3,520 | 1,312 | 37 % |

  Community North and National A (whose surrogate walks are stills) have no interlaced motion beyond the test's false alarms. National B is motion and film; the film's fields are one instant and decode well, but over a third of the side is video motion. The decoder guide now says so.

**7. Checking the held reference.** [`phase9_reference.py`](../tools/domesday2d-bench/phase9_reference.py).

- **Against the card.** Test Card F's reference (DS1 3001–3020, and DS6) registers on the bench's card truth with a scale of 0.9965 horizontally and 1.0022 vertically and offsets under a sample, the same on both discs. But the levels do not fit one curve: the disc's card has a darker background grey against its whites than videosynth's, the girl's picture is graded differently, and the chroma reads 0.69 of the artwork's and 5.4° turned. After a free monotone tone curve the flat parts of the card agree to a median of 1.0 % in luma, the picture to 6 %, and the r.m.s. stays near 20 % at the edges, where the artwork's elements do not sit exactly where the disc's do. videosynth's card is a re-drawing, not the slide the BBC filmed, so it cannot check the reference to the 0.1–0.3 % the decoders differ by.
- **The reference's horizontal chroma filter.** Built with a narrower filter (a 0.8 MHz raised cosine) and a wider one (videosynth's 1.3 MHz FIR) than PalColour's, and every current decode of the Community South held runs re-scored:

| Reference filter | `pal2d` | `transform2d` | `domesday2d` |
|---|---|---|---|
| 0.8 MHz raised cosine | 3.509 | 3.279 | 2.909 |
| PalColour (1.1 MHz) | 3.547 | 3.340 | 2.981 |
| 1.3 MHz videosynth FIR | 3.609 | 3.413 | 3.069 |

  The ranking of every still is the same under all three. A wider reference costs every decoder about 0.06–0.09 and Domesday2D a little more (0.088 against 0.073): the discs carry little chroma above PalColour's band, and the decoders lose about as much of it.

**1. The full Domesday chain.** [`phase9_stack.py`](../tools/domesday2d-bench/phase9_stack.py) builds the chain from Community South's stack project:
- the eleven captures, each through its disc map and FM Beat Cancel;
- Source Align, then the Stacker (`Weighted`, `still_frame_stacking` on);
- Dropout Correction (`interpolate`), then Picture Band Limit at the set's 5.36 MHz;
- Channel Equalise, then the three decoders.

The held runs go through the whole stack. The thirty single-frame stills go through each pressing group stacked on its own: P (DS1, DS2, DS4, DS8, DS12), Q (DS6, DS9, DS10), R (DS3) and S (DS5, DS11). Each chain is calibrated on its own output. The calibration ran with the Stacker's still-frame search off (see "Left open"), since what it measures does not need it. The stack's calibration came out as smear 0.232 with τ 6.22 samples, and `hanover_*` −0.002, 0.082, −0.114, −0.022. The groups' smear ranges from 0.217 (S) to 0.244 (R).

**Held runs on the stack** (mean over every frame of each run, ΔE / 99th percentile / luma %):

| Still | `pal2d` | `transform2d` | `domesday2d` |
|---|---|---|---|
| Test Card F | 2.25 / 15.9 / 2.73 | 1.55 / 9.1 / 1.18 | **1.19 / 4.9 / 1.02** |
| UK map | 1.46 / 9.4 / 1.26 | 1.52 / 9.8 / 1.36 | **1.23 / 8.1** / 1.26 |
| England map | 2.49 / 15.1 / 2.03 | 2.54 / 15.6 / 2.11 | **2.07 / 9.3** / 2.07 |
| Menu | 1.67 / 13.0 / 1.15 | 1.74 / 13.5 / 1.37 | **1.07 / 7.1** / 1.29 |
| UK colour map | 2.10 / 10.5 / 1.73 | 2.12 / 11.1 / 1.81 | **1.86 / 8.6** / 1.88 |
| OS map | 4.85 / 20.9 / 2.94 | **4.45** / 19.0 / **2.50** | 4.58 / **18.9** / 2.75 |
| Town map | 4.29 / 20.6 / 2.87 | **3.77 / 17.1 / 2.27** | 4.15 / 19.9 / 2.45 |
| Map key | 1.85 / 10.5 / 1.14 | 1.72 / 10.0 / 0.81 | **1.69 / 6.2 / 0.77** |
| Photograph | 2.18 / 10.7 / 1.30 | **1.99 / 10.1** / 1.11 | 2.17 / 10.4 / **1.10** |
| Mean [95 % interval] | 2.57 [1.91, 3.32] | 2.38 [1.78, 3.06] | **2.22** [1.52, 3.03] |

- **Against the single discs.** Every decoder gains about a third from the stack (Domesday2D 2.98 → 2.22, `transform2d` 3.34 → 2.38). Domesday2D's lead over `transform2d` shrinks from 0.36 [0.23, 0.51] to 0.15 [−0.05, 0.36]. Its 99th percentile lead stays (−2.4 [−4.3, −0.6]), and its luma is level (+0.01).
- **The ranking changes on three stills.** The OS map, the town map and the photograph go to `transform2d`, by 0.13, 0.38 and 0.18. These are the stills where item 8's noise-free real composite (X0) already put Domesday2D behind. Its lead on single discs came largely from handling noise better, which a stack has much less of; at the maps' luma detail it was already behind. The decoder guide's advice still holds for the stack: Domesday2D on everything that is not held, since in that schedule the held stills (these runs) go to `transform3d`. But for a single-frame map or photograph from a stack, the margin over `transform2d` is small or negative.
- **The reference.** The stack's held reference is built from its own run. The still-frame copies already average each frame with its repeats of the same phase, so leaving a frame out of its reference no longer keeps its noise out; the absolute figures read somewhat low. The comparison between decoders stands.

**Cross-disc agreement between pressing-group stacks** (every pair of the four group stacks is across groups; mean over the thirty stills [95 % interval]):

| Decoder | ΔE | p99 | Y % | Y 3.5–5 MHz % | U % | V % |
|---|---|---|---|---|---|---|
| `pal2d` | 3.24 [2.89, 3.63] | **13.9** | **3.76** | **0.75** | 0.74 | 0.76 |
| `transform2d` | 3.61 [3.19, 4.10] | 16.1 | 4.59 | 1.80 | 0.70 | 0.73 |
| `domesday2d` | **3.01** [2.60, 3.47] | 15.5 | 4.57 | 1.80 | **0.45** | **0.49** |

As on single discs (3.15 against 3.56 and 3.83), Domesday2D agrees best, lowest on 27 of 30 stills (−0.23 [−0.32, −0.13] against `pal2d`, −0.60 against `transform2d`). Its chroma agrees best by about a third. `pal2d`'s softer luma agrees best in luma and at the 99th percentile.

**6. Better measures.** [`phase9_tables.py`](../tools/domesday2d-bench/phase9_tables.py) scores every frame of every held run and keeps each picture's cross-disc disagreement. The intervals are 95 % by bootstrap: over a run's frames for one run, and over the runs or pictures for a mean, with Domesday2D's differences resampled in pairs. The tables of items 1 and 3 carry the 99th percentile of ΔE, the luma r.m.s. and its 3.5–5 MHz part. What they add to Phase 7's means:

- **The 99th percentile.** On the held runs Domesday2D's local worst is the lowest of the three on every side, by 1.6–5.8, more than its mean lead. The exceptions are the town map (20.4 against 18.2 on Community South) and National A's bars.
- **Luma.** Its luma r.m.s. is level with `transform2d`'s. Its 3.5–5 MHz part is above `pal2d`'s on every side but National B, and above `transform2d`'s on Community South only (+0.07 [+0.00, +0.14]). Across discs, `pal2d`'s luma agrees best everywhere.
- **Intervals.** The per-run intervals are narrow (±0.01–0.02 within a run), so the frame-to-frame spread does not decide anything. The intervals over runs and pictures are wide, as the stills differ. Paired, Domesday2D's lead over `transform2d` excludes 0 on every side's single discs, and does not on the stack.

**3. Other sides.** [`phase9_sides.py`](../tools/domesday2d-bench/phase9_sides.py) runs Phase 7's steps 1 and 2 on each side, with the disc maps of the side's stack project and each disc calibrated over the whole side. The held runs and pictures were found on the side's stacked video: a held run is 22 or more frames whose thumbnails do not change, and 24 frames of each are scored; a single-frame still is a frame unlike both neighbours and unlike their average. Pressing groups are from each side's capture comparison. Disc choices worked around the captures: National A's DS1 ends at frame 37,482, and National B's DS3 and DS9 end before its last cut. The stack's Source Align moves some discs on a frame, so they are read a frame earlier in their own maps.

| Side | Held runs (24 frames each, ×2 discs) | Pictures for cross-disc agreement | Discs, by pressing group |
|---|---|---|---|
| Community North | colour bars, UK and British Isles maps, menu, OS map, map key, three photographs (one captioned) | 24 single-frame stills: photographs, aerial and satellite pictures, OS maps | A: DS1, DS4; B: DS6, DS12; C: DS3. Held: DS1, DS6 |
| National A | colour bars, Gallery title and a Gallery view (surrogate walk), a data map, menu, a diamond graphic, three photographs (one captioned) | 24 single-frame stills: photographs, graphics, a map | A: DS2, DS7; B: DS3, DS9; C: DS4, DS8. Held: DS2, DS8 |
| National B (CLV) | six held graphic panels | 29 frames of motion and film at cuts (the side has no single-frame stills) | A: DS2, DS10; B: DS3, DS9; C: DS4, DS5; D: DS11. Held: DS10, DS9 |

**Calibration.** Smear fraction and τ (samples), and the `hanover_*` rotation (M_UV − M_VU)/2:

| Side | Smear | Rotation |
|---|---|---|
| Community North | 0.228–0.265, τ 5.8–6.3 | 0.083 to 0.107 |
| National A | 0.237–0.266, τ 5.7–6.1 | −0.017 to −0.009 |
| National B | none: Channel Calibration finds no smear on CLV (Phase 2), and the equaliser stays off | −0.078 to −0.037 for groups A, B, D; +0.010 and +0.011 for group C |

On National B the side's rotation follows the pressing: group C reads the opposite sign to the others. On Community South it followed the still (Phase 7); here a disc-wide average of motion video shows a part that belongs to the pressing as well.

**Held runs** (mean over the runs [95 % interval], bootstrap over runs; Domesday2D's difference paired per run):

| Side | `pal2d` ΔE | `transform2d` ΔE | `domesday2d` ΔE | Domesday2D − `transform2d`: ΔE / p99 / luma % / luma 3.5–5 MHz % | Runs where Domesday2D is lowest |
|---|---|---|---|---|---|
| Community South | 3.55 | 3.34 | 2.98 [2.52, 3.49] | −0.36 [−0.51, −0.23] / −2.2 / +0.03 [−0.02, +0.07] / +0.07 [+0.00, +0.14] | 16 of 18 |
| Community North | 3.39 | 3.35 | 2.95 [2.67, 3.28] | −0.41 [−0.53, −0.30] / −2.9 / −0.02 [−0.06, +0.02] / +0.02 [−0.04, +0.08] | 17 of 18 |
| National A | 2.74 | 2.66 | 2.43 [2.18, 2.70] | −0.23 [−0.35, −0.05] / −1.6 / +0.07 [−0.01, +0.17] / +0.11 [−0.02, +0.31] | 16 of 18 |
| National B | 4.31 | 4.35 | 3.22 [3.13, 3.32] | −1.13 [−1.14, −1.11] / −5.8 / −0.13 [−0.16, −0.10] / −0.33 [−0.39, −0.28] | 12 of 12 |

- **Domesday2D is best on every side.** Its 99th percentile is lowest everywhere, by 1.6–5.8.
- **Its losses.**
  - Community South's town map on both discs, as in Phase 7.
  - Community North's bars on DS6 (2.68 against `pal2d`'s 2.61).
  - National A's bars on both discs (3.57 and 3.20 against 2.54 and 2.52). The bars run's own line-alternating matrix is [[−0.009, 0.155], [−0.069, 0.020]]: beside a rotation of 0.11 it holds a symmetric leak of 0.043, which the disc's calibration (near 0) does not carry and the per-frame fit (rotation only, since Phase 8) cannot take out. On saturated bars that leak alternates line by line, the field decoders average it, and Domesday2D keeps it and hands part of it to luma (luma 4.7 % against 3.6 %). So the symmetric part can belong to the still as well.
- **Luma.** Level with `transform2d` everywhere but National B, where it is better. Its 3.5–5 MHz part is above `pal2d`'s on every side but National B (by 0.29–0.33 %), and above `transform2d`'s on Community South by an interval that only just excludes 0.
- **National A's capture faults.** Three frames of DS2's runs score ΔE 14–34 for every decoder alike (a skip in the capture: another picture). Frames every decoder reads at over three times the run's median are left out.
- **National B's panels** are where Domesday2D's lead is largest (ΔE 3.2 against 4.3): saturated graphics with sharp colour edges, the bench's kind of picture.

**Cross-disc agreement** (mean over pictures [95 % interval]):

| Side | Within a group: `pal2d` / `transform2d` / `domesday2d` ΔE | Lowest within | Across groups: `pal2d` / `transform2d` / `domesday2d` ΔE | Lowest across |
|---|---|---|---|---|
| Community South | 1.38 / 1.81 / **1.26** | 24 of 30 | 3.56 / 3.83 / **3.15** | 27 of 30 |
| Community North | **2.31** / 2.63 / 2.41 (+0.09 [−0.01, +0.20] against `pal2d`) | 10 of 24 | 5.07 / 5.17 / **4.78** | 17 of 24 |
| National A | **1.04** / 1.38 / 1.18 (+0.14 [+0.10, +0.18]) | 2 of 24 | 2.59 / 2.77 / **2.36** | 19 of 24 |
| National B | 1.88 / 2.12 / **1.80** | 21 of 29 | 3.96 / 3.94 / **3.50** | 28 of 29 |

- **Across pressing groups Domesday2D agrees best on every side**, and its U and V disagree least on every side, within groups too.
- **Within a group `pal2d` agrees best on Community North and National A.** These are luma: within a group Domesday2D's luma disagrees by 0.34–0.46 % more than `pal2d`'s, its chroma by 0.04–0.09 % less. The pictures are dense photographs and maps, and within a pressing the remaining differences are the captures' sub-sample registration, which `pal2d`'s softer luma hides, as Phase 7 found across groups on Community South.
- **National B's pictures are motion frames.** Agreement between discs cannot see combing, which every disc shows alike; item 5 measures that.

**Left open (for Phase 10):**

- **The noise scale.** Item 2: the reassignment wants a higher noise reading than the blank rows give, on discs and on the stack.
- **The maps at luma detail.** Item 8: with no noise, Domesday2D's chroma at the maps' luma detail is worse than `transform2d`'s. This is the town map's loss, and the first thing for Phase 10 item 2.
- **Held-out stills like Domesday's.** Item 8: the bench's stills are sharper and more synthetic than the discs', so its gains do not carry over. Phase 10 item 4's set should be soft, camera-shot pictures and real maps.
- **A still's symmetric leak.** Item 3: National A's bars carry a symmetric line-alternating term of their own (0.043) that neither the calibration nor the per-frame rotation takes out. Fitting the symmetric part per frame too, where the frame has two or more hues to fit it from, is a candidate.
- **Repairs.** Item 4: near a Dropout Correction repair Domesday2D loses more than the field decoders. Marking repaired samples so the 312-line partner check and the transform discount them is a candidate.
- **Motion.** Item 5: National B needs its video motion decoded by `transform2d` or `transform3d`; the schedule has no motion test of its own.
- **Why calibration is so slow behind a Stacker** (the user asked for this to be looked at). On one disc, Channel Calibration and Domesday2D Calibration over a whole side take a few minutes. Behind Community South's 11-disc Stacker they took:
  - **With `still_frame_stacking` on,** as the side's project has it: about 14 minutes for Channel Calibration (3 windows of 48 frames) and over 80 minutes for Domesday2D Calibration (200 frames spread over the side). The second was stopped unfinished.
  - **With it off:** both calibrations for the full stack and the four pressing-group stacks took 41 minutes together, Domesday2D Calibration on the full stack alone over 15.

  The likely causes are the following, and none is measured yet:
  - Every frame is stacked from 11 captures read from the NAS.
  - Frames spread over the side defeat the sources' read-ahead and the Stacker's caches.
  - The still-frame search reads a window of ±24 frames on every disc for each frame: about 760 MB per calibration frame, about 45 MB/s from the NAS at the time.

  Questions to answer:
  - Whether the calibrations should read their frames in runs rather than spread one by one.
  - Whether the Stacker's still-frame search is needed for what they measure. The VITS lies outside the active picture the copies cover, and the Hanover fit only gains noise reduction from them.
  - Whether they could run on the sources before the Stacker and combine the results.

## Phase 10 — Tuning and acceptance

**Goal:** change the decoder only where Phases 8 and 9 show a gain, check it on stills it was not tuned on, and settle the cost.

1. **Constants at every noise level.**
   - The fit's strength λ = 0.01 + 5 × the 3.5–5 MHz luma detail, and the detail measure includes noise. So a clean stacked input weakens the fit and a noisy disc strengthens it, and only one noise level (`pal_domesday_A`) was ever tuned.
   - Render the bench at 0, ½, 1, 2 and 4 times the model's noise. At each level find the best `kLambda0`, `kLambdaDetail`, `kDetailGate` and the shrinkage threshold's scale.
   - If the best values move with the noise, scale them by the noise the decoder measures (Phase 9 item 2's estimator).
   - **Acceptance:** at every level, the tuned decoder is no worse than the current one on any still and is better on the average.
2. **The town map.** Domesday2D loses there to `transform2d` on both discs, in the black outlines on pale fills. Item 1's noise-aware strength is the first thing to try, then a chroma-noise floor on λ.
   - **Acceptance:** the town map's held ΔE no worse than `transform2d`'s, with no other held still worse.
3. **Luma.** Wherever Phase 9 item 6 shows Domesday2D's luma worse than the others' (the edge card, the flat-colour maps), find whether it is cross-luminance from the transform's keep-or-drop decisions or detail lost to the residual's reassignment, and fix what is fixable.
   - **Acceptance:** the luma r.m.s. no worse than the better 2D decoder on any held still, or the cause recorded.
4. **Stills it was not tuned on.**
   - Every constant so far was chosen on the twelve-still bench that also judged acceptance, and Phase 3 measured a generalisation gap of about 1.2 points.
   - Make a held-out set of 8–12 stills, licence-clean and never used in tuning: photographs, maps, coloured text, a saturated graphic. Render it like the bench (clean and noisy, four rotations).
   - **Acceptance:** Domesday2D best on each class's primary metric. Its margin over `transform2d` is within a third of the bench's, or the gap is recorded as a limit.
5. **The acceptance again.** Re-run, with the final decoder:
   - the bench at four rotations;
   - the held-out set;
   - Phase 7 steps 1 and 2 on Community South;
   - Phase 9 items 1 and 3 (the stack and the other sides).

   **Acceptance:** Phase 7's targets on every side, and the new panels of the changed stills signed off by the user.
6. **The cost.** After the tuning, measure the cost as in Phase 6. Take to design review with it, under AGENTS.md §6:
   - the levers Phase 6 left (single precision in the fit, fewer iterations, a decimated detail band) with their measured accuracy cost;
   - the time and memory against `transform2d`.

   **Acceptance:** the review's decision recorded here.

### Phase 10 progress (2026-11-04, paused)

**Paused, at the user's decision, until videosynth models two things the discs have and the bench lacks:** softer programme pictures ([videosynth#18](https://github.com/decode-orc/videosynth/issues/18)) and the per-line timing error left after time-base correction ([videosynth#19](https://github.com/decode-orc/videosynth/issues/19)). Then retune on a bench that has both. Nothing is committed. Tools: [`phase10_tune.py`](../tools/domesday2d-bench/phase10_tune.py) (`sweep`, with `--heldout` for the held-out renders; `real`; `report`; `real-report`) and [`phase10_heldout.py`](../tools/domesday2d-bench/phase10_heldout.py).

**In the decoder (uncommitted), from item 1:**

- **The fit's detail leaves out chroma that the transform left in luma at colour edges.** That chroma raised λ and smeared the edges, which accounts for the edge card's ΔE and luma loss.
  - λ's detail is now max(min(H, 2.5·L), H − 0.25·C), where:
    - H is the 3.5–5 MHz luma r.m.s.;
    - L is the 2.5–3.2 MHz luma r.m.s. (`kBelowChromaLowHz`/`HighHz`, `kLeakRatio`);
    - C is the separated chroma's local r.m.s. (`kLeakChroma`).
  - Real luma either spans both bands or exceeds what the chroma could leak. A band test alone (min(H, a·L)) takes narrow-band real luma for leak: it wrecks the luma zone plate, the bars and the unit-test tolerance.
  - The reassignment's gate stays on H.
- **λ's floor rises with the noise:** 0.01 + 10 × the blank rows' noise squared (`kLambdaNoise`).
- **Cost:** one more plane (5.7 MB), two more inverse FFTs a row and one more box filter. Not yet timed (item 6). A first A/B on a loaded machine read both builds about 8× Phase 6's time, so it is not usable.
- **Against the C++:** within 0.007 ΔE of the prototype on every bench still.
- **Tests:** `SameOutputOnEveryPlatform` needs re-recording once the constants are final.

**What the detail rule gives** (bench, four rotations, mean ΔE against the Phase 8 decoder):

| Noise | Before | After | Notes |
|---|---|---|---|
| 0 | 2.124 | 1.920 | Edge card ΔE 5.85 → 3.62, luma 10.3 → 7.3 %; chroma zone plate 2.06 → 1.91 |
| 4× | 6.020 | 5.561 | Better on every still at 2× and 4× |

- **The strict item 1 acceptance is not met.** At low noise:
  - the bars go 0.337 → 0.345 ΔE (luma 0.35 → 0.43 %);
  - the luma zone plate goes 1.031 → 1.038 ΔE (luma 0.95 → 1.06 %);
  - at 1×, the tartan is +0.5 % ΔE and the luma zone plate +3.9 % luma.
- **On the real held runs it is neutral:** ±0.02 ΔE (Community South 2.729 → 2.726, the other sides 2.826 → 2.819).

**The bench and the real discs want different constants:**

| Set | Best | Against the alternative |
|---|---|---|
| Bench, every noise level | k = 5, tau = 1, d0 = 0.003 | Lower k costs the OSM town map 7–15 % ΔE. Higher tau or d0 costs up to 15 % ΔE and 25–75 % luma on single stills |
| Held-out set | k = 5, tau = 1, d0 = 0.003 at 1× noise and above | d0 = 0.03 costs the monochrome photograph +130 % luma at 0×. Tau = 2 costs 10–20 % luma on single stills |
| Community South stack and DS1 held runs | k = 2.5 with tau 2–3 or d0 0.03 | Luma falls 3–10 % on the maps and menus, below `transform2d`'s on most. The town and OS map ΔE falls 0.15–0.25. Costs the UK map and Test Card F 0.02–0.12 |

On the real discs, the split shows a different cause behind each open item:

- **Item 2 is the fit's strength.** At k = 1:
  - the CS DS1 town map gives 4.42 against `transform2d`'s 4.50, and the stack's OS map 4.27 against 4.44;
  - the stack's town map stays behind (3.79 against 3.76), and so does its photograph (1.989 against 1.985);
  - the UK map, Test Card F and the England map get worse.

  No setting tried makes the stack's town map or photograph no worse than `transform2d`.
- **Item 3 is the reassignment.** Turning it off, a higher gate or a higher tau all lower luma on every real still.

**Why the reassignment differs: timing jitter, not noise.**

- **Flat-area noise does not explain it.** An in-picture estimate from the fit's residual reads 0.83× the true chroma-band noise on the bench and 0.72× on DS1. So a per-source noise estimate would move tau by about 15 %, not 2×.
- **The excess is at edges.** Over the whole picture, DS1's frame-to-frame chroma-band variation is 1.6× its blank-row noise; the bench's is 0.92×.
- **Measured as a per-line horizontal shift** of each held frame against its run's mean, from the luma below 2.5 MHz:
  - **Size:** 6.3 ns r.m.s. a line on DS1, 5.8 ns on DS6, 4.1 ns on the stack.
  - **Field offset:** about 3 ns common to each field.
  - **Within a field:** a first-order process with a line-to-line correlation of 0.6.
  - **Between fields:** independent.
  - **Mostly a shift of the whole line:** the line's halves correlate at 0.85, and the slope between them is 3.5 ns.
- **How it reaches luma.** A jittered luma edge looks like line-alternating chroma-band energy to the frame-raster transform. The reassignment then hands it back to luma, while the held reference (a run mean) has it averaged out.
- **So a higher tau wins on the discs because it withholds jitter.**

**Domesday's pictures are soft** (videosynth#18). Measured as Test Card F's luma against the bench's card, after Channel Equalise, on DS1, DS6 and the stack within 0.7 dB:

- **Horizontal:** 0 dB to 2.5 MHz, −2.2 dB at 2.5–4 MHz, −3.6 dB at 4–4.5, −7.7 dB at 4.5–5 and −9.8 dB at 5–5.5 MHz.
- **Vertical:** −4 dB at about 0.28 cycles/row, and −10 dB above 3/8.
- **The player is not the cause.** The equalised multiburst is flat to 4.8 MHz, so the fall is in the pictures.
- **Most bench stills are sharper:**
  - The OSM town and rural maps, the text card and Test Card F carry 5–12 % of their luma at 3.5–5.5 MHz, against 0.1–1.3 % on the discs.
  - The bench's photographs are already as soft as the discs'.
  - Domesday's menu text is as soft as its camera pictures: 0.16 % at 3.5–5.5 MHz.
- **The held-out set was built on a wrong assumption:** it keeps its captions sharp (`TEXT_CAPTIONS` 3.6 %), assuming electronic text was sharp. Soften them before re-running item 5.

**Item 4** passed earlier, on the Phase 8 decoder:
- Domesday2D is best on all twelve held-out stills.
- Its margin over `transform2d` is 33.1 % noisy (minimum 15.1 %) and 35.5 % clean, against the bench's 39.7 % and 45.2 %.

**When videosynth#18 and #19 land:**
1. Put the measured response on frames 4, 7, 8 and 9 of `pal_stills.yaml`, and the measured jitter on the Domesday model. Soften the held-out captions.
2. Re-run the item 1 grid (k, tau, d0, and the detail rule's leak and β) on the bench and the held-out set, and compare with the real runs.
3. Then items 2, 3, 5 and 6 as written.

### Phase 10 resumed (2026-11-04, videosynth 049c712)

Both landed: videosynth#18 as a section's `picture_response` (preset `domesday`), videosynth#19 as the project's `line_timing` block.

**Bench changes:**
- **The response.** Frames 4, 7, 8 and 9 of `pal_stills.yaml` take `picture_response: domesday`. The picture truth includes it.
- **The jitter is in a separate copy.**
  - `line_timing` is a project-level block, and the variant script only strips section blocks. So jitter in `pal_stills.yaml` would also reach the clean copy, which measures separation alone.
  - Instead, `phase7_rotation.py write OUT --jitter` and `phase10_heldout.py write OUT --jitter` write it with videosynth's measured Domesday figures:
    - capture layer: 4.9 ns a line, correlation 0.6, 3.1 ns a field, 3.4 ns slope;
    - pressing layer: 3.0, 0.6, 1.5 and 2.0 ns.
  - It goes into the noisy and clean copies alike, with fixed seeds. The sweep's noise is noisy less clean, so both copies must carry the same jitter.
  - Checked on rot0: noisy less clean matches the unjittered pair to 0.6 LSB r.m.s., against 11.8 LSB of noise. The residue is the level-dependent noise following the shifted picture.
- **Held-out set.** Its captions, chart and data map now take the same response, and it no longer inherits a response from the bench frame it replaces.
- **Baselines.** The videosynth suite's baselines are re-recorded for the new pin and the softened stills. They also take in the detail rule's known cost on the bars and luma zone plate, which the Phase 10 commit left unrecorded.

**Item 1 grid on the new bench** (four rotations; mean ΔE; constants as λ0, k, tau, d0; current 0.01, 5, 1, 0.003):

| Noise | Soft stills, no jitter: best | ΔE current → best | With jitter: best | ΔE current → best |
|---|---|---|---|---|
| 0 | 0.003, 1, 1, 0.003 | 1.850 → 1.520 | 0.03, 1, 1, 0.003 | 2.913 → 2.692 |
| ½× | 0.01, 1, 1, 0.003 | 2.182 → 1.910 | 0.03, 1, 1, 0.003 | 3.078 → 2.870 |
| 1× | 0.03, 1, 1, 0.003 | 2.621 → 2.415 | 0.03, 1, 1, 0.003 | 3.397 → 3.220 |
| 2× | 0.03, 1, 1, 0.003 | 3.640 → 3.530 | 0.03, 2.5, 1, 0.003 | 4.230 → 4.143 |
| 4× | 0.03, 10, 1, 0.003 | 5.871 → 5.537 | 0.03, 10, 1, 0.003 | 6.253 → 5.868 |

- **The fit's strength falls because of the softened pictures, not the jitter.** The soft bench without jitter already wants k = 1. The jitter adds about 1 ΔE everywhere but moves the best constants only at 2×.
- **The grid is two crosses around the current constants,** so tau and d0 had only been tried at k = 5.

**Reassignment at the new strength** (jittered bench, λ0 = 0.03, k ∈ {1, 2.5}, tau × d0):

- **Tau barely matters at k = 1;** at 0× with d0 = 0.03 it doesn't matter at all.
- **d0 = 0.03 (the discs' value) is best at every level,** but its lead over 0.01 at ½–1× is under 0.003 ΔE.
- **The gain from d0 = 0.003 → 0.03 at k = 1 is mostly on test patterns:**

  | Still | Luma at 0× | Luma at 1× |
  |---|---|---|
  | Bars | −54 % | −18 % |
  | Tartan | −49 % | −16 % |
  | Chroma zone plate | −62 % | −21 % |
  | Edge card | −16 % | −12 % |

- **It costs the photographs and the town map:**

  | Still | Luma at 0× | Luma at 1× |
  |---|---|---|
  | Galtströms photograph | +39 % | +10 % |
  | OSM town | +3 % | +8 % |

**Where the bench and discs now stand:**
- The bench wants k = 1 at realistic noise, with d0 between 0.01 and 0.03 and tau free.
- The discs wanted k = 2.5 with tau 2–3 or d0 0.03.
- **The d0 direction now agrees.** The k gap (1 against 2.5) is still open.

**On the real held runs** (`phase10_tune.py real`, four frames of each run; 66 runs: Community South stack and DS1, Community North DS1/DS6, National A DS2/DS8, National B DS10/DS9; tau 1):

| λ0, k, d0 | Mean ΔE | Luma |
|---|---|---|
| 0.03, 2.5, 0.03 | **2.710** | 2.25 |
| 0.03, 2, 0.03 | 2.711 | 2.25 |
| 0.03, 1.5, 0.03 | 2.715 | 2.25 |
| 0.03, 2.5, 0.01 | 2.720 | 2.29 |
| 0.03, 1, 0.03 | 2.722 | 2.24 |
| 0.03, 1, 0.003 | 2.750 | 2.34 |
| 0.01, 2.5, 0.03 | 2.769 | 2.25 |
| Current (0.01, 5, 0.003) | 2.774 | 2.38 |
| 0.01, 1, 0.03 | 2.850 | 2.24 |
| `transform2d` | 3.224 | 2.35 |

- **λ0 = 0.03 and d0 = 0.03 win on the discs as on the jittered bench.** At λ0 = 0.01 every k is worse than at 0.03, and k = 1 worst of all.
- **k is nearly flat on the discs.** From 1 to 2.5 the mean moves 0.012, and k = 1 wins 28 of 66 runs:
  - k = 1 is better on the dense maps: the stack's town and OS maps by 0.14 and 0.12, DS1's town map by 0.10, Community North DS6's OS map by 0.10.
  - k = 2.5 is better on National A's colour bars by 0.26–0.30, and on National B's panels by up to 0.08. The bars carry the line-alternating leak the per-frame fit does not take out (Phase 9), and they are most of the difference.
- **Still behind `transform2d`** at λ0 0.03, d0 0.03: the stack's town map (+0.08 at k = 1, +0.22 at k = 2.5), the stack's photograph (+0.17, +0.21) and National A's bars. At k = 2.5 the stack's map key too, by 0.009.

**The same k values on the jittered bench** (λ0 0.03, tau 1, d0 0.03; mean ΔE):

| Noise | k = 1 | k = 1.5 | k = 2 | k = 2.5 | Current |
|---|---|---|---|---|---|
| 0 | **2.644** | 2.668 | 2.695 | 2.723 | 2.913 |
| ½× | **2.849** | 2.867 | 2.889 | 2.913 | 3.078 |
| 1× | **3.212** | 3.219 | 3.232 | 3.248 | 3.397 |
| 2× | 4.225 | 4.179 | 4.150 | **4.131** | 4.230 |
| 4× | 7.012 | 6.770 | 6.583 | 6.434 | **6.253** |

- **k = 2 is within 0.001 ΔE of the best on the discs, and within 0.02–0.05 of the best on the bench at 0–1×.**
- **λ0 = 0.03 costs at 4×,** where the current constants still win (k = 2: +5 %). The bench's 1× is the Domesday single-capture model; how often a real capture is as noisy as 4× is not measured here.

**Candidate constants: λ0 0.03, k 2, tau 1, d0 0.03.**

**The held-out set, re-rendered** (`phase10_heldout.py pictures` with the six CC0 photographs fetched again, then `write --jitter`, rendered with videosynth 049c712; four rotations, mean ΔE):

- **It is now as soft as Domesday.** Its luma share at 3.5–5.5 MHz is 0.01–1.5 % (the photographs 0.01–1.0 %), inside the 0.1–2.6 % measured on the Domesday stills.

| λ0, k, d0 | 0 | ½× | 1× | 2× | 4× |
|---|---|---|---|---|---|
| Current (0.01, 5, 0.003) | 1.592 | 1.782 | 2.159 | 3.154 | **5.359** |
| 0.03, 1, 0.03 | 1.516 | 1.739 | 2.148 | 3.319 | 6.387 |
| 0.03, 2, 0.003 | 1.553 | 1.736 | 2.114 | 3.177 | 5.853 |
| 0.03, 2, 0.03 | **1.506** | 1.720 | 2.109 | 3.177 | 5.854 |
| 0.03, 2.5, 0.03 | **1.506** | **1.716** | **2.096** | **3.125** | 5.654 |

- **It does not overturn the candidate.** At 0–1× the candidate takes 2–5 % off the current constants' ΔE. k = 2 and 2.5 are within 0.013 of each other there.
- **The monochrome photograph's cost is much smaller than before the softening.** At 0× its luma error goes 0.437 → 0.625 (+43 %, ΔE +0.07; it was +130 %). At 1× it is +2 % and ΔE improves. The fields photograph costs +14 % luma at 0×, +8 % at 1×.
- **The gain is on the text and graphics.** At 0×, luma: captions −31 %, chart −36 %, data map −38 %.
- **λ0 = 0.03 costs at high noise on all three sets.** On the held-out set the candidate is +0.7 % at 2× and +9 % at 4× against the current constants.

**Decision (the user's): fixed constants λ0 0.03, k 2, tau 1, d0 0.03.** `kLambda0`, `kLambdaDetail` and `kDetailGate` in `domesday2ddecoder.cpp`, and the prototype's `adaptive.DEFAULTS` and `phase10_tune.CURRENT` with them.

- **Why not a k that follows the noise.** The best k does rise with noise on the jittered bench (1 at 0–1×, 2.5 at 2×, 10 at 4×). But every high-noise loss is synthetic, and a noise-following k would rest on the blank-row estimate, which reads low on discs (Phase 9). It can be tried against a real noisy capture if one turns up.
- **Checked on the C++ decoder, not only the prototype.** The 57 single-disc held runs (Community South DS1, Community North DS1/DS6, National A DS2/DS8, National B DS10/DS9) were decoded from their equalised composites by `orc-cli` with the old and the new constants, and scored as `phase10_tune.py real` scores. The new constants are better on 56 of 57 runs: mean ΔE 2.855 → 2.794, against 3.358 for `transform2d`. The worst run moves +0.002; the OS maps gain 0.23.
- **The prototype overstates the edge card.** On the unjittered bench the prototype matches the C++ still by still (Galtströms 1.681 → 1.899 in both; butterfly 0.71 → 0.84; chroma zone plate +29 % against +36 %), except on the green/magenta edge card. The prototype starts that card at 5.85 ΔE where the C++ starts at 3.62, and gives it a −22 % gain where the C++ gives −11 %. The prototype does not model the C++ handling of that card, so prototype bench means that include it overstate the gain.

**What it moved in the suite** (re-recorded: `DecoderStillTruth.PAL_domesday2d_*` and `Domesday2DTruth.PAL_Hanover`, nothing else). The suite's stills are softened but carry no line-timing jitter, so they are the bench the constants were not chosen on:

| Case | Mean ΔE, 12 stills | Biggest moves |
|---|---|---|
| Clean (0×) | 1.646 → 1.704 (+3.5 %) | Chroma zone plate +36 %, butterfly +19 %, Galtströms +13 %, stress +13 %; edge card −11 % |
| Domesday (1×) | 2.435 → 2.415 (−0.8 %) | Chroma zone plate +8 %, edge card −13 % |
| Hanover | Luma 0.130 → 0.166 % | U/V +10–16 %; leak luma −17 % |

- **The unit test frame's worst luma** goes 0.0057 → 0.0103 of white (bars r.m.s. 0.00048 → 0.00075), from `kDetailGate` alone. `DecodesTheTestFrameWithinTolerance` now allows 0.0125; its comment says why. `SameOutputOnEveryPlatform` is re-recorded.

**Left open:** the Community South stack's town map and photograph and National A's bars, where `transform2d` still does better (items 2 and 5); a noise-following k, if a real noisy capture shows the need.

### Phase 10 closed (2026-11-04)

- **Done:**
  - item 1, with the strict acceptance missed as recorded above;
  - item 3, with the cause recorded (per-line timing jitter handed back to luma by the reassignment) and nothing fixed;
  - item 4.
- **Carried forward into Phase 11** under the narrowed scope:
  - item 2 as Phase 11 item 1;
  - item 5 as Phase 11 item 2;
  - item 6 as Phase 11 item 4.

## Review after Phase 10

**The scope has narrowed (2026-11-04, the user's).** `palstill` is for *single-frame stills*: a picture with no motion between its fields, whose neighbouring frames are unrelated. It is expected to do worse on motion and on held pictures. The video sink's schedule (`palstill, held:transform3d` plus hand ranges with `transform3d` for motion and film) sends those frames elsewhere. The user docs say so (5e8a7d62). Two consequences for what is left:

- **Held runs stay the real-disc reference, as a measuring device only.** One frame of a held run, decoded alone, is a single-frame still with a known answer (the run's mean). Scores on them measure single-frame decoding, not a recommendation to decode held pictures with `palstill`.
- **A miss on content that is never a single-frame still is out of scope.** It is recorded, not fixed.

**Where Phase 10 stands:**

| Item | State |
|---|---|
| 1. Constants at every noise level | **Decided.** Fixed constants λ0 0.03, k 2, tau 1, d0 0.03 in `palstilldecoder.cpp`. The strict acceptance (no worse on any still at any level) is not met; the losses are recorded above, and at 4× noise the old constants still win |
| 2. The town map | **Open.** The Community South stack's town map and photograph and National A's bars are still behind `transform2d`. The chroma-noise floor on λ has not been tried |
| 3. Luma | **Cause recorded, nothing fixed.** On the discs the reassignment hands per-line timing jitter back to luma. A higher tau withholds it, but costs the bench |
| 4. Stills it was not tuned on | **Met.** Best on all twelve held-out stills (Phase 8 decoder). The re-rendered, softened set does not overturn the chosen constants |
| 5. The acceptance again | **Partial.** Done: the C++ decoder on the 57 single-disc held runs (better on 56), and the suite re-recorded. Not done with the final decoder: the stack's runs in C++, cross-disc agreement (Phase 7 step 2), and the user's sign-off on new panels |
| 6. The cost | **Not done.** No timing since Phase 8, and the detail rule added a plane, two inverse FFTs a row and a box filter. No design review under AGENTS.md §6 |

**Not yet tested at all:** a PAL still-frame disc other than Domesday.

**What the branch carries besides the decoder** (`git diff --stat origin/main`, 175 files):

| Group | Paths | Ships? |
|---|---|---|
| Decoder | `decoders/palstilldecoder.*`, `orc/support/pal_still.*`, the video sink's `palstill` wiring and `hanover_*` parameters, `decoder_schedule.h` | Yes |
| PAL Still Calibration tool | `orc/core/analysis/pal_still/` | Yes, unless Phase 12 drops it |
| Channel Equalise stage and Channel Calibration tool | `orc/plugins/stages/channel_equalise/`, `orc/core/analysis/channel_equalise/`, `orc/support/channel_equalise.*` | Yes, on this branch (the user's decision) |
| Cache fixes | `orc/support/lru_cache.h`, `picture_band_limit` (Phase 8 item 5) | Yes |
| Tests | `orc-tests/core/unit/...` (CI); `orc-tests/videosynth/...` still truth, Channel Equalise truth, metrics support, `drop_channel_block.py`, baselines, projects migrated to `laserdisc:` blocks | Yes, as TESTING.md describes them. The four-rotation still cases and `rotate_stills.py` do not: the local suite drops to rotation 0 (the user's decision) |
| User docs | `docs/gui-user-guide/...`, `instructions.md`, TESTING.md | Yes |
| Unrelated fixes | `cmake/ClangFormat.cmake` (20870288); the Stacker's n == 2 marks (8f817a0a, merged to main as PR #361) | ClangFormat in its own PR; the Stacker change drops out when main is merged |
| videosynth pin | `flake.nix`, `flake.lock` (049c712) | Yes; the suite needs it |
| **Interim** | `tools/domesday2d-bench/` (40 Python prototypes and measurement scripts and a README), this document | **No** |
| **Interim references in shipped files** | Comments citing this document, its phases, the bench's `.py` files or "the prototype", in `palstilldecoder.*`, `pal_still.*`, `channel_equalise.h`, both calibration headers, `channel_calibration.cpp`, `video_sink_stage.cpp`, the videosynth `CMakeLists.txt`, `variants.json`, `rotate_stills.py` and five test files. Test constants named after the prototype (`kPrototypeFraction`, `kPrototypeTau`) | **No** (Phase 13) |

Phases 11–14 take the branch from here to a PR that holds the decoder, Channel Equalise, their tests as TESTING.md requires, and their docs, and nothing whose only purpose was developing them. The PR comes from this branch: the interim material is removed here, then the branch is squashed (the user's decision), so neither its diff nor its history carries the bench or this document.

## Phase 11 — Close the decoder

**Goal:** finish Phase 10 under the narrowed scope, so the decoder and its constants are final before anything is cleaned up. The bench tools are still used here; they go in Phase 13.

1. **The remaining losses to `transform2d`.**
   - First sort each one by content. National A's bars and any other held test signal are held pictures, which the schedule sends to `transform3d`: record them as out of scope. Check on the disc whether each loser is shown for one frame or held.
   - For the single-frame losers (the town map, the photograph), try the chroma-noise floor on λ that item 2 left untried. Keep it only if no bench, held-out or real still gets worse by more than the noise between runs.
   - **Acceptance:** each loss is either fixed or recorded here as a limit, with its size and its content class.
2. **The acceptance on the final decoder.** Re-run with the C++ decoder (`orc-cli`), not the prototype:
   - the Community South stack's held runs;
   - cross-disc agreement over the thirty stills (Phase 7 step 2);
   - new crop panels of every still whose decode moved since the user last signed off.

   **Acceptance:** Phase 7's targets on every side, apart from the recorded limits; the panels signed off by the user.
3. **A second kind of disc: the Roger Rabbit bonus disc (PAL CAV).** The capture is RF only: `~/raid/sftp/simoninns/LDV4300D_1/PAL/Roger Rabbit/Roger Rabbit Bonus Disc_CAV_PAL_side1_2019-12-21_18-22-37.ldf` (38 GB, LDV4300D). Cut sections of single-frame stills from it (never the whole capture) and decode them with an ld-decode of f7ddee5d or later (Phase 2: it removes the IEC group delay). Run them through the Domesday chain with a single capture, and run Channel Calibration on them: a different mastering may carry a different smear or none. Add a monochrome still and a frame without burst if the disc has them.
   - No truth exists and there is one pressing, so compare `palstill` with `pal2d` and `transform2d` by eye, and on any held runs the disc has, against their run means as in Phase 7 step 1.
   - **Acceptance:** no fault specific to that disc (colour where there should be none, a wrong hue, banding), or the fault is recorded and fixed.
4. **The cost and the design review.**
   - Time `palstill` against `transform2d` as Phase 6 did (`perf stat -e cpu-clock:u`, an idle machine, the same section), and measure peak memory.
   - Take both to the user with the levers Phase 6 left (single precision in the fit, fewer iterations, a decimated detail band) and their measured accuracy cost.
   - **Acceptance:** the time, the memory and the review's decision recorded here. If a lever is taken, items 1–2 are re-run on it.
5. **Final baselines, at rotation 0 only.** First drop the still bench's three rotated cases from the local suite: `rotate_stills.py`, its generation fixtures in `orc-tests/videosynth/CMakeLists.txt`, the rotated tests in `still_truth_test.cpp` and their baselines. Then, once the decoder stops changing, re-record `DecoderStillTruth`, `PalStillTruth` and `SameOutputOnEveryPlatform` in one pass. The bench scripts keep their four-rotation runs for the acceptance in items 1–2.
   - **Acceptance:** the full videosynth suite and the unit tests pass; `baselines.json` holds no rotated case; the new baseline values are explained in the commit message.

## Phase 12 — Settle what ships

**Goal:** decide the product surface. Channel Equalise stays on this branch (the user's decision); only the unrelated ClangFormat fix leaves.

1. **Measure what PAL Still Calibration adds on real discs.** It has only been shown on videosynth's synthetic leak (`PalStillTruth`: V 0.063 → 0.058 % of the leak's mark). No real-disc run has compared the side's measured `hanover_*` against all four set to 0, both with `hanover_per_frame` on. Decode the 66 held runs (Phase 10's set, C++ decoder) both ways.
   - **Acceptance:** recorded here for the two settings: the mean ΔE and luma, overall and per side; the paired per-run differences with their 95 % bootstrap interval; the worst run. Item 2's rule reads these figures.
2. **Decision point: keep or remove PAL Still Calibration.**
   - **What it is.** The line-alternating error is a 2×2 mix of U and V with two parts:
     - a rotation, mastered into each still, which the decoder fits on every frame by itself (`hanover_per_frame`);
     - a symmetric leak and gains, from the player, roughly constant over a side.
     
     The tool measures the whole matrix over 200 frames of a side and writes the four `hanover_*` parameters. The decoder takes only the symmetric part and gains from them. So the tool covers only the smaller part.
   - **Against it:**
     - shown only on videosynth's synthetic leak;
     - a side-wide matrix misses a still's own symmetric leak (National A's bars, 0.043) and a pressing's own rotation (National B);
     - a manual per-side step, with five parameters and a chroma-phase caveat;
     - over 80 minutes unfinished behind Community South's stack;
     - a core tool, its tests and a videosynth variant to maintain, and part of why `pal_still.h` is a public SDK header.
   - **For it:**
     - almost free at decode;
     - optional;
     - the measured leak is not negligible (DS1: symmetric about −0.03 beside a rotation of 0.08), and the per-frame fit cannot take it out.
   - **Rule, from item 1's paired per-run differences (calibrated minus all zero):**

     | Outcome | Decision |
     |---|---|
     | The mean ΔE gain's 95 % interval excludes 0, and the gain is at least 0.02 ΔE overall or 0.05 on any side, and no run is worse by more than 0.05 | **Keep** |
     | The interval includes 0, or the gain is under 0.02 overall and under 0.05 on every side | **Remove** |
     | Anything else (a gain on one side only, or a gain with runs made worse) | **The user decides**, on the table |

     The thresholds match what Phase 10 treated as flat: 0.012 ΔE across k from 1 to 2.5.
   - **Keep** means the tool, the four `hanover_*` parameters and the guide's per-side step all stay. Also decide whether `hanover_per_frame` becomes always on.
   - **Remove** means these go: the tool (`orc/core/analysis/pal_still/`), `pal_still_calibration_test.cpp`, the calibration half of `PalStillTruth`, the leaky-player variant in `variants.json`, and the calibration step in the guide and `instructions.md`. The user chooses whether the four parameters stay as a manual override or go. The per-frame rotation and its fit stay; with the parameters gone, `hanover_per_frame` goes too and the rotation is always fitted.
   - **Acceptance:**
     - item 1's table, the rule's outcome and the decision recorded here;
     - if anything is removed, its code, parameters, tests and docs go with it;
     - `palstill` still decodes with default parameters, and the unit tests and the videosynth suite pass.
3. **The ClangFormat fix in its own PR** (20870288), unless main already has an equivalent.
   - **Acceptance:** the branch no longer differs from main in `cmake/ClangFormat.cmake`.
4. **The public SDK surface.** `orc/support/pal_still.h` is a public SDK header because the core calibration tool and the sink plugin both use it. Confirm that is still needed after item 2, and that `sdk_headers.yaml`, the allowlist and `plugin-sdk.md` are regenerated from the manifest, not edited by hand.
   - **Acceptance:** `SdkHeaderManifestSync` and `SdkHeaderDocsSync` pass; no header ships that no longer needs to be public.
5. **Merge main.** This drops the Stacker change (PR #361) from the branch diff and picks up anything that conflicts.
   - **Acceptance:** `git diff origin/main` shows only the decoder, Channel Equalise, their tests and docs, the cache fixes, the pin, and the interim material that Phase 13 removes.

## Phase 13 — Remove the interim material

**Goal:** nothing in the final tree exists only to develop the decoder. The record stays in this branch's history.

1. **Make shipped comments stand alone.** Rewrite every reference to this document, its phases, the bench's `.py` files or "the prototype" (the list in the review above; re-run `grep -rniE "domesday2d|bench|prototype|phase ?[0-9]|investigation" orc orc-tests`). A comment that needs a measured figure states the figure and what it was measured on, without pointing to a file that will not exist. No planning terms in names or comments (AGENTS.md §1).
   - **Acceptance:** that grep finds no interim reference in `orc/` or `orc-tests/`, apart from unrelated uses such as "chroma phase".
2. **Re-anchor the tests that compare with the prototype.** `ChannelEqualiseTruth`'s `kPrototypeFraction`/`kPrototypeTau` and any unit test whose expected value came from a bench script are restated against what they measure: videosynth's channel model, or the value the C++ produced and why it is right. Name them for what they are.
   - **Acceptance:** the tests pass unchanged in strength; no test needs Python or the bench to explain its expected value.
3. **Check the tests against TESTING.md.** Every shipped test is either a CI unit test (no files, no clock, mocked dependencies) or a videosynth functional test listed in TESTING.md's coverage table. The table describes what the tests check now. Test-only helpers (`drop_channel_block.py`, `still_metrics`, `picture_truth`) carry their own explanation. The rotated still cases are already gone (Phase 11 item 5).
   - **Acceptance:** TESTING.md's rows for `DecoderStillTruth`, `PalStillTruth` and `ChannelEqualiseTruth` match the code; no shipped test exists only to compare prototypes.
4. **Carry over what a maintainer needs.** Before this document goes, make sure the decoder's design (the frame-raster transform, the vertical fit, the reassignment, the per-frame rotation, the colour killer), the meaning of each constant and its known limits are in the code's header comments and in the user guide's "How it works" and limits. Each must be readable without this document.
   - **Acceptance:** the user reads `palstilldecoder.h`, `pal_still.h` and the guide's PAL Still section and agrees nothing essential is missing.
5. **Remove the bench and this document** from this branch: `tools/domesday2d-bench/` and `docs-tech/domesday2d-decoder-investigation.md`, last of all. Before removing them, push a tag (or record the commit) that still holds both, so the record survives the squash, and note it in the project memory.
   - **Acceptance:** neither path exists on the PR branch; no link in `docs/`, `orc/` or `orc-tests/` points at either (`DocsSync` passes).

## Phase 14 — The pull request

**Goal:** a reviewable PR from this branch whose diff and history contain only what ships.

1. **Squash the branch.** After Phase 13, merge main once more, then squash `domesday2d-001` onto main as one commit (`git reset --soft origin/main` and commit; history is rewritten, so the push is forced, with the user's go-ahead). The message describes the decoder, Channel Equalise and the tests, with no planning terms.
   - **Acceptance:** `git log origin/main..` shows the single commit, and no commit on the branch adds a bench file or this document.
2. **Build and test as CI does.** A clean configure with unit and GUI tests; `ctest` including the format, clang-tidy, MVP, DocsSync, StageHelp, SDK manifest and ABI history gates. If any ABI constant moved, update `abi_history.yaml` and regenerate as AGENTS.md's table requires. Then the full local videosynth suite.
   - **Acceptance:** everything passes; any deviation is reported with its output.
3. **Docs check.** `instructions.md` and the user guide agree on every `palstill` and `hanover_*` parameter and on the schedule. The decoder-choice table matches the scope (single-frame stills → `palstill`; held pictures, moving video and film → `transform3d`). `mkdocs build --strict` is clean.
   - **Acceptance:** DocsSync, Instructions and StageHelp pass; the strict docs build has no warnings.
4. **Open the PR** when the user asks: secrets scan first (git-secrets or equivalent); no Co-Authored-By trailer and no "Generated with" footer (AGENTS.md §1.4). The description gives the scope as above and the bench, cost and limit figures a reviewer needs, inline, since this document is not in the PR.
   - **Acceptance:** the PR is open, CI is green on every platform, and `SameOutputOnEveryPlatform` passes on Windows, macOS ARM and Linux.
