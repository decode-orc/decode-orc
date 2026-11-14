# Sink Stages

Sink stages are the **endpoints of a decode-orc pipeline**. They consume processed data from upstream stages and write results to disk. Unlike transform stages, sink stages do not produce outputs that can be connected further downstream.

A pipeline may contain **multiple sink stages** in parallel, allowing the same processed stream to be written in different formats or to different destinations.

Sink stages are used to:

* Write final video outputs (TBC + metadata, CVBS files, or encoded video)
* Export auxiliary data such as audio, EFM, AC3, or closed captions
* Export intermediate data for inspection or external tools

---

## AC3 RF Sink

| | |
|-|-|
| **Stage id** | `AC3RFSink` |
| **Stage name** | AC3 RF Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Decode AC3 RF (Dolby Digital) samples and write AC3 frames to file |

**Use this stage when:**

* Processing later North American NTSC LaserDiscs that carry AC3 RF 5.1 surround sound
* You want the extracted AC3 audio alongside the video output in a single pipeline trigger

**What it does**

This stage reads AC3 RF samples from the incoming stream, decodes the RF-modulated Dolby Digital bitstream frame by frame, and writes the resulting AC3 audio frames sequentially to the output file. The output is a raw AC3 elementary stream with no container wrapping; it can be played back directly or muxed into a video container.

**Parameters**

* `output_path` (file path)
    - Path to the output AC3 file. The conventional extension is `.ac3`. Created or overwritten at trigger time.
    - Required.

**Notes**

* The upstream source must supply AC3 RF data (the source stage must have been given an AC3 RF file); the pipeline will abort at trigger time if none is present.
* This stage is specific to AC3 RF as found on LaserDiscs; it does not handle AC3 carried in other formats or containers.

---

## Audio Sink

| | |
|-|-|
| **Stage id** | `AudioSink` |
| **Stage name** | Audio Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Export any pipeline audio channel pair to a WAV file |

**Use this stage when:**

* Your source carries audio channel pairs
* You want to export audio independently of video output
* You want to inspect or process audio externally

**What it does**

This stage extracts one audio channel pair from the incoming stream and writes it to a standard WAV file. The pair can be any channel pair carried by the pipeline — analogue capture audio, decoded EFM digital audio, an imported WAV, or a channel pair derived by a transform. Audio remains synchronised to the processed video timeline, so any frame trimming or reordering performed upstream is reflected in the output.

The pipeline carries stereo audio channel pairs at exactly 48,000 Hz, frame-locked (synchronous) to the video for every system, following SMPTE 272M-1994. The WAV output is 24-bit signed little-endian PCM declaring 48,000 Hz; no resampling or bit-depth conversion is performed. A standard (RIFF) WAV can hold at most 4 GiB of samples, about 4 hours 8 minutes of this audio. A longer export is written as RF64, the 64-bit form of WAV that most audio tools read, so the whole recording is kept.

**Parameters**

* `output_path` (file path)
    - Path to the output WAV file. Created or overwritten at trigger time.
    - Required.

* `channel_pair` (string)
    - Audio channel pair to write, as a 0-based index (`0`–`7`) matching the CVBS container's `_audio_<p>.wav` numbering. In the GUI the drop-down lists only the pairs the input actually carries, with their descriptions.
    - Default `0`. Triggering fails if the selected channel pair does not exist.

**Notes**

* This stage writes whatever channel pair you select. Analogue capture audio arrives as channel pair 0 from the source; EFM digital audio (CD-quality stereo) becomes a channel pair when you add an **EFM Audio Decode** transform upstream, after which it can be written here like any other pair. For a bit-exact, un-resampled WAV of EFM audio use the EFM Decoder Sink instead; AC3 RF (Dolby Digital) is exported via the AC3 RF Sink.
* Audio stacking or selection must be performed upstream (e.g. the Stacker's `audio_stacking` parameter).
* Frames without audio are written as silence of the correct length, preserving audio/video sync.

---

## Closed Caption Sink

| | |
|-|-|
| **Stage id** | `CCSink` |
| **Stage name** | Closed Caption Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Extract and write one NTSC Line 21 closed-caption (CC) service, or every one in a pass |

**Use this stage when:**

* Working with NTSC sources containing Line 21 closed captions
* You want to extract captions for archival or conversion
* You want to inspect CC data independently of video

**What it does**

Line 21 carries four services multiplexed into the same two bytes on each field: **CC1** and **CC2** (captions) and **TEXT1** and **TEXT2** (pages of text such as schedules, scores or station information) on the first, **CC3**, **CC4**, **TEXT3** and **TEXT4** on the second, which also carries the **Extended Data Service** (XDS: the station, the programme, the time of day). Which service a byte pair belongs to is decided by the control codes before it, so a recording that used more than one produces garbled output if they are read together — a caption running through a page of listings.

The stage reads the two caption bytes from each field's Line 21 through the host's closed-caption observer, which reads a line only when it finds the clock run-in and the start bits where the run-in puts them, and checks each byte's parity over all eight bits — so a field carrying nothing reads as nothing. It takes a pair when its parity holds (a character whose partner's parity failed is kept alone), when it is a code or characters EIA-608 defines for its field, and when its field carries Line 21 data around it; keeps the pairs belonging to the service you select; and writes them in the chosen format. A capture whose first stored field is television field 2 is recognised from the control codes and the Extended Data Service and read the other way round, so its CC1 still comes out as CC1. A recording whose time base slipped a line, so that its captions sit on line 22 or line 20, is read there where line 21 carries nothing for half a second either side and the line reads as a caption line over the run: twenty fields or more, three quarters of them with good parity, control codes among them.

**Parameters**

* `output_path` (string)
    - Path to the closed-caption output file. Use `.scc`, `.srt`, `.txt` or `.html` to match the format. With the service `All`, the folder the services' files are written into.
    - Required.

* `service` (string)
    - Which of the services multiplexed onto Line 21 to export.
    - Allowed values: `CC1` (the primary caption service), `CC2` (a second caption service, often a translation), `TEXT1`, `TEXT2`, the second field's `CC3`, `CC4`, `TEXT3`, `TEXT4`, `XDS` (exported as `Plain Text` or `HTML`: each packet whose value changed, with when it arrived), and `All`: every service in one pass over the recording, into the folder `output_path` names — `CC1.srt` to `CC4.srt` as SubRip, `TEXT1.txt` to `TEXT4.txt` and `XDS.txt` as plain text, each what that service's own export writes, a file only for each service that carried anything, and the report as `line21.report.txt`. `format` does not apply to `All`.
    - Default: `CC1`.

* `format` (string)
    - Export format. Default: `Scenarist SCC`.
    - `Scenarist SCC` — Scenarist SCC V1.0: `HH:MM:SS:FF` timecodes and the service's byte pairs as transmitted. The archival format; loads into caption-authoring tools.
    - `SubRip SRT` — numbered cues with `HH:MM:SS,mmm` times; plays in any modern media player.
    - `Plain Text` — each caption under a `[HH:MM:SS:FF]` timestamp, for reading.
    - `HTML` — a monospaced transcript, each caption in a `<pre>` block beside its timestamp; keeps the column layout a text service (listings, scores) needs.

* `write_report` (boolean)
    - Write a report of everything Line 21 carried beside the output as `<output>.report.txt` (`line21.report.txt` in the folder for `All`): the byte pairs each field yielded and those refused, the characters of all eight services, whether the fields were read the other way round, the XDS station, network, programmes and time of day, and the ATVEF Transport A triggers (`<url>[...][checksum]`) on TEXT2 — counted only when well formed and their checksum holds. The same report always goes to the log.
    - Default: `false`.

**Notes**

* Handles NTSC Line 21 only; PAL sources do not carry Line 21 CC data.
* A pair whose first byte's parity failed is dropped whole (it may have been a control code, whose second copy stands in); two bit errors in one byte still pass, as they do for any receiver.
* A byte with no function on its field is ignored alone and its partner kept [47 CFR 15.119(i)(1)]: a text service can end a row's last character with 01h.
* The video sink's embedded subtitles take their pairs by these same rules, field order and lines 22 and 20 included. The GUI's caption preview applies them too, except that a pair needs Line 21 data around it and the lines next to it are not read: those rules look at frames after the one the preview shows.
* To export every service, choose `All`; to export some and not others, add a Closed Caption Sink for each with its own output path.
* `Scenarist SCC` records the selected service's byte pairs exactly as transmitted, including the duplicate copy of each control code; the tools that read an SCC file de-duplicate for themselves. The decoded formats keep the caption display's rows on separate lines with their indent, so a text service's columns still line up. A mid-row code (a colour or italics change) takes a column as a space does and a tab offset moves along one to three columns, so an italicised word stays a word apart; a text service is read as the scrolling page CTA-608-E §7.4 describes, a preamble address code moving the cursor along the current row only. Preamble address codes are read as CTA-608-E Table 53 lays them out on both data channels (13h is rows 12 and 13), so a two-line caption on rows 13 and 14 keeps both lines.
* Each caption style is decoded as 47 CFR 15.119(f) and CTA-608-E describe it. A roll-up cue is its window — the base row and the rows above it to the depth RU2, RU3 or RU4 chose — which a carriage return scrolls and a preamble address code for another base row moves intact. A paint-on cue is the displayed memory as it is painted, rows in place. End of Caption swaps the two memories without erasing either, and one caption cursor carries on through the swap and through any erasure (CTA-608-E C.15). Resume Text Display continues a text row from the same cursor, so a row a caption interrupted and the text service sent again is one line, not a fragment and then the row.
* End of Caption, Erase Displayed Memory and Erase Non-Displayed Memory act on the caption memories even in the midst of a text stream (CTA-608-E §7.7, C.16), and the text stream runs on until a code selects a caption style: a text service of the 1980s sends EDM, EOC, EDM inside its stream.
* XDS values are decoded as CTA-608-E §9.5 codes them — the programme's length and time in show, its type's keywords, its content advisory (U.S. TV Parental Guidelines, MPA, or the Canadian English or French ratings), its audio and caption services and copy control, and the channel's tape delay and transmission signal identifier; private data stays in hexadecimal.
* The decoded formats are UTF-8: the music note (♪) and the other special characters, the extended characters (each in place of the standard character sent before it), and the ten places EIA-608's basic set is not ASCII (2Ah is "á", 7Fh the solid block).
* CC data must be preserved upstream — masking Line 21 (for example with Mask Line) before this stage will destroy the caption payload.
* If the source contains no CC data the output file will be empty but the stage will not abort.

---

## CVBS Sink

| | |
|-|-|
| **Stage id** | `CVBSSink` |
| **Stage name** | CVBS Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Write CVBS frames to a CVBS file-format family output |

**Use this stage when:**

* You want to archive or exchange a processed CVBS signal in the standard CVBS file format.
* You want to produce a `.cvbs` or `.cvbsy`/`.cvbsc` output that can be re-opened by the CVBS Source stage.
* You need to write associated dropout, audio, EFM, or AC3 sidecars alongside the video.
* You have a Y/C project and want a single composite (Y+C) signal — for example to play it out as composite video through a digital-to-analogue converter (see `combine_yc`).

**What it does**

This stage writes processed frame data using the selected sample encoding, and a `.meta` SQLite sidecar. The output signal type follows the project type automatically: a composite project is written as a single `.cvbs` file and a Y/C project as a `.cvbsy`/`.cvbsc` pair (per the CVBS file format naming convention). Y/C cannot be derived from a composite signal, so a composite project always writes `.cvbs`; the only choice is the other direction — `combine_yc` sums a Y/C project's two channels into a single composite `.cvbs`. The `.meta` file records the signal type, the selected `sample_encoding_preset`, and a measured `signal_state_preset` and `sequence_continuous` flag (CVBS file format spec v1.6.0). Neither is user-configurable: they describe the file that was actually written.

Associated sidecars are written automatically when the upstream source provides them:

- `.dropouts.meta` — when dropout hints are present
- `_audio_0.wav` … `_audio_7.wav` — when audio is present (one 24-bit 48 kHz stereo WAV per channel pair). The CVBS file format allows only standard (RIFF) WAV files here, which hold at most about 4 hours 8 minutes of this audio, so a longer export with audio is refused before anything is written. Export a longer capture in parts.
- `.efm` + `.efm.meta` — when EFM data is present
- `.ac3` + `.ac3.meta` — when AC3 RF data is present

A CVBS file written by this stage can be round-tripped back through the CVBS Source stage.

**Parameters**

* `output_path` (file path)
    - Base path for output files. The payload extension and `.meta` are appended automatically; a trailing `.cvbs`, `.cvbsy`, or `.cvbsc` extension is stripped when present.
    - Required.

* `sample_encoding` (string)
    - Sample encoding of the output data, recorded as `sample_encoding_preset` in the `.meta` file.
    - Allowed values: `CVBS_U10_4FSC`, `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, `CVBS_S16_4FSC`.
    - Default: `CVBS_U10_4FSC` (lossless; preserves headroom). The other encodings clamp to their representable domain before scaling.

* `combine_yc` (bool)
    - Offered for Y/C projects only. Sums the luma and chroma channels sample by sample into one composite (Y+C) signal, written as a single `.cvbs` file (`signal_type` `composite` in the `.meta`) instead of a `.cvbsy`/`.cvbsc` pair.
    - The chroma channel's DC offset (512 for CVBS Y/C sources, derived from the uint16 midpoint for TBC Y/C sources) is removed before summing, so the result sits at the luma blanking level. Sums outside the 10-bit range are kept as headroom with `CVBS_U10_4FSC` and clamped by the other encodings.
    - Default: `false`.

* `capture_notes` (string)
    - Optional free-text notes written to the `.meta` file.
    - Default: `""` (not written when empty).

**Notes**

* `signal_state_preset` and `sequence_continuous` in the output `.meta` are measured rather than assumed, and cannot be overridden by the user. Each frame's colour-sequence phase is measured from its burst as it is written. The file is marked `STANDARD_STABLE_LOCKED` when the burst was measurable at the standard phase points, and `STANDARD_STABLE_UNLOCKED` when no burst could be measured at all (for example monochrome material). Separately (CVBS file format spec v1.6.0), `sequence_continuous` records whether the measured colour sequence ran unbroken through every frame written: `TRUE` when it did, `FALSE` when it broke at least once, and `NULL` (unknown) when nothing was measurable — a discontinuity does not downgrade the preset. Frames with no measurable burst — blank leader, lead-in, black frames — are not treated as breaks: the expected phase is projected across them and re-checked on the far side. The verdicts, with the output frame number of the first discontinuity, appear in the stage's completion status and in the log. Because the markers describe what was written, a pipeline that legitimately reorders or drops frames can turn a continuous input into a discontinuous output; run the Disc Mapper first to restore a continuous sequence.
* Absent upstream extensions (no audio, no EFM, etc.) produce no sidecar files — this is not an error.
* Every pipeline audio channel pair is exported — there is no pair selection at this sink. Frames without audio are written as silence so all pair files stay frame-aligned, and each pair's description is recorded in the `.meta` `audio_channel_pair` table.

---

## Daphne VBI Sink

| | |
|-|-|
| **Stage id** | `daphne_vbi_sink` |
| **Stage name** | Daphne VBI Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Write per-field VBI data in the format required by the Daphne arcade LaserDisc emulator |

**Use this stage when:**

* Archiving a LaserDisc title for use with the Daphne arcade LaserDisc emulator

**What it does**

Writes a `.vbi` file in the layout of the Daphne VBIInfo specification: a four-byte `1VBI` header followed by one 10-byte record per field — the white flag and the biphase words of lines 16, 17 and 18, each big-endian, with `0x7FFFFF` for a line that could not be read. That per-field VBI metadata is what Daphne needs to emulate the disc's interactivity.

The stage decodes the biphase code and white flag itself from the frames on its input, so it needs no other stage upstream. Records are written in field order, two per frame. A field with no readable biphase code gets all three lines as zero, and a frame the input cannot supply still gets its two records so that later fields stay at the right offset. The white flag is an NTSC feature and is always `0` on a PAL disc.

**Parameters**

* `output_path` (file path)
    - Path to the output `.vbi` file; `.vbi` is appended if absent.
    - Required.

**Notes**

* This sink produces a file specific to the Daphne emulation project and is not a general-purpose VBI archive format.
* The `.vbi` format is documented at the Daphne VBIInfo wiki page.
* Connect other sinks in parallel if you also need video output.

---

## EFM Decoder Sink

| | |
|-|-|
| **Stage id** | `EFMSink` |
| **Stage name** | EFM Decoder Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Decode EFM t-values to audio WAV or ECMA-130 binary sector data |

**Use this stage when:**

* Extracting digital audio from a LaserDisc source as a WAV file
* Extracting ECMA-130 data sectors from a LaserDisc source
* You want the fully decoded output of the EFM stream rather than the raw t-values

**What it does**

This stage accumulates EFM t-values from the incoming stream and runs the full EFM decode pipeline (demodulation, error detection, CIRC error correction, de-interleaving), producing either a standard PCM audio WAV file (44.1 kHz 16-bit stereo) or ECMA-130 binary sector data depending on the chosen decode mode. Optional sidecars (Audacity labels, bad-sector map, decode report) are written alongside when enabled.

Each EFM byte carries the t-value in its low nibble and the producer's doubt about it (0 trusted to 15 distrusted) in its high nibble. The demodulator reads only the t-value; the doubt is used as an error-correction erasure hint only if `doubt_erasure_threshold` is raised from its default of `0`.

**Parameters**

* `output_path` (string)
    - Path to the decoded output file. Use `.wav` for audio mode or `.bin` for data mode.
    - Required.

* `decode_mode` (string)
    - Selects the decode target. `audio` (default) produces a WAV or raw PCM file; `data` produces ECMA-130 binary sector data.
    - Allowed values: `audio`, `data`.
    - Default: `audio`.

* `no_timecodes` (boolean)
    - Disable timecode verification, for early CAV discs that pre-date the EFM timecode specification and will fail to decode without it. The decoded Q-channel timeline is ignored and a contiguous one synthesised, with every section treated as programme audio (track 1); leave it off for normal discs, as it overrides genuine lead-in/lead-out and track boundaries.
    - Applies to both `audio` and `data` modes.
    - Default: `false`.

* `audacity_labels` (boolean)
    - Write an Audacity label file alongside the audio output indicating the position of chapters as well as any missing samples.
    - Applies only in `audio` mode.
    - Default: `false`.

* `no_audio_concealment` (boolean)
    - Disable interpolation-based audio error concealment. When disabled, affected samples are zeroed instead of interpolated.
    - Applies only in `audio` mode.
    - Default: `false`.

* `ignore_preemphasis` (boolean)
    - Ignore the 50/15 µs pre-emphasis CONTROL flag (IEC 60908 §17.5) and write the audio exactly as decoded. When unchecked (default), sections flagged as pre-emphasised are de-emphasised during decode with a 50/15 µs filter so the output plays back with a flat response; enable this only if you want the raw pre-emphasised samples. When `audacity_labels` is enabled, a pre-emphasised track's label reads `Preemphasis:50/15us(removed)` when de-emphasis was applied, or `Preemphasis:50/15us` when this flag is set.
    - Applies only in `audio` mode.
    - Default: `false`.

* `video_sync` (boolean)
    - Align the start of the decoded audio with the video timeline of the capture, so it can be muxed against the video without manual correction. The CIRC de-interleaver's fixed 111-frame latency (666 stereo pairs, 15.1 ms) is removed, and the input consumed while the decoder acquired sync is restored as silence. The adjustment applied is stated in the decode report.
    - Ignored when `zero_pad` is enabled. Disable for bit-exact legacy output.
    - Applies only in `audio` mode.
    - Default: `true`.

* `offset_ms` (double)
    - Additional sync slip in milliseconds at the head of the audio, on top of the `video_sync` alignment (applied even when `video_sync` is off). Positive values delay the audio (silence is prepended); negative values advance it (samples are trimmed).
    - Range: −3,600,000 to 3,600,000. Applies only in `audio` mode.
    - Default: `0`.

* `zero_pad` (boolean)
    - Zero-pad the start of audio output so the sample starts from 00:00:00.0 relative to the first valid time-code. Overrides `video_sync`.
    - Applies only in `audio` mode.
    - Default: `false`.

* `no_wav_header` (boolean)
    - Output raw PCM samples without a WAV file header.
    - Applies only in `audio` mode.
    - Default: `false`.

* `output_metadata` (boolean)
    - Write a bad-sector map metadata file alongside the sector output.  This file contains the number of any missing or corrupt sectors.
    - Applies only in `data` mode.
    - Default: `false`.

* `doubt_erasure_threshold` (integer)
    - Treat an EFM symbol as a Reed-Solomon erasure when the producer's doubt about it reaches this value. Only the four most-doubted symbols of a codeword are ever flagged, so the code's erasure capacity cannot be exceeded. An `.efm` with no confidence information is unaffected at any setting.
    - Range: 0–15. Applies to both modes.
    - Default: `0` (off; the decode stays bit-exact). **Leave it there.** On a full Domesday data side, 13–15 left the recovered-sector count unchanged and lower values lost sectors. On an audio disc, low thresholds produce erasure-assisted miscorrections — clicks in the audio — while the reported C1/C2 and concealment counts look as good as or better than the baseline. If you experiment, compare the decoded output, not the report; 13 is the value to start from.

* `report` (boolean)
    - Write a detailed decode statistics report file.
    - Default: `false`.

**Reading the decode report**

The report separates losses caused by the input from those caused by the decode boundaries. The 111-frame CIRC warm-up at the start and drain at the end carry no disc data and are excluded from the C1/C2 uncorrectable and data-loss percentages, so a clean stream reports 0.0000 % and grades `EXCELLENT`. Concealed and muted samples are broken down by disc region (lead-in, pause, programme, lead-out) and by cause (warm-up, drain, genuine), with region boundaries computed on the audio timeline. The overall grade is driven only by genuine losses in the programme area:

| Grade | Condition |
|---|---|
| `EXCELLENT` | No concealment or muting in the programme area, no uncorrectable C1/C2, no missing sections |
| `GOOD` | Programme concealment < 0.10 %, no muting |
| `FAIR` | Programme concealment < 1.0 % |
| `POOR` | Programme loss ≥ 1.0 %, or missing / uncorrectable sections |

Warnings (`!`) are raised only for genuine losses and name the affected track and timecode; a capture that stops inside the programme area is warned about.

**Notes**

* The source stage must supply an EFM file; the pipeline will abort if no EFM data is present in the incoming stream.
* Audio and data decoding are mutually exclusive — select `decode_mode` before enabling mode-specific parameters. Parameters for the inactive mode are hidden in the GUI and silently ignored.
* For decoded EFM audio as a pipeline channel pair (to embed in a video or write with the Audio Sink), use the **EFM Audio Decode** transform instead.
* EFM stacking or correction should be performed upstream before this stage.

---


## NABTS Sink

| | |
|-|-|
| **Stage id** | `nabts_sink` |
| **Stage name** | NABTS Sink |
| **Provided by** | The teletext plugin, `org.decodeorc.stage.teletext` ([orc-plugin_teletext](https://github.com/decode-orc/orc-plugin_teletext)) |
| **Purpose** | Recover North American Basic Teletext from the VBI of a 525-line source, export the packet stream, and browse the records the recording carried |

The NABTS Sink is part of the teletext plugin, which lives in its own repository and is not installed with Decode-Orc. Install it from **Plugin Manager → Browse Plugins…** or with `orc-cli plugins install org.decodeorc.stage.teletext`. A project that uses the stage says which plugin to install when it is missing.

Its parameters, tools and **NABTS Records** viewer are documented with the plugin: [NABTS Sink](https://github.com/decode-orc/orc-plugin_teletext/blob/main/docs/nabts-sink.md).

---

## Raw EFM Sink

| | |
|-|-|
| **Stage id** | `RawEFMSink` |
| **Stage name** | Raw EFM Data Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Write raw EFM t-values to a binary file |

**Use this stage when:**

* Archiving LaserDisc EFM t-values for later processing
* Feeding raw EFM data into external decoding or analysis tools
* Verifying EFM integrity after stacking or correction

**What it does**

This stage extracts raw EFM (Eight-to-Fourteen Modulation) t-values from the incoming stream and writes them to a binary file: one byte per t-value, in field order, with no headers, field boundaries or framing.

Each byte is packed as the CVBS EFM extension format defines it: the t-value (conventionally T3–T11) in the low nibble and the producer's confidence, as a *doubt* value (0 fully trusted, 15 positively distrusted), in the high nibble. A fully trusted t-value packs to its plain value, so a capture with nothing doubted is byte-for-byte the plain t-value stream.

**Parameters**

* `output_path` (file path)
    - Path to the output EFM file. Conventionally uses the `.efm` extension. Created or overwritten at trigger time.
    - Required.

* `include_confidence` (boolean)
    - Write each byte as the pipeline carries it, doubt nibble included — the lossless export, and what the current CVBS EFM extension format specifies. Disable to mask the doubt nibble off and write bare t-values, for tools that pre-date the confidence field and would read a doubted byte as an out-of-range t-value.
    - Default: `true`.

**Notes**

* The source stage must supply an EFM file; the pipeline will abort if no EFM data is present in the incoming stream.
* EFM stacking behaviour is controlled upstream (e.g. via `stacker`).
* This stage does not modify or decode EFM data. Use the EFM Decoder Sink stage to decode t-values to audio or sector data.

---

## TBC Sink

| | |
|-|-|
| **Stage id** | `tbc_sink` (formerly `ld_sink`) |
| **Stage name** | TBC Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Write an ld-decode-compatible TBC and metadata output |

**Use this stage when:**

* Producing final archival-quality outputs
* Feeding results back into the ld-decode ecosystem (ld-chroma-decoder, ld-analyse, ld-process-vbi, …)
* Preserving full per-field metadata

**What it does**

This stage writes:

* A `.tbc` file containing processed video fields
* A `.tbc.db` metadata database compatible with ld-decode
* A `.pcm` analogue audio sidecar, when the pipeline carries audio
* An `.efm` t-value sidecar, when the pipeline carries EFM

The output can be used directly with existing ld-decode tools, and a TBC Source → TBC Sink pipeline is a round trip: field order and signal levels are preserved. The isFirstField (top) field is written first, samples are widened to the ld-decode 16-bit domain by the format's fixed ×64 factor (the picture is not rescaled), and a non-standard black level — such as an NTSC-J capture with no 7.5 IRE setup — is recorded in the metadata as read rather than replaced by the standard value.

For PAL the sink also converts the sample grid. The pipeline holds PAL frames on the 4fsc subcarrier lattice (1135.0064 samples per line); a `.tbc` stores every line at exactly 1135 samples. Each field is resampled back to that line-locked grid, the inverse of TBC Source, and dropouts are mapped back with it. A PAL TBC Source → TBC Sink round trip matches the original to within interpolation error (under one 10-bit code on band-limited picture content) rather than bit for bit. NTSC and PAL-M are written unchanged.

Padding frames (inserted upstream by Frame Map gap filling or Source Align, or marked as padding in the source TBC metadata) are written as two blanking-level fields with `pad` set in the field metadata, so the padding survives a round trip; their audio is written too, so a gap does not slide everything after it out of sync.

**Sidecar files**

The sidecars are named off the same base as the TBC with the `.tbc` replaced, so `disc.tbc` is accompanied by `disc.pcm` and `disc.efm` — the layout TBC Source auto-detects, which means an export drops straight back in as a source.

They are built from what reaches the sink through the pipeline, not from any file the original source read, so it makes no difference whether the chain started at a TBC Source, a CVBS Source, or a stage that produced the audio itself.

* `disc.pcm` — headerless signed 16-bit little-endian stereo at 44100 Hz, as ld-decode writes it, with the layout recorded in the metadata database. Pipeline audio is 48 kHz 24-bit, so it is resampled and narrowed on the way out; this is not lossless, and repeated export/re-import cycles will degrade the audio slightly each time. Only one channel pair fits, chosen with the **Audio Channel Pair** parameter.
* `disc.efm` — one byte per t-value in field order. The per-field counts written to the metadata database are what let TBC Source locate each frame's payload again. Each frame's run of bytes is split evenly between its two field records, which does not affect the file or any frame's payload on re-import.

**Parameters**

* `output_path` (file path)
    - Base path for the output files — the stage appends the `.tbc` and `.tbc.db` extensions automatically (a trailing `.tbc` is kept as-is). The `.pcm` and `.efm` sidecars replace the `.tbc` rather than extending it.
    - Required.
* `audio_channel_pair` (string)
    - Which audio channel pair is written to the `.pcm` sidecar, as a 0-based index matching the CVBS container channel pair numbering.
    - Defaults to `0`, the lowest pair — where a TBC or CVBS source puts the analogue audio it read.
    - Set another index when the pipeline carries several pairs and you want a different one exported, for example the digital audio pair added by EFM Audio Decode. The dialogue lists only the pairs the input actually carries and labels each with its name — `0: Analogue`, `1: EFM digital audio` — so you can pick by what the pair is; the project stores the bare index.
    - Ignored when the input has no audio; a pair the input does not carry falls back to the lowest one.

**Notes**

* This is the most common "final output" sink stage.
* All upstream corrections, stacking, and parameter overrides should be complete before this stage.
* The target directory must exist and be writable at trigger time.
* The `.pcm` audio is resampled as the frames are written, so memory use does not grow with the length of the export.
* AC3 RF is not written as a sidecar — use the AC3 RF Sink in parallel. The Audio Sink and Raw EFM Data Sink are still useful for putting a WAV or a standalone `.efm` somewhere other than beside the TBC, or for exporting a second channel pair as well.
* This stage was called **ld-decode Sink** (`ld_sink`) before it was renamed to pair with TBC Source. Projects saved under the old name are migrated automatically when they are opened.

---

## Teletext Sink

| | |
|-|-|
| **Stage id** | `teletext_sink` |
| **Stage name** | Teletext Sink |
| **Provided by** | The teletext plugin, `org.decodeorc.stage.teletext` ([orc-plugin_teletext](https://github.com/decode-orc/orc-plugin_teletext)) |
| **Purpose** | Recover World System Teletext from the VBI, export the packet stream, and browse the pages the recording carried |

The Teletext Sink is part of the teletext plugin, which lives in its own repository and is not installed with Decode-Orc. Install it from **Plugin Manager → Browse Plugins…** or with `orc-cli plugins install org.decodeorc.stage.teletext`. A project that uses the stage says which plugin to install when it is missing.

Its parameters, tools and **Teletext Pages** viewer are documented with the plugin: [Teletext Sink](https://github.com/decode-orc/orc-plugin_teletext/blob/main/docs/teletext-sink.md).

---

## Video Sink

| | |
|-|-|
| **Stage id** | `video_sink` |
| **Stage name** | Video Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Chroma-decode the processed video and write it to a file, either FFmpeg-encoded (MP4/MKV/MOV/MXF) or uncompressed raw (RGB/YUV/Y4M) |

**Use this stage when:**

* You want a playable, distributable, or archival video file (FFmpeg mode)
* You want optional embedded audio, closed captions, or chapter metadata (FFmpeg mode)
* You need an uncompressed output for external tools such as FFmpeg, VirtualDub, or image-processing scripts (raw mode)

**What it does**

Applies the selected chroma decoder to convert the incoming TBC video stream to colour video, then writes the result according to the selected output mode. In FFmpeg mode the video is encoded into the chosen container and codec, optionally embedding pipeline audio (up to 8 channel pairs, one output stream per pair), closed captions (as mov_text subtitles in MP4/MOV, SubRip in `mkv-h264`), and chapter markers derived from VBI data. In raw mode the decoded frames are written to a file without compression; the raw format determines the pixel layout and whether a Y4M header is prepended.

Frames are decoded on worker threads, one per hardware thread up to 32. The stages before the sink keep enough frames for that many workers, so on a host with more threads the rest stay idle rather than have those stages produce frames twice.

For guidance on selecting and tuning a decoder, see the [Chroma decoder guide](chroma-decoder-guide.md).

**Parameters**

* `output_path` (file path)
    - Output file path. Match the extension to the selected mode and format: `.mp4`, `.mkv`, `.mov`, or `.mxf` for FFmpeg output; `.rgb`, `.yuv`, or `.y4m` for raw output.
    - Required.

* `decoder_type` (string)
    - Chroma decoder to apply. The list follows the project format; the default is `pal2d` for PAL and PAL-M projects and `ntsc2d` for NTSC. PAL: `pal2d`, `transform2d`, `transform3d`, and in PAL projects `palstill`. SECAM (625-line sources stored as PAL, PAL projects only): `secam`. NTSC: `ntsc1d`, `ntsc2d`, `ntsc3d`, `ntsc3dnoadapt`. Other: `mono`.
    - `multiple` uses a different decoder for different frame ranges, set in `decoder_schedule`, and keeps every decoder-specific setting below editable.
    - `palstill` is a specialist decoder for still pictures (maps, text, photographs) that are on screen for one frame each, with unrelated pictures either side. It decodes each frame from its own two fields as one picture. It is not meant for moving video (comb-like edges) or for pictures held over many frames (use `transform3d`). A frame without a burst decodes as monochrome. See the [Chroma decoder guide](chroma-decoder-guide.md#pal-still).
    - For a Y/C source, `transform2d`/`transform3d`/`palstill` fall back to `pal2d`.

* `decoder_schedule` (string)
    - `decoder_type` `multiple` only. Comma-separated `first-last:decoder` entries (a single frame is `first:decoder`); a decoder without a range covers every frame no range lists. Example: `transform3d, 1001-1500:transform2d`.
    - Frame numbers are 1-based in the GUI and stored 0-based in the project file. At most one decoder may be given without a range; ranges must not overlap, and every frame needs a decoder or the export is refused before anything is written. See [Different decoders for different frames](chroma-decoder-guide.md#different-decoders-for-different-frames).
    - `held:decoder` decodes the frames whose picture is held, as an upstream Stacker with `still_frame_stacking` reports it, and needs a default beside it: `transform2d, held:transform3d`. A frame counts as held only when its picture is held for at least as many frames either side as the held decoder reads (4 for `transform3d`, 1 for `ntsc3d`); a range overrules it.

* `output_mode` (string)
    - Output path selection. Values: `ffmpeg` (encoded output via FFmpeg), `raw` (uncompressed file output). Default: `ffmpeg`.

* `raw_format` (string)
    - Raw output format (raw mode only). Values: `rgb` (RGB48, 16-bit per channel), `yuv` (YUV444P16, planar), `y4m` (YUV444P16 with Y4M header). Default: `rgb`.

* `ffmpeg_format` (string)
    - Container and codec (FFmpeg mode only). Values include `mp4-h264`, `mkv-ffv1`, `mkv-ffv1-bt601`, `mkv-h264`, `mov-prores`, `mov-v210`, `mov-v410`, `mxf-mpeg2video`, `mov-h264`, `mp4-hevc`, `mov-hevc`, and `mp4-av1`. Default: `mp4-h264`.
    - `mkv-ffv1-bt601` is the **FFV1 for VP415e** preset: lossless FFV1 resampled (band-limited, Lanczos-3) onto the ITU-R BT.601 13.5 MHz sampling grid instead of the 4fsc grid, for players driving real BT.601 hardware. 720 pixels wide, line count unchanged, horizontal-only so interlacing survives; the whole 720-sample window is taken from source (4fsc samples 173..1119 for 625-line, 129..893 for 525-line), so nothing is cropped and the margins carry the source's own blanking. The sample aspect ratio is signalled from the standard (128:117 for 625-line). Defaults to 8-bit 4:2:2 and 16 slices. `output_padding` does not apply. The 4fsc `mkv-ffv1` export remains the master.
    - `mkv-h264` is the **H.264 for VP415e** preset, the lossy companion to `mkv-ffv1-bt601`: about 1–5 GB a side instead of 18–25 GB. Everything but the codec matches the lossless format — BT.601 grid and sample aspect ratio, field structure, one file frame per output frame at a constant frame rate, disc metadata attachment, tags and chapters. H.264 High profile, 8-bit 4:2:0 reduced field by field, interlaced coding with the field order signalled, a closed one-second GOP (25 frames 625-line, 30 frames 525-line), AAC-LC audio. Quality is set with `encoder_crf` (the preset uses 20; 18–22 looks clean) and speed with `encoder_preset`. Software x264 only: `hardware_encoder`, `use_lossless_mode`, `encoder_bitrate` and `apply_deinterlace` are ignored for it. `output_padding` does not apply.
    - Both emulation formats, and the disc metadata they carry, are specified in [LaserDisc Emulation Formats](../../technical/laserdisc-emulation-formats.md). They are the only two formats an emulator needs to support. From the CLI, set `display_aspect_ratio=auto` with either format, or the default `4:3` overrides the standard sample aspect ratio.

* `bt601_bit_depth` (string)
    - Output bit depth for `mkv-ffv1-bt601`. Values: `8` (yuv422p), `10` (yuv422p10le). Default: `8`, which decodes roughly 1.5× faster and is about a third smaller.

* `ffv1_slices` (string)
    - FFV1 slices per frame (`mkv-ffv1`, `mkv-ffv1-bt601`). Values: `auto`, `4`, `12`, `16`, `24`, `30`, `36`. Default: `auto` — 4 for `mkv-ffv1`, 16 for `mkv-ffv1-bt601`. The count is fixed at encode time and caps decoder parallelism; higher counts cost a little compression.

* `chroma_gain` (double) / `chroma_phase` (double)
    - Chroma gain multiplier (0.0–10.0, default 1.0) and phase rotation in degrees (-180 to 180, default 0).

* `noise_reduction` (double) / `noise_reduction_mode` (string) / `noise_reduction_motion` (double)
    - Noise reduction in dB (0.0–12.0, default 0 = off, output identical to an export without it), applied after decoding for every decoder in both output modes. Still picture is averaged over successive frames; moving picture, scene cuts, the first frame and frames that do not continue the previous one (a disc of stills) are smoothed within the frame instead, by a spatial filter that reaches just under 6 dB.
    - `noise_reduction_mode`: `full` (default) as above; `still_only` averages still picture only and leaves everything else as it came, so a disc of single-frame stills keeps its stills as decoded while its held pictures are cleaned. Fed from a Stacker with `still_frame_stacking`, it averages only the frames the Stacker found repeated, as its own test cannot tell two related stills (two maps of one area) apart. It lengthens an export by about a fifth with the fastest decoders, sometimes up to a half. The preview shows the filtered picture; a seek runs the filter over the four frames before the target first.
    - `noise_reduction_motion` (0.5–4.0, default 1.0) scales how much frame-to-frame change counts as motion: above 1 smooths more at the risk of a short trail behind slow, low-contrast movement; below 1 protects movement. Only used when `noise_reduction` is above 0.
    - The coring controls `luma_nr` and `chroma_nr` of earlier releases have been removed; projects that set them still load, and the values are ignored.

* `ntsc_phase_comp` (bool)
    - Enable NTSC phase compensation: adjust chroma phase line by line from the measured burst phase. NTSC projects only. Default: `true`.

* `secam_temporal_chroma` (bool)
    - SECAM only (`secam` decoder). Fill the colour difference a line does not carry from the same line of the previous frame where the picture is still (motion-adaptive), so horizontal colour edges do not move by one line every frame. Roughly doubles SECAM decoding time. Default: `true`.

* `secam_cross_colour_suppression` (bool)
    - SECAM only (`secam` decoder). Tight chroma band-pass plus fading out colour where the subcarrier envelope or frequency shows luma interference, removing most cross-colour. Off uses the plain wide band-pass. Default: `false`.

* `secam_field_identification` (bool)
    - SECAM only (`secam` decoder). Decide the D'R/D'B line order from the field identification lines 7–15 / 320–328 instead of the back-porch reference, falling back to the back-porch reference where they are missing. Default: `true`.

* `simple_pal` (bool)
    - Enable simple PAL chroma decoding (1D UV filter for Transform PAL: simpler, faster, lower quality). `transform2d`/`transform3d` decoders only. Default: `false`.

* `transform_threshold` (double)
    - Similarity threshold for the Transform PAL decoder. Higher = more transform filtering. Range: 0.0–1.0. Default: 0.4. `transform2d`/`transform3d`/`palstill` decoders only.

* `chroma_weight` (double)
    - Chroma weight for the NTSC 3D adaptive filter. It scales the bias the filter gives to line, field and frame candidates over same-line (1D) ones, so higher values favour those candidates — and the 3D result — more strongly, in the same direction as `adapt_threshold`; `0` removes the bias. Range: 0.0–10.0. Default: 1.0. `ntsc3d`/`ntsc3dnoadapt` decoders only.

* `adapt_threshold` (double)
    - NTSC 3D adaptive filter threshold. Higher = prefer more 3D result. Range: 0.0–10.0. Default: 1.0. `ntsc3d` decoder only.

* `output_padding` (int)
    - Pad the output frame to a multiple of this many pixels on both axes. Range: 1–32. Default: 8. Ignored by `mkv-ffv1-bt601` and `mkv-h264`.

* `encoder_preset` (string)
    - FFmpeg mode only. Encoder speed/quality trade-off; slower presets give smaller files at the same quality. Values: `fast`, `medium`, `slow`, `veryslow`. Default: `medium`.

* `encoder_crf` (int)
    - FFmpeg mode only. Constant Rate Factor for quality-based encoding. Range: 0–51 (lower = higher quality). Default: 18. AV1 compact delivery typically uses 30–35; `mkv-h264` 18–22 (its preset uses 20). Used when lossless mode is off and `encoder_bitrate` is 0.

* `encoder_bitrate` (int)
    - FFmpeg mode only. Target bitrate in bits per second (e.g. 10000000 = 10 Mbps). When non-zero, overrides CRF mode; lossless mode overrides both. Range: 0–100,000,000. Default: 0 (use CRF).

* `hardware_encoder` (string)
    - FFmpeg mode only, H.264/H.265 formats. Hardware-accelerated encoding backend. Values: `none`, `vaapi`, `nvenc`, `qsv`, `amf`, `videotoolbox`. Default: `none`. The FFmpeg Preset Config tool can auto-detect one.

* `prores_profile` (string)
    - FFmpeg mode only, `mov-prores` format. ProRes quality profile: `proxy`, `lt`, `standard`, `hq`, `4444`, `4444xq`. Default: `hq`.

* `use_lossless_mode` (bool)
    - FFmpeg mode only. Enable mathematically lossless encoding (H.264/H.265/AV1 only, overrides CRF). Default: `false`.

* `apply_deinterlace` (bool)
    - FFmpeg mode only. Apply bwdif deinterlacing for progressive web playback. One frame is produced per field, so the output frame rate doubles (50 fps PAL, 59.94 fps NTSC). Default: `false`.

* `deinterlace_mode` (string)
    - FFmpeg mode only, read only while `apply_deinterlace` is on. `motion_only` (default) deinterlaces only the frames whose two fields are different instants and weaves the rest, showing both fields exactly as recorded, so stills, maps, text pages and film keep their full vertical detail. `all` deinterlaces every frame with bwdif; a project saved without the parameter now exports with `motion_only`, so set `all` to keep every frame deinterlaced. The output keeps the field rate either way, a woven frame shown for both its field periods.
    - A frame is woven when an upstream Stacker with still-frame stacking reports it as held, or when its own fields score as one instant: the score over the whole picture below 0.75, and the second-worst 32-line by 64-sample block's below 1.2, which catches an animated graphic over a still. Unflagged dropouts can make a still frame score as motion, so put Dropout Correct before the sink.

* `display_aspect_ratio` (string)
    - FFmpeg mode only. Display aspect ratio signalled to players. Metadata only — the video is not rescaled; players stretch the picture at playback. Values: `4:3` (standard-definition television), `16:9` (widescreen), `auto` (square pixels, no aspect metadata). Default: `4:3`, which suits most SD LaserDisc and tape material.
    - For the LaserDisc emulation formats (`mkv-ffv1-bt601`, `mkv-h264`) set this to `auto`: the export then signals the BT.601 sample aspect ratio from the standard (128:117 for 625-line), as their specification requires. Any other value replaces it with a whole-frame ratio, which squeezes the picture into the blanking margins (`4:3` gives 16:15 on 625-line). The FFmpeg Preset Config tool's emulation presets set `auto` for you; from the CLI, set it yourself, e.g. `orc-cli nodes set <project> <node> display_aspect_ratio=auto`.

* `video_filter` (string)
    - FFmpeg mode only. Custom FFmpeg video filter chain applied before encoding, using the same syntax as ffmpeg's `-vf` option (e.g. `fieldmatch,decimate` to inverse-telecine NTSC film to 23.976 fps, `bwdif=mode=send_frame` to deinterlace without doubling the frame rate, `crop=692:554`). Filters may change output dimensions and frame rate; the encoder follows the filter output automatically. An invalid filter string fails the export with the FFmpeg error message. Default: empty (no filtering).

* `embed_audio` (bool)
    - FFmpeg mode only. Embed pipeline audio into the output file, one output audio stream per selected channel pair, each titled with the pair's name. Requires audio in the pipeline. Default: `false`.
    - Every stream is declared at 48,000 Hz, the pipeline's only audio rate, and is never resampled. The codec follows the container: 24-bit FLAC with FFV1, 24-bit PCM with ProRes/V210/V410/D10, AAC (fed full-scale float) with H.264/H.265/AV1. Frames without audio are filled with silence so the audio matches the video duration.

* `audio_channel_pairs` (string)
    - FFmpeg mode only; available only when `embed_audio` is enabled. Which audio channel pairs to embed: `all` (default) or a comma-separated list of 0-based channel pair indices, e.g. `0,2`. Indices match the CVBS container's `_audio_<p>.wav` numbering. The export fails if a listed channel pair does not exist.

* `audio_gain_db` (double)
    - FFmpeg mode only; available only when `embed_audio` is enabled. Gain applied to the embedded audio in decibels. `0` = unchanged; positive boosts (6 dB roughly doubles the amplitude), negative attenuates. Samples are clipped at full scale. Range: -24 to 24. Default: `0`.

* `embed_closed_captions` (bool)
    - FFmpeg mode only. Embed closed captions as a text subtitle track: mov_text in MP4/MOV output, SubRip in `mkv-h264`. Default: `false`.
    - The container carries one subtitle track, so this embeds the primary caption service, CC1. Use the Closed Caption Sink to export CC2 or a text service.
    - Pairs are taken as the Closed Caption Sink takes them — both bytes' parity, the bytes with no function on their field ignored, the codes EIA-608 defines for their field, Line 21 data around them, and the field order — so the subtitles are the sink's CC1, and a capture whose first stored field is television field 2 still has its captions found.

* `embed_chapter_metadata` (bool)
    - FFmpeg mode only, MKV/MP4/MOV formats. Write chapter markers derived from VBI data (chapter numbers decoded by the biphase observer) into the output file. Default: `false`.

* `embed_disc_metadata` (bool)
    - FFmpeg mode, Matroska only. Attach the LaserDisc VBI metadata — picture numbers, programme time codes, chapters, stop codes, lead-in/lead-out and programme status — as a file inside the output, so a player emulator needs no sidecar. Default: `false`.
    - The attachment is `orc-disc-metadata.yaml`, and the disc-level summary is mirrored into container tags so `ffprobe` shows it without extracting anything. MP4 and MOV cannot carry attachments, so asking for this on another container fails the export rather than quietly writing a file without the document.
    - Enabling it adds a VBI pre-scan over the export range: Matroska writes attachments into the file header, so the whole map must exist before the first frame is encoded. `embed_chapter_metadata` shares that pass when both are on.
    - The pass reads the frames in order, with the frames just ahead produced on the other threads, so a stacked input is stacked on every core. On 140 frames of eleven captures stacked with still-frame repeats, the pass took 8.4 s on 16 threads against 13.7 s for the export itself. Closed-caption embedding's pre-scan works the same way.
    - The format is documented in full in [LaserDisc Emulation Formats](../../technical/laserdisc-emulation-formats.md), which also specifies the two emulation formats that carry it.

* `disc_metadata_detail` (string)
    - Offered when `embed_disc_metadata` is on. How much disc metadata to write: `map` (address map, events and disc status — a few kilobytes) or `full` (also the raw biphase words for every field, about 2.3 MB for a full side, so codes decode-orc does not yet interpret stay recoverable). Default: `map`.

**Preview**

The stage preview shows the decoded colour frame with the configured decoder and chroma settings (and noise reduction). The preview mode selector offers **Interlaced** (both fields woven together, field 1 on top) and **Sequential** (the two fields as separate blocks, field 1 above field 2, for inspecting field order or comb artefacts).

**Stage tools**

* **FFmpeg Preset Config** — a preset helper dialog that applies well-tested encoder combinations without setting each parameter manually. Applying a preset switches the stage to FFmpeg output mode. The **LaserDisc Emulation** category holds the two emulation presets: **FFV1 for VP415e** (`mkv-ffv1-bt601`, also listed under Lossless/Archive) and **H.264 for VP415e** (`mkv-h264`, CRF 20, encoder preset slow). Both turn audio embedding on and return the display aspect ratio to Auto, because those exports signal the BT.601 sample aspect ratio themselves. Because they exist to produce a self-contained file for a player emulator, they turn on disc metadata and chapter markers too — an emulator needs the picture-number map to seek at all, and chapters fall out of the same VBI pass. The Embed disc metadata option is disabled for MP4 and MOV presets, which cannot carry attachments. **Weave still frames**, under Deinterlace for web and enabled only while it is ticked, is ticked by default and sets `deinterlace_mode` to `motion_only`; untick it for `all`. The tool is GUI only; from the CLI, set the parameters with `orc-cli nodes set`.

**Notes**

* Raw mode does not support audio, closed caption, chapter, or disc metadata embedding; those options apply to FFmpeg output only.
* Raw output files can be very large; ensure sufficient disk space before triggering.
* The `y4m` raw format is directly readable by tools such as FFmpeg and rav1e without specifying the pixel format manually.
* CRF and bitrate modes are mutually exclusive; set `encoder_bitrate` to a non-zero value to switch from CRF mode.
* Video filtering (`apply_deinterlace` or `video_filter`) is not supported with hardware encoders that use GPU surfaces (`vaapi`, `qsv`, `videotoolbox`); the export automatically falls back to the software encoder in that case.
* When a video filter chain is active, interlaced coding flags are not forced on the encoder and the container field order is left unstated; the field structure of the filter output determines how frames are flagged. The horizontal-only resample the BT.601 export performs is exempt, since it cannot disturb the field structure.
* SD colour signalling (`bt470bg` for 625-line, `smpte170m` for 525-line) and limited colour range are written for every codec, so no downstream converter has to guess.
* Projects created with the earlier separate `raw_video_sink` and `ffmpeg_video_sink` stages are migrated to this stage automatically when loaded.

---

## Notes on Sink Stages

* Sink stages terminate pipeline branches.
* Multiple sink stages may consume the same upstream output.
* Sink stages do not alter timing or metadata beyond their specific export role.
* A sink writes nothing until it is triggered: **Trigger Stage** in the node's context menu in the GUI, or `orc-cli <project> --process`, which triggers every sink in the project (see the [CLI user guide](../../cli-user-guide/overview.md)).

---

## Removed stages

### HackDAC Sink (removed in v2.0)

The `hackdac_sink` stage was removed in Decode-Orc 2.0. It is no longer available in the plugin registry. Projects that referenced this stage must be recreated without it.
