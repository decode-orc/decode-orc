# Frame Map

Reorders, filters, and pads the frame stream from a single source. Without any configuration the stage passes all frames through unchanged. Once a range specification is set, only the listed frames are emitted in the declared order. This is the primary tool for correcting capture offsets, removing bad regions, removing duplicate frames, and filling gaps caused by player skips.

## When to use

Use Frame Map when a capture starts before the desired programme content, contains a damaged or duplicate region, or needs to be trimmed to a specific range. For example: a capture of 5,000 frames where frames 101–4,901 (as shown in the preview) contain the actual disc content — set `ranges` to `101-4901` so downstream stages only see those frames.

Use `remove_duplicates` when a player hesitated and captured the same frame twice in a row. Use `pad_gaps` when a player skipped forward and left a hole in the colour-frame index sequence.

## What it does

Frame Map builds a virtual remapping from output frame indices to source frame IDs using the `ranges` parameter. No video data is copied; the remapping is applied lazily when a downstream stage requests a frame. The output frame count equals the total number of frames described by the range specification.

If `ranges` is empty the stage acts as a passthrough and all source frames are forwarded unchanged.

When `remove_duplicates` is enabled, the stage removes the second of any two consecutive frames that share the same `colour_frame_index` value (indicating the same disc position was captured twice).

When `pad_gaps` is enabled, the stage inserts synthetic padding frames wherever a break in the `colour_frame_index` sequence is detected (indicating the player skipped). Padding frames render as a black-level signal and always have the full frame geometry of the source, including the two extra samples PAL carries on lines 312 and 624, so a CVBS export stays frame-aligned after every pad. The same applies to `PAD_N` entries written by the Disc Mapper.

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

**Include Lead-in/Lead-out** (bool, default on). One frame of lead-in is kept at the head of the mapping and one frame of lead-out at the tail, when the capture contains them. Disable this option to cover the programme content only, discarding every lead-in and lead-out frame. Downstream sinks read the disc's lead-in metadata — the user's code in particular — from that frame, so applications such as LaserDisc emulation need it present. The lead-in frame kept is one that carries the user's code on VBI line 16; where several qualify (or none do) the frame closest to the start of the programme is used. The lead-out frame kept is the first one after the programme. The retained frames are attached after gap detection, so they never register as a break in the picture numbering.

Picture numbers are cross-checked before they are trusted. IEC 60856/60857 carries the number twice, on VBI lines 17 and 18 — the whole picture number on a CAV disc (§10.1.3), the hours and minutes of the programme time code on a CLV one (§10.1.6) — and the tool only trusts a reading outright when both lines decoded and agreed. When both decoded and disagreed, one of them is corrupt and neither can be trusted, so the number is discarded and the disc picture padded. On a CAV disc the one exception is a frame whose neighbours are two pictures apart: only the picture between them can sit there, so a copy that names it is kept, and the report counts it under `correctedVBIErrors`. This recovers a picture whose bit error is on the disc itself, where every capture reads it the same way and padding would lose it from the stack.

Where a dropout left only one readable line there is no redundancy behind the number, and one flipped bit inside a BCD digit yields a wrong picture number that still decodes cleanly. On a CAV disc a `4` arriving as a `6` displaces the picture by 20000; on a CLV disc the hours digit sits outside the pattern the decoder matches the line on, so a single flipped bit there moves the picture by a whole hour of running time. Such a number is therefore checked against the picture numbers of the nearest confirmed frames on either side, and discarded if it does not fit; the disc picture is padded instead, so the other sources of a stack can supply it. The report counts the numbers discarded each way separately — those whose two copies disagreed, and those read from one line that did not fit the sequence.

Every field of the code is also held to the range the standard gives it, so a reading that no disc could legally carry is rejected before it reaches the timeline: for CAV, the picture number the format allows (79999 under IEC 60857, 99999 under IEC 60856); for CLV, hours 0–9 and minutes 0–59 from §10.1.6, seconds 0–59 from §10.1.10, and a picture within the second below the format's frame rate. That last one is tighter than the literal text of §10.1.10, which writes the field as 0–29 in the PAL standard as well as the NTSC one: a 25 Hz disc never reaches picture 25, so a frame that reports one has misread the line.

A capture frame whose fields come from two different pictures holds halves of two pictures: the player skipped in mid-frame. Filed under either picture it would put a foreign field into every stack built on this source, so it is dropped and the pictures it straddled are padded instead. The report counts these frames as `splitFieldFrames`. Such a frame is recognised when its two fields carry different picture numbers or, on discs that number field 1 only, when field 2's colour-sequence phase does not follow field 1's. The phase is compared within the 4-field cycle, because the 8-field PAL reading is not reliable on real captures, so a skip of an even number of frames leaves the phase consistent and is not seen here; the Stacker's field check catches it when the source is stacked.

NTSC and PAL-M discs carry film with 3:2 pulldown and number it by film frame, so one frame in every five carries no picture number: the disc never gave it one. Such a frame is kept in its place, not dropped as unmappable, because dropping it would take a frame in five out of the film, and the sound with it. On CAV it is recognised by its neighbours, which carry consecutive picture numbers; a frame whose number was merely unreadable sits between numbers two apart, so it is dropped and its picture padded. On CLV the running time counts video frames and skips the frame, so the frame must also have read cleanly, with a code on its VBI lines and no number. At the ends of a capture, or beside the lead-in or lead-out, it is kept when other pulldown frames lie nearby. Where a capture lacks a pulldown frame that the film's cadence places (lost to a dropout, or in a gap), its place is padded, so every capture of the disc holds the same frames in the same places for stacking. The report counts `pulldownFrames` and `pulldownFramesPadded`.

A frame whose VBI never yielded a usable picture number is dropped rather than placed by guesswork, and the disc picture it would have filled is padded. The output is a picture-number-indexed timeline, so a frame put at a guessed index would misalign that source against every other source stacked with it; a padding frame is the honest answer, and the other sources supply the picture. The report counts these separately from duplicates and lead-in/out frames, and the output frame count it quotes is the mapping actually generated.

As a backstop, the capture itself decides how wide a gap may be. Between two mapped frames the disc advances one picture per captured frame, so the frames the capture holds between them — the ones dropped for want of a readable picture number — account for the missing pictures one for one, and a gap is allowed that many plus 100 for the player mistracking forward over pictures the capture never recorded. A gap beyond that is reported rather than padded: a picture number that landed in the wrong place opens a wide gap the capture crossed in a handful of frames, often in a negative number of them, because the frame's true position lies further back in the capture. The exception is a capture that plays on from the gap: when the 10 frames after it carry consecutive picture numbers, the player really did skip that far (a mistracking player can jump over a hundred pictures in a few frames), and the gap is padded, so that the rest of the capture stays in step with the others of a stack. A misplaced number is one frame on its own, with no such run behind it. The frames themselves are still mapped — the tool does not discard captured content on the strength of one suspect number — but the timeline is not continuous across the gap, and the report says so.

That test cuts both ways. A long run of frames whose VBI was unreadable is a long run of pictures genuinely missing from the mapping, and padding them is what keeps this source lined up with the others it will be stacked with, however many there are.

Where the player captured the same disc picture more than once, the tool keeps the best copy rather than the first one. Each candidate frame is scored 0–100 from its measured colour-burst level (deviation from the amplitude required by the video standard), its white-flag SNR and black-level PSNR readings, and the share of its samples flagged as dropout, so a copy hit by RF dropouts or a noisy pass loses to a clean one. The dropout share scores 100 for an unflagged frame and falls to 0 at 1 % of the frame flagged; a source that carries no dropout hints scores every frame the same on it. Readings are taken per frame; discs without VITS test signals are scored on burst level and dropouts alone. The score only separates copies whose picture numbers are equally sure: a copy whose number both VBI lines confirmed is kept over a single-line read however its frame measured. The analysis report lists each duplicate group with the score and readings behind the choice.

### Frame Map Range Finder
Locates a specific section of the disc by picture number or CLV timecode and generates the corresponding `ranges` string for this stage. Enter a start and end address (either as a picture number such as `12345` or a CLV timecode such as `0:1:23.4`) and the tool calculates the exact range and writes it to `ranges`. The search uses a binary chop over the disc's VBI picture numbers, so even large captures resolve in a handful of frame decodes; a sequential scan is used only as a fallback when the disc numbering is not monotonic (for example, the player skipped backwards during capture). The search can be cancelled at any time from the tool dialog. Available for LaserDisc sources only. Invoke from the Stage Tools menu.

### Frame Corruption Generator
Injects synthetic corruption patterns (skips, repeats, gaps) into the frame mapping for testing and validation of the Disc Mapper tool. Choose a corruption pattern (`simple-skip`, `simple-repeat`, `skip-with-gap`, `heavy-skip`, `heavy-repeat`, `mixed-light`, `mixed-heavy`) and the tool seeds the frame map with that pattern. A random seed is generated on first use; subsequent runs can reuse the same seed for reproducibility. Use this tool to verify that Disc Mapper correctly identifies and fixes the pattern you inject. Invoke from the Stage Tools menu.

### From the command line
The Disc Mapper (`field_mapping`) and the Frame Map Range Finder (`frame_map_range`) also run from `orc-cli`. Without `--apply` the command reports the `ranges` value it found and changes nothing; with `--apply` it writes the value to this stage, keeping the stage's other settings, and saves the project. Tool options are passed as `--param key=value` (`include_lead_in_out` for the Disc Mapper, `startAddress` and `endAddress` for the Range Finder). A capture the Disc Mapper cannot map ends the command with exit code 6.

```
orc-cli tools run disc.orcprj field_mapping --node "Disc 1 map" --apply
orc-cli tools run disc.orcprj frame_map_range --node "Disc 1 map" --param startAddress=1000 --param endAddress=2000 --apply
```

### Refreshing from the command line
Each tool's dialog has a **Use this tool when refreshing from the CLI** option. Tick it and press **Apply to Stage** or **OK** to record in the project that `orc-cli tools refresh` runs that tool for this stage, with the parameters set in the dialog; save the project to keep it. A stage has one refresh tool, so ticking the option in another tool's dialog replaces it, and unticking it stops refreshing the stage. `orc-cli tools refresh-config <project> <this stage> field_mapping` makes the same choice from a script. A refresh maps every Frame Map before running the Source Align that reads them, and saves the project only if every tool succeeded.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
