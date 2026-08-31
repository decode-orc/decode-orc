# CVBS Sink

Writes processed CVBS video data to the CVBS file format for archival, interchange, or round-trip testing. The output consists of a `.cvbs` binary file (or `.cvbsy`/`.cvbsc` pair) and a `.meta` SQLite sidecar, and can be re-opened by the CVBS Source stage.

## When to use

Use this sink at the end of a CVBS processing pipeline when you want to save the result in the native CVBS file format — for example, after stacking multiple captures or applying corrections, and you want to store the result for later re-processing or sharing.

## What it does

Writes the incoming video stream using the selected sample encoding, plus a `.meta` SQLite sidecar database. The output signal type follows the project type automatically and is not user-selectable: a composite project is written as a single `.cvbs` file, and a Y/C project is written as a `.cvbsy`/`.cvbsc` pair (per the CVBS file format naming convention). The sidecar records the video standard preset, the selected `sample_encoding_preset`, the signal type, the frame count, and a measured `signal_state_preset` and `sequence_continuous` flag (CVBS file format spec v1.6.0).

Associated data are written automatically as sidecar files when present in the incoming stream:

- `.dropouts.meta` — dropout annotations (dropout extension format)
- `_audio_0.wav` … `_audio_7.wav` — stereo 24-bit 48 kHz PCM audio, one file per pipeline audio channel pair (up to 8), with the single-digit file number matching the pipeline pair index (CVBS file format spec v1.4.0). All pipeline audio is 48 kHz synchronous (frame-locked), so every file follows the output frame sequence; frames without audio are written as silence so all pair files stay frame-aligned and equal-length. Each pair's description is recorded in the `.meta` `audio_channel_pair` table.
- `.efm` + `.efm.meta` — EFM t-value data
- `.ac3` + `.ac3.meta` — AC3 RF data

## Parameters

### output_path (file path)
Base path for output files. The stage appends the payload extension (`.cvbs` for a composite project, `.cvbsy`/`.cvbsc` for a Y/C project) and `.meta` automatically; a trailing `.cvbs`, `.cvbsy`, or `.cvbsc` extension is stripped when present. Required.

### sample_encoding (string)
Sample encoding of the output data, recorded as `sample_encoding_preset` in the `.meta` file. One of `CVBS_U10_4FSC` (default), `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, or `CVBS_S16_4FSC`. `CVBS_U10_4FSC` preserves the internal 10-bit domain losslessly, including headroom values outside 0–1023; the other encodings clamp to their representable domain before scaling, as required by the CVBS file format specification.

### capture_notes (string)
Optional free-text notes written to the `.meta` file. When left empty, no notes field is written. Default: `""`.

## Notes

- The output signal type (`composite` vs `yc`) follows the project type and cannot be overridden — Y/C cannot be derived from a composite signal.
- `signal_state_preset` and `sequence_continuous` in the output `.meta` are measured, not assumed, and cannot be overridden. As each frame is written its colour-sequence phase is measured from the burst. The file is marked `STANDARD_STABLE_LOCKED` when the burst was measurable at the standard phase points, and `STANDARD_STABLE_UNLOCKED` when no burst could be measured at all (for example monochrome material). Separately (CVBS file format spec v1.6.0), `sequence_continuous` records whether the measured colour sequence ran unbroken through every frame written: `TRUE` when it did, `FALSE` when it broke at least once, and `NULL` (unknown) when nothing was measurable — a discontinuity does not downgrade the preset. Frames with no measurable burst — blank leader, lead-in, black frames — are not breaks in themselves: the expected phase is projected across them and re-checked on the far side. The verdicts appear in the stage's completion status and in the log, naming the output frame of the first discontinuity. Because the markers describe the file that was written, a stage that legitimately reorders or drops frames can turn a continuous input into a discontinuous output; running the Disc Mapper first is what restores a continuous sequence.
- Sidecar files for dropout, audio, EFM, and AC3 data are written automatically alongside the main output when those data streams are present; absent streams produce no sidecar files (this is not an error).
- Every pipeline audio channel pair is exported — there is no pair selection at this sink. Per-pair descriptions are recorded in the `.meta` `audio_channel_pair` table.
- The output is compatible with the CVBS Source stage for round-trip workflows.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
