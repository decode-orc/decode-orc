# Picture Band Limit

Cuts the band above the picture. A LaserDisc carries no picture above a certain frequency (about 5.4 MHz on PAL), but the decode passes everything up to the sample rate's limit, so the band above the picture holds only disc noise and the ringing that follows sharp edges. This stage low-passes the luma at the frequency where the picture ends and leaves the chroma as it is. Its output is composite (or Y/C), like its input, so it works with every sink.

## When to use

Place it after stacking and Dropout Correction, and before any sink. It lowers the noise that reaches the decoded picture's luma from above the picture band. On a stacked set, run Source Alignment Analysis first: its capture comparison measures where the set's picture ends (`lumaCutoffMHz`) when the set has captures from two or more pressings, and that is the value to give `cutoff_mhz`.

## What it does

On a composite source the luma and chroma are first separated line by line: the chroma is the line demodulated against the subcarrier and kept within 1.3 MHz of it, and the luma is what remains. The luma is low-passed, and the chroma is added back unchanged, so the colour is not touched even where the chroma band reaches above the cutoff (PAL's chroma reaches 5.7 MHz). Any luma within 1.3 MHz of the subcarrier stays with the chroma and passes too. Where the whole roll-off lies above the chroma band (an NTSC or PAL-M cutoff above 4.88 MHz plus half the roll-off), the composite is low-passed as it is, as nothing of the chroma lies there. On a Y/C source the luma plane is low-passed and the chroma plane passes unchanged.

The low-pass is a Kaiser-windowed sinc with 50 dB of stop band: its gain is within 0.3 % of unity below the cutoff less half the roll-off, half at the cutoff, and below 0.3 % above the cutoff plus half the roll-off. Only the active picture (the active lines, over the active samples of each) is filtered. Blanking, sync, the burst and the VBI lines, with their data and test signals, pass unchanged. Dropout hints pass through.

## Parameters

### cutoff_mhz (double)
The frequency at which the low-pass halves the luma: where the picture ends. Range: 2.0–8.0. Default: `5.4` for PAL, where the Domesday captures' picture ends and nothing above it is common to captures of different pressings, and `5.0` for NTSC and PAL-M, which are not yet measured on real discs.

### rolloff_mhz (double)
Width of the low-pass's transition, centred on the cutoff. A narrower roll-off cuts more sharply but rings longer after edges. Range: 0.1–2.0. Default: `0.4`.

## Tools

This stage has no interactive tools.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
