# CVBS Stream Source

Reads composite video sequentially from standard input (or a real named pipe) instead of a random-access file, for use in a CLI pipeline such as `producer | orc-cli process project.orc-project`. This is the sequential counterpart to CVBS Source: same sample domain and normalisation, but no `.meta` sidecar, no audio, no dropout/EFM/AC3 sidecars, and no random access — everything that convention cannot carry through a single, one-directional stream.

## When to use

Use this instead of CVBS Source when the composite data is arriving from a pipe rather than sitting in a file on disk — a live capture process, or a producer that cannot (or should not) write an intermediate file. If the data already exists as a `.cvbs` file, use CVBS Source instead: this stage exists specifically for the CLI, not as a general replacement.

Runs only via the CLI: `input_path=-` reads from the CLI process's real standard input, which a GUI process does not meaningfully have. The GUI's parameter editor still accepts and saves the value, since the officially supported workflow is to build the project in the GUI and run it via `orc-cli ... --process` — the GUI itself just cannot preview or trigger this stage while it is configured that way.

## Parameters

| Parameter | Meaning |
|-----------|---------|
| Input Path (`input_path`) | `-` reads from standard input; a real named pipe path also works. Required — there is no file-based fallback. |
| Sample Encoding (`sample_encoding`) | `CVBS_U10_4FSC`, `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, or `CVBS_S16_4FSC`. Required — there is no sidecar to read it from. |
| Frame Count (`frame_count`) | Total number of frames the input will provide. Required — with no sidecar and no seekable input, this cannot be measured from a file size the way CVBS Source does. |
| Buffer Frames (`buffer_frames`) | Read-ahead depth of the internal ring buffer. Default `32`. Every stage between this source and the piped endpoint has to be answerable from within this window at once; raise it if a downstream decoder needs more temporal lookahead/lookbehind than the default covers, or if export parallelism spreads frame requests wider than it. |

## What it does

Reads exactly `frame_count` frames from `input_path`, each `frame_samples_from_system(video_system)` 16-bit words, in strict forward order — no seeking, no re-reads. A background thread performs the actual reads into a bounded ring buffer sized by `buffer_frames`, so callers (including the several worker threads a video sink's export can use) never block each other on I/O; a request for a frame already outside the buffer window fails the export with a clear error naming the frame and the buffer size, rather than returning stale or corrupt data.

Sample normalisation matches CVBS Source exactly:

- `CVBS_U10_4FSC` — identity transform; values are already in the 10-bit domain.
- `CVBS_U16_4FSC` — divide the 16-bit unsigned value by 64.
- `CVBS_TPG21_4FSC` — divide the signed 16-bit value by 64 and add 508.
- `CVBS_S16_4FSC` — divide the signed 16-bit value by 32 and add the 10-bit blanking level.

There is no colour-frame index measurement from the burst here — that happens later, in the same `colour_frame_phase` observer every other source's frames go through, so behaviour downstream is unaffected by which source produced the frame.

## Notes

- No audio, dropout correction sidecar, EFM, or AC3 extension data. This is a video-only source, and there is no plan to carry audio through this stage.
- If the actual input is shorter than `frame_count` declares, the export fails partway through with an "unexpected end of input" error rather than silently producing a truncated result.
- Every configuration of this stage reports itself streaming-compatible (see `IStreamingCompatibility` in the plugin SDK) — unlike CVBS Source, there is no parameter combination here that isn't safe to pipe, since `frame_count` is always explicit and access is always forward-only within the buffer window.
- Ending the process while the reader thread is blocked waiting for more input that never arrives (a stalled or dead producer) can leave the process waiting indefinitely on that read — the same limitation any blocking-stdio pipe consumer has.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. Set `input_path`, `sample_encoding`, and `frame_count` before triggering. |

Parameters can be set via **Edit Parameters...** in the node context menu, or from the CLI project file directly.
