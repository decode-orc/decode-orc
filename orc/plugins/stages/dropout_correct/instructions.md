# Dropout Correction

Replaces corrupted sample regions (dropouts) with substitute data drawn from neighbouring lines. The corrector reads the dropout hints attached to each frame by upstream stages (source stages, Stacker or Dropout Map) and overwrites the flagged sample ranges with the nearest suitable line in the same field. After correction, the output frame carries no dropout hints.

## When to use

Use Dropout Correction whenever your TBC source contains dropouts from disc damage, tape degradation, or other signal loss. Place it after the source stage for single-source pipelines, or after Stacker for multi-source pipelines where stacking alone did not recover all dropouts. Enable `highlight_corrections` first to see exactly which regions will be corrected before committing to the final output.

## What it does

Each flagged region is filled by one of two methods, chosen with `method`. With `line_copy`, for each flagged dropout region the stage looks for the nearest line above and the nearest line below, within the same field and within the field's active picture, whose own dropout hints do not overlap the region being repaired. Some lines inside the active picture are never borrowed from because they carry no picture where a full line does: on PAL the half-lines 23 and 623 (blanked over half the line, often all of it on LaserDisc), and on NTSC and PAL-M the closed-caption lines 21 and 284 and the half-line 263. The closer of the two is used; when they are equally close, the one whose samples over the region vary least wins. The flagged samples are then copied from that line. A region that starts in the colour burst and runs on into the picture is split at the end of the burst, and a region is cut off at the end of the active line.

Dropouts on the VBI and blanking lines outside each field's active picture are left as they are, with either method: those lines carry data, and picture copied or interpolated over picture numbers or VITC cannot repair it; it only destroys what a decoder could still read. The closed-caption lines 21 and 284 of NTSC and PAL-M are still corrected, as they carry picture on a disc without captions. On a stack of four Dragon's Lair captures, the Stacker's low-confidence marks on lines 17 and 18 were once filled this way, and the frame lost its picture number.

Composite sources are always repaired from the same field: a line from the other field does not carry the same subcarrier phase and would invert the colour. With `match_chroma_phase` on, only lines whose subcarrier is in phase with the damaged line are candidates — every 4th line of the field for PAL, every 2nd for NTSC and PAL-M — so the replacement may lie further away. A dropout with no usable line in its field is left as it is.

Y/C sources are repaired channel by channel. Luma carries no subcarrier, so it takes the nearest clean line in the same field regardless of phase, falling back to the nearest clean line of the other field. Chroma is taken from the same field only, from a phase-matched line when `match_chroma_phase` is on.

With `interpolate`, a region in the picture is filled from the nearest clean lines above and below it in its field, up to 4 lines away each way, weighted by their distance, so the fill follows a slope. Luma and chroma are separated first, so the lines immediately next to the damaged one can be used: in composite they carry a different subcarrier phase, which rules them out for a copy. Each of those lines is split into luma and baseband chroma; its chroma is taken against the line beyond it, whose chroma is inverted (one line further on NTSC, two on PAL and PAL-M), so that luma the two lines share, such as fine horizontal detail in the chroma band, stays luma, except where their luma differs by more than 8 IRE and the line's own split is used. The luma is interpolated; the chroma is made relative to each line's burst, moved across PAL's V-switch where the lines differ in it, interpolated, and re-modulated at the damaged line's phase, read from its own burst or, where a dropout covers the burst, from the nearest line in phase with it. On a Y/C source the luma plane is interpolated as it is and the chroma plane the same way as composite chroma; a source without a burst is interpolated as it is. A region in the colour burst, or one with no clean line within reach on either side, is copied from a line as with `line_copy`.

How the two compare, as RMS error inside the dropouts (IRE) on videosynth sources with random dropouts and scratches, against the same source without them:

| Picture | `line_copy` luma / chroma | `interpolate` luma / chroma |
|---|---|---|
| PAL Test Card F | 21.5 / 4.6 | 8.8 / 3.5 |
| PAL tartan (lines one row high) | 16.8 / 21.7 | 0.3 / 0.3 |
| NTSC tartan | 28.5 / 14.9 | 10.2 / 6.0 |
| PAL-M tartan | 29.6 / 17.1 | 10.2 / 6.4 |
| NTSC and PAL-M SMPTE bars (constant down the picture) | 0 / 0 | 0.7 / 0.3 |
| PAL moving zone plate | 35.2 / 17.5 | 42.6 / 19.8 |
| NTSC moving zone plate | 28.6 / 16.3 | 25.9 / 12.5 |

On the Domesday Community South captures, with clean samples of one capture flagged at random and filled by each method, `interpolate` left about half the error of `line_copy` across the side (all bands: 17.5 against 9.1 IRE on a map page with text, 14.1 against 10.3 and 10.2 against 4.2 on two other maps, 23.4 against 12.6 on the disc's Test Card F, 10.4 against 5.6 on a photograph, 8.2 against 4.0 on the animated introduction), lower in luma and in chroma on every range tried, and kept roads and lines on the maps whole where `line_copy` pasted a streak from four lines away. The zone plate is the one picture where it does worse on PAL and PAL-M: its rings hold every vertical frequency and happen to repeat at line copy's four-line distance, which no real picture does.

The `overcorrect_extension` parameter widens each dropout region by a fixed number of samples on each side before correction, which helps on heavily damaged sources where the flagged region slightly underestimates the true damage extent. Frames without dropout hints pass through unchanged. The output carries no dropout hints, including for a region that could not be repaired, so place any stage that reads the hints (such as a Stacker) before this one. Correction operates on the full frame buffer regardless of any active-area relabelling upstream.

## Parameters

### method (string)
How a dropout in the picture is filled: `line_copy` copies it from one nearby line, `interpolate` interpolates it from the nearest clean lines above and below with luma and chroma separated (see above). Default: `interpolate`. Projects saved before the parameter existed load with it too, so their output changes; set `line_copy` to repair dropouts as earlier versions did.

### overcorrect_extension (uint32)
Extend each dropout region by this many samples on each side before applying correction. Useful for heavily damaged sources where the detected boundary is slightly too narrow; 24 is typical for damaged sources. Range: 0–48. Default: `0`.

### match_chroma_phase (bool)
When `true`, only replacement lines whose colour subcarrier is in phase with the damaged line are considered (every 4th line of the field for PAL, every 2nd for NTSC and PAL-M). This prevents hue errors in the repaired region. When `false`, the nearest clean line is used whatever its phase. For Y/C sources it applies to chroma only. With `interpolate` it applies only where a region is copied rather than interpolated. Default: `true`.

### highlight_corrections (bool)
When `true`, corrected regions are filled with the source's white level (100 IRE) instead of replacement data. Use this to visualise which regions will be corrected before performing the final export. Regions the stage cannot repair stay as they are. Default: `false`.

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
