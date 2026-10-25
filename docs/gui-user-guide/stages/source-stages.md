# Source stages

Source stages are the **starting point of every decode-orc pipeline**. They load captured video (and any associated audio or disc data) from disk and make it available for processing.

You normally use **one source stage per capture**. If you have multiple captures of the same material, you add multiple source stages and combine them later using transform stages.

Source stages do not improve or modify the signal. Their purpose is to:

* Load captured files correctly
* Validate the video system (PAL, NTSC, or PAL-M)
* Keep video, audio, and disc data synchronised

> **Project format note:** Decode-Orc 2.0 requires that every project declares a `video_format` (PAL, NTSC, or PAL-M) and a `source_format` (Composite or YC) at creation time. These fields are read-only after the project is created. The stage picker shows only source stages that match the declared format: the per-system variants (CVBS Source and the two stream sources) appear only in a project of their video system, and the stream sources and VBI Capture Source, which carry composite data only, appear only in composite projects.

| Stage | Reads | Use |
|-------|-------|-----|
| [TBC Source](#tbc-source) | `.tbc` (or `.tbcy`/`.tbcc`) + `.tbc.db` from ld-decode/vhs-decode | Most LaserDisc and tape decodes |
| [TBC Stream Source](#tbc-stream-source) | Composite TBC fields from stdin or a named pipe | CLI pipelines only |
| [CVBS Source](#cvbs-source) | `.cvbs` (or `.cvbsy`/`.cvbsc`) + `.meta` | CVBS file-format captures and exports |
| [CVBS Stream Source](#cvbs-stream-source) | Composite CVBS samples from stdin or a named pipe | CLI pipelines only |
| [VBI Capture Source](#vbi-capture-source) | Raw VBI line dumps (bt8x8, cx2388x, cx23885, `.tbc` crops) | Teletext/NABTS from VBI-only captures |

---

## TBC Source

| | |
|-|-|
| **Stage id** | `tbc_source` |
| **Stage name** | *derived from metadata at load time* (e.g. `PAL TBC Composite`, `NTSC TBC YC`) |
| **Connections** | No inputs → 1 output |
| **Purpose** | Load TBC files produced by ld-decode or vhs-decode |

**Use this stage when:**

* Your capture comes from a LaserDisc or colour-under tape format
* You decoded the RF capture using ld-decode or vhs-decode into `.tbc` files

**What it does**

This stage reads one or more TBC files, detects the video system and signal type (composite or Y/C) from the `.tbc.db` metadata database, and assembles full-frame CVBS_U10_4FSC buffers for downstream processing. The stage display name is resolved at load time from the metadata (`PAL TBC Composite`, `NTSC TBC YC`, etc.).

All TBC level values are remapped from the ld-decode/vhs-decode internal 16-bit domain to the CVBS_U10_4FSC 10-bit domain. PAL frames have exactly 709,379 samples; NTSC frames have 477,750 samples; PAL-M frames have 477,225 samples.

Associated audio (analogue `.pcm`), EFM disc data (`.efm`), and AC3 RF symbols (`.ac3sym`) are attached when their path parameters are set and the files exist. The sidecars are not searched for at run time: when you pick the `.tbc` (or `.tbcy`/`.tbcc`) file in **Edit Parameters...**, the dialog fills in any empty sidecar path for which a file with the same base name exists, and from a script you set them yourself (see below). When a `.pcm` sidecar is present it becomes **audio channel pair 0** (named from `pcm_name`, default `Analogue`). The `.pcm` sidecar — raw signed 16-bit little-endian stereo PCM, nominally 44100 Hz as written by ld-decode — is always converted on ingest to the pipeline's only audio form: 48 kHz synchronous (frame-locked) 24-bit stereo per SMPTE 272M (widened to 24-bit and resampled with SoXR HQ). The conversion is deferred until audio is first read, so video-only preview never pays for it. When the TBC metadata records the PCM parameters, the declared sample rate is used as the resampler's input rate; an unsupported layout (unsigned, big-endian, or a bit depth other than 16) raises an error observation and disables audio.

EFM is exposed downstream (for example to the EFM Decoder Sink) only when the `.efm` file exists **and** the metadata carries per-field T-value counts, which is how the stage finds each frame's T-values; AC3 RF follows the same rule with per-field symbol counts. Unlike CVBS captures, TBC captures have no separate `.efm.meta` index.

Dropouts recorded in the TBC metadata are passed downstream as dropout hints. Fields the decoder marked as padding (`pad`, written when the player skipped) are honoured: a frame whose two fields are both padding is flagged as a padding frame, so the observers record it as "padded, no data" and padding-aware stages (Stacker, TBC Sink) skip it. Colour-sequence phase is never taken from the TBC metadata; it is measured from the burst downstream.

**Composite variant user-facing inputs**

* **TBC file** (`.tbc`)
* Accompanying metadata database (`.tbc.db`)
* PCM audio file (`.pcm`, optional)
* EFM data file (`.efm`, optional)
* AC3 RF symbols file (`.ac3sym`, optional)

**Y/C variant user-facing inputs**

* **Luma (Y) file** (`.tbcy`)
* **Chroma (C) file** (`.tbcc`)
* Accompanying metadata database (`<base>.tbc.db` beside the `.tbcy`/`.tbcc` pair)
* PCM audio file (`.pcm`, optional)
* EFM data file (`.efm`, optional)
* AC3 RF symbols file (`.ac3sym`, optional)

**Parameters**

The file-path parameters offered match the project's source type: a composite project shows only `input_path`, while a Y/C project shows only `y_path` and `c_path`.

* `input_path` (file path, **TBC File Path**) — Composite `.tbc` file. Composite projects only.
* `y_path` / `c_path` (file paths, **Luma TBC File Path (YC)** / **Chroma TBC File Path (YC)**) — Luma `.tbcy` and chroma `.tbcc` files. Y/C projects only; set together. Changing one in the dialog updates the other to the same base name.
* `pcm_path` (file path, **PCM Audio File Path**) — Analogue audio `.pcm` sidecar (raw signed 16-bit stereo PCM as written by ld-decode). Becomes channel pair 0, converted to 48 kHz frame-locked 24-bit stereo. Optional; empty means no audio.
* `pcm_name` (string, **Audio Channel Pair Name**) — Name for the analogue audio channel pair (shown in the CVBS container and as the Video Sink stream title). Default `Analogue`; empty also uses `Analogue`.
* `efm_path` (file path, **EFM Data File Path**) — EFM t-value `.efm` sidecar. Optional.
* `ac3rf_path` (file path, **AC3 RF Symbols File Path**) — AC3 RF symbols `.ac3sym` sidecar. Optional.

**Notes**

* The stage accepts any video system; it reads PAL, NTSC or PAL-M from the metadata rather than declaring one, so it is offered in every project.
* NTSC-J sources are detected automatically from the metadata levels: when picture black sits at the 0 IRE blanking level instead of the 7.5 IRE setup pedestal, the stage reports the non-standard black level downstream through a per-frame black level override.
* Legacy `.tbc.json` metadata produced by older ld-decode/vhs-decode versions is used when no `.tbc.db` is present (the log says so). Parsing a large `.tbc.json` is slow, so re-decoding with a current version (which produces `.tbc.db`) is recommended.
* The status dot is green when the TBC file and its metadata are readable, yellow when the TBC file is present but its metadata is missing or unreadable (the stage cannot run until it is available), and red when no path is set or the path does not point to an accessible file.

**From a script**

To point a template project at a new decode, set the file paths with `orc-cli nodes set` (see [Editing project nodes](../../cli-user-guide/overview.md#editing-project-nodes)):

```bash
# Composite project
orc-cli nodes set disc.orcprj "Disc 1" input_path=/decodes/disc1.tbc \
    pcm_path=/decodes/disc1.pcm efm_path=/decodes/disc1.efm
# Y/C project
orc-cli nodes set disc.orcprj "Disc 1" y_path=/decodes/disc1.tbcy c_path=/decodes/disc1.tbcc
```

The keys are `input_path` (composite) or `y_path` and `c_path` (Y/C), plus the optional `pcm_path`, `efm_path` and `ac3rf_path` sidecars. The metadata (`.tbc.db`, or `.tbc.json`) must sit beside the `.tbc` file; without it the stage cannot run, so check for it before queuing a decode.

---

## TBC Stream Source

| | |
|-|-|
| **Stage id** | `pal_tbc_stream_source`, `ntsc_tbc_stream_source`, or `palm_tbc_stream_source` (one variant per video system; the stage picker offers the one matching the project) |
| **Stage name** | `PAL TBC Stream Source`, `NTSC TBC Stream Source`, or `PAL-M TBC Stream Source` |
| **Connections** | No inputs → 1 output |
| **Purpose** | Read composite TBC video sequentially from standard input or a named pipe, for CLI pipelines |

**Use this stage when:**

* TBC data is arriving through a pipe rather than sitting in a file on disk — a live ld-decode/vhs-decode process, or a producer that cannot (or should not) write an intermediate file
* You are running the pipeline with `orc-cli`, for example `producer | orc-cli project.orcprj --process`

If the data already exists as `.tbc` + `.tbc.db`/`.tbc.json` files, use [TBC Source](#tbc-source) instead: this stage exists specifically for the CLI, not as a general replacement.

**What it does**

This is the sequential counterpart to TBC Source: the same TBC-to-CVBS level mapping and frame assembly, but with no metadata sidecar, no audio, dropout, EFM or AC3 sidecars, no Y/C, no NTSC-J detection, and no random access — everything a single one-directional stream cannot carry.

Each frame is read as two consecutive TBC fields (field 1 then field 2) in strict forward order, with no seeking and no re-reads. As in ld-decode's own layout, both fields are stored at field 1's size, so field 2's trailing padding line is read and discarded. `black_16b_ire` and `white_16b_ire` define the linear map from the 16-bit TBC domain to the 10-bit CVBS domain, and the fields are assembled into one frame per the video system's convention (PAL: 313 + 312 lines; NTSC/PAL-M: 263 + 262 lines). Piping a real `.tbc` file through this stage reads byte-for-byte the same data as reading it with TBC Source, provided `frame_count` matches.

A background thread reads into a bounded ring buffer of `buffer_frames` frames, so the worker threads of a sink's export never block each other on I/O. A request for a frame that has already left the buffer window fails the run with an error naming the frame and the buffer size, rather than returning stale data.

Runs only from the CLI: `input_path=-` is the CLI process's own standard input, which a GUI process does not meaningfully have. The GUI's parameter editor still accepts and saves the value, since the supported workflow is to build the project in the GUI and run it with `orc-cli <project> --process`; the GUI cannot preview or trigger the stage while it is configured that way.

**Parameters**

* `input_path` (file path, **Input Path**) — Required. `-` reads standard input; a named pipe path also works. A regular file is also read sequentially, but for a file on disk TBC Source is the right stage. Composite only.
* `black_16b_ire` (integer, **Black Level (16-bit)**) — The capture's `black16bIre` value; copy it verbatim from the `.tbc.db`/`.tbc.json` metadata the producer would normally write. Default is the nominal level for the system: `16384` for PAL, `18048` for NTSC and PAL-M (the standard 7.5 IRE setup).
* `white_16b_ire` (integer, **White Level (16-bit)**) — The capture's `white16bIre` value, copied the same way. Default `54016` for PAL, `51200` for NTSC and PAL-M. Must be greater than `black_16b_ire`, or the stage refuses to run.
* `frame_count` (integer, **Frame Count**) — Total number of frames the input will provide. Default `0`, meaning unbounded: the stage reads until a clean end of stream. Without a sidecar or a seekable input the count cannot be measured the way TBC Source does.
* `buffer_frames` (integer, **Buffer Frames**) — Read-ahead depth of the ring buffer. Default `32`, minimum `1`. Every stage between this source and the piped endpoint must be answerable within this window at once; raise it if a downstream stage needs more temporal look-ahead or look-behind, or if export parallelism spreads frame requests wider than it.

**Notes**

* Video only: there is no audio, dropout, EFM or AC3 data, and no plan to carry audio through this stage.
* No Y/C support: a Y/C capture needs two files (`.tbcy`/`.tbcc`), which one stream cannot carry. The stage is offered only in composite projects.
* Standard NTSC/PAL-M setup is always assumed. A genuine NTSC-J capture piped through this stage decodes with a slightly wrong black level; use TBC Source for it.
* There is no colour-frame index measurement in the stage itself; it is done downstream by the same colour-frame-phase observer every other source goes through, so later stages behave the same whichever source produced the frame.
* If `frame_count` is set and the input turns out to be shorter, the run fails partway through with an "unexpected end of input" error rather than silently producing a truncated result. Left at `0`, the same short input is a normal, clean stop.
* Every configuration of this stage is streaming-compatible: access is always forward-only within the buffer window.
* A stalled or dead producer can leave the process waiting indefinitely on the blocked read, as with any blocking-stdio pipe consumer.
* Reading `-` or a named pipe, the stage can feed several sinks: the stream is read once and each sink gets the whole of it, the sinks running side by side at the pace of the slowest. A sink that stops early (fails, or refuses an unbounded source) does not hold the others up.
* Stage tools (the Disc Mapper, Source Alignment) cannot run on a project that reads from a stream source, because they read their input more than once; `orc-cli tools run` refuses with exit code `5`. Decode to files first.
* The status dot does not check the parameters: it is green whatever is set (an empty `input_path` is reported as an error only when the stage runs), and red in the GUI whenever `input_path` is `-` or a named pipe, because such a node can only be run from the CLI.

**From a script**

```bash
producer | orc-cli -i "pal_tbc_stream_source=input_path=-:black_16b_ire=16384:white_16b_ire=54016" \
    -o "video_sink=output_path=out.mkv"
```

See [Piping between processes](../../cli-user-guide/overview.md#piping-between-processes) for the rules every piped pipeline must follow.

---

## CVBS Source

| | |
|-|-|
| **Stage id** | `PAL_CVBS_Source`, `NTSC_CVBS_Source`, or `PAL_M_CVBS_Source` (one variant per video system; the stage picker offers the one matching the project) |
| **Stage name** | `CVBS Source` until a file is loaded, then derived from it (e.g. `PAL CVBS Composite`, `NTSC CVBS YC`) |
| **Connections** | No inputs → 1 output |
| **Purpose** | Load CVBS captures stored in the CVBS file-format family |

**Use this stage when:**

* Your source is a CVBS file (`.cvbs`, or a `.cvbsy`/`.cvbsc` pair for Y/C projects) rather than a TBC capture — for example one written by the CVBS Sink or by the decode-orc capture toolchain
* The file is on disk. For CVBS data arriving through a pipe on the command line, use [CVBS Stream Source](#cvbs-stream-source) instead

**What it does**

This stage reads CVBS payloads from `.cvbs` files (or `.cvbsy`/`.cvbsc` pairs) and normalises them to the CVBS_U10_4FSC 10-bit domain. By default the video system, sample encoding, and signal state are read from the `.meta` SQLite sidecar; because the CVBS file format declares metadata optional, the sample encoding can also be selected manually so that sources without a sidecar can be used.

With **Sample Encoding** at `From metadata`, the sidecar also supplies the frame count, the NTSC-J black level, and the decode provenance (the upstream decoder name and its git branch/commit, passed through unchanged). The `STANDARD_STABLE_LOCKED` and `STANDARD_STABLE_UNLOCKED` signal-state presets are accepted. Files in any other state (for example `STANDARD_RAW`, a `NONSTANDARD_*` sample rate, or the pre-v1.6.0 `*_TBC_*` preset names), or whose video standard does not match the stage variant, are rejected with a clear error before any frame data is returned, because the stage's frame geometry assumes time-base-stable samples at the standard 4fsc rate. When a sample encoding is selected manually the sidecar is ignored (it need not exist): the video standard comes from the stage variant, the signal is assumed to be `STANDARD_STABLE_LOCKED`, the frame count is measured from the file size, and audio channel pairs get derived names.

Phase lock is not required. Colour-sequence phase is measured from each frame's burst rather than read from the sidecar, so an unlocked source (for example monochrome material) decodes normally. Whether the stored content is one unbroken sequence is declared separately by the `sequence_continuous` metadata field (CVBS file format spec v1.6.0). Opening a source marked `sequence_continuous = FALSE` shows an advisory and the source loads: the marker means the content contains at least one discontinuity, which for a LaserDisc source usually means the player skipped or jumped during the decode. Run the Disc Mapper (right-click a Frame Map stage, then **Stage Tools > Disc Mapper**) to put the frames back into their recorded order before exporting, or from a script `orc-cli tools run <project> field_mapping --node <frame map> --apply`.

The following sample encodings are normalised automatically:

| Encoding | Normalisation |
|----------|---------------|
| `CVBS_U10_4FSC` | Identity (already 10-bit) |
| `CVBS_U16_4FSC` | `value = uint16_value / 64` |
| `CVBS_TPG21_4FSC` | `value = int16_value / 64 + 508` |
| `CVBS_S16_4FSC` | `value = int16_value / 32 + blanking_10bit` |

Associated sidecars are found beside the data file by its base name and loaded automatically if present: dropouts (`<base>.dropouts.meta`), audio (`<base>_audio_0.wav` … `_audio_7.wav`), EFM (`<base>.efm` with its `.efm.meta` index) and AC3 (`<base>.ac3` with its `.ac3.meta` index).

**Audio channel pairs**

Each `<base>_audio_N.wav` file (single-digit N, CVBS file format spec v1.4.0) becomes the pipeline audio channel pair with the same index. Numbers need not be contiguous: a missing intermediate number becomes a silent placeholder pair, so pipeline indices always match the container's. Pair names come from the `.meta` file's `audio_channel_pair` table; a file with no table row is reported as a warning and named `Channel pair N`. Each WAV header must be PCM, 2 channels, 48000 Hz, 24-bit signed little-endian — a mismatch is reported as an error, and non-conforming files are not converted. All pair files must be the same length, one cadence-sized block per frame; a length mismatch is a warning and short frames are served as silence. Because the container audio is already in the pipeline's form (48 kHz synchronous 24-bit stereo), it is read directly with no conversion.

**Parameters**

The file-path parameters offered match the project's source type: a composite project shows only the CVBS file path, while a Y/C project shows only the Y (luma) and C (chroma) paths.

* `input_path` (file path, **CVBS File Path**)
    - Path to the composite data file (`.cvbs`). Composite projects only.

* `y_path` / `c_path` (file paths, **CVBS Y (Luma) File Path** / **CVBS C (Chroma) File Path**)
    - Paths to the luma (`.cvbsy`) and chroma (`.cvbsc`) channel files. Y/C projects only; set together. Changing one in the dialog updates the other to the same base name.

* `sample_encoding` (string, **Sample Encoding**)
    - `From metadata` (default) reads the encoding from the `.meta` sidecar.
    - Selecting `CVBS_U10_4FSC`, `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, or `CVBS_S16_4FSC` manually makes the sidecar optional.

**Notes**

* Colour-frame index (PAL: 1–4, NTSC: 0–1, PAL-M: 1–4) is measured from the colour burst of each frame by the host's colour-frame-phase observer, not read from the sidecar. Frames where the burst is absent or unmeasurable carry `colour_frame_index = -1`.
* The `lock_audio` parameter of earlier versions has been removed: all audio is carried as 48 kHz synchronous (frame-locked) 24-bit stereo channel pairs.
* The status dot is green when the data file is present and valid and the metadata is either valid or not needed (manual sample encoding); yellow when the data file is present but its `.meta` sidecar is missing or unreadable while **Sample Encoding** is `From metadata` (provide the sidecar or pick an encoding); and red when no path is set, the path is not accessible, or the file's video system does not match the stage.

**From a script**

To point a template project at a new capture, set the file paths with `orc-cli nodes set` (see [Editing project nodes](../../cli-user-guide/overview.md#editing-project-nodes)):

```bash
# Composite project
orc-cli nodes set disc.orcprj "Disc 1" input_path=/captures/disc1.cvbs
# Y/C project
orc-cli nodes set disc.orcprj "Disc 1" y_path=/captures/disc1.cvbsy c_path=/captures/disc1.cvbsc
```

The keys are `input_path` (composite) or `y_path` and `c_path` (Y/C). Audio, EFM and AC3 sidecars are found beside the capture, so they have no keys. Leave `sample_encoding` at `From metadata` so the `.meta` sidecar is read.

`orc-cli nodes list` reports each CVBS source's signal state and `sequence_continuous` (in `--json` output as `signal_state` and `sequence_continuous`), so a script can see which captures need the Disc Mapper before stacking them. The report of `orc-cli tools run` carries the same two values for the sources a tool reads from.

---

## CVBS Stream Source

| | |
|-|-|
| **Stage id** | `pal_cvbs_stream_source`, `ntsc_cvbs_stream_source`, or `palm_cvbs_stream_source` (one variant per video system; the stage picker offers the one matching the project) |
| **Stage name** | `PAL CVBS Stream Source`, `NTSC CVBS Stream Source`, or `PAL-M CVBS Stream Source` |
| **Connections** | No inputs → 1 output |
| **Purpose** | Read composite CVBS samples sequentially from standard input or a named pipe, for CLI pipelines |

**Use this stage when:**

* Composite CVBS data is arriving through a pipe rather than sitting in a file on disk — a live capture process, another `orc-cli` run writing a CVBS Sink to `-`, or a producer that cannot (or should not) write an intermediate file
* You are running the pipeline with `orc-cli`

If the data already exists as a `.cvbs` file, use [CVBS Source](#cvbs-source) instead: this stage exists specifically for the CLI, not as a general replacement.

**What it does**

This is the sequential counterpart to CVBS Source: the same sample domain and normalisation, but with no `.meta` sidecar, no audio, no dropout, EFM or AC3 sidecars, no Y/C and no random access.

Each frame is read as one full frame of 16-bit sample words for the stage's video system, in strict forward order with no seeking and no re-reads, and normalised exactly as CVBS Source does (see the encoding table above). A background thread reads into a bounded ring buffer of `buffer_frames` frames, so the worker threads of a sink's export never block each other on I/O. A request for a frame that has already left the buffer window fails the run with an error naming the frame and the buffer size, rather than returning stale data.

Runs only from the CLI: `input_path=-` is the CLI process's own standard input, which a GUI process does not meaningfully have. The GUI's parameter editor still accepts and saves the value, since the supported workflow is to build the project in the GUI and run it with `orc-cli <project> --process`; the GUI cannot preview or trigger the stage while it is configured that way.

**Parameters**

* `input_path` (file path, **Input Path**) — Required. `-` reads standard input; a named pipe path also works. A regular file is also read sequentially, but for a file on disk CVBS Source is the right stage.
* `sample_encoding` (string, **Sample Encoding**) — Required, because there is no sidecar to read it from: `CVBS_U10_4FSC`, `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, or `CVBS_S16_4FSC`. Any other value is rejected.
* `frame_count` (integer, **Frame Count**) — Total number of frames the input will provide. Default `0`, meaning unbounded: the stage reads until a clean end of stream. Without a sidecar or a seekable input the count cannot be measured from the file size the way CVBS Source does.
* `buffer_frames` (integer, **Buffer Frames**) — Read-ahead depth of the ring buffer. Default `32`, minimum `1`. Every stage between this source and the piped endpoint must be answerable within this window at once; raise it if a downstream stage needs more temporal look-ahead or look-behind, or if export parallelism spreads frame requests wider than it.

**Notes**

* Video only: there is no audio, dropout, EFM or AC3 data, and no plan to carry audio through this stage.
* Composite only; the stage is offered only in composite projects. (A Y/C CVBS Sink can feed it only with `combine_yc=true`, which sums the two channels into one composite stream.)
* There is no colour-frame index measurement in the stage itself; it is done downstream by the same colour-frame-phase observer every other source goes through, so later stages behave the same whichever source produced the frame.
* If `frame_count` is set and the input turns out to be shorter, the run fails partway through with an "unexpected end of input" error rather than silently producing a truncated result. Left at `0`, the same short input is a normal, clean stop.
* Every configuration of this stage is streaming-compatible: access is always forward-only within the buffer window.
* A stalled or dead producer can leave the process waiting indefinitely on the blocked read, as with any blocking-stdio pipe consumer.
* Reading `-` or a named pipe, the stage can feed several sinks: the stream is read once and each sink gets the whole of it, the sinks running side by side at the pace of the slowest. A sink that stops early (fails, or refuses an unbounded source) does not hold the others up.
* Stage tools (the Disc Mapper, Source Alignment) cannot run on a project that reads from a stream source, because they read their input more than once; `orc-cli tools run` refuses with exit code `5`. Decode to files first.
* The status dot does not check the parameters: it is green whatever is set (an empty `input_path` is reported as an error only when the stage runs), and red in the GUI whenever `input_path` is `-` or a named pipe, because such a node can only be run from the CLI.

**From a script**

```bash
orc-cli -i "tbc_source=input_path=capture.tbc" -o "CVBSSink=output_path=-" \
  | orc-cli -i "ntsc_cvbs_stream_source=input_path=-:sample_encoding=CVBS_U10_4FSC" \
            -o "video_sink=output_path=-" \
  | ffplay -
```

See [Piping between processes](../../cli-user-guide/overview.md#piping-between-processes) for the rules every piped pipeline must follow, including fan-out to several sinks.

---

## VBI Capture Source

| | |
|-|-|
| **Stage id** | `vbi_source` |
| **Stage name** | VBI Capture Source |
| **Connections** | No inputs → 1 output |
| **Purpose** | Ingest raw VBI teletext captures by laying their lines onto CVBS frames at the timing point and amplitude the standard puts them at |

**Use this stage when:**

* Your material is a raw VBI dump rather than a decoded capture — a bt8x8 card dump (`.vbi`, commonly FLAC-compressed as `.vbi.flac`) from a PAL, a SECAM or a 525-line source, a cx2388x (CX23880/1/2/3, Linux `cx88`) or cx23885 card dump from a 525-line source, or the VBI lines cropped off a decoded `.tbc` (which circulate as `.flac`, `.u16` or `.tbc`)

If you have a whole `.tbc` or `.cvbs` capture, use [TBC Source](#tbc-source) or [CVBS Source](#cvbs-source) instead — those carry a picture. The stage is offered only in composite projects.

**What it does**

A raw VBI capture holds nothing but the vertical-blanking line records: no sync, no burst, no picture, and no timing reference of any kind. This stage reads those records and places them on otherwise blank CVBS frames at the data service's own time from 0H. The result is an ordinary CVBS_U10_4FSC representation, so the existing teletext decoders see exactly what they see from a native decode. Nothing else about the raster is manufactured: no stage that reads this output looks for sync, a vertical interval or a burst, so synthesising them would cost far more than placing the data does and be spent entirely on samples nobody reads.

Frames are built lazily, one at a time, as they are asked for: the full-frame expansion of a raw capture is 21.6× its size, so a four-hour bt8x8 capture would be 522 GB if it were materialised.

Records are level-mapped from the card's relative levels into the CVBS amplitude domain (logic 0 from the quiet region ahead of the clock run-in, logic 1 from the larger of the run-in peaks and the framing code's leading ones), resampled onto the 4×fsc lattice with a band-limited filter, and placed at the data service's nominal time from 0H. The mapping is linear and nothing else is done to the samples — a deconvolving slicer downstream recovers data by matching the blurred waveform it is given.

Because no capture format records the time from 0H to sample 0 of a record, and the card families' documented figures are unreliable, the stage measures it: records sampled from across the whole capture are correlated against a generated clock-run-in and framing-code template and the median becomes a single global offset. A fit that fails its health checks stops the run rather than decoding hours of material at a wrong offset.

A capture cropped from a decoded `.tbc` needs the opposite: its records start at 0H exactly, so its offset is known and never fitted, but the time at which the *broadcaster* transmitted the run-in is not — and the tabulated 525-line figures were measured on particular captures that others disagree with by up to a microsecond. The same survey is therefore run and read as the service anchor instead, so the written region is cut from where the run-in actually is rather than where the table predicts. Without it, a transmission a microsecond early has the head of every run-in replaced by blanking. Here a failed fit is not fatal — it leaves the tabulated figure standing, which is what earlier versions always used — and the log says which was used.

Only the data region of each line is written — the region the standard gives the packet, plus one bit period of guard at each end — and every sample is clamped into the legal 10-bit range. A capture already sampled at 4×fsc with its records starting at 0H (the `.tbc` crops) is copied rather than filtered. Records are never dropped or reordered, so a packet recovered downstream resolves back to the source bytes it came from.

The last four bytes of every bt8x8 frame are the driver's frame sequence number. Comparing it at the two ends of the capture says how many frames were dropped across the whole of it without reading the capture through, which is what the `drops` policy acts on.

**The capture formats**

Everything about a capture — its geometry, its sampling rate, the data service it carries, where 0H is, what its logic levels mean, which field it starts on — is a property of the format rather than something you could be expected to know, so it all follows from the format you pick. Only the formats belonging to the project's television system are offered.

| Format | Projects | What it is |
|--------|----------|------------|
| `bt8x8 card dump, 8-bit (WST)` | PAL | A 625-line capture-card dump: 2048 samples per record of which 2044 are real, unsigned 8-bit, at 8×fsc (35 468 950 Hz); 16 records per field carrying field lines 7–22, the whole of the WST line list |
| `bt8x8 card dump, 8-bit (WST, SECAM source)` | PAL | The same container, byte for byte, from a SECAM source — see below |
| `bt8x8 card dump, 8-bit (WST, NTSC source)` and `(NABTS, NTSC source)` | NTSC | The same card on the driver's other television norm, which is not the same container: 2048 samples per record of which 1600 are real, unsigned 8-bit, at 8×fsc NTSC (28 636 363 Hz); 16 records per field of which the first 12 carry field lines 10–21, the whole of the 525-line teletext list — see below |
| `cx2388x card dump, 8-bit (WST, NTSC source)` and `(NABTS, NTSC source)` | NTSC | A 525-line capture-card dump from a Conexant CX23880/1/2/3 card: the bt8x8 NTSC clock and record — 2048 samples of which 1600 are real, unsigned 8-bit, at 28 636 363 Hz — but 12 records per field carrying field lines 10–21, and no frame counter — see below |
| `cx23885 card dump, 8-bit (WST)` and `(NABTS)` | NTSC | A 525-line capture-card dump from a Hauppauge HVR-1250 or sibling: 1440 samples per record with no padding, unsigned 8-bit, at 27 MHz; 12 records per field carrying field lines 10–21, the whole of the 525-line teletext list — see below |
| `.tbc VBI crop, 16-bit (WST)` and `(NABTS)` | NTSC | The first 16 line records of each field of a decoded 525-line luma `.tbc`: 910 samples per record with no padding, unsigned 16-bit, at 4×fsc, records 1–12 carrying field lines 10–21 |

There is deliberately no "custom" entry. Every fact behind these formats had to be measured off real captures — none of it is recoverable from the file, and guessing at it produces output that looks right and is not.

**bt8x8 dumps of a 525-line source**

The same card and the same driver as the PAL entries, and not the same container. The `bttv` driver's NTSC television norm has its own sampling clock (28 636 363 Hz, 8×fsc NTSC), its own `vbipack` of 144 — so 1600 of the 2048 samples per record are real rather than 2044 — and its own `vbistart` of `{10, 273}` rather than `{7, 320}`. What carries over is the card's geometry: the 2048-byte record stride, the sixteen records a field stores, the 65 536-byte frame and the driver's frame counter at the tail of it, which lands in the last record's padding here as it does on PAL. Unlike the other 525-line containers, then, **a bt8x8 dump of a 525-line source can report dropped frames**.

`vbistart` opening at field line 10 is exactly where the 525-line teletext list starts, so records 0–11 carry field lines 10–21 and 273–284 and records 12–15 are the start of the picture. Two calibration thresholds differ from the PAL entries and no others: this entry expects the run-in on a tenth of the sampled records rather than a quarter, for the same reason the cx23885 entry does, and its spread limit is the PAL entry's 226 ns expressed at this norm's slower clock — 6,5 samples rather than 8. The reference capture, a 1984 off-air recording of TBS Keyfax, locks on 33,1% of its records with a spread of 3,3 samples and fits its offset at 249,1 samples against the driver's folkloric 244.

Choose the service to match the broadcast, as on any card capture: the offset is fitted against the service's own run-in template, so the wrong choice fails the calibration rather than quietly mis-slicing the data.

**cx2388x card dumps**

The Conexant CX23880/1/2/3 family, served by the Linux `cx88` driver. Its line-mode VBI is sampled at 8×fsc (data sheet §2.6.16), which on 525 lines is the card's 28 636 363 Hz crystal itself, into the same 2048-byte record and the same 1600 real samples as a bt8x8 NTSC dump. What differs is the field: the driver stores twelve records from field lines 10 and 273, so every record is one of field lines 10–21 and 273–284, and a frame is 49 152 bytes. The padding after the real samples holds a constant rather than a sequence number, so a cx2388x dump cannot report dropped frames.

The driver's reported offset of 244 samples is the starting point and the real value is measured from the clock run-in; the calibration thresholds are the bt8x8 NTSC entry's. The reference capture, an off-tape recording of CBS ExtraVision, carries NABTS on field lines 15–18 of both fields, locks on 36,2% of its records with a spread of 2,6 samples and fits its offset at 230,1 samples. Choose the service to match the broadcast, as on any card capture. There is no 625-line cx2388x entry yet: no 625-line dump has been measured to confirm that norm's container.

**cx23885 card dumps**

The other card container, and nothing like the bt8x8 one. Its figures are the Linux `cx23885` driver's own: 27 MHz, 1440 samples per record, 12 records from field line 10. 1440 samples at 27 MHz is exactly the ITU-R BT.601 digital active line written at twice the 13,5 MHz rate, so what the card hands over is the active line and nothing else — no sync, no burst, no padding, and therefore nowhere for a frame counter to live. A cx23885 dump cannot report dropped frames.

Where the window sits relative to 0H is neither in the file nor usefully in the driver, whose reported offset is a stub zero, so the configured figure is the window SMPTE 125M says the timing generator produces (122 samples at 13,5 MHz after 0H, 244 here) and the real value is measured from the clock run-in as on any card capture. A US network carried its magazine on two or three of the twelve lines, so this entry expects the run-in on a tenth of the sampled records rather than the quarter the PAL entries ask for; every other health check is the bt8x8 one expressed at this card's sampling rate.

Choose the service — WST or NABTS — to match the broadcast. Getting it wrong on a card capture fails the calibration outright and stops the run with a diagnostic, because the offset is fitted against the service's own run-in template; that is intended, and makes the wrong choice loud rather than silent.

**`.tbc` VBI crops: WST or NABTS?**

Pick the entry matching the service the broadcast carried. Nothing in the file records it: the two share the same lines and bit rate and differ only in framing code (`0xE4` against `0xE7`) and packet length. Of the material in circulation, TBS Electra is **WST**, and CBS ExtraVision and NBC Teletext are **NABTS** — US network teletext was NABTS almost throughout. The filename is usually the best evidence you have. Picking the wrong one is not a disaster here: the record is copied index for index, so only about fourteen samples at the front and six at the back of the copied region change, and the choice is passed downstream so the decoder knows which service it was handed.

**SECAM sources**

Pick the SECAM entry when the capture came from a SECAM broadcast or tape. The container is identical — the driver's SECAM television norm shares the PAL one's sampling clock, its `vbipack` and its `vbistart` — and post-decode SECAM is a 625-line signal carrying the same World System Teletext, which is why it is placed on PAL frames.

What differs is how much of the line list can carry teletext. A SECAM transmission with vertical colour identification puts the identification signal (the "green bottles") on field lines 8–15 and 321–328, so half the records of every stored field are spoken for before any teletext is inserted, and broadcasters using those idents typically left the remainder to test signals and a very few teletext lines. The SECAM entry therefore expects the clock run-in on far fewer of the sampled records before it trusts the calibration; every other health check is the same. Using the PAL entry on such a capture gets a perfectly good fit rejected on the line count alone.

**Parameters**

* `input_path` (file path, **VBI Capture Path**) — Path to the raw capture. FLAC-wrapped captures are unwrapped transparently; the wrapper's declared sample rate is a placeholder and is never used for timing. The browse dialog filters on `.vbi`, `.flac`, `.u8`, `.u16` and `.tbc`, but nothing is read from the file name, so anything else can be picked through the All Files filter.
* `format` (string, **Capture Format**) — Which of the capture formats above the file is. Only the formats for the project's television system are offered. Default `bt8x8 card dump, 8-bit (WST)` on a PAL project and `.tbc VBI crop, 16-bit (WST)` on an NTSC project.
* `drops` (string, **Dropped Frames**) — `preserve` (default) emits only the frames present, so output frame numbering no longer matches the capture's own after a drop; `pad` emits a blank frame in each gap, flagged as carrying no source data, so output frame *n* stays aligned with source frame *n*. A format carrying no frame counter (cx2388x and cx23885 dumps, `.tbc` crops) cannot report drops at all, so neither policy has anything to act on, and the stage says so.

**Notes**

* Nothing is written to disk by this stage. To export the frames as a `.cvbs` + `.meta` pair, connect a CVBS Sink. Bear in mind that what is exported is a legal CVBS file carrying teletext on an otherwise blank raster, not a reconstruction of the broadcast the capture was cut from.
* A capture that ends short of a whole frame — on an odd field, as the `.tbc` crops usually do, or part-way through a line record, as a card dump stopped at the keyboard does — is loaded normally. The trailing bytes cannot make a frame, so they are not emitted, and the log says what was dropped. Only a file too short to hold a single whole frame is refused. A wrong format choice is caught by the capture-offset fit, not by the file length: a wrong stride, sampling rate or television system scatters the clock run-in far beyond the fit's limits and stops the run with a diagnostic.
* Which frame lines carry data follows from the television and teletext systems (WST occupies broadcast frame lines 7–22 and 320–335) and from which stored records the container declares as data records; it is not configured directly. A capture holding more data records than the standard defines is reported as an error rather than truncated.
* The status dot is green when the capture is accessible and a known format is chosen (the capture's length and word size are checked against the format when the stage runs), and red when no path is set, the path is not accessible, or the format is not one of the listed entries.
