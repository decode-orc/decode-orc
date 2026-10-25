# Dropout Correction

Replaces corrupted sample regions (dropouts) with substitute data drawn from neighbouring lines. The corrector reads the dropout hints attached to each frame by upstream stages (source stages, Stacker or Dropout Map) and overwrites the flagged sample ranges with the nearest suitable line in the same field. After correction, the output frame carries no dropout hints.

## When to use

Use Dropout Correction whenever your TBC source contains dropouts from disc damage, tape degradation, or other signal loss. Place it after the source stage for single-source pipelines, or after Stacker for multi-source pipelines where stacking alone did not recover all dropouts. Enable `highlight_corrections` first to see exactly which regions will be corrected before committing to the final output.

## What it does

For each flagged dropout region the stage looks for the nearest line above and the nearest line below, within the same field and within the field's active picture, whose own dropout hints do not overlap the region being repaired. Some lines inside the active picture are never borrowed from because they carry no picture where a full line does: on PAL the half-lines 23 and 623 (blanked over half the line, often all of it on LaserDisc), and on NTSC and PAL-M the closed-caption lines 21 and 284 and the half-line 263. The closer of the two is used; when they are equally close, the one whose samples over the region vary least wins. The flagged samples are then copied from that line. A region that starts in the colour burst and runs on into the picture is split at the end of the burst, and a region is cut off at the end of the active line.

Composite sources are always repaired from the same field: a line from the other field does not carry the same subcarrier phase and would invert the colour. With `match_chroma_phase` on, only lines whose subcarrier is in phase with the damaged line are candidates — every 4th line of the field for PAL, every 2nd for NTSC and PAL-M — so the replacement may lie further away. A dropout with no usable line in its field is left as it is.

Y/C sources are repaired channel by channel. Luma carries no subcarrier, so it takes the nearest clean line in the same field regardless of phase, falling back to the nearest clean line of the other field. Chroma is taken from the same field only, from a phase-matched line when `match_chroma_phase` is on.

The `overcorrect_extension` parameter widens each dropout region by a fixed number of samples on each side before correction, which helps on heavily damaged sources where the flagged region slightly underestimates the true damage extent. Frames without dropout hints pass through unchanged. The output carries no dropout hints, including for a region that could not be repaired, so place any stage that reads the hints (such as a Stacker) before this one. Correction operates on the full frame buffer regardless of any active-area relabelling upstream.

## Parameters

### overcorrect_extension (uint32)
Extend each dropout region by this many samples on each side before applying correction. Useful for heavily damaged sources where the detected boundary is slightly too narrow; 24 is typical for damaged sources. Range: 0–48. Default: `0`.

### match_chroma_phase (bool)
When `true`, only replacement lines whose colour subcarrier is in phase with the damaged line are considered (every 4th line of the field for PAL, every 2nd for NTSC and PAL-M). This prevents hue errors in the repaired region. When `false`, the nearest clean line is used whatever its phase. For Y/C sources it applies to chroma only. Default: `true`.

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
