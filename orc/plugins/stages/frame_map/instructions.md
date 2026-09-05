# Frame Map

Reorders, filters, and pads the frame stream from a single source. Without any configuration the stage passes all frames through unchanged. Once a range specification is set, only the listed frames are emitted in the declared order. This is the primary tool for correcting capture offsets, removing bad regions, removing duplicate frames, and filling gaps caused by player skips.

## When to use

Use Frame Map when a capture starts before the desired programme content, contains a damaged or duplicate region, or needs to be trimmed to a specific range. For example: a capture of 5,000 frames where frames 101–4,901 (as shown in the preview) contain the actual disc content — set `ranges` to `101-4901` so downstream stages only see those frames.

Use `remove_duplicates` when a player hesitated and captured the same frame twice in a row. Use `pad_gaps` when a player skipped forward and left a hole in the colour-frame index sequence.

## What it does

Frame Map builds a virtual remapping from output frame indices to source frame IDs using the `ranges` parameter. No video data is copied; the remapping is applied lazily when a downstream stage requests a frame. The output frame count equals the total number of frames described by the range specification.

If `ranges` is empty the stage acts as a passthrough and all source frames are forwarded unchanged.

When `remove_duplicates` is enabled, the stage removes the second of any two consecutive frames that share the same `colour_frame_index` value (indicating the same disc position was captured twice).

When `pad_gaps` is enabled, the stage inserts synthetic padding frames wherever a break in the `colour_frame_index` sequence is detected (indicating the player skipped). Padding frames render as a black-level signal.

Audio follows the mapping. Every audio channel pair remaps in lockstep with the video, per frame; padding frames carry silence. Each output frame always carries the exact per-frame sample count required by its position in the output timeline (constant on PAL; alternating per the SMPTE 272M five-frame sequence on NTSC/PAL-M), so a mapping that breaks the NTSC/PAL-M sequence phase trims or silence-pads the mapped frame's audio by a single trailing stereo pair. Mappings that preserve the phase — and all PAL mappings — are sample-exact. Sample values and rates are never changed.

## Parameters

### ranges (string)
Comma-separated list of frame ranges to include, in output order. Each element is either a single frame number (`42`) or an inclusive range (`1-11`). Frame numbers are entered 1-based in the GUI, matching the preview window. Default: `""` (passthrough). Example: `1-11,21-31` emits frames 1–11 then frames 21–31 (as numbered in the preview), producing 22 output frames total.

Note: the project file (YAML) stores this parameter 0-based (`1-11,21-31` in the GUI is stored as `0-10,20-30`); the conversion is automatic when editing through the parameter dialog.

### remove_duplicates (bool)
When enabled, removes the second of any two consecutive frames with matching `colour_frame_index` values. Use this to eliminate duplicate frames caused by player hesitation or double-capture. Default: `false`.

### pad_gaps (bool)
When enabled, inserts synthetic padding frames wherever a break in the `colour_frame_index` sequence is detected, indicating that the player skipped forward. The inserted frames maintain the correct colour-frame phase for downstream stacking. Default: `false`.

### pad_strategy (string)
Controls how padding frame content is synthesised when `pad_gaps` is enabled. `black` fills each padding frame with a black-level signal. Default: `black`. (The legacy `nearest` value never differed from `black` and is accepted only for backward compatibility with older project files.)

## Tools

### Disc Mapper
Analyses the incoming frame stream for tracking errors — skipped, repeated, or missing frames caused by laserdisc player seek problems — and writes a corrected `ranges` specification back to the stage. Works for both CAV and CLV discs. Run this tool after capture to detect and fix disc tracking issues before stacking. The analysis can be cancelled at any time from the tool dialog. Available for LaserDisc sources only. Invoke from the Stage Tools menu.

**Include Lead-in/Lead-out** (bool, default off). By default the mapping covers the programme content only: every lead-in and lead-out frame is discarded. Enable this option to keep one frame of lead-in at the head of the mapping and one frame of lead-out at the tail, when the capture contains them. Downstream sinks read the disc's lead-in metadata — the user's code in particular — from that frame, so applications such as LaserDisc emulation need it present. The lead-in frame kept is one that carries the user's code on VBI line 16; where several qualify (or none do) the frame closest to the start of the programme is used. The lead-out frame kept is the first one after the programme. The retained frames are attached after gap detection, so they never register as a break in the picture numbering.

Where the player captured the same disc picture more than once, the tool keeps the best copy rather than the first one. Each candidate frame is scored 0–100 from its measured colour-burst level (deviation from the amplitude required by the video standard) and its white-flag SNR and black-level PSNR readings, so a copy hit by an RF dropout or a noisy pass loses to a clean one. Readings are taken per frame; discs without VITS test signals are scored on burst level alone, and frames with no readings at all fall back to a neutral score. The analysis report lists each duplicate group with the score and readings behind the choice.

### Frame Map Range Finder
Locates a specific section of the disc by picture number or CLV timecode and generates the corresponding `ranges` string for this stage. Enter a start and end address (either as a picture number such as `12345` or a CLV timecode such as `0:1:23.4`) and the tool calculates the exact range and writes it to `ranges`. The search uses a binary chop over the disc's VBI picture numbers, so even large captures resolve in a handful of frame decodes; a sequential scan is used only as a fallback when the disc numbering is not monotonic (for example, the player skipped backwards during capture). The search can be cancelled at any time from the tool dialog. Available for LaserDisc sources only. Invoke from the Stage Tools menu.

### Frame Corruption Generator
Injects synthetic corruption patterns (skips, repeats, gaps) into the frame mapping for testing and validation of the Disc Mapper tool. Choose a corruption pattern (`simple-skip`, `simple-repeat`, `skip-with-gap`, `heavy-skip`, `heavy-repeat`, `mixed-light`, `mixed-heavy`) and the tool seeds the frame map with that pattern. A random seed is generated on first use; subsequent runs can reuse the same seed for reproducibility. Use this tool to verify that Disc Mapper correctly identifies and fixes the pattern you inject. Invoke from the Stage Tools menu.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
