# Channel Equalise

Removes the edge smear that PAL LaserDisc CAV masters carry, and restores the chroma level the smear took. The smear is a faint trail after every sharp edge: about a quarter of the step, decaying over a third of a microsecond. It softens fine detail and lowers the colour subcarrier by about 2 dB. Its output is composite, like its input.

## When to use

Use it on PAL LaserDisc CAV sources whose test signals show the smear, such as the Domesday Community and National A sides. Run the **Channel Calibration** tool on the node first: it measures the smear on the test signals of field 1 line 19, and reports when a source has none (National B, which is CLV, and the other discs measured so far). Leave the stage out of projects whose source has no smear.

Place it after the Stacker and Dropout Correction, beside Picture Band Limit (either order). Its filter spreads each sample over the next few, so in front of Dropout Correction it would spread a dropout's spike beyond the flagged samples. It helps every PAL decoder in the Video Sink.

## What it does

The smear is a one-pole tail: of a step of height 1, 1 − *c* arrives at once and *c* follows with time constant *τ*. The stage runs the exact inverse filter over each frame, sync and blanking included, in the order the lines were recorded. The inverse is stable for any fraction below 1; at 0.24 and 6.1 samples its own tail lasts about 5 samples.

The inverse lifts the subcarrier back by what the smear took, and the decode's own corrections leave the chroma level differing from frame to frame anyway. The stage therefore measures each frame's colour burst after the inverse (the median over the picture lines of both fields) and scales the subcarrier band of every line by the gain that brings the burst to the nominal level, with the Stacker's `chroma_gain_match` shelf (0 at DC, 1 at the subcarrier). Saturation then follows the burst, as the standard intends. A frame whose burst would need a gain beyond 3 either way has no burst to go by, and its chroma level is left as it is: a stacked frame whose two inputs carry one field in antiphase cancels that field's burst, and scaling it to the nominal would multiply the subcarrier band about twelvefold, wrecking the picture and the VBI data with it.

With `smear_fraction` 0 every frame passes **bit-identical**, chroma gain included. Only PAL composite sources are processed; any other source passes through unchanged with a warning. Dropout hints pass through.

## Parameters

### smear_fraction (double)
The share of each step that the smear moves into its tail. Set it with the Channel Calibration tool. Range: 0–0.45. Default: `0`, which passes every frame through unchanged.

### smear_tau_samples (double)
The tail's time constant, in 4fsc samples (one is 56 ns). Set it with the Channel Calibration tool. Range: 2–50. Default: `6.1`, the Domesday CAV sides'.

## Tools

### Channel Calibration
Averages field 1 line 19 over 48 frames at the inner, middle and outer radius (or over the whole input, when it is short), each frame aligned on its white bar's rising edge, and fits the smear to the long tails of the white bar and the 2T pulse. Only samples 0.4 µs or more from an edge are fitted, so the test signal generator's own edge shaping does not count. A fit is a smear only where it explains the tails clearly better than no smear and is clear of the bounds where the fit degenerates. The source's smear is the median of the accepted fits. **Apply to Stage** sets `smear_fraction` and `smear_tau_samples`, or `smear_fraction` 0 when the source has no smear. A smear whose time constant is beyond the stage's 50 samples is reported with its fit and not applied: the stage could not take it. A source without the CCIR 17 bar and pulse on line 19 cannot be measured, and the stage's values are left as they are.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
