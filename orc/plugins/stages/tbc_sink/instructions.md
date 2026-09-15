# TBC Sink

Writes the processed pipeline result back to the ld-decode / vhs-decode TBC file format, producing a `.tbc` video file, a `.tbc.db` metadata database, and the `.pcm` and `.efm` sidecars the pipeline is carrying. The output is fully compatible with the ld-tools ecosystem.

## When to use

Use this stage as the final output stage when you want to feed the processed result into existing ld-decode tools such as ld-chroma-decoder, ld-analyse, or ld-process-vbi. This is the most common archival output stage for LaserDisc and tape workflows — run stacking, dropout correction, and any other transforms upstream, then connect TBC Sink at the end of the pipeline.

## What it does

For every field that passes through the pipeline, TBC Sink writes the raw field samples to a `.tbc` binary file and records the associated metadata (field number, parity, VBI observations, hints, and all per-field observations) to a `.tbc.db` SQLite database. The stage also supports pipeline preview — you can inspect what will be written before triggering.

Field order and signal levels are preserved end to end, so a TBC Source → TBC Sink pipeline is a round trip. The isFirstField (top) field is written first, matching what TBC Source reads, and samples are widened to the ld-decode 16-bit domain by the fixed ×64 factor the format defines — the picture is not rescaled. A non-standard black level, such as an NTSC-J capture whose picture black sits at the 0 IRE blanking level rather than on the 7.5 IRE setup pedestal, is recorded in the output metadata as it was read rather than being replaced by the standard value.

Padding frames (inserted upstream by Frame Map gap filling or Source Align, or marked as padding in the source TBC metadata) are written as two blanking-level fields with `pad` set in the field metadata, so the padding identity survives a round-trip back through TBC Source. Their audio is written as well, so a gap does not slide everything after it out of sync.

## Sidecar files

Alongside the `.tbc` and `.tbc.db`, the stage writes the sidecars that ld-decode expects, named off the same base with the `.tbc` replaced — `disc.tbc` is accompanied by `disc.pcm` and `disc.efm`. That is the layout TBC Source auto-detects, so an export drops straight back in as a source.

Both come from whatever reaches the sink through the pipeline, not from any file the original source read: it makes no difference whether the chain started at a TBC Source, a CVBS Source, or a stage that produced the audio itself.

### disc.pcm — analogue audio

Written whenever the input carries at least one audio channel pair. The file is headerless signed 16-bit little-endian stereo at 44100 Hz, matching what ld-decode produces, and the `pcm_audio_parameters` record in the metadata database describes that layout.

Pipeline audio is 48000 Hz 24-bit, so the export resamples it down and narrows it to 16-bit. The resample is a single pass over the whole stream, which means a long export holds the audio in memory while it runs. It is also not lossless: repeatedly exporting and re-importing the same audio will degrade it slightly each time.

Only one channel pair can go into the sidecar, because the ld-decode layout has room for exactly one. The **Audio Channel Pair** parameter chooses which; see below.

### disc.efm — EFM t-values

Written whenever the input carries EFM. One byte per t-value, in field order, with no index — TBC Source finds a frame's payload by running-summing the per-field `efm_t_values` counts in the metadata database, which this stage fills in.

The pipeline exposes EFM per frame rather than per field, so each frame's run of bytes is split evenly between its two field records. Where that internal boundary falls does not affect the file or any frame's payload on re-import.

## Parameters

### output_path (string)
Base path for the output files. Required. The stage appends the extensions automatically: `.tbc` for video fields (a trailing `.tbc` in the parameter is kept as-is) and `.tbc.db` for the metadata database. The `.pcm` and `.efm` sidecars replace the `.tbc` rather than extending it.

`-` writes the `.tbc` payload to the CLI process's standard output instead of a file (e.g. `orc-cli process disc.orc-project -o "tbc_sink=output_path=-" | orc-cli ...` for chaining) — runs only via the CLI; settable here for a project you'll execute there. No `.tbc.db` metadata (SQLite needs to seek, which a pipe cannot do) and no `.pcm`/`.efm` sidecars either — a pipe carries the primary stream alone, matching the TBC Stream Source stage's own read side.

### audio_channel_pair (string)
Which audio channel pair is written to the `.pcm` sidecar, as a 0-based index matching the CVBS container's channel pair numbering. Defaults to `0`, the lowest pair — that is where a TBC or CVBS source puts the analogue audio it read, so the default is the right answer for an ordinary disc or tape pipeline.

Set it to another index when the pipeline carries several pairs and you want a different one exported — for example pair 1 when EFM Audio Decode has added the disc's digital audio alongside the analogue pair. In the GUI the dropdown lists only the pairs the input actually carries, each labelled with its name — `0: Analogue`, `1: EFM digital audio` — so you can pick by what the pair is rather than by counting. The project still stores the bare index. The parameter is ignored when the input has no audio, and a pair the input does not carry falls back to the lowest one.

## Notes

The output path must be writable. If the target directory does not exist the stage will fail at trigger time.

The sidecars cover analogue audio and EFM only. AC3 RF is not written — use the AC3 RF Sink in parallel for that. Audio Sink and Raw EFM Data Sink remain useful when you want a WAV or a standalone `.efm` somewhere other than beside the TBC, or when you want a second channel pair exported as well.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
