# Channel Equalise

Removes the edge smear that PAL LaserDisc CAV masters carry, and restores the chroma level the smear took. The smear is a faint trail after every sharp edge: about a quarter of the step, decaying over a third of a microsecond. It softens fine detail and lowers the colour subcarrier by about 2 dB. Its output is composite, like its input.

## When to use

Use it on PAL LaserDisc CAV sources whose test signals show the smear, such as the Domesday Community and National A sides. Run the **Channel Calibration** tool on the node first: it measures the smear on the test signals of field 1 line 19, and reports when a source has none (National B, which is CLV, and the other discs measured so far). Leave the stage out of projects whose source has no smear.

The **Channel Tracking** tool is an optional refinement for sources where it is worth the run. It measures the smear on line 19 of every field and on the picture's edges and follows it along the source, so each window of frames is corrected with a smear certified never worse than Channel Calibration's. It reads the whole source (about 9.4 minutes for a Domesday side; on a stacked input, a full pass of the stack), and the gain is small: on the Domesday sides measured, on frames held out from the tool, the bar's tail 3–8 samples after an edge is at most 0.03 % of the step smaller than with Channel Calibration's smear on average over a side, and up to 0.4 % in the windows that take their own correction. Its track is tied to the frame numbers of the stage's input, so it is stale once anything upstream changes the frames (a different stack, a cut or a frame range): run it again then.

Place it after the Stacker and Dropout Correction, beside Picture Band Limit (either order). Its filter spreads each sample over the next few, so in front of Dropout Correction it would spread a dropout's spike beyond the flagged samples. It helps every PAL decoder in the Video Sink.

## What it does

The smear is a one-pole tail: of a step of height 1, 1 − *c* arrives at once and *c* follows with time constant *τ*. The stage runs the exact inverse filter over each frame, sync and blanking included, in the order the lines were recorded. The inverse is stable for any fraction below 1; at 0.24 and 6.1 samples its own tail lasts about 5 samples.

The inverse lifts the subcarrier back by what the smear took, and the decode's own corrections leave the chroma level differing from frame to frame anyway. The stage therefore measures each frame's colour burst after the inverse (the median over the picture lines of both fields, with the window moved on each line to follow 0H, which on the PAL 4fsc lattice falls up to 2 samples later at the bottom of a field than at the top) and scales the subcarrier band of every line by the gain that brings the burst to the nominal level, with the Stacker's `chroma_gain_match` shelf (0 at DC, 1 at the subcarrier). Saturation then follows the burst, as the standard intends. A frame whose burst would need a gain beyond 3 either way has no burst to go by, and its chroma level is left as it is: a stacked frame whose two inputs carry one field in antiphase cancels that field's burst, and scaling it to the nominal would multiply the subcarrier band about twelvefold, wrecking the picture and the VBI data with it.

### The smear along the source

The smear is not one value per source. Measured on line 19 of every field of whole Community North and British Garden Birds sides, one field's white bar reads the smear's tail (3–8 samples after the edge) to about 0.5 % of the bar, and that tail drifts by about 1 % from the inner to the outer radius, steps by 0.6 % where a capture's field-1 response changes, and differs between captures of one disc (0.8 % on one capture of British Garden Birds side 2, none on another). Averaged over the fields within 128 frames of a frame, the reading is good to under 0.05 % (its Allan deviation is smallest there; longer windows start to follow the drift).

The one-pole smear does not describe the tail near the edge either: fitted to the long tails, as Channel Calibration fits it, it over-corrects the first 3–8 samples after an edge by 0.4–1.1 % of the bar on every Domesday side measured. The tracked smear therefore has a second, fast component, of signed fraction *f* and time constant *τ*f (0.5–4.5 samples): of a step of height 1, *c* e^−t/τ + *f* e^−t/τf is missing *t* samples after it. On the Domesday sides *f* is usually negative: the tail starts more slowly than one exponential. Its inverse is a second-order filter; a smear whose inverse would not be stable (poles beyond 0.98) is never fitted and never applied.

With `smear_track` set (by Channel Tracking), each frame takes its own smear from the track: the straight line between the knots either side of it, `smear_track` for the first field of each frame and `smear_track_field2` for the second (the two fields' smears differ by up to 0.5 % of the bar on some captures).

### Never worse than Channel Calibration

A correction can only be judged on what it does, and what it does to the picture can differ from what it does to the VITS: a disc's picture can carry a tail of its own (a telecine's, on the Domesday test card about 1.5 % of the step 3–8 samples after an edge) that the VITS, inserted after it, does not. Channel Calibration's one-pole smear over-corrects the VITS and so removes part of that picture tail; a correction fitted to the VITS alone leaves it. Channel Tracking therefore chooses each window's correction by measuring it, and keeps Channel Calibration's smear wherever nothing is shown to be at least as good.

A window spans 128 frames either side of its knot (knots every 256 frames). Its frames are split three ways by frame number: one third fits the candidates, one third certifies them, and one third is left unused, so the certification is made on frames the fit never saw. The measures are, per field parity, the bar's tails 3–8 and 9–30 samples after its rising and falling edges and the noise on its flat parts, and the tails 3–8 and 9–20 samples after the picture's own sharp, isolated edges (found on a two-line comb's luma in every 32nd frame within 1024 of the knot, more often on a source under 8,192 frames and every frame under 512; soft and natural edges are not used). The candidates are the two-component fit to each parity's VITS, and a constrained search for the two-component smear per parity that lowers the measures while keeping each at or below Channel Calibration's and each tail at or below the uncorrected picture's. A candidate is chosen only when, over 200 paired bootstrap resamples of the certifying third's fields and edges, the 97.5th percentile of its excess over Channel Calibration's correction is at most 0.1 % of the step on every tail (a fifth of an 8-bit code) and 1 % on the noise, and of its excess over no correction at most 0.1 % on every tail. Otherwise the window keeps Channel Calibration's smear; where Channel Calibration finds none, it stays uncorrected.

No correction can lower the noise below that of the uncorrected picture: the inverse of a smear lifts the band the smear lowered, noise included. The chosen correction never adds more noise than Channel Calibration's.

The track is then certified the same way, on each window's certifying third, as the stage applies it: with knots merged where the smear drifts slowly (within 0.002 in each fraction), rounded as the parameter holds them, and interpolated frame by frame, so the frames between two knots are judged on the blend they receive. A window that fails keeps the knots around it unmerged; if it still fails, it takes Channel Calibration's smear (the report counts these windows as reverted).

With `smear_fraction` 0 and no `smear_track`, or where the track has no smear, every frame passes **bit-identical**, chroma gain included. Only PAL composite sources are processed; any other source passes through unchanged with a warning. Dropout hints pass through.

## Parameters

### smear_fraction (double)
The share of each step that the smear moves into its tail. Set it with the Channel Calibration tool. Range: 0–0.45. Default: `0`, which passes every frame through unchanged.

### smear_tau_samples (double)
The tail's time constant, in 4fsc samples (one is 56 ns). Set it with the Channel Calibration tool. Range: 2–50. Default: `6.1`, the Domesday CAV sides'.

### smear_track (string)
The smear measured along the source: knots `frame:fraction:tau`, or `frame:fraction:tau:fast_fraction:fast_tau` with a fast component (fast fraction −0.45 to 0.3, its time constant 0.5–4.5 samples), separated by `;`, in frame order, each within the ranges above and invertible. Set it with the Channel Tracking tool. When set, each frame takes the smear on the straight line between the knots either side of it (the first or last knot beyond them), and `smear_fraction` and `smear_tau_samples` are not used. Default: empty, so every frame takes `smear_fraction` and `smear_tau_samples`.

### smear_track_field2 (string)
The second field's smear along the source, in the same form. Set it with the Channel Tracking tool. Empty: the second field takes `smear_track`.

## Tools

### Channel Calibration
Averages field 1 line 19 over 48 frames at the inner, middle and outer radius (or over the whole input, when it is short), each frame aligned on its white bar's rising edge, and fits the smear to the long tails of the white bar and the 2T pulse. Only samples 0.4 µs or more from an edge are fitted, so the test signal generator's own edge shaping does not count. A fit is a smear only where it explains the tails clearly better than no smear and is clear of the bounds where the fit degenerates. The source's smear is the median of the accepted fits. **Apply to Stage** sets `smear_fraction` and `smear_tau_samples`, or `smear_fraction` 0 when the source has no smear. A smear whose time constant is beyond the stage's 50 samples is reported with its fit and not applied: the stage could not take it. A source without the CCIR 17 bar and pulse on line 19 cannot be measured, and the stage's values are left as they are.

### Channel Tracking
An optional refinement of Channel Calibration. Measures line 19 of both fields of every frame and the picture edges of both fields of every 32nd frame (more often on a short source, every frame under 512), runs Channel Calibration on the input, and chooses each window's correction as described under *Never worse than Channel Calibration*. **Apply to Stage** sets `smear_track` and `smear_track_field2`; when no window has a correction it clears both and sets `smear_fraction` 0. The report lists the runs of windows by what they chose (`constrained`, `tracked` or `upstream`). A source without the CCIR 17 bar and pulse on line 19 cannot be measured, and the stage's values are left as they are. It reads every frame once, so it takes at least as long as reading the source (about 9.4 minutes for a Domesday side). The track is tied to the frame numbers of the stage's input: if anything upstream changes the frames (a different stack, a cut, a frame range), run it again. The `near_edge` option (off by default) also offers the tracked fit where only a tail confined near the edge is seen; it is applied only where certified as above.

## Video systems

PAL only. Its shelf is measured against the colour burst, and SECAM has none, so a SECAM project does not offer the stage; nor do NTSC and PAL-M projects.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
