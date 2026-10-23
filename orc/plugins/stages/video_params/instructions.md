# Video Parameters

Overrides any or all of the video parameter hints that flow through the pipeline alongside field data. Parameters set to `-1` are inherited from the source unchanged, making partial overrides straightforward. Downstream decoding and export stages read these hints to determine frame geometry, IRE levels, sample ranges, and active-area boundaries.

## When to use

Use Video Parameters when your source metadata contains incorrect or non-standard values. Example: a capture made with hardware that used a different black or white level than the standard — set **Black Level** and **White Level** here to correct the interpretation before downstream chroma decoding. Alternatively, use the active-area parameters to re-frame the visible picture when the source metadata describes a non-standard active window.

## What it does

The stage wraps the incoming frame representation and overrides the value returned by `get_video_parameters()`. For each parameter set to a value other than `-1`, the corresponding field in the `SourceParameters` structure is replaced; parameters left at `-1` are copied from the source's existing parameters. No pixel data is altered.

Overriding black or white level sets `has_nonstandard_values` on the output parameters when the resulting level differs from the standard for the video system — setting a level to its spec value does not flag the source as non-standard.

The active-area parameters only relabel the active window; they never set `active_area_cropping_applied`, because the sample buffer is forwarded uncropped and downstream stages must keep indexing it as a full frame. The preview shows the whole frame with the area outside the active window dimmed, so the un-dimmed region is exactly what an export will contain.

## Parameters

All parameters are `int32`. A value of `-1` means "inherit from source".

Defaults are the standard values for the project's video system, so a standard source can be edited from its own geometry rather than from a blank form. The ranges below come from the video system too: a sample offset cannot name a sample the line does not have, and a line number cannot name a line the frame does not have. `Active Video End` and `Last Active Frame Line` are exclusive, so each may sit one past the last sample or line.

### activeVideoStart — Active Video Start
First active video sample within the line (0-based). Default: PAL 157, NTSC/PAL-M 126. Range: `-1` to samples-per-line − 1 (PAL 1134, NTSC 909, PAL-M 908).

### activeVideoEnd — Active Video End
One past the last active video sample within the line. Default: PAL 1105, NTSC/PAL-M 894. Range: `-1` to samples-per-line (PAL 1135, NTSC 910, PAL-M 909).

### firstActiveFrameLine — First Active Frame Line
First active line of the frame (0-based, frame-flat). Default: PAL 44, NTSC/PAL-M 40. Range: `-1` to lines-per-frame − 1 (PAL 624, NTSC/PAL-M 524).

### lastActiveFrameLine — Last Active Frame Line
One past the last active line of the frame (0-based, frame-flat). Default: PAL 620, NTSC/PAL-M 523. Range: `-1` to lines-per-frame (PAL 625, NTSC/PAL-M 525).

### whiteLevel — White Level (10-bit)
White level (100 IRE) in the CVBS_U10_4FSC domain. Default: PAL 844, NTSC/PAL-M 800. Range: `-1` to 1023.

### blackLevel — Black Level (10-bit)
Black level in the CVBS_U10_4FSC domain. Default: PAL 256 (no setup pedestal; black = blanking), NTSC/PAL-M 282 (7.5 IRE above blanking). Range: `-1` to 1023.

### Values that must agree

Three pairs are rejected when they contradict each other, because the resulting parameters would describe no picture:

| Must hold | Why |
|-----------|-----|
| `activeVideoStart` < `activeVideoEnd` | The active window would be empty or inverted |
| `firstActiveFrameLine` < `lastActiveFrameLine` | The active picture would contain no lines |
| `blackLevel` < `whiteLevel` | No contrast range is left to map |

A pair with either end left at `-1` is not checked: the inherited end is whatever the source reports, which this stage cannot know until it runs. In the GUI the offending pair is named before the values are applied; on the command line the stage refuses the parameters.

### Resetting to the source's values

**Reset to Metadata Values** in the parameter dialog fills every parameter with the value arriving at this stage's input: what the capture reports, after any stage upstream of it. When the input cannot be read (no capture configured yet), the button reads **Reset to Defaults** and fills in the format's standard values instead.

From the command line, `orc-cli nodes reset <project> <node>` does the same and saves the project, so a template's Video Parameters can follow each capture it is pointed at. It refuses (exit code `5`) rather than fall back to the standard values when the input cannot be read.

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
