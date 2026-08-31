# CVBS Source

Reads composite video from a `.cvbs` file (or a `.cvbsy` / `.cvbsc` pair for Y/C captures) and presents the decoded frames as a VideoFrameRepresentation for downstream stages. By default the stage detects the sample encoding and other capture details from the `.meta` SQLite sidecar; because the CVBS file format declares metadata optional, the sample encoding can also be selected manually so that sources without a sidecar can be used. All sample values are normalised to the internal 10-bit CVBS domain before passing data downstream.

## When to use

Add CVBS Source as the first stage in any pipeline that starts from a CVBS file produced by the decode-orc capture toolchain. Files with a signal state of `STANDARD_STABLE_LOCKED` or `STANDARD_STABLE_UNLOCKED` are accepted; any other state (for example `STANDARD_RAW`, a `NONSTANDARD_*` sample rate, or a pre-v1.6.0 `*_TBC_*` preset name) is rejected before any sample data is read, because the stage's frame geometry assumes time-base-stable samples at the standard 4fsc rate.

## Parameters

The file-path parameters offered match the project's source type: a composite project shows only **CVBS File Path**, while a Y/C project shows only the **Y (Luma)** and **C (Chroma)** paths.

| Parameter | Meaning |
|-----------|---------|
| CVBS File Path (`input_path`) | Path to the composite data file (`.cvbs`). Composite projects only. |
| CVBS Y (Luma) File Path (`y_path`) | Path to the luma channel file (`.cvbsy`). Y/C projects only; set together with `c_path`. |
| CVBS C (Chroma) File Path (`c_path`) | Path to the chroma channel file (`.cvbsc`). Y/C projects only; set together with `y_path`. |
| Sample Encoding (`sample_encoding`) | `From metadata` (default) reads the encoding from the `.meta` sidecar. Selecting `CVBS_U10_4FSC`, `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, or `CVBS_S16_4FSC` manually makes the sidecar optional. |

The `lock_audio` parameter has been removed: the pipeline no longer has a free-running audio regime. All audio is carried as 48 kHz synchronous (frame-locked) 24-bit stereo channel pairs — the only audio format the CVBS file format specification (v1.4.0) permits.

## What it does

With **Sample Encoding** at its default (`From metadata`), the stage opens the `.meta` sidecar at execute time and reads the video standard, sample encoding, signal state, sequence continuity, frame count, NTSC-J black level, and decode provenance (the upstream decoder name and its git branch/commit, passed through unchanged to downstream stages). If the signal state is neither `STANDARD_STABLE_LOCKED` nor `STANDARD_STABLE_UNLOCKED`, or the video standard does not match the stage, the stage reports a configuration error and stops.

Phase lock is not required. Colour-sequence phase is measured from each frame's burst by the `colour_frame_phase` observer rather than taken from the sidecar, so a `STANDARD_STABLE_UNLOCKED` source (for example monochrome material) decodes normally. Continuity of the content is declared separately by the `sequence_continuous` metadata field (CVBS file format spec v1.6.0): a source marked `sequence_continuous = FALSE` contains at least one discontinuity, which for a LaserDisc source usually means the player skipped or jumped during the decode. The stage logs a warning and continues — run the Disc Mapper (a Frame Map stage tool) to restore the recorded frame order before exporting.

When a sample encoding is selected manually the `.meta` sidecar is ignored (it need not exist). The video standard comes from the stage itself, the signal is assumed to be `STANDARD_STABLE_LOCKED`, the frame count is measured from the file size, and audio channel pairs carry derived names (the `audio_channel_pair` metadata table is not read).

Each frame's sample words are read in order, applying the normalisation appropriate to the encoding:

- `CVBS_U10_4FSC` — identity transform; values are already in the 10-bit domain.
- `CVBS_U16_4FSC` — divide the 16-bit unsigned value by 64.
- `CVBS_TPG21_4FSC` — divide the signed 16-bit value by 64 and add 508.
- `CVBS_S16_4FSC` — divide the signed 16-bit value by 32 and add the 10-bit blanking level.

Each frame carries a colour-frame index measured from the colour burst: 1–4 for PAL and PAL-M, 0–1 for NTSC. Frames whose burst is absent or cannot be measured carry a colour-frame index of -1.

Dropout, audio, EFM, and AC3 sidecars are loaded automatically when the corresponding files are present alongside the primary data file.

### Audio channel pairs

Every `<basename>_audio_0.wav` … `_audio_7.wav` sidecar (single-digit suffix, CVBS file format spec v1.4.0) becomes the pipeline audio channel pair with the same index. Container pair numbers need not be contiguous; absent intermediate numbers become silent placeholder pairs so pipeline indices always match container numbers. Per-pair descriptions are read from the `.meta` file's `audio_channel_pair` table; existing files without a table row (a spec violation) are reported as a warning and derive names of the form `Channel pair N`.

Each file's RIFF header is validated against the specification — PCM, 2 channels, 48000 Hz, 24-bit signed little-endian — and mismatches are reported as errors (there is no conversion of non-conforming containers). All pair files must be equal-length, carrying exactly one cadence-sized block per frame; a length mismatch is reported as a warning and short frames are served as silence. Because the container payload is already in the pipeline audio form (48 kHz synchronous 24-bit stereo, SMPTE 272M-1994), frames are read directly from the file by cadence offset with no conversion.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. The source file is present and valid, and metadata is either present and valid or not required (manual sample encoding). |
| Yellow | The source file is present but its `.meta` sidecar is missing or unreadable, and the sample encoding is set to `From metadata`. Provide the sidecar or select an encoding manually. |
| Red | Not configured. No file path is set, the configured path does not point to an accessible source file, or the file's video system does not match the stage. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
