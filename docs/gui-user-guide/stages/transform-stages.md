# Transform stages

Transform stages sit **between source stages and sink stages** in a decode-orc pipeline. They consume one or more upstream stage outputs and produce one or more outputs for downstream stages.

Transform stages are used to:

* Reorder or align captured frames from one or more sources.
* Combine multiple captures into a single improved output.
* Override or edit per-frame metadata and hints used by later processing.
* Apply signal modifications such as dropout correction, band limiting or masking.
* Import external audio, decode EFM digital audio to a channel pair, re-route SMPTE 272M channels, and correct audio/video sync.

Audio in a decode-orc pipeline travels as **audio channel pairs** — up to eight per representation, each a synchronous (frame-locked) 48 kHz 24-bit stereo stream per SMPTE 272M. The audio transform stages below (Audio Import, EFM Audio Decode, Audio Channel Map, Audio Align) add, route, and time-align these channel pairs; sinks then select, embed, or export them.

> Terminology note (decode-orc DAGs): stages connect via **connections** between stage inputs and outputs.

---

## Frame Map

| | |
|-|-|
| **Stage id** | `frame_map` |
| **Stage name** | Frame Map |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Reorder frames by specifying explicit ranges, remove duplicates, and pad gaps |

**Use this stage when:**

* You need to reorder or stitch together ranges of frames from a single capture.
* You want to skip a bad region by omitting it from the range list.
* You need to remove duplicate frames detected by matching colour-frame index values.
* You need to pad sequence gaps introduced by player skips.

**What it does**

Frame Map parses a comma-separated list of frame ranges (e.g. `1-11,21-31,12-20`) and remaps output frame IDs to the specified input frames. No video data is copied; the remapping is applied when a downstream stage asks for a frame, and the output frame count is the total number of frames the ranges describe. It can optionally remove consecutive duplicate frames and insert synthetic padding frames to fill detected gaps.

Padding frames — from `pad_gaps` or from the `PAD_N` entries the Disc Mapper writes — render as a black-level signal with the full frame geometry of the source, including the two extra samples PAL carries on lines 312 and 624, so a CVBS export stays frame-aligned after every pad.

Audio follows the mapping: every audio channel pair is remapped with the video, frame by frame, and padding frames carry silence. Each output frame carries the sample count its position in the output requires (constant on PAL; the SMPTE 272M five-frame sequence on NTSC/PAL-M), so a mapping that breaks the NTSC/PAL-M sequence trims or silence-pads the mapped frame's audio by one stereo pair. PAL mappings, and NTSC/PAL-M mappings that keep the sequence, are sample-exact.

**Parameters**

* `ranges` (string)
    - Comma-separated list of frame ranges to include, in output order. Each element is a single frame (`42`) or an inclusive range (`1-11`), entered 1-based in the GUI (matching the preview window). For example `1-11,21-31` emits 22 frames.
    - The project file (YAML) stores the value 0-based; the conversion is automatic when editing through the parameter dialog.
    - Default: `""` (empty) meaning passthrough.

* `remove_duplicates` (bool)
    - When enabled, removes the second of any two consecutive frames with matching `colour_frame_index` values.
    - Default: `false`.

* `pad_gaps` (bool)
    - When enabled, inserts synthetic padding frames when a break in the `colour_frame_index` sequence is detected (indicating a player skip). The inserted frames keep the colour-frame phase that downstream stacking needs.
    - Default: `false`.

* `pad_strategy` (string)
    - Strategy for synthesising padding frame content.
    - Allowed values: `black` (padding frames render as a black-level signal).
    - Default: `black`.
    - The legacy `nearest` value never behaved differently from `black`; it is still accepted for backward compatibility with older project files and is treated as `black`.

**Stage tools**

* **Disc Mapper** — analyses the incoming frame stream for tracking errors (skipped, repeated, or missing frames caused by LaserDisc player seek problems) and writes a corrected `ranges` specification back to the stage. Works for both CAV and CLV discs; run it after capture and before stacking. On NTSC and PAL-M film with 3:2 pulldown it keeps the frames the disc never numbered (one in five) in their places, and pads one where a capture lost it, so every capture of the disc keeps the same frames; the report counts them as `pulldownFrames` and `pulldownFramesPadded`. See [Film with 3:2 pulldown](../../technical/disc-mapping.md#93-film-with-32-pulldown). Its report's statistics also say what the disc says of itself over the whole side: user code, side, CX, sound mode, Amendment 2, the first and last picture mapped, whether lead-in and lead-out were seen, and how well the status words agree. These are the values the Video Sink's embedded disc metadata holds; see [Disc Identity](../../technical/disc-mapping.md#17-disc-identity). The analysis can be cancelled from the tool dialog. LaserDisc sources only.
    - **Include Lead-in/Lead-out** (default on) keeps one lead-in frame at the head of the mapping and one lead-out frame at the tail, when the capture contains them, so that downstream sinks can read the disc's lead-in metadata, such as the user's code (LaserDisc emulation needs it). The lead-in frame kept is one carrying the user's code on VBI line 16, closest to the programme; the lead-out frame is the first after the programme. Untick it to map the programme content only.
    - **Picture numbers are checked before they are trusted.** IEC 60856/60857 carries the number on both VBI lines 17 and 18 (the picture number on CAV, the hours and minutes of the time code on CLV). A number whose two copies disagree is discarded; one read from a single line is kept only if it fits the confirmed numbers either side, since one flipped bit can move a CAV picture by 20000 or a CLV picture by an hour. Every field must also lie in the range the standard allows. The report counts the numbers discarded each way.
    - **Frames that cannot be placed are padded, not guessed.** A frame whose VBI gave no usable picture number, or whose two fields come from different pictures (the player skipped mid-frame, counted as `splitFieldFrames`), is dropped and the disc picture it would have filled is padded, so the other sources of a stack can supply it.
    - **Gaps are checked against the capture.** A gap wider than the dropped frames between two mapped frames plus 100 is reported rather than padded, as it usually means a picture number landed in the wrong place; the frames are still mapped, and the report says the timeline is not continuous there.
    - **Repeated pictures keep the best copy.** Where a disc picture was captured more than once, each copy is scored 0–100 on colour-burst level, white-flag SNR, black-level PSNR and the share of the frame flagged as dropout (100 for no dropout, 0 at 1 % of the frame). A copy whose number both VBI lines confirmed always beats a single-line read. The analysis report lists each duplicate group with the readings behind the choice.
* **Frame Map Range Finder** — locates a section of the disc by picture number or CLV timecode: enter a **Start Address** and **End Address** (e.g. `12345` or `0:1:23.4`) and the tool writes the corresponding `ranges` string to the stage. Uses a binary-chop search over the disc's VBI picture numbers, so even large captures resolve in a handful of frame decodes, falling back to a sequential scan when the numbering is not monotonic (for example where the player skipped backwards). Cancellable. LaserDisc sources only.
* **Frame Corruption Generator** — seeds the frame mapping with a synthetic corruption pattern, to check that the Disc Mapper finds and fixes it. **Corruption Pattern** is one of `simple-skip` (default), `simple-repeat`, `skip-with-gap`, `heavy-skip`, `heavy-repeat`, `mixed-light` or `mixed-heavy`. A random seed is generated on first use; after that a **Regenerate Seed** option appears, and leaving it unticked reuses the seed so the pattern is reproducible.
* Supports the standard GUI preview.

**From a script**

The Disc Mapper (`field_mapping`) and the Frame Map Range Finder (`frame_map_range`) run from the command line too. Without `--apply` the command only reports the `ranges` value it found; with it, the value is written to the stage and the project is saved. The stage's other settings are kept. Tool options are passed as `--param key=value`: `include_lead_in_out` for the Disc Mapper, `startAddress` and `endAddress` for the Range Finder.

```bash
orc-cli tools run disc.orcprj field_mapping --node "Disc 1 map" --apply
orc-cli tools run disc.orcprj field_mapping --node "Disc 1 map" --param include_lead_in_out=true --apply
orc-cli tools run disc.orcprj frame_map_range --node "Disc 1 map" --param startAddress=1000 --param endAddress=2000 --apply
```

A capture the Disc Mapper cannot map (no readable picture numbers, for example) ends the command with exit code 6, so a script does not stack it by mistake. See [Running stage tools](../../cli-user-guide/overview.md#running-stage-tools) in the CLI guide.

**Refreshing from the CLI**

The Disc Mapper, Frame Map Range Finder and Frame Corruption Generator dialogs have a **Use this tool when refreshing from the CLI** option. Tick it and press **Apply to Stage** or **OK** to record, in the project, that `orc-cli tools refresh` runs this tool for this Frame Map with the parameters set in the dialog. Each stage has one refresh tool, so ticking it in one tool's dialog replaces another tool's; untick it to stop refreshing the stage. Save the project to keep the choice. The same choice can be made with `orc-cli tools refresh-config <project> "Disc 1 map" field_mapping`. A refresh maps every Frame Map before running the Source Align that reads them, and saves the project only if every tool succeeded. See [Refreshing a project](../../cli-user-guide/overview.md#refreshing-a-project).

---

## Source Join

| | |
|-|-|
| **Stage id** | `source_join` |
| **Stage name** | Source Join |
| **Connections** | 1–16 inputs → 1 output (fan-out supported) |
| **Purpose** | Join several sources end to end into a single output sequence, in a specified order |

**Use this stage when:**

* The material you want as one output is spread across several decodes of the same physical medium — most commonly a tape recorded at more than one speed (SP/LP/EP), where each region has to be decoded separately so the correct filter profile is applied.
* A tape carries recordings made on different machines that need different decode parameters.
* You want a single export step instead of exporting each decode and cutting the results together in a video editor.

**What it does**

Source Join concatenates the connected inputs into one continuous sequence and renumbers the output frames from zero, so the result behaves like a single long capture. No sample data is copied — the stage builds a lookup table from output frame to (input, input frame) — so joining is cheap however long the inputs are.

The order is given as a list of **source node IDs**: the numbers the graph editor draws in the corner of each node. Input order cannot be read off the connections themselves (a multi-input stage has one input port, and the artifacts arrive in whatever order the connections were made), so the node ID is the handle you have on each source. Decode-Orc tells the stage which nodes are connected to it, which is also how the stage notices when the graph has changed under a stored order.

Cutting each source to the frames it contributes is done upstream with a **Frame Map** stage per source; Source Join then only has to put the pieces in order.

Everything a frame carries travels with it: dropout hints (renumbered to the output frame), audio channel pairs, EFM and AC3 RF data all come from the input that supplied the frame. The joined output takes its channel-pair layout from the first joined input; an input carrying fewer channel pairs contributes silence for the ones it lacks. For NTSC and PAL-M the audio window of an output frame is trimmed or silence-padded by at most one stereo pair where the join moves a frame to a different position in the five-frame audio sequence (SMPTE 272M-1994 §14.3); PAL is constant-cadence and always sample-exact.

All joined inputs must share one signal geometry — same video system, nominal frame width and frame height — and must all be composite or all be Y/C. The stage fails with an explanatory error rather than producing an output whose geometry changes part way through. With a single input it is a passthrough.

**Example** — a tape whose first half is SP and second half is LP:

* Node 12: SP decode → Frame Map (`1-4500`)
* Node 14: LP decode → Frame Map (`4501-9000`)
* Source Join with `input_order` = `12,14` → one 9000-frame output to export

**Parameters**

* `input_order` (string)
    - Comma-separated source node IDs giving the join order, e.g. `16,2,4`. These are the IDs of the stages connected to this stage's input; the parameter dialog lists them, with their names, above the field.
    - Each ID must be a whole number and may appear only once. An ID that is not connected is ignored; a connected source that is not named contributes no frames (recorded as a `source_join.inputs_skipped` observation). Both are logged, and the node's status dot turns yellow.
    - If the list names no connected node at all, the stage fails rather than guessing.
    - Default: `""` (empty) meaning the sources are joined in the order their connections were made.

Source Join has a second parameter, `input_node_ids`, which is filled in by Decode-Orc from the node's incoming connections. It is host-owned: it does not appear in the parameter dialog or in the project file, and there is nothing to set.

**Status indicator**

Yellow means either that `input_order` is empty (the sources will be joined in connection order) or that the order no longer names exactly the sources connected to the stage — which is what you will see after adding, removing or rewiring an input. Re-open the parameters and update the order to clear it.

**Stage tools**

* This stage has no interactive tools.
* Supports the standard GUI preview of the joined output.

---

## Source Align

| | |
|-|-|
| **Stage id** | `source_align` |
| **Stage name** | Source Align |
| **Connections** | 1–16 inputs → N outputs (one per input) |
| **Purpose** | Align multiple sources so the same output frame index refers to the same underlying disc/tape position in each source |

**Use this stage when:**

* You have multiple captures of the same material that start at different positions.
* You are preparing multiple sources for stacking (typically before `stacker`).
* You used `frame_map` (or otherwise produced streams where output frame 0 does not mean the same real-world frame across sources).

**What it does**

Source Align applies a per-input frame offset from its `alignmentMap` parameter so that output frame N refers to the same disc picture in every aligned output. It does not read the VBI itself: the map is worked out by the **Source Alignment Analysis** tool from the discs' VBI picture numbers (CAV) or CLV timecodes, or entered by hand. Each input named in the map produces one output; an input the map does not name is left out of the outputs.

* In `pad_for_alignment` mode the offset is the number of padding frames prepended to the input, and every output is then padded at the tail to the length of the longest, so all outputs have the same frame count and a Stacker downstream keeps the pictures that only the longer captures carry. Padding frames have the source's full frame geometry (including PAL's two extra samples on lines 312 and 624) and are marked as padding, so the Stacker ignores them.
* In `first_common_frame` mode the offset is the number of leading frames dropped from the input, and nothing is padded.

Audio follows the alignment: every audio channel pair is remapped per frame, and padding frames carry silence. Each output frame carries the sample count its position requires, so an NTSC/PAL-M offset that is not a multiple of 5 trims or silence-pads each shifted frame's audio by one stereo pair; PAL offsets, and NTSC/PAL-M multiples of 5, are sample-exact.

With `alignmentMap` empty the stage is not configured (red status dot): a single input passes through unchanged, but with two or more inputs the stage fails and asks for the map to be generated.

**Parameters**

* `alignmentMode` (string)
    - How the offsets in `alignmentMap` are applied: `pad_for_alignment` prepends padding frames and pads every output at the tail to the longest; `first_common_frame` drops leading frames and leaves the tails as they are.
    - Allowed values: `pad_for_alignment`, `first_common_frame`.
    - Default: `pad_for_alignment`.
    - Set by the Source Alignment Analysis tool along with the map.

* `alignmentMap` (string)
    - Per-input frame offsets, one `input_id+offset` entry per input, comma-separated, e.g. `1+2, 2+2, 3+1, 4+1`. `input_id` is the 1-based input number; `offset` is a number of frames, used as `alignmentMode` describes.
    - Default: `""` (empty: not configured).
    - Use the Source Alignment Analysis tool to generate this value.

**Stage tools**

* **Source Alignment Analysis** — analyses all connected sources and writes the offset map to `alignmentMap` and the mode to `alignmentMode`. Cancellable. LaserDisc sources only.
    - `pad_for_alignment` pads the sources so they all start from the earliest picture any of them holds (no frames are lost). Each source is placed in frames, by the frames at which it and the others read the same pictures (the median offset over the pictures they share, at least three), so a source that opens on a lead-in frame, an unreadable frame or one corrupt early number still lines up. That matters on film with 3:2 pulldown, where a source starting eight pictures late starts ten frames late. A source sharing no pictures with any other in the frames scanned is placed by its picture numbers alone, a picture per frame. A source with no picture numbers at all cannot be placed; it is left out of the map, and so out of the outputs, and the report names it.
    - `first_common_frame` trims every source to the first picture present in all of them (may discard unique leading content).
    - The report adds a **Capture comparison**, measured on up to 16 frames of the same picture across the stretch every source covers, with the same measurement as the Stacker's `Weighted` mode. The frames are spread as the Stacker spreads the frames it groups its inputs from, so for the same sources in the same order the two find the same pressings and families. Its noise figures are averages over those frames; the Stacker measures again for every 500 frames, and its figures vary with the picture around the comparison's. It gives each source's noise in 10-bit codes, measured on flat areas of the picture after shifting the source onto the first source's sample grid and skipping flagged samples, so that a source's gain or level difference is not counted as noise. It gives the **pressings** the sources group into (for example A {1, 2, 3}, B {4, 5}), which needs 4 or more sources, and how much of each disc's noise its pressing accounts for. Where pressings share a mother, a **Families** line names them and the share of their discs' noise that is the family's. It lists the luma **levels** of any source more than 1 % in gain or 2 codes in level from the common picture; players and decodes differ like this. With 3 or more sources it gives each source's **response at 5–6 MHz** against the typical source's, measured between sources of different families where there are enough of them to settle it, because discs of one pressing or family share their noise in that band. It warns of any source below 0.75: its capture chain loses fine detail, and stacking it with sharper sources blurs the result slightly. If all the sources come from one pressing, the report says that stacking them cannot remove the stamper's noise. A source alone in its group is either the only disc of its pressing or much noisier than the rest; the two look the same. If no grouping into pressings explains the sources' differences, the report says so. Each noise figure is then the source's spread against the others, no response is given, and the Stacker stacks such sources equally. Last comes **what each source adds**: the noise σ predicted for a `Weighted` stack of every source, then, most first, the σ the stack would have without each source and its share of the weight. Read this, not each source's own noise, when choosing which discs to leave out. The discs that share the noise recorded into one stamper form a *stamper group* (the groups the Pressings line letters). A disc alone in its stamper group reads noisier than the rest yet often adds the most, because no other disc carries its noise. Stamper groups are groups of shared noise, not versions of the content, so `Weighted` needs no one pressing chosen for the video; which pressing to take still matters for EFM data. When the noise model is not trusted, the figures describe the equal stack the Stacker then makes. A **Suggested reference** line names the source to put first among a Stacker's inputs, since the first input sets the sample grid the stack is made on: the source covering the most output frames; of those, the ones within 10 padding frames of the fewest count as equal, and of them the one that adds the most to the stack (the highest stack σ without it, where 0.01 codes or less counts as equal) is taken; then the one whose sample grid is nearest the median. The line also names the **next choice** with the same figures, so a close call shows as one. It gives each source's **edge tails** on the VITS white bar (PAL line 19, NTSC and PAL-M line 20), averaged over 32 frames: the share of the rising and the falling edge that settles slowly after the fast part, and its time constant. A tail the same on every capture comes from the decoder; one that differs between players or decodes belongs to the capture. With a trusted grouping into two or more stamper families it gives the **picture band**: where the coherence between captures of different families, which share only the picture, falls to its floor, for luma (separated by a vertical comb) and for chroma above the subcarrier. Above the luma cutoff the captures carry only noise. Where every pressing comes from one mother, that mother's noise is common above the picture too and no cutoff is found. With `orc-cli tools run --json` the same figures come as data; see [Reading the capture comparison from a script](../../cli-user-guide/multi-disc-stacking.md#reading-the-capture-comparison-from-a-script).
    - The report then checks each source's **picture numbers against its content**. A capture whose numbers are off by one holds the neighbouring picture at every output frame, and a stack blends it in with its chroma in another phase. In 24 windows across the source covering the most frames, the check takes frames whose picture differs from both neighbours' and finds which of each other source's frames, up to two either side, holds the same picture, on a luma map that chroma averages out of. A change of offset is narrowed to the frame where the content allows it, and each source is judged against what most sources read, so a reference that is itself off is the one reported. A source that differs gets a warning with the output frames and the offset, or says its frames hold none of the others' pictures; one off by the same offset everywhere gets the alignment map that would correct it. The comparison and the check only report; neither changes the map.
* Supports the standard GUI preview, with one preview option per aligned output.

**One capture format per stack**

Take all the sources feeding one Source Align from `.tbc` captures (TBC Source) or all from `.cvbs` captures (the CVBS sources). A stack that mixes the two is not supported, and `orc-cli tools run` refuses one.

**From a script**

Map each disc first (see Frame Map above): the analysis reads the discs through their Frame Maps. With `--apply` the command writes `alignmentMap` and `alignmentMode` to the stage and saves the project; without it, it only reports them. Choose the mode with `--param alignmentMode=pad_for_alignment` (the default) or `first_common_frame`.

```bash
orc-cli tools run stack.orcprj source_alignment --node Align --apply
orc-cli tools run stack.orcprj source_alignment --node Align --param alignmentMode=first_common_frame --apply
```

The command refuses a stack that mixes `.tbc` and `.cvbs` sources (exit code 5) and names the sources of each kind. See [Running stage tools](../../cli-user-guide/overview.md#running-stage-tools) in the CLI guide.

**Refreshing from the CLI**

The Source Alignment Analysis dialog has a **Use this tool when refreshing from the CLI** option. Tick it and press **Apply to Stage** or **OK** to record that `orc-cli tools refresh` runs the analysis for this stage, with the alignment mode set in the dialog; save the project to keep it. A refresh maps every disc before aligning them, so the alignment always uses the new frame lists. The same choice can be made with `orc-cli tools refresh-config <project> Align source_alignment`. See [Refreshing a project](../../cli-user-guide/overview.md#refreshing-a-project).

---

## Stacker

| | |
|-|-|
| **Stage id** | `stacker` |
| **Stage name** | Stacker |
| **Connections** | 1–16 inputs → 1 output (fan-out supported) |
| **Purpose** | Combine multiple captures into one improved output by stacking corresponding frames |

**Use this stage when:**

* You have multiple captures of the same LaserDisc (or other source) and want to reduce dropouts / noise by combining them.
* You want an in-pipeline equivalent to legacy "disc stacker" workflows.
* A disc holds still pictures over many frames (slides, maps, menus on CAV discs): with `still_frame_stacking` the repeats within each capture are stacked too, which reduces noise and fills dropouts even from a single capture.

To stack new batches of captures from a script, with one template project and one command per batch, see [Stacking multiple discs](../../cli-user-guide/multi-disc-stacking.md) in the CLI guide.

**What it does**

Stacker expects its inputs to already be aligned (typically using `frame_map` and `source_align`). For each output frame it takes the frame with the same frame id from every input and combines them pixel by pixel; it does not search for a matching frame itself. Differences in where each capture starts a line are corrected by `sample_align`. The output covers every frame id that any input carries, so pictures at the end of the longer captures are kept; an output frame is padding only when every input is padding there. Padding frames (`is_padding_frame = true`) contribute no samples. AC3 RF is not stacked: each frame's symbols come from the source with the fewest dropouts there that carries AC3 RF.

Before combining, each input's two fields are compared with the per-sample median of all the inputs on low-passed luma. An input whose field correlates with the others at less than 0.8 carries a different picture there (a mid-frame player skip, or a remaining mapping error) and is left out of that field, while its other field is still used. With only two inputs there is no majority to say which is wrong, so both are kept and the whole field is marked as dropout for a later stage to repair. Fields with too little detail to judge, such as black frames, are not checked. Each exclusion is logged.

Every mode is an averaging or median policy over the usable values for each pixel; no mode picks a whole "best" source or frame. A value a source flags as dropout is not usable; where at least one source has a clean value, only the clean values are combined. Where every source is flagged, differential dropout detection (composite sources, 3 or more inputs) keeps the flagged values within 10 % of their median, as ld-disc-stacker does, and combines those; failing that the pixel is black, or with `passthrough` the combination of the flagged values. Either way the output pixel stays marked as dropout (severity 50).

If only **1 input** is provided, the stage acts as a passthrough, unless `still_frame_stacking` is on.

**Parameters**

* `mode` (string)
    - Stacking algorithm, applied per pixel to the usable (non-dropout) values across all sources.
    - Allowed values: `Weighted`, `Mean`, `Median`.
    - Default: `Weighted`.

| Mode | Behaviour per pixel |
|------|--------------------|
| `Weighted` | Mean of the values within `reject_sigma` times the noise scale of the median, each weighted by its source's measured noise and pressing, rounded to the nearest code. With 3 or more values, those further from the median are rejected; with 2, both are always used. Marks samples the sources do not agree on as dropout. |
| `Mean` | Arithmetic mean of all usable source values. |
| `Median` | Median of the usable source values (mean of the middle two for an even count). |

`Mean` and `Median` divide with truncation, which biases their output about half a code low; they keep doing so, so saved projects reproduce exactly. `Weighted` rounds, with ties to even.

**How `Weighted` measures the sources.** In each 500-frame segment, the first time a frame in it is stacked, the stage measures the variance of every pair of sources' difference, away from dropouts. Once the sample grids are aligned the sources carry the same picture, so that difference is noise where the picture is flat. Captures differ by a few per cent in gain, which leaves picture detail in the difference elsewhere, so only flat blocks of the picture are measured, each about its own mean. Discs of one pressing share the noise recorded into its stamper, so the stage groups the sources by pressing (once, from 16 frames spread across the stack; in the GUI the groups are kept with the project's observations, so later previews and sessions read them back rather than measuring again until the sources or parameters change), solves each source's own noise and each pressing's shared noise, and weights the sources to give the least noise in the result: each pressing counts about once however many of its discs are stacked, and within a pressing the cleaner discs count more. Stampers are made from mothers, and pressings of one mother share its noise too, so the stage also groups pressings into families where that explains the pair variances clearly better, and counts each family about once. Where the pair variances allow two groupings, it takes the one that leaves the discs' total noise most alike, as every disc went through the same generations. Pressings can only be told apart with 4 or more sources; with fewer, sources are weighted by their noise alone, and with two each is given half. A segment that cannot be measured holds the nearest segment that could. For Y/C sources the noise is measured on luma and the same weights are used for chroma. In each field, the still-frame copies of one frame are weighted as those copies combined, since they share no stamper noise with the frame they join. The stage then checks that its model of pressings and families explains the pair variances it was fitted to. If any pair misfits by more than 35 %, the pressings are wrong and the weights cannot be trusted, so that segment is stacked with equal weights and the log warns. The log records the pressings and any families once, and each source's noise and share of the weight whenever its noise changes by more than a quarter, with the noise the model predicts for the stack, as a `weights` report record (see the notes below).

The noise scale is 1.2 times the median noise (σ) of the sources, so at the default `reject_sigma` of 5 a value is rejected when it lies more than 6 median σ from the median: about 28 codes on a typical clean set, about 60 on a noisy one. `Weighted` marks an output sample as dropout with severity 25 when the sources do not agree on it — when no majority of the usable values lies within that distance of the result, or when exactly two usable values differ by more than it — so Dropout Correction can repair it. With the other modes such samples pass downstream unmarked.

* `reject_sigma` (double)
    - `Weighted` only: how far from the per-pixel median, in multiples of the noise scale, a value may lie and still be used. The same distance decides which samples are marked as low-confidence.
    - Range: 2–10.
    - Default: 5.

* `group_pressings` (bool)
    - `Weighted` only: find which sources share a pressing and count each pressing about once.
    - Default: `true`.
    - Turn off for sources known to be one pressing, for example when Source Align's Source Alignment Analysis puts them all in one group but the Stacker's log reports more than one; every source is then weighted by its own noise alone. Leave it on when the sources may come from more than one pressing: with it off, one pressing cannot explain the noise between pressings, so the model check fails and the sources are stacked equally.

**Choosing a video mode**

Use `Weighted`. It measures each source's noise, works out which discs share a pressing, and weights the sources to give the least noise in the result. On captures of four LaserDisc sides, stacking all but one disc and measuring against the disc held out, it gained more than any other mode: 5.0 dB over a single disc with five discs of National B, against 4.8 dB for `Mean`, 1.7 dB for `Median` and 1.05 dB for the old `Auto`. `Mean` treats every disc as equally good, so one noisy or soft disc pulls the stack down; `Median` throws away most of the averaging. `Mean` and `Median` are kept so that projects built with them reproduce exactly.

* With **2 sources** no mode can outvote a bad value, so `Weighted` and `Mean` give almost the same picture; `Weighted` still weights the cleaner disc more and marks the samples the two disagree on as dropout, so prefer it.
* A disc that is **alone in its pressing** looks noisy to `Weighted` and gets little weight. That is usually right, but if the disc is known to be good, `Mean` may do better. Run Source Align's **Source Alignment Analysis** first to see the pressing groups and any soft captures; after stacking, the log lists each source's noise and weight.

The full comparison table is in the stage's help (**Help...** in the node context menu).

`Smart Mean` and `Auto` are deprecated and no longer offered. `Smart Mean` averaged the values within a fixed threshold of the median (`smart_threshold`, 15 codes by default), and `Auto` was `Smart Mean` whenever 3 or more values were usable; on real captures that threshold rejects most of the good values. A project whose Stacker used either, or stored no mode, is switched to `Weighted` when it is loaded, and its `smart_threshold` is dropped: the log warns that the stage's output will differ from earlier runs, and the project is marked as changed. An old `Smart Mean` or `Auto` result cannot be reproduced exactly. `Smart Neighbor` and `Neighbor` were never implemented; a project that stored one is loaded as `Median`, which is what they always did.

* `no_diff_dod` (bool)
    - Disable differential dropout detection: a pixel every source flags as dropout is never recovered from the flagged values; it is black, or with `passthrough` the combination of the flagged values.
    - Default: `false`.
    - When `false`, the stage may recover pixels every source marks as dropout by comparing values across sources (composite sources, 3+ sources).

* `passthrough` (bool)
    - Applies where every source flags a pixel as dropout and differential dropout detection recovers nothing: when `true`, the pixel is the combination of the sources' flagged values, still marked as dropout; when `false` it is black.
    - Default: `false`.

* `sample_align` (bool)
    - Measure each source's 4FSC sample grid against the first input and shift it into line before stacking.
    - Default: `true`.
    - Independent decodes of one disc do not always start a line on the same sample (real PAL captures have measured up to 12 samples apart), and one sample is 90° of subcarrier, so combining a source a sample out erodes chroma and leaves the stack desaturated.
    - The offset is found by correlating active picture against the first input over ±32 samples, on low-passed luma and then refined on the raw samples. It drifts across a side, so it is measured per 500-frame segment; a segment that cannot be measured (correlation under 0.5, too little detail) holds the offset of the nearest measured segment up to 4 segments away, and otherwise the source is stacked unshifted there. The log records each change of offset.
    - The first input sets the grid the stack is made on. Where it has no frame (past the end of a short capture, or across a stretch it is missing), the others are measured against a stand-in, the first input that has frames there and was measured against the first input in some other segment, and the stand-in's offset from the first input in the nearest such segment is added, so the output stays on the first input's grid. The log reports the stand-in once per run of segments. Where no input links to the first input, those frames are stacked without grid correction and the log warns. Source Align's capture comparison names a suggested reference to put first.
    - Set to `false` to combine sources on their own grids, as earlier versions did.

* `chroma_gain_match` (bool)
    - Measure each source's colour burst on every frame and bring its chroma to the sources' median burst before stacking.
    - Default: `true`.
    - A decode can lift one capture's chroma, and its burst with it, by up to a fifth over stretches of a side while its luma still matches the other captures'. Unmatched, `Mean` averages that chroma in and `Weighted` measures it as noise and gives the source almost no weight there.
    - The burst is measured on clean, dropout-free lines of both fields. A source more than 5 % from the median has its subcarrier band scaled with a (1 − cos ωT)² shape: unity at the subcarrier, nothing at DC, and luma below about 3 MHz left as it was. For Y/C sources the chroma plane is scaled. Frames without a measurable burst, and gains beyond 1.5 either way, are left alone.
    - Set to `false` to combine the sources' chroma as it is, as earlier versions did.

* `luma_level_match` (bool)
    - Measure each source's luma gain and level against the picture common to every source on every frame and bring it to the typical (median) source's before stacking.
    - Default: `true`. Saved projects load with it on, so their output changes.
    - Captures through different players and decodes differ by a few per cent in contrast and a few codes in level (one of eleven Community South captures was 7 % and 11–18 codes up). Unmatched, the stack's contrast follows whichever sources carry the most weight, and on bright areas the per-sample rejection reads the difference as disagreement and drops good values.
    - The fit is made on low-passed luma away from dropouts, and the whole frame is mapped. A source within 0.2 % and half a code of the median is left exactly as it was, as are frames with too little picture (a black frame), gains beyond 15 % and levels beyond 40 codes. It runs before `chroma_gain_match`. Still-frame copies take their source's correction; for Y/C sources the luma plane is matched.
    - Set to `false` to combine the sources' luma as it is, as earlier versions did.

* `still_frame_stacking` (bool)
    - Also stack the repeats of a held still picture within each source.
    - Default: `false`.
    - Use it for discs that hold still pictures over many frames, such as slides, maps and menus on CAV discs. Each repeat was recorded at a different place on the disc, so its noise and dropouts are independent, including the stamper noise that stacking discs of one pressing cannot remove. It works with a single input too. On the map stills of one disc, repeats took about 6.8 dB of noise out, against about 1.3 dB from stacking five captures without them.
    - A frame is taken as a copy only when it is at the colour cadence (a multiple of 4 frames away in PAL and PAL-M, 2 in NTSC) within `still_frame_search`; shows the same picture field by field, both as block means (after fitting each row of blocks for the slow level drift between repeats) and sample by sample, so cursors, captions, fades, cuts and moving drawings fail; has the same measured subcarrier phase; and is not a bit-identical repeat of the same read. A field takes copies from a source only when it has at least two.
    - Copies are used on active picture lines only: VBI lines (picture numbers, VITC, teletext, closed captions — in NTSC also lines 21 and 284), the frame descriptor, audio and EFM come from the frame itself. The descriptor does say how far the frame's picture is held either side (the furthest copy before and after that matched in both fields), which the Video Sink's `held:` decoder schedule entry reads to decode held pictures with a 3D decoder. Each copy joins the stack with its source's luma level correction; in `Weighted` the copies of one frame are weighted, field by field, as those copies combined. The same rejection and dropout rules apply. At DEBUG the log lists the copies each frame used.
    - Limits: on limited animation a drawing held for two frames or more is a true still and takes copies, but a move much smaller than a sample, or a change at very low contrast, can still pass; fine detail with timebase jitter can cost a true still its copies; and a fade slower than about 0.4 % between copies is stacked as a still.
    - Cost: on 200 frames of five captures, export took 11.0 s instead of 2.8 s on stills and 3.4 s instead of 2.8 s on motion, with 11–13 % more memory. With the option off nothing changes. Frames are stacked on as many threads as read the stack: an export's decode workers, or the read-ahead that the Video Sink's VBI pre-scan and the analysis sinks use. Threads that need the same frame, or the same frame's signature, wait for one build rather than repeat it.

* `still_frame_search` (int32)
    - Shown when `still_frame_stacking` is on: how many frames either side of each frame to look for copies.
    - Range: 2–50.
    - Default: 24, which tries ±4 to ±24 in PAL and ±2 to ±24 in NTSC, so every frame of a hold up to 25 frames (one second in PAL) reaches all its repeats. PAL needs at least 4. A wider search finds more copies on long holds, but each frame then reads more frames: on eleven captures of a held test card, 24 took noise from 1.26 to 1.06 units against 8 (the same as 50) for an export about a quarter longer, and about a tenth longer on single-frame stills. Projects saved with a value keep it.

* `pressing_repair` (bool)
    - `Weighted` only: repair defects that every disc of one pressing carries, from the other pressings. Needs two pressings or more, as the noise model groups them; composite sources only.
    - Default: `false`.
    - A stamper defect is on every disc of its pressing, so they agree on it and the stack keeps it. On each active line, where one pressing's consensus (the median of its sources, on luma) differs from the other pressings' by more than the rejection distance and departs from its own neighbouring lines by that much more than theirs do, a run of 6 samples or more is replaced by the other pressings' stack, if that brings the line nearer its neighbours. A pressing of one source is not judged. Replaced samples are not marked as dropouts; the per-frame report counts them and the debug log lists each run.
    - What it does: on a synthetic case with unflagged dropouts on every capture of one pressing, it brought the stack's low band from 0.96 to 0.43 IRE from the truth, the level of the stack without the defects, and changed nothing without them. On 2,000 frames of Community North maps it repaired 46 runs visibly (spikes on every disc of one pressing, single lines starting early) and none falsely; on 2,000 frames of photographs, nothing visible.

* `audio_stacking` (string)
    - How to combine audio across sources, per channel pair, for every pair present in all inputs; pairs not common to all inputs come from the source with the fewest dropouts. Sources are combined sample by sample at the same frame position, saturating at the 24-bit range.
    - Allowed values:
        - `Disabled` (use audio from the source with the fewest dropouts)
        - `Mean`
        - `Median`
    - Default: `Mean`.

* `efm_stacking` (string)
    - How to combine EFM t-values across sources. Each EFM byte carries the t-value in its low nibble and the producing source's doubt about it in the high nibble.
    - Allowed values:
        - `Disabled` (pass the EFM of the source with the fewest dropouts through, bytes and doubt untouched; the source is chosen frame by frame)
        - `Confidence` (align the sources on the EFM frame-sync grid and let them vote on each transition, weighted by the doubt each recorded; emits a doubt of its own) — experimental
        - `Mean` (mean of the t-values at each sample index, emitted with zero doubt)
        - `Median` (median of the t-values at each sample index, emitted with zero doubt)
    - Default: `Disabled`.

**Notes**

* Stacking reduces noise only when the sources contain independent noise (separate captures). Sources that are identical apart from dropouts stack to the same underlying signal, so SNR will not improve on dropout-free picture areas.
* EFM combining is off by default because none of the modes has yet been shown to beat passing the best single source through. On six independent captures of one Domesday side, the best source alone recovered 21,695 sectors with 8 uncorrectable; `Confidence` (three captures) managed 21,577 with 140, and `Mean` just 5. `Confidence` more than halves C1 uncorrectable but raises C2 uncorrectable sharply, so treat it as experimental. Only `Disabled` is recommended for production use.
* `Mean` and `Median` combine EFM by sample index, which only holds while the sources stay in step: EFM t-values are run lengths, so a single spurious or missed transition in one capture shifts every t-value after it and the streams are then averaged out of step for the rest of the video frame. They also emit averages that no source reported. `Confidence` combines on the channel bit axis and re-anchors on the frame sync every 588 bits (IEC 60908 §20.2), so an insertion or a deletion costs at most the channel frame it is in, and it only emits t-values some source reported. Its doubt can be used by `efm_sink` and `efm_audio_decode` as an error-correction erasure hint through their `doubt_erasure_threshold` parameter. Keep `Mean` or `Median` only to reproduce an earlier result.
* For scripts, the stage logs what it measures as report records: lines that start `StackerStage report: ` and continue with a JSON object whose `v` key is 1. That prefix and `v` are the contract; the readable lines beside them may change. A `weights` record gives a block's frames and, per input, `sigma`, `share`, `stamperGroup` and `family`, with `fitTrusted`, `fellBackToMean`, the predicted `weightedSigma`, `equalSigma` and `bestSingleSigma`, and why a fit was or was not trusted: `worstPair`, `worstMisfitPercent`, `rmsMisfitPercent` and `pairs`. A `grid` record gives an input's new `offset`, the same to a fraction of a sample (`fractional_offset`), its `correlation` and the input it was measured against (`reference`). A `reference` record names the stand-in for the first input (`input`, `null` when there is none) and its `offset`. At DEBUG only (`orc-cli --log-level debug`), a `frame` record for each stacked frame gives, per field, the `inputs` and still-frame `copies` stacked, and the samples left marked: `lowConfidenceSamples` (the inputs disagreed) and `unrecoverableSamples` (no input could supply them), so a script can find a side's weak stretches. Frames are numbered from 1. Keep them with `orc-cli --log-file`; the stage's help lists every key.

**Analysis / preview tools**

* This stage has no interactive tools.
* Supports the standard GUI preview. Stacked frames are cached, so stepping back and forth in the preview does not recompute them.

---

## Dropout Map

| | |
|-|-|
| **Stage id** | `dropout_map` |
| **Stage name** | Dropout Map |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Manually override dropout hints on a per-frame basis without modifying samples |

**Use this stage when:**

* You want to add dropouts that were not detected.
* You want to remove false-positive dropout detections.
* You want to adjust dropout boundaries before correction.
* You want to create custom dropout patterns for testing.

**What it does**

This stage modifies the **dropout hint regions** seen by downstream stages. It does not change the underlying video samples. For each frame with an entry in the `dropout_map` parameter, additions are merged into the hint list and removals are subtracted before the hints are passed downstream; frames without an entry are forwarded unchanged. Downstream stages such as Dropout Correction then act on the edited hints.

**Parameters**

* `dropout_map` (string)
    - Per-frame dropout overrides in a JSON-like format.
    - Frame numbers are entered 1-based in the GUI (matching the preview window); the project file (YAML) stores them 0-based and the conversion is automatic. Line, start, and end values are frame-flat 0-based coordinates; `start` and `end` are sample positions within the line, both inclusive.
    - Default: `[]`.
    - Example:
        - `[{frame:1,add:[{line:10,start:100,end:200}],remove:[{line:15,start:50,end:75}]}]`
    - Use the **Edit Dropout Map** tool to build this string interactively rather than writing it by hand.

**Stage tools**

* **Edit Dropout Map** — opens the interactive [Dropout Map Editor](../dialogues/dropout-editor.md) for drawing new dropout regions or removing existing ones on a per-frame basis. Edits are written to the `dropout_map` parameter when you press **Apply** or **OK**; **Cancel** discards them.
    - The frame is shown exactly as the preview window shows it (same rendering, sequential frame layout), with the same aspect-ratio choice (1:1 square samples or 4:3 display). Dropouts detected by the source are drawn in **red**, regions added in the editor in **green**, and source dropouts marked for removal in **grey, struck through**.
    - Editing is modeless. **Add**: click and drag along a line on an empty area. **Select**: click a region, or its row in the region table (the selection stays in sync both ways). **Move / resize**: drag a selected addition to move it, drag the square handles at its ends to resize it, or nudge it one line or sample with the arrow keys.
    - **Resize a source dropout**: a selected red source dropout also shows resize handles (unless it is already marked removed). Dragging one marks the source dropout as removed and creates an addition covering the new extent, as a single undoable edit.
    - **Delete / remove**: press `Delete` (or use the right-click menu). On an addition it deletes the region; on a red source dropout it toggles the removed mark; on a struck-through region it restores it.
    - **Undo / redo** (`Ctrl+Z` / `Ctrl+Shift+Z`, or the Undo/Redo buttons) step through every edit, including edits on other frames.
    - The region table lists every region on the frame with its status (Source / Removed / Added), line (1-based) and start/end samples. Zoom with the toolbar buttons (including **1:1** and **Fit**) or Ctrl+mouse-wheel, which zooms at the cursor; the plain wheel pans, and the arrow keys pan when no addition is selected.
    - Frame navigation matches the preview window: first/previous/next/last buttons, a scrub slider, a 1-based jump box, and Page Up/Page Down. Frames that carry edits are marked in orange on the slider, and the **◀ Edit** / **Edit ▶** buttons jump between them. The pipeline runs and frames render in the background, so the editor opens immediately and shows progress in its status bar.
* Supports the standard GUI preview.

---

## Dropout Correction

| | |
|-|-|
| **Stage id** | `dropout_correct` |
| **Stage name** | Dropout Correction |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Correct dropouts by replacing corrupted samples using data from other lines and/or frames |

**Use this stage when:**

* Your source contains dropouts and you want to reconstruct damaged regions.
* You want configurable correction behaviour (how a region is filled, chroma-phase matching, widening the flagged regions).
* You want to visualise what would be corrected.

**What it does**

Dropout Correction reads dropout hints (from the source, a Stacker, or an upstream `dropout_map` stage) and fills each flagged region by one of two methods, chosen with `method`. With `line_copy` it copies the samples from the nearest suitable line in the same field. It looks for the nearest line above and the nearest below, within the field's active picture, whose own dropouts do not overlap the region; the closer one wins, and between two equally close lines the one whose samples over the region vary least. Some lines inside the active picture are never borrowed from because they carry no picture where a full line does: on PAL the half-lines 23 and 623 (blanked over half the line, often all of it on LaserDisc), and on NTSC and PAL-M the closed-caption lines 21 and 284 and the half-line 263. A region that runs from the colour burst into the picture is split at the end of the burst, and a region is cut off at the end of the active line. Correction operates on the full frame buffer regardless of any active-area relabelling upstream.

* **Composite sources** are always repaired from the same field: a line from the other field does not carry the same subcarrier phase and would invert the colour. With `match_chroma_phase` on, only in-phase lines are candidates — every 4th line of the field for PAL, every 2nd for NTSC and PAL-M.
* **Y/C sources** are repaired channel by channel. Luma takes the nearest clean line in the same field regardless of phase, falling back to the other field; chroma is taken from the same field only, from an in-phase line when `match_chroma_phase` is on.

With `interpolate`, a region in the picture is filled from the nearest clean lines above and below it in its field, up to 4 lines away each way, weighted by their distance, with luma and chroma separated so the lines immediately next to it can be used whatever their subcarrier phase. On a composite source each of those lines is split into luma and chroma (its chroma taken against the line beyond it, whose chroma is inverted, so fine horizontal detail stays luma where the two lines agree), the luma and the chroma are interpolated, and the chroma is put back at the damaged line's own phase, read from its burst (or, where a dropout covers the burst, from the nearest line in phase with it). On a Y/C source the luma plane is interpolated as it is, and the chroma plane the same way as composite chroma. A monochrome source (no burst) is interpolated as it is. A region in the burst, or one with no clean line within reach on either side, is copied from a line as `line_copy` does.

A region with no usable line in its field is left as it is. Frames without dropout hints pass through unchanged. The output carries no dropout hints, including for any region that could not be repaired, so put stages that read the hints (such as a Stacker) before this one.

**Parameters**

* `method` (string)
    - How a dropout in the picture is filled: `line_copy` copies it from one nearby line; `interpolate` interpolates it from the nearest clean lines above and below, with luma and chroma separated.
    - Default: `interpolate`. Projects saved before the parameter existed load with it too, so their output changes; set `line_copy` to repair dropouts as earlier versions did.
    - On synthetic sources and on the Domesday captures `interpolate` leaves about half the error of `line_copy` inside the dropouts, in luma and in chroma, and keeps map lines and text whole where `line_copy` pastes a streak from four lines away. It does worse only on a moving zone plate, a test pattern whose rings happen to repeat at line copy's distance. The figures are in the stage's help (**Help...** in the node context menu).

* `overcorrect_extension` (uint32)
    - Extend each dropout region by this many samples on each side before correcting it. Useful on heavily damaged sources where the detected boundary is slightly too narrow; 24 is typical for damaged sources.
    - Range: 0–48.
    - Default: 0.

* `match_chroma_phase` (bool)
    - When enabled, only replacement lines whose colour subcarrier is in phase with the damaged line are used (every 4th line of the field for PAL, every 2nd for NTSC and PAL-M), which prevents hue errors in the repaired region. When disabled, the nearest clean line is used whatever its phase. For Y/C sources it applies to chroma only. With `interpolate` it applies only where a region is copied rather than interpolated.
    - Default: `true`.

* `highlight_corrections` (bool)
    - When enabled, fills corrected regions with the source's white level (100 IRE) instead of replacement data, to show which regions will be corrected before the final export.
    - Default: `false`.

**Analysis / preview tools**

* This stage has no interactive tools.
* Supports the standard GUI preview.
* The `highlight_corrections` parameter is specifically intended as an analysis/diagnostic view.

---

## FM Beat Cancel

| | |
|-|-|
| **Stage id** | `fm_beat_cancel` |
| **Stage name** | FM Beat Cancel |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Formats** | PAL only |
| **Purpose** | Remove the FM-beat herringbone some PAL LaserDisc masters carry in flat saturated colour |

**Use this stage when:**

* A PAL LaserDisc shows a faint diagonal herringbone in flat saturated colour (bars, menus, map seas), strongest in reds and magentas. The Domesday masters carry it.
* Place one on each source, **before** the Stacker: the beat is recorded into the master, so every capture of a pressing carries the same beat and stacking keeps it, and the beats of different masters have independent phase, so a fit after they are blended removes much less.
* Run its **FM Beat Calibration** tool first (below).

**What it does**

The beat is a tone at the difference between the FM video carrier and the colour subcarrier: about 2.7 MHz on blue to 3.2 MHz on yellow, as strong as the colour is saturated. Its phase is that of the carrier, which the signal level sets, so the stage builds the beat's waveform from each field itself (the carrier at `carrier_black_mhz` plus 8 kHz per IRE of the pre-emphasised composite, its phase integrated along the field and mixed with the field's own chroma, then de-emphasised and band-passed to 2.2–3.9 MHz). It fits one complex amplitude per line to the field in that band, over flat parts of the picture only, tracks the beat's phase from line to line, and subtracts the fitted beat from the line's active picture. A line is corrected only where its amplitude is well above its standard error and below the largest a disc carries. A field with too little flat saturated colour is not fitted, and a frame neither of whose fields is corrected passes **bit-identical**. Blanking, sync, the burst and the VBI lines are never changed; dropout hints pass through. Any source other than PAL composite passes through unchanged, with a warning.

On a videosynth pressing with a −24 dB beat and the Domesday capture noise, the stage removes 23 dB of the beat on bars and on a red field; with the carrier 15 kHz off it removes under 2 dB. On single captures of Community South (Domesday) it removes 11.5 and 12.3 dB of the beat on the bars of two masters. What it removes is the same on two captures of one master (correlation 0.95–0.98 on bars and maps, 0.84–0.95 on photographs) and unrelated between masters (0.00), so it removes the master's beat and nothing of the picture. It corrects most map pictures too, whose seas are flat colour; photographs carry less (0.2 IRE RMS removed against 0.6 on maps).

**Parameters**

* `carrier_black_mhz` (double)
    - The master's FM video carrier at black, in MHz. The beat's phase along a line follows it closely: 10 kHz off turns it by about 3 radians across the picture, which leaves most of the beat in place. Set it with FM Beat Calibration.
    - Range: 6.9–7.3.
    - Default: 7.1, the PAL LaserDisc standard's. The Community South captures measure 7.088–7.106 MHz.

**Analysis / preview tools**

* **FM Beat Calibration** (`fm_beat_calibration`) reads 300 frames spread over the stage's input, takes the 24 fields with the most flat saturated colour, and searches the carrier from 6.9 to 7.3 MHz (2 kHz steps, then 0.25 kHz around the best) for the value at which the stage removes the most beat. **Apply to Stage** sets `carrier_black_mhz`. A source with no flat colour, or whose best carrier corrects no line, is reported as having no beat to remove. From the command line: `orc-cli tools run disc.orcprj fm_beat_calibration --node <node> --apply`. It takes under a minute on one capture.
* Supports the standard GUI preview.

---

## Picture Band Limit

| | |
|-|-|
| **Stage id** | `picture_band_limit` |
| **Stage name** | Picture Band Limit |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Cut the band above the picture: low-pass the luma, leaving the chroma as it is |

**Use this stage when:**

* You want to remove the disc noise and edge ringing that lie above the frequency where a LaserDisc's picture ends.
* Place it after stacking and Dropout Correction, before any sink.

**What it does**

A LaserDisc carries no picture above a certain frequency (about 5.4 MHz on PAL), but the decode passes everything up to the sample rate's limit, so that band holds only noise. On a composite source the stage separates each line's luma and chroma (the chroma is the line demodulated against the subcarrier and kept within 1.3 MHz of it), low-passes the luma at `cutoff_mhz`, and adds the chroma back unchanged, so the colour is not touched even where PAL's chroma band reaches above the cutoff. Where the whole roll-off lies above the chroma band (an NTSC or PAL-M cutoff more than half the roll-off above 4.88 MHz), the composite is low-passed as it is. On a Y/C source the luma plane is low-passed and the chroma plane passes unchanged. Only the active picture is filtered: blanking, sync, the burst and the VBI lines pass unchanged, and so do the dropout hints. The output stays composite (or Y/C), so it works with every sink.

On the synthetic picture-band cases (the Domesday noise model with the picture ending at a known frequency) the picture below the cutoff changes by under 1 % in every band, and the noise falls by 0.5–0.9 dB, all of it from above the cutoff.

**Parameters**

* `cutoff_mhz` (double)
    - The frequency at which the low-pass halves the luma: where the picture ends.
    - Range: 2.0–8.0.
    - Default: 5.4 for PAL, where the Domesday captures' picture ends; 5.0 for NTSC and PAL-M, not yet measured on real discs.
    - On a stacked set with captures of two or more pressings, Source Align's **Source Alignment Analysis** reports where the set's picture ends as `lumaCutoffMHz` in its capture comparison; enter that value here.

* `rolloff_mhz` (double)
    - Width of the low-pass's transition, centred on the cutoff. Its gain is within 0.3 % of unity below the cutoff less half the roll-off, and below 0.3 % above the cutoff plus half the roll-off.
    - Range: 0.1–2.0.
    - Default: 0.4.

**Analysis / preview tools**

* This stage has no interactive tools.
* Supports the standard GUI preview.

---

## Mask Line

| | |
|-|-|
| **Stage id** | `mask_line` |
| **Stage name** | Mask Line |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Mask (blank) specified lines by frame-flat line number |

**Use this stage when:**

* You want to hide visible VBI content.
* You want to mask the NTSC closed-caption line.
* You want to blank other unwanted content in fixed line regions.

**What it does**

Mask Line overwrites selected frame lines with a constant 10-bit sample level. Line numbers are **frame-flat** positions, entered 1-based in the GUI (equal to the full-frame broadcast line number). For Y/C sources both the luma and chroma buffers of a masked line are filled with the mask value. All other lines, and the audio/EFM streams, are forwarded unchanged.

**Parameters**

* `lineSpec` (string)
    - Comma-separated list of frame-flat line numbers or inclusive ranges to mask.
    - Format: `LINE` or `START-END`. Entered 1-based in the GUI; the project file (YAML) stores the value 0-based and the conversion is automatic.
    - Examples (as entered in the GUI):
        - `21` — mask broadcast line 21 (field 1 in PAL/NTSC; stored as `20` in YAML)
        - `6-22` — mask broadcast lines 6 through 22 (stored as `5-21` in YAML)
        - `6-22,319-335` — mask the VBI area in both fields of a PAL frame (stored as `5-21,318-334` in YAML)
    - Default: `""` (no masking, pass-through).
    - Use the **Mask Line Config** tool to generate this value from broadcast line numbers without writing the spec manually.

* `maskSampleLevel` (integer)
    - 10-bit sample level (0–1023) written to all masked lines.
    - Default: 0 (sync tip).
    - Typical values: 240 = blanking (NTSC/PAL-M; picture black is 282, 7.5 IRE above it), 256 = blanking/black (PAL), 800 = white (NTSC/PAL-M), 844 = white (PAL).

**Stage tools**

* **Mask Line Config** — a configuration dialog that accepts **full-frame broadcast line numbers** (1-based; PAL 1–625 with field 1 = lines 1–313, NTSC/PAL-M 1–525 with field 1 = lines 1–263) and converts them automatically to the frame-flat `lineSpec` format, masking each range in **both** fields: a range in field 1 also masks the equivalent field 2 lines, and the other way round. Example: PAL range 6–22 produces `lineSpec` = `6-22,319-335` (stored as `5-21,318-334` in the project file); PAL 10–20 produces `10-20,323-333`.
    - Add each range with **Start Line**, **End Line** and **Add Range**; **Remove Selected** and **Clear All** edit the list.
    - **Level Preset** sets `maskSampleLevel`: **Blanking** or **White** take the levels of the stage's input (PAL 256 and 844 by default), and **Custom** takes any 10-bit value from 0 to 1023.
* Supports the standard GUI preview.

**Status indicator**

Green when `lineSpec` is set; yellow when it is empty (pass-through, no lines masked).

---

## Video Parameters

| | |
|-|-|
| **Stage id** | `video_params` |
| **Stage name** | Video Parameters |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Override video parameter hints (signal levels, active-area boundaries) |

**Use this stage when:**

* Source metadata describes the wrong black or white level.
* The source metadata describes a non-standard active window and the visible picture needs re-framing.
* Downstream chroma decoding or export must work from corrected level or active-area hints.

**What it does**

Video Parameters wraps the upstream frame representation and overrides specified `SourceParameters` fields. Any parameter left at `-1` is inherited from the source. No pixel data is altered.

Overriding black or white level sets `has_nonstandard_values = true` only when the resulting level differs from the standard for the video system; setting a level to its spec value does not flag the source as non-standard.

The active-area parameters relabel the active window only. They never set `active_area_cropping_applied`: the sample buffer is forwarded uncropped so that downstream stages keep indexing it as a full frame. The preview shows the whole frame with the region outside the active window dimmed, so the un-dimmed area is exactly what an export will contain.

All level values are in the CVBS_U10_4FSC 10-bit domain.

**Parameters**

All parameters are int32 and use `-1` as "inherit from source"; no other negative value is accepted. Defaults are the standard values for the project's video system, so a standard source can be edited from its own geometry rather than from a blank form. Ranges come from the video system too — a sample offset cannot name a sample the line does not have, and a line number cannot name a line the frame does not have. `activeVideoEnd` and `lastActiveFrameLine` are exclusive, so each may sit one past the last sample or line.

* `activeVideoStart` — Active Video Start
    - First active video sample within the line (0-based).
    - Default: PAL 157, NTSC/PAL-M 126.
    - Range: `-1` to samples-per-line − 1 (PAL 1134, NTSC 909, PAL-M 908).

* `activeVideoEnd` — Active Video End
    - One past the last active video sample within the line.
    - Default: PAL 1105, NTSC/PAL-M 894.
    - Range: `-1` to samples-per-line (PAL 1135, NTSC 910, PAL-M 909).

* `firstActiveFrameLine` — First Active Frame Line
    - First active line of the frame (0-based, frame-flat).
    - Default: PAL 44, NTSC/PAL-M 40.
    - Range: `-1` to lines-per-frame − 1 (PAL 624, NTSC/PAL-M 524).

* `lastActiveFrameLine` — Last Active Frame Line
    - One past the last active line of the frame (0-based, frame-flat).
    - Default: PAL 620, NTSC/PAL-M 523.
    - Range: `-1` to lines-per-frame (PAL 625, NTSC/PAL-M 525).

* `whiteLevel` — White Level (10-bit)
    - White level (100 IRE) in the CVBS_U10_4FSC domain.
    - Default: PAL 844, NTSC/PAL-M 800.
    - Range: `-1` to 1023.

* `blackLevel` — Black Level (10-bit)
    - Black level in the CVBS_U10_4FSC domain.
    - Default: PAL 256 (no setup pedestal; black = blanking), NTSC/PAL-M 282 (7.5 IRE above blanking).
    - Range: `-1` to 1023.

**Values that must agree**

Three pairs are rejected when they contradict each other, because the resulting parameters would describe no picture:

| Must hold | Why |
|-----------|-----|
| `activeVideoStart` < `activeVideoEnd` | The active window would be empty or inverted |
| `firstActiveFrameLine` < `lastActiveFrameLine` | The active picture would contain no lines |
| `blackLevel` < `whiteLevel` | No contrast range is left to map |

A pair with either end left at `-1` is not checked: the inherited end is whatever the source reports, which the stage cannot know until it runs. In the GUI the offending pair is named before the values are applied; on the command line the stage refuses the parameters.

**Resetting to the source's values**

The parameter dialog's **Reset to Metadata Values** button fills every parameter with the value arriving at the stage's input: what the capture reports, after any stage upstream of it. The values are applied when you press **Update** or **OK**. If the input cannot be read, for example because no capture is configured yet, the button is labelled **Reset to Defaults** and fills in the format's standard values instead.

`orc-cli nodes reset <project> <node>` does the same from a script and saves the project, so a template's Video Parameters can follow each capture it is pointed at. Unlike the button, it does not fall back to the standard values: if the input cannot be read it refuses, with exit code `5`, and leaves the project unchanged. See [Resetting a stage's parameters](../../cli-user-guide/overview.md#resetting-a-stages-parameters).

---

## Audio Import

| | |
|-|-|
| **Stage id** | `audio_import` |
| **Stage name** | Audio Import |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Attach an external WAV file to the pipeline as a new audio channel pair |

**Use this stage when:**

* You have separately captured or restored audio (a cleaned-up soundtrack, a commentary track, an externally decoded recording) that should travel with the video.
* You want an imported track available to sinks so it can be selected, embedded, or exported alongside the decoder-provided audio.
* If the recording needs nudging into sync with the picture, follow this stage with an [Audio Align](#audio-align) targeting the new channel pair.

**What it does**

Audio Import wraps the upstream representation and appends one audio channel pair served from a WAV file, after all existing pairs (existing pair numbers are unchanged); video, dropout hints, EFM/AC3 signal data, and all existing channel pairs pass through untouched. The WAV must be RIFF/WAVE PCM, stereo, 16- or 24-bit, at any sample rate; anything else fails validation with a reason. On import the material is converted to the pipeline's only audio form — 48 kHz synchronous (frame-locked) 24-bit stereo per SMPTE 272M: 16-bit samples are widened to 24-bit (exactly reversible), the stream is resampled to exactly 48000 Hz (high-quality SoXR; a 48000 Hz input passes through unchanged), and the result is segmented into per-frame blocks (PAL 1920 stereo pairs per frame; NTSC/PAL-M 1602/1601 over the five-frame sequence). Material shorter than the project is padded with silence; excess is truncated. The appended pair reports origin `IMPORTED` and counts toward the pipeline's 8-channel-pair limit: the stage fails validation if the input already carries 8 pairs.

**Parameters**

* `wav_path` (file path, required) — The WAV file to attach. Until it is set the node's status dot is red.
* `pair_name` (string) — Human-readable name for the channel pair (shown by sinks, e.g. as the Video Sink stream title). Empty uses the WAV file name. Default: `""`.

**Stage tools**

* This stage has no interactive tools.

---

## EFM Audio Decode

| | |
|-|-|
| **Stage id** | `efm_audio_decode` |
| **Stage name** | EFM Audio Decode |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Decode the EFM t-value stream (LaserDisc digital audio) to CD audio and append it as a new pipeline channel pair |

**Use this stage when:**

* You have an ld-decode LaserDisc source carrying an `.efm` sidecar and want the disc's digital audio available as a pipeline channel pair — for example so the Video Sink embeds it alongside the analogue audio, or the Audio Sink / CVBS Sink can export it.
* Place it after dropout correction and stacking, so it decodes the best available EFM stream, and after any frame mapping or source alignment, so the decoded pair is born aligned to the output timeline.

**What it does**

The stage wraps the incoming representation and appends one audio channel pair (origin `EFM`), named by `pair_name`; video, dropout hints, undecoded EFM/AC3 signal data and all existing channel pairs pass through untouched. EFM decoding is whole-stream sequential (CIRC interleaving spans sectors), so it runs lazily at most once, on first access to the appended pair: the stage gathers the t-values across the input's full frame range, runs the shared EFM decode pipeline, widens the decoded 44.1 kHz 16-bit CD audio to 24-bit, resamples it to 48 kHz (SoXR HQ), and caches the frame-locked PCM in a scratch file on disk. Video-only preview and project validation never trigger the decode.

The decoded audio is aligned to the video automatically. The CIRC de-interleaver's constant 111-frame latency (666 stereo pairs, 15.1 ms of warm-up filler) is removed from the head of the stream, and the input the decoder consumed while acquiring sync is restored as silence, so the pair's first sample is synchronous with the first frame of this stage's input — the same sync the analogue audio has. If it still needs nudging, use `offset_ms`, or a downstream [Audio Align](#audio-align).

Each EFM byte packs the t-value into its low nibble and the producer's doubt about it (0 trusted to 15 distrusted) into its high nibble; the doubt can seed C1/C2 erasures through `doubt_erasure_threshold`. A source that recorded no doubt decodes exactly as before.

Data discs (ECMA-130 sectors) cannot become an audio pair: the decode fails and the appended pair is silent — use the EFM Decoder Sink in data mode instead. For a bit-exact, un-resampled WAV of the CD audio, use the EFM Decoder Sink rather than this transform. The appended pair counts toward the 8-channel-pair limit; the stage fails validation if the input already carries 8 pairs.

**Parameters**

* `no_timecodes` (bool) — Disable timecode verification during decode, for early CAV discs pre-dating the EFM timecode spec. On such discs the Q-channel timeline is absent or untrustworthy, so the decoder synthesises a contiguous one and treats every section as programme audio (track 1). Leave it off for normal discs: it overrides genuine lead-in/lead-out and track boundaries. Default: `false`.
* `no_audio_concealment` (bool) — Disable interpolation-based audio error concealment; uncorrectable samples are left as decoded. Default: `false`.
* `ignore_preemphasis` (bool) — Ignore the 50/15 µs pre-emphasis CONTROL flag (IEC 60908 §17.5) and decode the audio exactly as stored. When unchecked (default), sections flagged as pre-emphasised are de-emphasised during decode with a 50/15 µs filter so the appended pair has a flat response; enable this only if you want the raw pre-emphasised samples. Default: `false`.
* `pair_name` (string) — Human-readable name for the decoded channel pair; surfaces in the CVBS container and as the Video Sink stream title. Empty falls back to the default. Default: `EFM digital audio`.
* `offset_ms` (double) — Additional sync offset in milliseconds on top of the automatic alignment. Positive delays the audio relative to the video, negative advances it. Range ±3,600,000. Default: `0`.
* `doubt_erasure_threshold` (int32) — Treat an EFM symbol as a Reed-Solomon erasure when the producer's doubt about it reaches this value. Only the four most-doubted symbols of a codeword are ever flagged, so the code's erasure capacity cannot be exceeded. Range: 0–15. Default: `0` (off; the decode stays bit-exact). Leave it at 0: on a PAL CLV audio side no setting improved the decoded audio, and thresholds below 9 add clicks while the C2, concealed and muted counts in the report stay at their baseline values, so the statistics do not reveal the damage. If you experiment, compare the decoded audio, not the report. See the EFM Decoder Sink's notes on the same parameter.
* `report` (bool) — Write a detailed decode statistics report (the same CIRC/error/timing statistics the EFM Decoder Sink writes) once the lazy decode runs. It separates losses caused by the input from the decoder's own warm-up and end-of-stream drain, and attributes concealment to disc regions (lead-in, pause, programme, lead-out); only losses in the programme area affect the overall assessment. Default: `false`.
* `report_path` (file path) — Destination for the decode report; shown and used only when `report` is enabled. If `report` is on and this is empty, no report is written. Default: `""`.

**Stage tools**

* This stage has no interactive tools.

---

## Audio Channel Map

| | |
|-|-|
| **Stage id** | `audio_channel_map` |
| **Stage name** | Audio Channel Map |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Route the left/right channels of audio channel pairs following SMPTE 272M |

**Use this stage when:**

* You need to remove a channel pair, extract one channel as mono in place, or copy one channel as mono into another pair.
* You are splitting a dual-mono pair (e.g. a bilingual LaserDisc carrying two independent mono programmes as the left and right channels of one stereo pair) into two independent, named mono pairs. One operation per stage instance; chain instances for compound routing.
* A LaserDisc's analogue sound is mono over some or all of the disc, as its VBI programme status says. `vbi_mono_average` averages left and right on those frames: the two FM carriers' noise is independent, so the mean is quieter (1.0–1.3 dB on the silent mono passages of Domesday Community North) and a click on one channel is halved. Put it after the Stacker, on the analogue pair.

**What it does**

The stage remaps channels on the fly; video, dropout hints, EFM/AC3 signal data, and all untouched pairs pass through unchanged. Per SMPTE 272M-1994 §6.4 a mono programme occupies one channel with the other silenced — the chosen channel is placed on the left and the right is zeroed (`[L, 0]` / `[R, 0]`); a channel is never duplicated across both. The operation is a pure per-sample channel remap on the synchronous 48 kHz 24-bit audio; timing and per-frame sample counts are untouched. Pairs the stage writes are marked origin `DERIVED`. With a copy to target `new` the mono pair is appended; with an existing target that pair is overwritten.

`vbi_mono_average` is the exception to the `[M, 0]` rule. On the frames whose VBI programme status (lines 16–18) says the sound is mono, it writes the mean of left and right to both channels. Every other frame passes through untouched: stereo, bilingual, LV-ROM data frames, and frames whose status is missing or fails parity. The pair stays a stereo pair with its original origin. A frame is averaged only when its two channels also carry the same programme: levels within 6 dB, and a correlation of 0.5 or better unless the louder channel is at or below −50 dBFS (silence, where only the balance is checked). A disc can say mono with the programme on one carrier alone, as Dragon's Lair does, and the mean would halve it. Reading the status reads each frame's video, so an audio-only export through it costs a full picture read (about 5 minutes for a side from one CVBS capture). Use it only on the disc's analogue pair.

When the input cannot satisfy the operation — the selected pair does not exist, the chosen target pair does not exist, or appending would exceed the 8-pair limit — the stage passes its input through unchanged and logs a warning, so nothing downstream is interrupted. The most common cause is a source that carries no audio at all (for example a plain TBC without an Audio Import), in which case the parameter dialog says so.

**Example** — splitting a bilingual dual-mono pair into two named mono pairs with two chained instances:

1. `copy_right_to_target`, target `new`, **Add description** ticked, description `French language` — appends a pair holding the right programme as mono.
2. `left_to_mono` on the original pair, **Add description** ticked, description `English language` — keeps the left programme in place.

**Parameters**

* `channel_pair` (string) — The pair the operation reads from (source for copies, or the pair modified/deleted otherwise). 0-based, matching the CVBS container `_audio_<n>.wav` numbering; a GUI dropdown restricted to the pairs the input carries. Default: `0`.
* `operation` (string) — Default `left_to_mono`. One of:

| Value | Effect |
|-------|--------|
| `delete` | Remove the pair; later pairs shift down one index |
| `left_to_mono` | Keep the left channel, silence the right (`[L, 0]`), in place |
| `right_to_mono` | Move the right channel to the left, silence the right (`[R, 0]`), in place |
| `copy_left_to_target` | Copy the left channel as mono to the target pair; source untouched |
| `copy_right_to_target` | Copy the right channel as mono to the target pair; source untouched |
| `vbi_mono_average` | On frames whose VBI programme status says mono and whose channels carry the same programme, replace both channels with their mean; other frames untouched |

* `target_pair` (string) — Destination for the copy operations (shown only for `copy_*`). `new` appends a pair; otherwise the mono channel overwrites the chosen existing pair. Default: `new`.
* `set_description` (bool, **Add description**) — Shown for every operation except `delete`. When off, the produced pair keeps its existing description; tick to reveal `description`. Default: `false`.
* `description` (string) — Name for the pair the operation produces (e.g. `English language`). Shown only when **Add description** is ticked. Default: `""`.

**Stage tools**

* This stage has no interactive tools.

---

## Audio Align

| | |
|-|-|
| **Stage id** | `audio_align` |
| **Stage name** | Audio Align |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Shift one audio channel pair in time relative to the video to correct audio/video sync |

**Use this stage when:**

* A channel pair is consistently early or late against the picture — for example an EFM digital audio pair whose decode start-up left it slightly offset, or an imported WAV captured with a different lead-in than the video.
* Add one instance per channel pair that needs adjustment; multiple instances may target different pairs.

**What it does**

The stage serves the target channel pair shifted by the requested offset, converted to a whole number of stereo pairs at the synchronous 48 kHz rate (exactly 48 pairs per millisecond). Positive offsets delay the audio (insert lead-in); negative advance it (trim from the start). Only the target pair moves — video, all other pairs, and undecoded EFM/AC3 data are untouched. Each frame's audio window is reassembled from neighbouring frames' samples with silence past either end; per-frame sample counts (including the NTSC/PAL-M 1602/1601 sequence) are preserved, so the pair stays spec-conformant. Sample values and the sample rate are never changed — the shift is pure placement, no resampling. A zero offset passes the input through unchanged. So does a target pair the input does not carry: the stage logs a warning and forwards audio and video untouched rather than interrupting the pipeline (the most common cause is a source with no audio at all, which the parameter dialog points out).

**Parameters**

* `channel_pair` (string) — The pair to shift. 0-based, matching the CVBS container `_audio_<n>.wav` numbering; a GUI dropdown restricted to the pairs the input carries. Default: `0`.
* `offset_ms` (double) — Milliseconds, range ±3,600,000. Positive delays, negative advances. Rounded to the nearest whole stereo pair at 48 kHz (about 0.021 ms resolution). Default: `0.0`.

**Stage tools**

* This stage has no interactive tools.
