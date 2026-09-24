# TBC Stream Source

Reads composite TBC video sequentially from standard input (or a real named pipe) instead of a random-access file plus its metadata database, for use in a CLI pipeline such as `producer | orc-cli process project.orc-project`. This is the sequential counterpart to TBC Source: same TBC-to-CVBS level mapping and frame assembly, but no `.tbc.db`/`.tbc.json` sidecar, no audio, dropout, EFM, or AC3 sidecars, no Y/C, no NTSC-J auto-detection, and no random access — everything that convention cannot carry through a single, one-directional stream.

## When to use

Use this instead of TBC Source when the TBC data is arriving from a pipe rather than sitting in a file on disk — a live ld-decode/vhs-decode process, or a producer that cannot (or should not) write an intermediate file. If the data already exists as `.tbc` + `.tbc.db`/`.tbc.json` files, use TBC Source instead: this stage exists specifically for the CLI, not as a general replacement.

Runs only via the CLI: `input_path=-` reads from the CLI process's real standard input, which a GUI process does not meaningfully have. The GUI's parameter editor still accepts and saves the value, since the officially supported workflow is to build the project in the GUI and run it via `orc-cli ... --process` — the GUI itself just cannot preview or trigger this stage while it is configured that way.

## Parameters

| Parameter | Meaning |
|-----------|---------|
| Input Path (`input_path`) | `-` reads from standard input; a real named pipe path also works. Required — there is no file-based fallback. Composite only. |
| Black Level (16-bit) (`black_16b_ire`) | The capture's `black16bIre` value — copy it verbatim from the `.tbc.json`/`.tbc.db` sidecar the producer's own metadata would normally carry, for an accurate result. Optional: defaults to the nominal SMPTE/ITU-R level for this system (the standard 7.5 IRE setup for NTSC/PAL_M) when left unset, since most captures are close to nominal and a piped source has no metadata of its own to read this from. |
| White Level (16-bit) (`white_16b_ire`) | The capture's `white16bIre` value, copied the same way. Optional, same default reasoning as Black Level. |
| Frame Count (`frame_count`) | Total number of frames the input will provide. With no sidecar and no seekable input, this cannot be measured from a file size the way TBC Source does. Leave at `0` (the default) for an unbounded/live source: the stage then reads until the input reaches a clean end-of-stream instead of requiring an exact count up front. |
| Buffer Frames (`buffer_frames`) | Read-ahead depth of the internal ring buffer. Default `32`. Every stage between this source and the piped endpoint has to be answerable from within this window at once; raise it if a downstream decoder needs more temporal lookahead/lookbehind than the default covers, or if export parallelism spreads frame requests wider than it. |

## What it does

Reads exactly `frame_count` frames from `input_path`, each frame being two consecutive TBC fields (field 1 then field 2), in strict forward order — no seeking, no re-reads. Each field is stored at the same size on disk regardless of parity (ld-decode's own convention: field 2 is padded out to field 1's line count), so this stage reads that full stride for both fields and discards field 2's trailing padding line before assembling the frame — identical to how TBC Source reads the same on-disk layout via random access.

Level mapping and frame assembly exactly match TBC Source: the two TBC-domain levels (`black_16b_ire`, `white_16b_ire`) are converted to a linear map from the ld-decode 16-bit domain to the internal CVBS_U10_4FSC 10-bit domain, then each field's samples are mapped through it and the two fields are assembled into one frame per the video system's own field-ordering convention (PAL: 313+312 lines with EBU-3280 bridge samples; NTSC/PAL_M: 263+262 lines, orthogonal).

Standard NTSC/PAL_M setup is always assumed for the black level's 7.5 IRE pedestal — there is no per-capture NTSC-J auto-detection here (TBC Source measures this from the metadata; this stage has no metadata to measure it from). A genuinely NTSC-J capture piped through this stage will decode with a slightly incorrect black level; use TBC Source for that material instead.

There is no colour-frame index measurement from the burst here — that happens later, in the same `colour_frame_phase` observer every other source's frames go through, so behaviour downstream is unaffected by which source produced the frame.

## Notes

- No audio, dropout correction sidecar, EFM, or AC3 RF extension data. This is a video-only source, and there is no plan to carry audio through this stage.
- No Y/C support: a Y/C capture stores luma and chroma in two separate files (`.tbcy`/`.tbcc`), which a single `-` stream cannot carry. Composite only.
- If `frame_count` is set explicitly and the actual input is shorter, the export fails partway through with an "unexpected end of input" error rather than silently producing a truncated result. Left at `0` (unbounded), the same short input is a normal, clean stop instead — no error, no truncated frame written.
- Every configuration of this stage reports itself streaming-compatible (see `IStreamingCompatibility` in the plugin SDK) — there is no parameter combination here that isn't safe to pipe, since access is always forward-only within the buffer window regardless of whether `frame_count` is explicit or unbounded.
- Ending the process while the reader thread is blocked waiting for more input that never arrives (a stalled or dead producer) can leave the process waiting indefinitely on that read — the same limitation any blocking-stdio pipe consumer has.
- Reading `-` or a named pipe, this stage can feed several sinks: the stream is read once and each sink gets the whole of it, the sinks running side by side at the pace of the slowest. A sink that stops early (fails, or refuses an unbounded source) does not hold the others up.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. Set `input_path` before triggering (`black_16b_ire`, `white_16b_ire`, and `frame_count` are all optional — see their descriptions above for what leaving them unset means). |

Parameters can be set via **Edit Parameters...** in the node context menu, or from the CLI project file directly.
