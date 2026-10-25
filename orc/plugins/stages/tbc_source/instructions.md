# TBC Source

Reads one or more TBC files produced by ld-decode or vhs-decode and presents the decoded video as a frame-domain representation for downstream stages. The stage detects the video system (PAL, NTSC, PAL-M) and signal type (composite or Y/C) from the `.tbc.db` metadata database, remaps all sample levels from the ld-decode 16-bit internal domain to the 10-bit CVBS domain, and assembles full-frame buffers ready for processing.

## When to use

Add TBC Source as the first stage in any pipeline that starts from ld-decode or vhs-decode output. Select the composite `.tbc` file for a standard composite capture, or supply the `.tbcy` luma and `.tbcc` chroma files for a Y/C capture. The audio, EFM disc data and AC3 RF sidecars are set by their own path parameters. They are not searched for when the stage runs: when you pick the TBC file in **Edit Parameters...**, the dialog fills in any empty sidecar path for which a file with the same base name exists; from a script, set them explicitly. The metadata database must sit beside the TBC file (`<base>.tbc.db`, also for a `.tbcy`/`.tbcc` pair).

## Parameters

The file-path parameters offered match the project's source type: a composite project shows only **TBC File Path**, while a Y/C project shows only the **Luma** and **Chroma** paths.

| Parameter | Meaning |
|-----------|---------|
| TBC File Path (`input_path`) | Path to the composite `.tbc` file. Composite projects only. |
| Luma TBC File Path (`y_path`) | Path to the luma `.tbcy` file. Y/C projects only; set together with `c_path`. |
| Chroma TBC File Path (`c_path`) | Path to the chroma `.tbcc` file. Y/C projects only; set together with `y_path`. |
| PCM Audio File Path (`pcm_path`) | Path to the analogue audio `.pcm` sidecar (raw signed 16-bit stereo PCM as written by ld-decode). Always converted to 48 kHz 24-bit frame-locked audio on ingest. |
| Audio Channel Pair Name (`pcm_name`) | Human-readable name for the analogue audio channel pair; surfaces in the CVBS container and as the embedded stream title in the Video Sink. Empty uses "Analogue". |
| EFM Data File Path (`efm_path`) | Path to the EFM t-value `.efm` sidecar. |
| AC3 RF Symbols File Path (`ac3rf_path`) | Path to the AC3 RF symbols `.ac3sym` sidecar. |

## What it does

At execute time the stage opens the `.tbc.db` metadata database (falling back to legacy `.tbc.json` metadata produced by older ld-decode/vhs-decode versions) and reads the video system, field dimensions, and signal levels. It then selects the correct converter (PAL composite, PAL Y/C, NTSC composite, NTSC Y/C, or PAL-M composite) and reads each pair of TBC fields, remapping the 16-bit ld-decode levels to the 10-bit CVBS domain. The resulting full-frame buffers are assembled in order and returned as a VideoFrameRepresentation.

Frame sizes after assembly: PAL frames contain 709,379 samples; NTSC frames contain 477,750 samples; PAL-M frames contain 477,225 samples.

NTSC-J sources are detected automatically from the metadata signal levels: when the recorded picture black sits at the 0 IRE blanking level instead of the standard 7.5 IRE setup pedestal, the stage reports the non-standard black level to downstream stages (exactly as a CVBS source with an explicit NTSC-J black level would). Fields the decoder marked as padding (`pad` in the TBC metadata, written when the player skipped) are honoured: a frame whose two fields are both padding is flagged as a padding frame, so the host observer pass records "padded, no data" instead of measuring the synthetic content, and padding-aware stages (stacker, TBC Sink) skip it. Colour-sequence phase is never taken from the TBC metadata — stages that need it measure it from the burst via the host colour-frame-phase observer. Dropouts recorded in the metadata are passed downstream as dropout hints. Associated audio (`.pcm`), EFM disc data (`.efm`), and AC3 RF (`.ac3sym`) sidecars are attached to each frame when their path parameters are set and the files exist.

When a `.pcm` sidecar is present it becomes audio channel pair 0 (named from `pcm_name`, default "Analogue"). The sidecar — raw signed 16-bit little-endian stereo PCM, nominally 44100 Hz as written by ld-decode — is always converted on ingest to the pipeline's only audio form: 48 kHz synchronous (frame-locked) 24-bit stereo, per SMPTE 272M. Samples are widened to 24-bit and resampled with SoXR HQ (duration- and sync-preserving) for all systems, including PAL. The conversion is deferred until audio is first read, so video-only preview never pays for it. When the TBC metadata records the PCM parameters, the declared sample rate is honoured as the resampler input rate; an unsupported layout (unsigned, big-endian, or a bit depth other than 16) raises an error observation and disables audio.

The raw `.efm` sidecar stores one byte per EFM T-value in field order; the stage locates each frame's T-values using the per-field T-value counts recorded in the `.tbc.db` metadata. (Unlike CVBS captures, TBC captures have no separate `.efm.meta` index — the counts in the video metadata are authoritative.) EFM is therefore exposed to downstream stages, such as the EFM Sink, only when both the `.efm` file exists and the metadata carries T-value counts.

The display name shown in the pipeline is resolved at load time from the detected metadata, for example "PAL TBC Composite" or "NTSC TBC YC".

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. The TBC file and its metadata are present and readable. |
| Yellow | The TBC file is present but its metadata sidecar is missing or unreadable. The stage cannot run until the metadata is available. |
| Red | Not configured. No file path is set, or the configured path does not point to an accessible TBC file. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
