# Transform stages

Transform stages sit **between source stages and sink stages** in a decode-orc pipeline. They consume one or more upstream stage outputs and produce one or more outputs for downstream stages.

Transform stages are used to:

* Reorder or align captured frames from one or more sources.
* Combine multiple captures into a single improved output.
* Override or edit per-frame metadata and hints used by later processing.
* Apply signal modifications such as dropout correction or masking.
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

Frame Map parses a comma-separated list of frame ranges (e.g. `1-11,21-31,12-20`) and remaps output frame IDs to the specified input frames. It can optionally remove consecutive duplicate frames and insert synthetic padding frames to fill detected gaps.

**Parameters**

* `ranges` (string)
    - Comma-separated list of frame ranges, entered 1-based in the GUI (matching the preview window).
    - The project file (YAML) stores the value 0-based; the conversion is automatic when editing through the parameter dialog.
    - Default: `""` (empty) meaning passthrough.

* `remove_duplicates` (bool)
    - When enabled, removes the second of any two consecutive frames with matching `colour_frame_index` values.
    - Default: `false`.

* `pad_gaps` (bool)
    - When enabled, inserts synthetic padding frames when a break in the `colour_frame_index` sequence is detected (indicating a player skip).
    - Default: `false`.

* `pad_strategy` (string)
    - Strategy for synthesising padding frame content.
    - Allowed values: `black` (padding frames render as a black-level signal).
    - Default: `black`.
    - The legacy `nearest` value never behaved differently from `black`; it is still accepted for backward compatibility with older project files and is treated as `black`.

**Stage tools**

* **Disc Mapper** — analyses the incoming frame stream for tracking errors (skipped, repeated, or missing frames caused by LaserDisc player seek problems) and writes a corrected `ranges` specification back to the stage. Where a disc picture was captured more than once, the best copy is kept: candidates are scored on colour-burst level, white-flag SNR, and black-level PSNR, and the analysis report itemises each duplicate group with the readings behind the choice. Works for both CAV and CLV discs; the analysis can be cancelled from the tool dialog. LaserDisc sources only. The **Include Lead-in/Lead-out** option keeps one lead-in frame and one lead-out frame at the ends of the mapping (default off, programme content only) so that downstream sinks can collect the disc's lead-in metadata, such as the user's code.
* **Frame Map Range Finder** — locates a section of the disc by picture number or CLV timecode (e.g. `12345` or `0:1:23.4`) and writes the corresponding `ranges` string to the stage. Uses a binary-chop search over the disc's VBI picture numbers, falling back to a sequential scan when the numbering is not monotonic. Cancellable. LaserDisc sources only.
* **Frame Corruption Generator** — injects synthetic corruption patterns (skips, repeats, gaps) into the frame mapping for testing the Disc Mapper tool.
* Supports standard GUI previews (via `PreviewableStage`).

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
    - Each ID may appear only once. An ID that is not connected is ignored; a connected source that is not named contributes no frames. Both are logged, and the node's status dot turns yellow.
    - If the list names no connected node at all, the stage fails rather than guessing.
    - Default: `""` (empty) meaning the sources are joined in the order their connections were made.

Source Join has a second parameter, `input_node_ids`, which is filled in by Decode-Orc from the node's incoming connections. It is host-owned: it does not appear in the parameter dialog or in the project file, and there is nothing to set.

**Status indicator**

Yellow means either that `input_order` is empty (the sources will be joined in connection order) or that the order no longer names exactly the sources connected to the stage — which is what you will see after adding, removing or rewiring an input. Re-open the parameters and update the order to clear it.

**Stage tools**

* This stage has no interactive tools.
* Supports standard GUI previews (via `PreviewableStage`).

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

Source Align finds the **first common frame** across all inputs using **VBI frame numbers (CAV)** or **CLV timecodes**, then drops the leading fields that precede it in each input so output frame indices are synchronised across all aligned outputs. The stage verifies that `colour_frame_index` is consistent across all aligned sources.

**Parameters**

* `alignmentMap` (string)
    - Manual alignment specification. When set, automatic detection is skipped and the specified per-input offsets (in fields) are applied directly.
    - Format: `input_id+offset` per input, e.g. `1+2, 2+2, 3+1, 4+1`.
    - Default: `""` (empty) meaning auto-detect from VBI/timecode.

**Stage tools**

* **Source Alignment Analysis** — analyses all connected sources and writes the optimal offset map to `alignmentMap`. Two alignment modes are offered: `pad_for_alignment` prepends synthetic padding frames so all sources start from the earliest VBI frame available (no frames are lost), while `first_common_frame` trims all sources to the first VBI frame present in every input (may discard unique leading content). Cancellable. LaserDisc sources only.
* Supports standard GUI previews (via `PreviewableStage`).
* Generates a stage report after execution, including per-input alignment offsets and related alignment details.

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

**What it does**

Stacker expects its inputs to already be aligned (typically using `frame_map` and/or `source_align`). It then stacks "frame N from all sources" into a single output using one of several algorithms. Frames with matching `colour_frame_index` values are aligned for stacking; frames with `colour_frame_index == -1` fall back to temporal alignment. Padding frames (`is_padding_frame = true`) produce no sample accumulation.

If only **1 input** is provided, the stage acts as a passthrough.

**Parameters**

* `mode` (string)
    - Stacking algorithm, applied per pixel to the usable (non-dropout) values across all sources.
    - Default: `Auto`.

| Mode | Behaviour per pixel |
|------|--------------------|
| `Auto` | `Smart Mean` when 3 or more usable source values are available; plain `Mean` otherwise. |
| `Mean` | Arithmetic mean of all usable source values. |
| `Median` | Median of the usable source values (mean of the middle two for an even count). |
| `Smart Mean` | Mean of the values within `smart_threshold` of the median, rejecting outliers; falls back to the median when no value qualifies. |
| `Smart Neighbor` | Not yet implemented — currently behaves as `Median`. |
| `Neighbor` | Not yet implemented — currently behaves as `Median`. |

* `smart_threshold` (int32)
    - Pixel-value threshold used by `Smart Mean` (and by `Auto` when it selects `Smart Mean`) to exclude outliers before averaging.
    - Range: 0–128.
    - Default: 15.
    - Has no effect for other modes.

* `no_diff_dod` (bool)
    - Disable differential dropout detection.
    - Default: `false`.
    - When `false`, the stage may recover pixels marked as dropouts by comparing values across sources (requires 3+ sources).

* `passthrough` (bool)
    - Passthrough "universal" dropouts that appear in all sources.
    - Default: `false`.

* `audio_stacking` (string)
    - How to combine audio across sources.
    - Allowed values:
        - `Disabled` (use audio from the source with the fewest dropouts)
        - `Mean`
        - `Median`
    - Default: `Mean`.

* `efm_stacking` (string)
    - How to combine EFM t-values across sources.
    - Allowed values:
        - `Disabled` (use EFM from the source with the fewest dropouts)
        - `Mean`
        - `Median`
    - Default: `Mean`.

**Notes**

* Stacking reduces noise only when the sources contain independent noise (separate captures). Sources that are identical apart from dropouts stack to the same underlying signal, so SNR will not improve on dropout-free picture areas.

**Analysis / preview tools**

* Supports standard GUI previews (via `PreviewableStage`).
* Generates a stage report ("Stacker Configuration") that records the effective mode and related configuration.

---

## Dropout Map

| | |
|-|-|
| **Stage id** | `dropout_map` |
| **Stage name** | Dropout Map |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Manually override dropout hints on a per-field basis without modifying samples |

**Use this stage when:**

* You want to add dropouts that were not detected.
* You want to remove false-positive dropout detections.
* You want to adjust dropout boundaries before correction.
* You want to create custom dropout patterns for testing.

**What it does**

This stage modifies the **dropout hint regions** seen by downstream stages. It does not change the underlying video samples. For each field with an entry in the `dropout_map` parameter, additions are merged into the hint list and removals are subtracted before the hints are passed downstream; fields without an entry are forwarded unchanged.

**Parameters**

* `dropout_map` (string)
    - Per-frame dropout overrides in a JSON-like format.
    - Frame numbers are entered 1-based in the GUI (matching the preview window); the project file (YAML) stores them 0-based and the conversion is automatic. Line, start, and end values are frame-flat 0-based coordinates.
    - Default: `[]`.
    - Example:
        - `[{frame:1,add:[{line:10,start:100,end:200}],remove:[{line:15,start:50,end:75}]}]`
    - Use the Dropout Editor tool to build this string interactively rather than writing it by hand.

**Stage tools**

* **Dropout Editor** — opens an interactive editor for drawing new dropout regions or deleting existing ones on a per-field basis. Changes are saved back to the `dropout_map` parameter automatically.
* Supports standard GUI previews (via `PreviewableStage`).

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
* You want configurable correction behaviour (intra-field only, search distance limits, etc.).
* You want to visualise what would be corrected.

**What it does**

Dropout Correction reads dropout hints (from the source or from an upstream `dropout_map` stage), then replaces affected samples using nearby lines and/or the opposite field block within the frame, subject to configured constraints. Correction operates on the full frame buffer regardless of any active-area crop applied upstream.

**Parameters**

* `overcorrect_extension` (uint32)
    - Extend dropout regions by this many samples.
    - Range: 0–48.
    - Default: 0.

* `intrafield_only` (bool)
    - When enabled, forces correction using only the same field block (never the opposite block).
    - Default: `false`.

* `max_replacement_distance` (uint32)
    - Maximum distance (in lines) to search for replacement data.
    - Range: 1–50.
    - Default: 10.

* `match_chroma_phase` (bool)
    - When enabled, matches chroma phase when selecting replacement lines (PAL only).
    - Default: `true`.

* `highlight_corrections` (bool)
    - When enabled, fills corrected regions with white level (100 IRE) to visualise dropout locations.
    - Default: `false`.

**Analysis / preview tools**

* Supports standard GUI previews (via `PreviewableStage`).
* The `highlight_corrections` parameter is specifically intended as an analysis/diagnostic view.

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
    - Typical values: 240 = blanking/black (NTSC/PAL-M), 256 = blanking/black (PAL), 800 = white (NTSC/PAL-M), 844 = white (PAL).

**Stage tools**

* **Mask Line Config** — a configuration dialog that accepts **full-frame broadcast line numbers** (1-based; PAL 1–625, NTSC/PAL-M 1–525) and converts them automatically to the frame-flat `lineSpec` format, masking each range in **both** fields. Example: PAL range 6–22 produces `lineSpec` = `6-22,319-335` (stored as `5-21,318-334` in the project file).
* Supports standard GUI previews (via `PreviewableStage`).

---

## Video Parameters

| | |
|-|-|
| **Stage id** | `video_params` |
| **Stage name** | Video Parameters |
| **Connections** | 1 input → 1 output (fan-out supported) |
| **Purpose** | Override video parameter hints (dimensions, signal levels, active-line boundaries) |

**Use this stage when:**

* Source metadata is missing or incorrect.
* You need to adjust active video boundaries for later processing.
* You need to override signal levels for correct black/white interpretation.
* You need to force dimensions to match a known-good configuration.

**What it does**

Video Parameters wraps the upstream frame representation and overrides specified `SourceParameters` fields. Any parameter left at `-1` is inherited from the source. When any level override (`black_level`, `white_level`) is applied, the stage sets `has_nonstandard_values = true`. When any crop parameter is set, the stage sets `active_area_cropping_applied = true`.

All level values are in the CVBS_U10_4FSC 10-bit domain.

**Parameters**

All parameters are int32 and use `-1` as "inherit from source".

* `frame_width_nominal` — Override nominal frame width in samples.
* `frame_height` — Override frame height in lines.
* `sync_tip_level` — Override sync tip level (10-bit domain).
* `blanking_level` — Override blanking level (10-bit domain).
* `black_level` — Override black level (10-bit domain). Sets `has_nonstandard_values = true`.
* `white_level` — Override white level (10-bit domain). Sets `has_nonstandard_values = true`.
* `peak_level` — Override peak level (10-bit domain).
* `active_video_start` — Override active video start sample index. Sets `active_area_cropping_applied = true`.
* `active_video_end` — Override active video end sample index. Sets `active_area_cropping_applied = true`.
* `first_active_frame_line` — Override first active frame line. Sets `active_area_cropping_applied = true`.
* `last_active_frame_line` — Override last active frame line. Sets `active_area_cropping_applied = true`.

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

**What it does**

Audio Import wraps the upstream representation and appends one audio channel pair served from a WAV file; video, dropout hints, EFM/AC3 signal data, and all existing channel pairs pass through untouched. The WAV must be RIFF/WAVE PCM, stereo, 16- or 24-bit, at any sample rate. On import the material is converted to the pipeline's only audio form — 48 kHz synchronous (frame-locked) 24-bit stereo per SMPTE 272M: 16-bit samples are widened to 24-bit, the stream is resampled to exactly 48000 Hz (high-quality SoXR; a 48000 Hz input passes through unchanged), and the result is segmented into per-frame blocks. Material shorter than the project is padded with silence; excess is truncated. The appended pair reports origin `IMPORTED` and counts toward the pipeline's 8-channel-pair limit.

**Parameters**

* `wav_path` (file path, required) — The WAV file to attach.
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

**What it does**

The stage wraps the incoming representation and appends one audio channel pair (origin `EFM`). EFM decoding is whole-stream sequential (CIRC interleaving spans sectors), so it runs lazily at most once, on first access to the appended pair: the stage gathers the t-values across the input's full frame range, runs the shared EFM decode pipeline, widens the decoded 44.1 kHz 16-bit CD audio to 24-bit, resamples it to 48 kHz (SoXR HQ), and caches the frame-locked PCM on disk. Video-only preview and validation never trigger the decode. EFM sync acquisition makes the pair's time origin approximate; nudge it with a downstream Audio Align if needed. Data discs (ECMA-130 sectors) cannot become an audio pair — use the EFM Decoder Sink in data mode instead. For a bit-exact, un-resampled WAV of the CD audio, use the EFM Decoder Sink rather than this transform. The appended pair counts toward the 8-channel-pair limit.

**Parameters**

* `no_timecodes` (bool) — Disable timecode verification during decode (needed for early CAV discs pre-dating the EFM timecode spec). Default: `false`.
* `no_audio_concealment` (bool) — Disable interpolation-based audio error concealment. Default: `false`.
* `ignore_preemphasis` (bool) — Ignore the 50/15 µs pre-emphasis CONTROL flag (IEC 60908 §17.5) and decode the audio exactly as stored. When unchecked (default), sections flagged as pre-emphasised are de-emphasised during decode with a 50/15 µs filter so the appended pair has a flat response; enable this only if you want the raw pre-emphasised samples. Default: `false`.
* `pair_name` (string) — Human-readable name for the decoded channel pair; surfaces in the CVBS container and as the Video Sink stream title. Default: `EFM digital audio`.
* `report` (bool) — Write a detailed decode statistics report once the lazy decode runs. Default: `false`.
* `report_path` (file path) — Destination for the decode report; used only when `report` is enabled. Default: `""`.

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

**What it does**

The stage remaps channels on the fly; video, dropout hints, EFM/AC3 signal data, and all untouched pairs pass through unchanged. Per SMPTE 272M-1994 §6.4 a mono programme occupies one channel with the other silenced — the chosen channel is placed on the left and the right is zeroed (`[L, 0]` / `[R, 0]`); a channel is never duplicated across both. The operation is a pure per-sample channel remap on the synchronous 48 kHz 24-bit audio; timing and per-frame sample counts are untouched. Pairs the stage writes are marked origin `DERIVED`. Validation fails when the selected or target pair does not exist, or when appending would exceed the 8-pair limit.

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

The stage serves the target channel pair shifted by the requested offset, converted to a whole number of stereo pairs at the synchronous 48 kHz rate (exactly 48 pairs per millisecond). Positive offsets delay the audio (insert lead-in); negative advance it (trim from the start). Only the target pair moves — video, all other pairs, and undecoded EFM/AC3 data are untouched. Each frame's audio window is reassembled from neighbouring frames' samples with silence past either end; per-frame sample counts (including the NTSC/PAL-M 1602/1601 sequence) are preserved, so the pair stays spec-conformant. Sample values and the sample rate are never changed — the shift is pure placement, no resampling. A zero offset passes the input through unchanged; validation fails when the target pair does not exist.

**Parameters**

* `channel_pair` (string) — The pair to shift. 0-based, matching the CVBS container `_audio_<n>.wav` numbering; a GUI dropdown restricted to the pairs the input carries. Default: `0`.
* `offset_ms` (double) — Milliseconds, range ±3,600,000. Positive delays, negative advances. Rounded to the nearest whole stereo pair at 48 kHz (about 0.021 ms resolution). Default: `0.0`.

**Stage tools**

* This stage has no interactive tools.
