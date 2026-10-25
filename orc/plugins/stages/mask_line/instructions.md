# Mask Line

Blanks (overwrites with a fixed sample level) specified lines in the CVBS frame buffer. The stage does not affect any other lines or the audio/EFM streams. It is typically placed at the end of a processing pipeline to clean up lines that should not appear in the final output.

## When to use

Use Mask Line when specific lines contain data that must be removed before encoding or export. For example, to black out VBI lines before producing a final video file, or to suppress a closed-caption line that interferes with a downstream analysis tool. Add Mask Line after all other processing stages and configure it with the Mask Line Config tool.

## What it does

The stage wraps the incoming frame representation and intercepts all frame and line access calls (`get_line`, `get_frame`, `get_frame_copy`, and the YC variants `get_line_luma`, `get_line_chroma`, `get_frame_luma`, `get_frame_chroma`). For each requested line it checks whether the frame-flat line index falls within any of the specified ranges. If it does, the returned buffer is filled with the configured mask sample level using the standard-defined samples-per-line for the video system. All other lines are forwarded from the source unchanged. YC dual-channel sources are handled independently: both the luma and chroma buffers for a masked line are filled with the mask value.

## Parameters

### lineSpec (string)
Comma-separated list of frame-flat line numbers or inclusive ranges to mask. Format: `LINE` or `START-END`. Line numbers are entered 1-based in the GUI parameter dialog, equal to the full-frame broadcast line number; the project file (YAML) stores the value 0-based and the conversion is automatic. Default: `""` (no masking, pass-through).

Examples (as entered in the GUI parameter dialog):
- `21` — mask broadcast line 21 (field 1 in PAL/NTSC; stored as `20` in YAML)
- `334` — mask broadcast line 334 (field 2 in PAL; stored as `333` in YAML)
- `6-22` — mask broadcast lines 6 through 22 (stored as `5-21` in YAML)
- `6-22,319-335` — mask VBI area in both fields of a PAL frame (stored as `5-21,318-334` in YAML)

Use the **Mask Line Config** tool to generate this value from broadcast line numbers without writing the spec manually.

### maskSampleLevel (integer)
10-bit sample level (0–1023) written to all masked lines. Default: `0` (sync tip).

Typical values:
| Value | Meaning |
|-------|---------|
| 0 | Sync tip |
| 240 | Blanking (NTSC/PAL-M; picture black is 282, 7.5 IRE above it) |
| 256 | Blanking/black (PAL) |
| 800 | White (NTSC/PAL-M) |
| 844 | White (PAL) |

## Tools

### Mask Line Config
Opens a configuration dialog that lets you specify lines to mask using **full-frame broadcast line numbering** and converts them automatically to the frame-flat `lineSpec` format, masking each range in **both** field 1 and field 2 of the CVBS frame.

Enter broadcast line numbers (1-based, full-frame):
- **PAL**: valid range 1–625; field 1 = lines 1–313, field 2 = lines 314–625
- **NTSC/PAL-M**: valid range 1–525; field 1 = lines 1–263, field 2 = lines 264–525

The dialog maps each input range to the corresponding frame-flat positions in both fields:
- A range in field 1 (e.g. PAL 1–313) also masks the equivalent field 2 lines
- A range in field 2 (e.g. PAL 314–625) also masks the equivalent field 1 lines

Example — PAL, entering range 6–22 (field 1) produces `lineSpec` = `6-22,319-335` (stored as `5-21,318-334` in YAML), blanking the VBI region in both fields.
Example — PAL, entering range 10–20 produces `lineSpec` = `10-20,323-333` (stored as `9-19,322-332` in YAML).

Add each range with **Start Line**, **End Line** and **Add Range**; **Remove Selected** and **Clear All** edit the list. The dialog also sets `maskSampleLevel` from a **Level Preset**: **Blanking** or **White** take the levels of the stage's input (PAL 256 and 844 by default), and **Custom** takes any 10-bit value from 0 to 1023.

Select this tool from the stage context menu to open the dialog.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. `lineSpec` is non-empty. |
| Yellow | Pass-through mode. `lineSpec` is empty; no lines are masked. |

Parameters can be set via **Edit Parameters…** in the node context menu, or through the **Mask Line Config** stage tool.
