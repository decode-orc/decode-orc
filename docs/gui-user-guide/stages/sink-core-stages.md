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

* `output_path` (string)
    - Path to the output AC3 file. The conventional extension is `.ac3`.
    - Required.

**Notes**

* The upstream source must supply AC3 RF data; the pipeline will abort at trigger time if none is present.
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

The pipeline carries stereo audio channel pairs at exactly 48,000 Hz, frame-locked (synchronous) to the video for every system, following SMPTE 272M-1994. The WAV output is 24-bit signed little-endian PCM declaring 48,000 Hz; no resampling or bit-depth conversion is performed.

**Parameters**

* `output_path` (string)
    - Path to the output WAV file.
    - Required.

* `channel_pair` (integer)
    - Audio channel pair to write, 0-based (0–7), matching the CVBS container's `_audio_<p>.wav` numbering.
    - Default 0. Triggering fails if the selected channel pair does not exist.

**Notes**

* This stage writes whatever channel pair you select. Analogue capture audio arrives as channel pair 0 from the source; EFM digital audio (CD-quality stereo) becomes a channel pair when you add an **EFM Audio Decode** transform upstream, after which it can be written here like any other pair. For a bit-exact, un-resampled WAV of EFM audio use the EFM Decoder Sink instead; AC3 RF (Dolby Digital) is exported via the AC3 RF Sink.
* Audio stacking or selection must be performed upstream (e.g. via `stacker`).

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
    - Export format.
    - Allowed values: `Scenarist SCC`, `SubRip SRT`, `Plain Text`, `HTML`.
    - Default: `Scenarist SCC`.

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
* CC data must be preserved upstream — masking Line 21 before this stage will destroy the caption payload.
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

**What it does**

This stage writes processed frame data using the selected sample encoding, and a `.meta` SQLite sidecar. The output signal type follows the project type automatically: a composite project is written as a single `.cvbs` file and a Y/C project as a `.cvbsy`/`.cvbsc` pair (per the CVBS file format naming convention) — Y/C cannot be derived from a composite signal, so this is not a choice. The `.meta` file records the signal type, the selected `sample_encoding_preset`, and a measured `signal_state_preset` and `sequence_continuous` flag (CVBS file format spec v1.6.0). Neither is user-configurable: they describe the file that was actually written.

Associated sidecars are written automatically when the upstream source provides them:

- `.dropouts.meta` — when dropout hints are present
- `_audio_0.wav` … `_audio_7.wav` — when audio is present (one 24-bit 48 kHz stereo WAV per channel pair)
- `.efm` + `.efm.meta` — when EFM data is present
- `.ac3` + `.ac3.meta` — when AC3 RF data is present

A CVBS file written by this stage can be round-tripped back through the CVBS Source stage.

**Parameters**

* `output_path` (string)
    - Base path for output files. A trailing `.cvbs`, `.cvbsy`, or `.cvbsc` extension is stripped when present.
    - Required.

* `sample_encoding` (string)
    - Sample encoding of the output data, recorded as `sample_encoding_preset` in the `.meta` file.
    - Allowed values: `CVBS_U10_4FSC`, `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, `CVBS_S16_4FSC`.
    - Default: `CVBS_U10_4FSC` (lossless; preserves headroom). The other encodings clamp to their representable domain before scaling.

* `capture_notes` (string)
    - Optional free-text notes written to the `.meta` file.
    - Default: `""` (not written when empty).

**Notes**

* `signal_state_preset` and `sequence_continuous` in the output `.meta` are measured rather than assumed, and cannot be overridden by the user. Each frame's colour-sequence phase is measured from its burst as it is written. The file is marked `STANDARD_STABLE_LOCKED` when the burst was measurable at the standard phase points, and `STANDARD_STABLE_UNLOCKED` when no burst could be measured at all (for example monochrome material). Separately (CVBS file format spec v1.6.0), `sequence_continuous` records whether the measured colour sequence ran unbroken through every frame written: `TRUE` when it did, `FALSE` when it broke at least once, and `NULL` (unknown) when nothing was measurable — a discontinuity does not downgrade the preset. Frames with no measurable burst — blank leader, lead-in, black frames — are not treated as breaks: the expected phase is projected across them and re-checked on the far side. The verdicts, with the output frame number of the first discontinuity, appear in the stage's completion status and in the log. Because the markers describe what was written, a pipeline that legitimately reorders or drops frames can turn a continuous input into a discontinuous output; run the Disc Mapper first to restore a continuous sequence.
* Absent upstream extensions (no audio, no EFM, etc.) produce no sidecar files — this is not an error.

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

Reads VBI data from each frame in the incoming stream and writes binary VBI records field by field to a `.vbi` file according to the Daphne VBIInfo specification. The `.vbi` file carries the per-field VBI metadata that Daphne requires to emulate the disc's interactivity correctly.

**Parameters**

* `output_path` (string)
    - Path to the output `.vbi` file.
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

This stage accumulates EFM t-values from the incoming stream and runs the full EFM decode pipeline (demodulation, error detection, CIRC error correction, de-interleaving), producing either a standard PCM audio WAV file or ECMA-130 binary sector data depending on the chosen decode mode.

**Parameters**

* `output_path` (string)
    - Path to the decoded output file. Use `.wav` for audio mode or `.bin` for data mode.
    - Required.

* `decode_mode` (string)
    - Selects the decode target. `audio` (default) produces a WAV or raw PCM file; `data` produces ECMA-130 binary sector data.
    - Allowed values: `audio`, `data`.
    - Default: `audio`.

* `no_timecodes` (boolean)
    - Disable timecode verification (early discs did not include time-codes in the EFM and will fail to decode without this option).
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

* `zero_pad` (boolean)
    - Zero-pad the start of audio output so the sample starts from 00:00:00.0 relative to the first valid time-code.
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

* `report` (boolean)
    - Write a detailed decode statistics report file.
    - Default: `false`.

**Notes**

* The source stage must supply an EFM file; the pipeline will abort if no EFM data is present in the incoming stream.
* Audio and data decoding are mutually exclusive — select `decode_mode` before enabling mode-specific parameters. Parameters for the inactive mode are silently ignored.
* EFM stacking or correction should be performed upstream before this stage.

---


## NABTS Sink

| | |
|-|-|
| **Stage id** | `nabts_sink` |
| **Stage name** | NABTS Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Recover North American Basic Teletext from the VBI of a 525-line source, export the packet stream, and browse the records the recording carried |

**Use this stage when:**

* Preserving the NABTS data carried by an NTSC or PAL-M capture — CBS ExtraVision, NBC Teletext and their contemporaries
* Reading the pages a recording carried without leaving decode-orc
* Producing a `.t33` packet stream, or per-record files, for an external NAPLPS tool

**What it does**

NABTS is **ITU-R BT.653 System C**, specified by CEA-516 (formerly EIA-516): 5.727272 Mbit/s in 33-byte packets on the 525-line signal. Its presentation layer is **NAPLPS** (ANSI X3.110-1983, also ITU-T T.101 Data Syntax III), so a page is a drawing program rather than a grid of characters — lines, arcs, polygons, mosaics and redefinable characters as well as text.

Triggering the stage makes one linear pass over the whole frame range and carries the recovery up through every layer CEA-516 defines. It probes the candidate VBI lines of both fields of every frame, recovers the packets and writes them as a flat, headerless stream in strictly temporal order (frame → field → ascending line), keeping their transmission coding — Hamming 8/4 on the five prefix bytes, odd parity on the data block — so a consumer decodes the stream exactly as a receiver decodes a live broadcast. Above that it reassembles the packets into data groups per data channel, decodes the teletext record in each group and joins any linked series back into one message, and interprets the NAPLPS of a presentation record into a display list. An application record's data is decoded as function descriptors instead.

A line yields a packet only when its five prefix bytes — packet address, continuity index and packet structure, all under the same Hamming 8/4 code (CEA-516 §3.2.1) — survive correction. Random bytes clear all five about once in a million, and a packet whose prefix is unrecoverable cannot be placed in a data group anyway. The threshold detector also takes the framing code only exactly as CEA-516 §2.2.3 defines it: the framing code is the only thing separating NABTS from 525-line WST, so tolerating an error in it would weaken that separation as well as raising the false-positive rate on noise.

Frames are decoded on several threads while the stream is written from one, and the pass learns where in the line this recording puts its data and which lines carry it, so later frames cost far less than the first ones. Once enough lines have been read, the search for where each line's data burst starts narrows to where this recording's lines have already been seen to start; a narrowed search that finds nothing is repeated over the full window, so this cannot lose a packet. After the first frames only the candidate lines this recording has been seen to carry data on are read, with the full window rechecked periodically so a service starting part way into a recording is still picked up. The thread count changes nothing that is recovered: see `decode_threads` below.

The catalogue of records rides along with the packet stream, which is why this is a sink rather than an analysis sink. That is what the **NABTS Records** tool shows, and leaving `output_path` empty makes it the only thing the run produces.

!!! warning "Not World System Teletext"

    On a 525-line capture NABTS and WST share the clock run-in, the bit rate, the VBI lines and the data levels, and are told apart by the framing code alone — `0xE7` against `0xE4`. Use the **Teletext Sink** for WST. A stage pointed at the wrong service recovers nothing rather than recovering nonsense, so a run that finds almost no packets is the signal to try the other one.

Recovery quality tracks the source's luma bandwidth, and NABTS is far less forgiving of a marginal tape than WST is: a WST page is a grid of independent parity-coded bytes, so damage stays where it lands, while a NABTS record is a stateful byte stream in which one wrong byte changes how the several after it are read. The report's **mean decision confidence** is the number to read first — around 0.5 and above, expect readable pages; well below 0.3, the recording is near the noise floor. The two reference recordings bracket the range: a VHS SP capture can read at 0.55 with 0.02 % of record bytes failing parity, while a VHS EP capture read at 0.22 and browsed as nonsense until the detector modelled the ringing such tapes leave after every bit; it now reads at 0.28, three packets in four arrive with every byte checking out, and its records agree with another transfer of the same tape on 99.9 % of their bytes.

**Parameters**

* `output_path` (file path)
    - Path to the output packet stream; the `.t33` extension is appended if absent.
    - Optional. Left empty, the run decodes exactly as it would but writes no file, which is what to do when the records themselves are what you are after — the **NABTS Records** tool is filled either way. `write_report`, `export_records` and `export_captions` are all written beside the packet stream, so enabling any of them without a path fails the run.
* `first_vbi_line` (integer)
    - First candidate field line probed, 1-based, both fields.
    - Default: `10`.
* `last_vbi_line` (integer)
    - Last candidate field line probed, 1-based, both fields. With the above this gives the window of CEA-516 §1.1.1 and ITU-R BT.653 §2, broadcast lines 10–21 and 273–284, short of line 21 itself: that is the caption line (47 CFR 73.682(a)(22)), whose EIA-608 otherwise gives the occasional noise packet. Set it to 21 for a service that put NABTS there. Narrowing it to the lines a service actually uses keeps noise out of the packet stream and helps the data-phase pin engage; the records catalogued come out the same either way.
    - Default: `20`.
* `keep_empty_packets` (boolean)
    - Emit a whole zero packet for candidate lines with no data so packet position maps 1:1 to (frame, field, line).
    - Default: `false`.
* `detector` (string)
    - How data bits are recovered: `Threshold` (slice at bit centres; exact on direct captures), `MLSE` (fit the recording's frequency response to the known start of each line and detect against it, then refit it to the packet just read — with the ringing a recorder and a capture card leave after every bit — and read it again; recovers data from tape, where limited bandwidth smears bits into their neighbours), or `Automatic` (threshold first, MLSE only where it fails, so a clean source pays nothing extra).
    - A packet sent with the longitudinal check whose MLSE reading fails it or a byte's parity is read again along its own bit timing, which drifts within a line on tape, and then, if it still fails, as the closest bit sequence that passes them, where the samples support that nearly as well — this mends the transposed bits (0x58 read as 0xA8) that parity cannot see.
    - A packet that still fails a check is read once more through a wider window, where its own channel needs one: Some EP tapes respond to a bit three bit periods before its sample, which no narrower window models, and lose the same bytes to it in every copy. It is not done on a packet that opens a page's data group, whose header bytes name the page. The bytes it changed pass their parity because they were read to, so they are never taken as evidence that two copies are different pages. On those tapes it puts right about a quarter of the record bytes the vote of copies had wrong.
    - Default: `Automatic`.
* `decode_threads` (integer)
    - Threads to recover lines on; `0`, the default, uses one per processor. The recovered stream is identical whatever this is set to, so lower it only to leave the machine free for other work.
    - Default: `0`.
* `write_report` (boolean)
    - Write the run's diagnostic report next to the packet stream under its full name plus `.txt` (`mydata.t33` gives `mydata.t33.txt`), so it needs `output_path` set. Beyond the packet-level recovery profile it accounts for every layer above: packets orphaned rather than placed in a group, groups completed, record headers refused, linked series joined, records catalogued. The same report always goes to the log at debug level. Its **Other services (not teletext)** block names the addresses that carried something other than teletext, with the figures they were recognised by: an FEC datacast whose two-byte suffix is RFC 2728's Reed-Solomon row code, and how its bundles lay out data and check rows; an encoder's idle fill (a few packet contents repeated, each byte the one before it shifted a place — the BERT pattern Ross Video (Norpak) encoders send on address F00); a test sequence (one short shift register's output, with its polynomial); clock packets (a Hamming-coded time of day, with the span it covered in UTC); seven-bit text sent in data groups of the provider's own type (such as a network's teletype traffic), which is written beside the report as `mydata.t33.other.txt`. The viewer's notices name them too. The data groups an encoder's idle fill is sent in are not catalogued as records; the report's **Idle fill** line counts them.
    - Default: `false`.
* `export_records` (boolean)
    - Write each record as its own file beside the packet stream, named for the channel, record address and version that identify it (`mydata.t33.000-1A4-v2.rec`), with `-2`, `-3` and so on after it for a second or later page the recording carried under the same identity (`mydata.t33.000-1A4-v2-2.rec`). The file holds the record's data exactly as transmitted — NAPLPS presentation code, or application data for a type 2 record — which is what to hand to an external NAPLPS tool. The caption service is not written this way: its captions reuse sixteen versions (CEA-516 §7.3.10.1), so no file name could tell them apart; `export_captions` carries them.
    - Default: `false`.
* `export_captions` (boolean)
    - Write the recording's captioning as a SubRip file beside the packet stream (`mydata.t33.srt`). The cues are the records the service marked with the caption flag of CEA-516 §5.2.7.3 — the flag, not the data channel, is what selects them — taken as they arrived rather than from the record catalogue: each new caption is a new version of the same address, and the sixteen versions recur throughout a recording (§7.3.10.1), so a caption is a run of consecutive copies carrying one version, combined only with its own copies and dated by the first of them. Cues run in transmission order, each until the next replaces it, timed from the 59.94 fields per second of SMPTE 170M; consecutive captions with the same text are one cue. At most 16,384 captions are kept — about nine times the densest recording measured — and any past that are counted in the report. Colour and positioning are dropped; SRT carries the plain text. A recording that carried no captioning writes no file.
    - Default: `false`.
* `reference_records` (string)
    - Other recordings of the same service — the packet streams (`.t33`) this stage wrote for them, the record files `export_records` wrote beside those streams, or folders of either — separated by semicolons. Their records are what the record viewer's **Reconstruct lost data** fills this recording's lost stretches from, as well as from its own records; nothing else uses them, and they change neither the recovery nor anything a run exports. A record file is read as the record that recording's own run voted, in a moment; a packet stream is replayed through the record assembly again, which takes minutes on a long recording and has no line positions or byte confidences to vote with, so a folder stands for the record files in it and for each stream in it whose records were not exported beside it. A path that cannot be read is reported and skipped. Recordings whose bytes can be trusted make the best references, whether or not they lost packets: recordings that lost the same packets still hold the shared templates at other offsets, while bytes filled from worn EP tapes are wrong a few times in a hundred.
    - Default: empty.
**Stage tools**

* **NABTS Records** — the record viewer for this node. It lists every record the range carried, with its channel and record address, version, record type, how often it was seen and over which frames, and the classification flags the service set. It opens automatically when the node is triggered, which is how the records are reached: leave `output_path` empty and triggering the node is a decode-and-browse with no file written. The catalogue stays with the stage afterwards, so closing the viewer and picking **NABTS Records** from the **Stage Tools** menu re-opens it immediately rather than decoding the source again; on a node that has not been triggered that entry says there is nothing to show instead of starting the decode itself. Editing any stage's parameters rebuilds the graph and discards the catalogue with it, closing the viewer — trigger again to rebuild it from the new settings. Because the catalogue comes from a pass over the whole source rather than a window around the preview position, the list is the service's full carousel. **Page** finds a page by the number a viewer typed on the decoder's keypad — three digits for most pages, four for a service's larger ones (1985 is long address 000198500), the whole nine digits for a More Record — and **Back**, **Index**, **Next** and **More** beside it move between pages as the decoder's keys did: where the page's header names its next page, its index and its continuation (CEA-516 §5.2.8), and otherwise by number, to the section's index and to the Master Index (000). The numbers a page offers — an index's entries, a quiz's answers — are listed beside them as links, and the **Section** column names the index page each page belongs to, where the service named or flagged one, and otherwise the section the Master Index's list of sections ("1.News 10.Weather 20.Finance ...") puts it in — the nearest listed start at or below its number, where the pages run on from there to it — which groups the pages of a service whose headers name no index. Pages of a game are marked in the list: **Game: keypad** on a page played on the Option keys and **Game: quiz** on the pages of a quiz played by page number (the board, its questions and their answers, each answer drawn over the page that offers it). Type **game** into **Page** to list only the pages of games — or any other mark shown in the list, such as **Game: quiz** or **unconfirmed** — and step through them with **Enter** and **Shift+Enter** or the arrows; typing a number, or following a key to a page without the mark, lists every page again. A page written to be drawn over the page it was asked from, as a quiz's pages were drawn over their game board on a decoder that did not clear its screen, is shown over the pages that lead to it — an answer over its question and the board — and the line under it says which. **Save PNG…** writes the record on screen out as an image at the 4:3 shape of its display area, drawn as it stands once finished — every stage drawn and every blinking colour at rest, whatever moment of the animation is on screen; it is offered for a drawn record and not for an application record, which is a listing rather than a picture. Every receiver saves at the same 1536 by 1152 — comparing one with another is what the choice of receiver is for, and images of different sizes would make the comparison about the sizes. That width is a whole multiple of all three grids, so every column of the receiver's raster gets the same number of image columns and the pixels stay hard-edged. **Save SVG…** writes the record on screen as an SVG drawing at the same size, animated as the viewer plays it: each stage appears when it did and blinking colours change, as SMIL animation a web browser plays. It is offered for presentation records only. **Save All PNGs…** writes every drawn record in the catalogue into a chosen folder in one pass, each the image **Save PNG…** would have written for it and named the same way, with a progress dialogue that can be cancelled partway; application records are listings and are skipped. **Save All SVGs…** does the same with the animated drawings **Save SVG…** writes, one per presentation record.
    - A **presentation record** is drawn as its NAPLPS display list, beside the plain text of the page in reading order — an index page is mostly words, and picking them off a rasterised page is tedious. **Show display area** outlines the lower 0.78125 of the unit screen that every receiver is guaranteed to show (ANSI X3.110 Table D1), which is how to tell a record drawn deliberately into one corner from one that was mis-scaled.
    - **Receiver** chooses which of four receivers the page is drawn for, and is the only control for it — there is no parameter, on purpose. ANSI X3.110 draws into an abstract unit screen but sizes stroke width, line-texture dot and dash lengths, hatch spacing and the incremental raster in the physical pixels of the receiver displaying them (§5.3.2.2.6), so a page has no single correct appearance until a receiver is named. `256 x 200` is the grid Table D1 item 10 requires, which is what a set-top decoder of the period put on screen and what the window opens on; `512 x 400` and `768 x 600` are that grid at twice and three times, for a sharper reading of the same page; `256 x 200 (vector)` draws the reference receiver's page as shapes rather than pixels, smooth at any zoom but without the staircase a receiver drew slanted edges with. It draws by the standard's rules, and as the receivers of the day drew: the logical pel is a rectangle hung from the drawing point by one corner (§5.3.2.2.6), never narrower than one of the receiver's pixels and laid from the corner of the pixel the point falls in (Appendix B), so a stroke has the pel's own width and height rather than a round pen's and lies on the pixels the receiver lit; a stroke is as heavy across its slope as the pel is across the axis it runs nearer, as the receiver's line algorithm drew it; a dotted or dashed straight line takes its pattern from the screen, as the receiver laid it, so its dots fall on the receiver's pixels and the dot leaders of a page line up, and round a curve dots and dashes are whole pels along the stroke; a filled figure includes the outline the pel traces round it; a spline of more than three points is its series of straight lines (§5.3.3.3); and in colour mode 2 the spaces a texture leaves are drawn in the background colour. Text is drawn from the platform's fixed-pitch font, stretched to fill each character field whatever its shape; each character turns about its own origin and a negative field size mirrors it in place (§5.3.2.3), and a character the font cannot draw is shown as a hollow box rather than left blank. Only whole multiples of the reference grid are offered: a page is authored against it, and a fraction of it lands between pixels, thickening strokes unevenly and breaking letterforms. The built-in character patterns follow the receiver too — §5.1 leaves them to it "for a given display resolution", so `256 x 200` draws text from a 6 by 10 face, `512 x 400` from a 10 by 20 and `768 x 600` from a 9 by 15 at double size, all from the public-domain X11 misc-fixed family, rather than magnifying one coarse face at every resolution. Headings are drawn larger, as the receivers of the day drew them: those sets carried a second, larger face on an 8 by 12 cell, and a field it fills better — titles set in an 8 by 12 field above all — is drawn in it, at every resolution. Nothing in the recovery depends on the choice — the records are read off the recording, and the receiver decides only how they are drawn afterwards — so picking one redraws the page you are looking at and leaves you on it, without reading the recording again, and nothing is saved with the project. A parameter would instead rebuild the graph and discard every stage's results, which is a full re-run of a recovery pass that cannot see the setting.
    - **Reconstruct lost data** puts back bytes the recording never received, from evidence rather than guesswork, and is off unless you tick it. A lost packet takes a stretch of every record it carried, which NAPLPS reads as nothing, so whatever came after it is drawn wrongly. With the box ticked, a lost stretch is copied from another record — of this recording, or of the reference recordings named by `reference_records` — where the bytes on both sides of it agree over a long enough stretch and every record that agrees as well offers the same bytes — or where one set of bytes is offered by at least ten times as many records as any other: a service built its pages from templates, so what one record lost another very often kept, and a stray reading of it turns up in one record where the template turns up in a dozen. A byte the recording received failing its parity — which proves it wrong — is put right the same way, where another record agrees all round it. What nothing can vouch for is left out, and the rest of the record is read as a receiver would read it with those bytes gone — except that a lost shift between text and drawing commands is put back where the bytes after it plainly read the other way. Pages drawn from rebuilt bytes are marked `~` in the list, and the line under the page says how many bytes were lost and how many were rebuilt, and from which recordings — the first three by name, and how many more. It changes only what is drawn here: the packet stream, the record files and the report always carry the recording's own bytes. On tapes that lost one packet in three to a test signal recorded over their data, it brings back the pages whose templates another recording kept — how many depends on how close in time the references are. A stretch every copy of a record lost is left lost rather than taken from a copy whose bytes moved by a packet it lost unmarked, which held the next packet's bytes there, so the record files carry a hole for it and a reference recording can fill it.
    - **Show text** puts away the text pane beside the drawing, giving the page the whole width. It is offered for a drawn record that carries a text form of itself, which a presentation record does and an application record does not. Words a later filled figure covers for the most part — a timed page's earlier messages, a More page's predecessor — are read a screen at a time before the words that replaced them, rather than interleaved with them, and a word drawn twice a pixel apart, as a shadow, is read once.
    - The panel along the foot of the window is where everything the viewer has to say is collected: what record you are looking at and what shape it is in, what the catalogue as a whole carries, and how the run went. The counts behind those lines — per record, per byte — go to the log and to the run's report rather than into the window.
    - A record listed with **(2)**, **(3)** and so on after its version is another page the recording carried under the same channel, address and version. A carousel gives each page in turn the next of the sixteen versions a record can have, and when it comes round again with new pages the versions are used again; each page the recording saw is listed and drawn on its own rather than blended with the others, numbered in the order they first arrived. A copy counts as another page only by what it carries — compared with every copy held of the pages already seen, a stretch at a time where its bytes moved — or by its length where it and a copy of the other page both arrived intact: a damaged tape cuts copies short at their tail, and a copy cut short is the same page. Two pages built on one template — a carousel's ratings or scores pages — agree on most of their bytes, and are told apart by what each carries that the other does not: text or a picture in a stretch both received cleanly, or bytes the error check vouched for in both. They are listed apart rather than blended.
    - An address that carried more than one record outside a More chain — a carousel, a magazine edition replaced while the tape ran — is listed as one page with its records as sub-pages, in the order they first arrived, which is the order a receiver left on the address showed them. A carousel with fewer pages than versions comes round with the same page under the next version, and such a page is listed once, as one sub-page labelled with every version it was sent under (`v0, v2`). Two records count as one page only where every byte both received undamaged is the same, so a page revised as it was re-sent, even by one score or one word, stays a sub-page of its own, and so do the pages of a carousel built on one template.
    - A record whose name — channel, address or version — the recording never once received without a correction is folded into the page its bytes are, compared over the bytes both received; where it is no other record's page but its copies agree with one another, it is kept and marked **unconfirmed**, and otherwise it is dropped. It is never folded into another page built on the same template: such a record seen once is dropped, or kept and marked **unconfirmed** where its record header — address and version — arrived without a correction and only its channel was corrected. The run's report and the foot of the window say how many were folded, kept and dropped. A record type the standard reserves (4 to 15) is one no service sent and no decoder of the day accepted: a copy reading one is a misread, and such a record is never listed as a page of its own. Nor is a linked series that never received its first record by the end of the recording: the first record is what a receiver starts a page from (CEA-516 §5.2.6), so the rest of a series without it — usually a record whose address or link number was misread — is counted in the report and not listed. A version seen only a few times in the middle of another version of the same address that was sent far more often, and carrying its bytes, is that version with its version number misread, and is folded into it. A record on a data channel the service never used, under the address and version of one of its pages that was seen four times as often or more, and never received whole, is that page's packets misaddressed there by a burst that turned one address code into another — the group it opened took in other pages' blocks — and is not listed; the run's report and the foot of the window count these apart. Where no record's name ever arrived without a correction, there is nothing to judge the rest against: every record is listed, marked **unconfirmed**. And where fewer than a tenth of the packets arrived with their prefix whole (a recording of NABTS gives 70 % or more), the lines read carry another signal behind NABTS's run-in and framing code: the window says so, and nothing is listed or exported.
    - A polygon, arc or curve with a point off the screen is not drawn, as the Panasonic TU-2000x did not draw it; the standard allows a receiver to reject such a figure or to clip it. On a damaged recording such a figure is nearly always a stretch of the page read wrongly after bytes that were lost, and drawn it would cover the page with a shape nobody drew. Rectangles and lines running off the edge are clipped as before.
    - An **application record** is shown as its function descriptors instead, each with its code in the code-table notation of §7.2.2 and its arguments.
    - A record sent as nothing but NUL bytes has no page to draw (ANSI X3.110 §6.1.4), and neither has one that arrived whole and draws nothing at all, so it is named for the role its header gives it rather than shown blank: **Cyclic Marker** (the first record of a data channel's cycle, CEA-516 §5.2.7.7), **Support Record** (§5.2.7.12), **Auto Acquire → 020** (a record that hands the receiver straight on to its More Record, §5.2.7.8), or **Empty record** where the header gives it none. It stays in the list; a record whose NULs stand in for bytes the recording lost is drawn as recovered instead. A record whose code only resets the decoder and draws nothing — NSR, SO and SI, a RESET that resets nothing (ANSI X3.110 §5.3.2.9.3) and NULs, such as a record a service sends every few seconds at an address no keypad reaches — is marked **Reset only** and listed with its bytes.
    - A record whose bytes are not NAPLPS at all — another service's text or binary data sent under a presentation record type, such as a news wire, a Dow Jones test page or a block of binary data — is marked **Not NAPLPS** and listed as that data, as text or in hexadecimal, with a note saying why, rather than drawn as the stray characters and shapes a receiver would have put on screen. The Dow Jones test page is listed as the text it carries with its check bytes taken out — the codes that clutter its text are the sums each of its eight framings adds, not damage — with a note of the framing and how many of its checks held.
    - A record whose classification sequence has more than one pointer byte — is one a Full Service Standard receiver ignores entirely (CEA-516 §8.5.2.7.1), and the receivers of the period refused them. It is drawn all the same, and the line under the page says it is outside the Full Service Standard. That line also lists any flag bytes CEA-516 §5.2.7.2 reserves that the service set anyway.
    - **Enable animations** presents the record the way a receiver put it on screen, on the timing the record itself carries. A record that pauses part way through (the WAIT of ANSI X3.110 §5.3.2.8) is drawn in stages, each appearing once the pause before it has run out. A blink process belongs to a colour map entry rather than to a figure, so what alternates is everything drawn in that one colour and nothing else — and it alternates with the second colour the record named, which may be another colour of the picture (a figure that twinkles) or the ground (one that vanishes and returns). Each process keeps the ON and OFF intervals the record gave it (§5.3.2.7.3), so two processes set up with different intervals blink out of step, as they did on a receiver. The stage works the timing out when it reads the record; the viewer only plays it back, starting from the beginning whenever a record is selected. Clear the box to show the finished record held still — every stage drawn and every blinking colour at rest — for reading or capture; ticking it again plays the record from the start.
    - **Receiver pace** plays each record at the pace a receiver of the day drew it, rather than drawing everything between two pauses at once: every command takes what the Norpak TTX 650 took over it — 3.4 ms a character, a line by the pixels it plots, a filled shape by its rows and pixels (214 ms for the whole screen), an arc by its length, 169 ms a clear, and a few milliseconds even for a command that draws nothing — so a busy page is seen being built up, in its own colours, and runs about as long as it ran on the set. A filled shape and a clear sweep across the screen a row at a time as they did on the set, up the screen from the bottom on the TTX 650. It is off unless you tick it, and changes only when each figure appears, never what is drawn: a saved still is the finished page either way.
    - **TU-2000x pace** plays each record at the pace of the Panasonic TU-2000x instead — a home decoder of the day — whether or not Receiver pace is ticked: 4.7 ms a character, slower than the TTX 650 over large lettering and filled polygons and quicker over arcs and clears (70 ms), a colour loaded at the next field, and fills and clears swept down the screen from the top, as a head-end decoder of the day drew them too. Each receiver's timings put most of its own drawing of the recorded pages within a quarter of their length, and the other's noticeably fewer, so pick the one whose viewers you want to see the page as.
    - The **playback controls** under the drawing play, pause and seek the record on screen, as a player for a moving sign does. They are offered only while animations are enabled and the record changes as it is shown — one drawn in stages, or with something blinking. **Replay** plays it again from the start, with the stages drawn afresh and each blink process restarting its timing; **<** and **>** hold the display before and the next one — each stage, and each turn of a blink — and the slider seeks anywhere in the record, with a clock beside it in minutes, seconds and hundredths. A record that settles plays to two seconds past its last change and stops; one that blinks plays once round the cycle its blinks share and then goes on round it, held as a receiver held it. **Speed** plays the record from a quarter to four times as fast as it was timed, and **Loop** starts it again from the beginning each time it ends; both stay set from one record to the next. With the drawing clicked, Space plays and pauses, Left and Right step, and Shift with Left or Right seeks five seconds. What is on screen at any moment is what the saved SVG shows at that moment of its animation.
    - **Option keys** play a page that was made to be played. ANSI X3.110 lets a page link the macros it defines to a receiver's keys (§5.5, Table D1 item 7), and games made for RCA's decoders are such pages: each defines macros 2/0 to 2/9, never runs them itself, and tells the viewer to "Press Option 1-9". A record like this is marked **Game: keypad** in the list, and while it is on screen a keypad stands beside the drawing, its digits laid out as on a set-top keypad and those the page links enabled. Click a key, or type the digit with the drawing clicked, and the page is redrawn as the decoder redrew it — the key's macro run from wherever the page and the keys before it left the drawing point and the colours, at the receiver's pace if one is ticked. **Reset game** shows the page as sent again, as does moving to another record; where the keypad also types page numbers, **Play** and **Page** over the keys choose which the digits do; a game in progress is redrawn, not lost, when you change **Receiver** or the pace boxes.
    - **Caption track** switches the pane to the recording's captioning as a list of cues with their timings, read from the same captions as `export_captions`. §7.3.10 carries captioning as a run of records that each replace the last, so the cues rather than the individual records are what the service says, and the caption service appears in the list as that one item rather than record by record. The control is enabled only on a recording that carried captioning, and the line above the list says which channels and addresses those were.

**Notes**

* NTSC and PAL-M sources are accepted. A 625-line source reports an error: CEA-516 §1.1.1 specifies NABTS on the 525-line signal, and no 625-line service exists to recover.
* This stage writes no CSV — its file output is the packet stream, and optionally the per-record files, the caption document and the report.
* Nothing is corrected in the packet stream. Where a packet carries the longitudinal check byte of §3.4, that byte and the per-byte parity form a product code and a single-bit error in the data block is located and corrected before the record layer sees it; whether a service sends it is the service's choice.
* Byte parity is not used to repair damaged bytes as it is for World System Teletext. CEA-516 §3.3 gives the data block odd parity only when its data group is of type 0, and a single packet does not say which type its group is.
* A byte of a presentation record that still fails its parity once the copies are combined is known to be wrong, and is not run as a control or a drawing command: a clear or a shift it reads as is skipped, as the Panasonic TU-2000x skipped it, so one damaged byte no longer wipes a page or turns its drawing into text. The line under the page says how many were skipped.
* Where no copy of a record arrives undamaged, its copies are combined byte by byte into the record shown; the moment an undamaged copy arrives, it is shown as it stands. A copy whose data group ended short of its declared length — its last packets lost — is held at that length with the lost blocks as a hole, so it still lines up with the others.
* A copy that lost its packets over a run long enough for the continuity index to have come round — sixteen or more lines that carry data gone by with nothing on them, as a dropout, a run of unreadable fields or a break in the recording leaves — cannot say where the bytes after the run belong, and they are left out of that copy rather than placed. A page's copy no longer runs on into the next page's packets and gets listed as a page of its own.

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

This stage extracts raw EFM (Eight-to-Fourteen Modulation) t-values from the incoming stream and writes them to a binary file. The output contains only 8-bit unsigned integers representing valid t-values in the range 3–11, stored field by field with no headers or additional formatting.

**Parameters**

* `output_path` (string)
    - Path to the output EFM file (raw t-values). Conventionally uses the `.efm` extension.
    - Required.

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

The output can be used directly with existing ld-decode tools.

**Sidecar files**

The sidecars are named off the same base as the TBC with the `.tbc` replaced, so `disc.tbc` is accompanied by `disc.pcm` and `disc.efm` — the layout TBC Source auto-detects, which means an export drops straight back in as a source.

They are built from what reaches the sink through the pipeline, not from any file the original source read, so it makes no difference whether the chain started at a TBC Source, a CVBS Source, or a stage that produced the audio itself.

* `disc.pcm` — headerless signed 16-bit little-endian stereo at 44100 Hz, as ld-decode writes it, with the layout recorded in the metadata database. Pipeline audio is 48 kHz 24-bit, so it is resampled and narrowed on the way out; this is not lossless, and repeated export/re-import cycles will degrade the audio slightly each time. Only one channel pair fits, chosen with the **Audio Channel Pair** parameter.
* `disc.efm` — one byte per t-value in field order. The per-field counts written to the metadata database are what let TBC Source locate each frame's payload again.

**Parameters**

* `output_path` (string)
    - Base path for the output files — the stage appends the `.tbc` and `.tbc.db` extensions automatically. The `.pcm` and `.efm` sidecars replace the `.tbc` rather than extending it.
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
* AC3 RF is not written as a sidecar — use the AC3 RF Sink in parallel. The Audio Sink and Raw EFM Data Sink are still useful for putting a WAV or a standalone `.efm` somewhere other than beside the TBC, or for exporting a second channel pair as well.
* This stage was called **ld-decode Sink** (`ld_sink`) before it was renamed to pair with TBC Source. Projects saved under the old name are migrated automatically when they are opened.

---

## Teletext Sink

| | |
|-|-|
| **Stage id** | `teletext_sink` |
| **Stage name** | Teletext Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Recover World System Teletext from the VBI, export the packet stream, and browse the pages the recording carried |

**Use this stage when:**

* Preserving teletext carried by a LaserDisc, CVBS capture, or tape source
* Reading the pages a recording carried without leaving decode-orc
* Producing a packet stream for external teletext tools (vhs-teletext, wxTED)

**What it does**

Triggering the stage makes one linear pass over the whole frame range. It probes the candidate VBI lines of both fields of every frame for teletext data lines, recovers the packets, and writes them as a flat, headerless packet stream in strictly temporal order (frame → field → ascending line). Frames are decoded on several threads while the stream is written from one, and the pass learns where in the line this particular recording puts its data and which lines carry it, so later frames cost far less than the first ones — together these take a pass over the reference captures from 1970 ms to 295 ms (625-line) and 946 ms to 156 ms (525-line). The thread count changes nothing that is recovered: see `decode_threads` below. Packets keep their transmission coding (Hamming 8/4 addressing, odd-parity display bytes), so consumers decode the stream exactly as a receiver decodes a live broadcast.

A line yields a packet only when its magazine/row address survives Hamming 8/4 correction (ETSI EN 300 706 §8.2), which suppresses false locks on noise; a packet whose address is unrecoverable could not be placed on a page anyway. The threshold detector also takes the framing code only exactly as §6.2 defines it: accepting one wrong bit would recover a few more packets from a noisy source at the cost of more false locks.

Most of the work of reading a line is searching the whole of the standard's data-timing window for where the data burst starts. Every line of a time-base-corrected recording starts at very nearly the same place, so once enough lines have been read the search narrows to where they agreed. A narrowed search that finds nothing is repeated over the full window, so this cannot lose a packet; it costs a few percent on lines that carry no data. Measured on the reference captures this is the larger of the two savings, and it also *recovers* packets an exhaustive search misses — narrowing the window rejects the false correlation peaks a whole-window search can settle on. The report says where the window was pinned, or why it was not.

A service uses a few of the lines its standard permits, and on a line carrying picture content, VITS, VITC or captions, reading it spends work reaching a rejection already reached on the frame before. So every line is read for the first 50 frames, then only the lines that have carried a packet, with the full window rechecked every 50th frame so a service that starts part way into a recording is still picked up. Unlike the narrowed search this can lose a packet: a line that carries data exactly once, outside both the learning frames and a recheck frame, is not read. On the reference PAL capture that cost one packet in 3,964.

Both television systems ITU-R BT.653 defines System B on are covered, and the service decides the file the run writes:

* **625 lines** (PAL) — ETSI EN 300 706, 42-byte packets, written as `.t42`
* **525 lines** (NTSC, PAL-M) — BT.653 Table 1b, 34-byte packets, written as `.t34`

The same pass assembles the pages, so the run also produces a catalogue of every page the recording carried — where each was first and last seen, how often the carousel brought it round, and its best assembly from every copy recovered. That is what the **Teletext Pages** tool shows. Because it comes from a pass over the whole source rather than a window around the preview position, the list is the service's full carousel. A page number transmitted as a sequence of sub-pages (ETSI EN 300 706 Annex A.1 — on a receiver, a page that changes on its own every few seconds) is catalogued as the whole sequence, one assembly per sub-page.

Recovery quality tracks the source's luma bandwidth: LaserDisc and broadcast-quality CVBS captures are read exactly by threshold slicing, while consumer VHS loses the clock run-in entirely and needs the MLSE detector, which recovers readable pages from PAL SP and LP recordings. The default `detector` setting picks between the two per line, so neither source needs configuring. By default the stage also mends display bytes that fail their parity check and combines the repeated transmissions of each page row before writing (`repair_damaged_bytes`, `squash_repeated_rows`); turn both off to write the packets exactly as recovered.

**Parameters**

* `output_path` (file path)
    - Path to the output packet stream. The service's extension is appended if absent — `.t42` on a 625-line source, `.t34` on a 525-line one.
    - Optional. Left empty, the run decodes exactly as it would but writes no file, which is what to do when the pages themselves are what you are after — the **Teletext Pages** tool is filled either way. `write_report` and `export_subtitles` are written beside the packet stream, so enabling either without a path fails the run.
* `first_vbi_line` (integer)
    - First candidate field line probed, 1-based, both fields.
    - Default: `6` on a 625-line project, `10` on a 525-line one.
* `last_vbi_line` (integer)
    - Last candidate field line probed, 1-based, both fields.
    - Default: `22` on a 625-line project, `20` on a 525-line one: a 525-line recording's line 21 is the caption line (47 CFR 73.682(a)(22)), whose EIA-608 otherwise gives the occasional noise packet. Set it to 21 for a service that put teletext there.
* `keep_empty_packets` (boolean)
    - Emit a whole zero packet for candidate lines with no data so packet position maps 1:1 to (frame, field, line) — the vhs-decode convention.
    - Default: `false`.
* `detector` (string)
    - How data bits are recovered: `Threshold` (slice at bit centres; exact on discs and direct captures), `MLSE` (fit the recording's frequency response to the known start of each line, detect against it, then refit that response to the whole packet just read — with the ringing a recorder and a capture card leave after every bit — and read it again; recovers teletext from tape, where limited bandwidth smears bits into their neighbours), or `Automatic` (threshold first, MLSE only where it fails — same behaviour and cost as threshold alone on a disc source).
    - Default: `Automatic`.
* `character_set` (string)
    - Which alphabet the page codes are read in: `Latin`, `Cyrillic (Russian/Bulgarian)`, `Cyrillic (Ukrainian)` or `Cyrillic (Serbian/Croatian)`. A display code means nothing on its own — 4/4 is `D` in the Latin set and `Д` in the Russian one — and a service that says which set it uses (a packet X/28/0 or M/29/0, ETSI EN 300 706 §15.2) is always believed, so this is consulted only for pages that designate nothing. That is most of what survives on tape: those packets are a Level 2.5 facility, and for Level 1 the standard falls back on "a local Code of Practice" — the region the receiver was sold in. Nothing in the recording can settle it, so the choice has to be yours. It affects only the pages you read and any subtitles exported from them; the exported packet stream is the transmitted bytes and carries no character set. Within Latin, the national option sub-set (`£` for a UK service, `é` for a French one) still comes from each page's own header bits and needs no setting.
    - Default: `Latin`.
* `second_character_set` (string)
    - The alphabet the `ESC` control character switches into, for services that mix two on one page — a Russian or Ukrainian service writes foreign names in Latin letters in among the Cyrillic, marking each switch with the code that toggles the rest of the display row into the other alphabet (ETSI EN 300 706 §12.2 Table 26 code 1/B, §15.3). `None` or one of the four sets `character_set` offers. As with `character_set`, a service that designates its own pair is always believed and this is ignored — including when it says it uses only one, which switches the control code off. `None` means the same for material that designates nothing: one alphabet throughout and the code inert, which is how pages were read before. Set it only where a recording really does mix alphabets; a wrong pairing turns the switched runs into nonsense rather than leaving them alone.
    - Default: `None`.
* `repair_damaged_bytes` (boolean)
    - Every display byte carries a parity bit, so a byte that fails its parity check is known to be damaged. Restore it by flipping the bit the MLSE detector came closest to reading the other way. Recovers characters a difficult tape would otherwise lose; the cost is that a repaired byte can no longer be told from an undamaged one, so a repair that guessed wrong is no longer marked as damage. Applies to the MLSE detector only, so a disc or direct capture is unaffected.
    - Default: `true`.
* `calibrate_bit_clock` (boolean)
    - A capture card sampling against its own clock rather than the video's reads the data a little faster or slower than the standard's bit rate, which slips the last bytes of every packet. The first packets are read again at a range of trial clocks, and the rest of the recording is read at whichever clock reads the most packets with every byte checking out, where it beats the standard's clock by more than chance allows; a time-base-corrected capture keeps the standard's clock. A byte of alternating bits (0x55 or 0xAA) is left out of that test: it never passes parity, a service sends one only as something other than a character — VirText pages in Zenith's layout frame every row with 0x55 — and a slightly wrong clock misreads it into a byte that does pass, which once had such recordings read a few tenths of a per cent slow, slipping a bit near the end of their packets.
    - Default: `true`.
* `decode_threads` (integer)
    - Threads to recover lines on; `0`, the default, uses one per processor. Each line is recovered from its own samples, so frames are decoded several at a time while the packets are written from one thread in the order they were transmitted.
    - The recovered stream is identical whatever this is set to, so lower it only to leave the machine free for other work. Measured on the 625-line reference capture: 1406 ms on one thread, 442 on four, 295 on eight; past the processor's physical cores there is nothing more to win, because the run is by then waiting on the source stage to hand out frames.
    - Default: `0`.
* `squash_repeated_rows` (boolean)
    - Teletext pages are transmitted on a loop, so a recording holds several copies of every page row, damaged in different places. Combine them byte by byte — preferring values that pass their parity check, then weighting by how sure the detector was of each byte — and write the combined rows. Packet order, count and timing are unchanged; only damaged display bytes move. The pages shown in the viewer are built from the combined rows too. Needs a second pass over the recovered packets, held in memory (roughly 50 bytes each).
    - Copies are combined only within one run of a page: a header with the erase bit set (C4) says the content is being replaced, so what follows is a different page sharing a number. A service that erases on every transmission gives each one a run of its own, and nothing can be combined — the report says so, as a run count matching the transmission count.
    - Default: `true`.
* `page_separation` (string)
    - How the transmissions of a page number are filed into pages: `Page number` or `Transmission (VirText)`. Needs `squash_repeated_rows`, which is what compares the transmissions.
    - `Page number` is the standard's model: a page number and its sub-code name one page, and a transmission is a new version of it only where its rows show the content was replaced.
    - `Transmission (VirText)` is for VirText, a 1980s text service that sent a slideshow under one page number to the Zenith terminals at cable head-ends — a page rotating through its screens a few seconds apart, with nothing in the header to tell them apart. Every transmission may be a screen of its own: it stays with the screen on show where none of its rows changed, goes to the screen it differs from least where that is by fewer than three rows, and otherwise starts a new screen. The screens become the page's versions, and the viewer steps through them. A row numbered below the last one ends a transmission, since it begins a page whose header was lost. Every screen of the slideshow comes round again, so a screen only one transmission showed, of a page another screen of which came round more than once, is left out. Pages sent in Zenith's own packet layout — 30-column rows framed by the byte 0x55, columns 30-39 in packets 20 and 21 — are read in it; the standard pages beside them are read as always.
    - Default: `Page number`.
* `write_report` (boolean)
    - Write the run's diagnostic report next to the packet stream under its full name plus `.txt` (`mydata.t42` gives `mydata.t42.txt`, `mydata.t34` gives `mydata.t34.txt`), so it needs `output_path` set. It opens with the result in one line — `Data loss 1.14% — 30 of 2,640 recovered characters are damaged` — and the same figure appears in the stage's status when the run finishes. Below that it covers what was exported, how recovery went, how many pages were catalogued, and what combining repeated rows changed. The same report always goes to the log at debug level.
    - Damage is counted by the odd parity every display byte carries, over the display rows as written. It is a floor rather than an exact count — a byte damaged in two bits passes parity — and it says nothing about rows that never arrived.
    - Default: `false`.
* `export_subtitles` (boolean)
    - Decode the subtitle page alongside the packet export and write timed cues to a `.srt` file next to the output. Offered on 625-line projects only: the cue timing derives from 50 fields per second. Text is written as UTF-8 through the page's own character set, so a UK page's `£` and a Cyrillic service's cues both reach the file as themselves.
    - Default: `false`.
* `subtitle_page` (string)
    - Teletext page carrying the subtitles: magazine digit (1–8) plus two hexadecimal page digits, e.g. `888`.
    - Default: `888`.
* `subtitle_format` (string)
    - Subtitle output format; currently `SRT` (SubRip) only.
    - Default: `SRT`.

**Stage tools**

* **Teletext Pages** — the page viewer for this node. It lists every page the range carried, with how many times each was seen and the frames it was first and last seen at, and renders the selected page as a Level 1 display alongside the run's recovery summary. It opens automatically when the node is triggered, which is how the pages are reached: leave `output_path` empty and triggering the node is a decode-and-browse with no file written. The catalogue stays with the stage afterwards, so closing the viewer and picking **Teletext Pages** from the **Stage Tools** menu re-opens it immediately rather than decoding the source again; on a node that has not been triggered that entry says there is nothing to show instead of starting the decode itself. Editing any stage's parameters rebuilds the graph and discards the catalogue with it, closing the viewer — trigger again to rebuild it from the new settings. **Save PNG…** writes the page on screen out as an image, drawn on its own at whole pixels per character rectangle — a 40 x 25 page comes out 960 x 1000 — with flashing characters held lit and the data-error overlay included if it is switched on. **Save All PNGs…** writes every page and sub-page in the catalogue into a chosen folder in one pass, each the image **Save PNG…** would have written for it and named the same way — `Page-100-0001.png`, `Page-100-0002.png`, and so on — with a progress dialogue that can be cancelled partway.
    - Where a page number carries a sequence of sub-pages, the control under the display says how many there are, which one is on screen and its sub-code (`0001`, `0002`, …), and the arrows either side step through the sequence, wrapping round at each end. The status line then describes the sub-page on screen rather than the whole set; the page list keeps counting the set together, because a row there is about the page number. A page's row says in its tooltip when it is a sequence.
    - Characters are drawn in the typeface of the receiver's own character generator, the Mullard SAA5050 series — the chip that put a page on screen on a British receiver and on the BBC Micro in Mode 7 — including the character rounding it applied on the way to the display, which is what gives teletext letterforms their sloped diagonals. Both the Latin and the Cyrillic G0 sets are covered, the latter from the SAA5057 that drew them. A character the family never held is drawn in the platform's monospaced font instead, which looks out of place but stays legible.
    - **Enable animations** flashes the characters the service marked to flash, on the 0.75 Hz cycle a World System Teletext receiver used — three quarters of each second showing the character and the last quarter blank. Only the characters alternate; their background stays put. Clear the box to hold the page still, which is what reading a flashing headline or capturing the display wants.

**Notes**

* PAL, NTSC and PAL-M sources are accepted; any other video system reports an error. NABTS (System C) shares the 525 lines but not the framing code, so its lines are seen and rejected rather than decoded — use the **NABTS Sink** for those. NTSC line-21 captions are handled by the Closed Caption Sink instead.
* This stage writes no CSV — its file output is the packet stream, and optionally the subtitle document and the report.
* The `.t42` format is described on the zxnet teletext wiki (T42 packet stream); `.t34` is the same flat, headerless convention at the 525-line packet length.
* A 525-line service sends the last eight columns of its rows in separate row-extension packets, which the page viewer reassembles; the packet stream holds them as transmitted.
* A page header's sub-code arrives in four Hamming 8/4 bytes, and a correction can land on the wrong digit without a sign. Where a header's sub-code needed correction and names one the page has never been sent with whole, the page keeps its last sub-code, so a tape's misreadings do not appear as sub-pages of their own; a page's first header, and a corrected sub-code the page has been sent with, are taken as read.
* Subtitle export drops Level 1 colour and positioning attributes; the `.srt` carries plain text timed from the field rate. With `squash_repeated_rows` enabled the cues are decoded from the combined rows, so they benefit from the same correction.
* Combining repeated rows ("squashing") is an idea taken from [vhs-teletext](https://github.com/ali1234/vhs-teletext) by Alistair Buxton. A row transmitted only once cannot be corrected, so the benefit grows with how long the recording runs and how often each page recurs.

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

Applies the selected chroma decoder to convert the incoming TBC video stream to colour video, then writes the result according to the selected output mode. In FFmpeg mode the video is encoded into the chosen container and codec, optionally embedding pipeline audio (up to 8 channel pairs, one output stream per pair), closed captions (as mov_text subtitles, MP4/MOV only), and chapter markers derived from VBI data. In raw mode the decoded frames are written to a file without compression; the raw format determines the pixel layout and whether a Y4M header is prepended.

For guidance on selecting and tuning a decoder, see the [Chroma decoder guide](chroma-decoder-guide.md).

**Parameters**

* `output_path` (string)
    - Output file path. Match the extension to the selected mode and format: `.mp4`, `.mkv`, `.mov`, or `.mxf` for FFmpeg output; `.rgb`, `.yuv`, or `.y4m` for raw output.
    - Required.

* `decoder_type` (string)
    - Chroma decoder to apply. PAL: `pal2d`, `transform2d`, `transform3d`. NTSC: `ntsc1d`, `ntsc2d`, `ntsc3d`, `ntsc3dnoadapt`. Other: `mono`.

* `output_mode` (string)
    - Output path selection. Values: `ffmpeg` (encoded output via FFmpeg), `raw` (uncompressed file output). Default: `ffmpeg`.

* `raw_format` (string)
    - Raw output format (raw mode only). Values: `rgb` (RGB48, 16-bit per channel), `yuv` (YUV444P16, planar), `y4m` (YUV444P16 with Y4M header). Default: `rgb`.

* `ffmpeg_format` (string)
    - Container and codec (FFmpeg mode only). Values include `mp4-h264`, `mkv-ffv1`, `mkv-ffv1-bt601`, `mov-prores`, `mov-v210`, `mov-v410`, `mxf-mpeg2video`, `mov-h264`, `mp4-hevc`, `mov-hevc`, and `mp4-av1`. Default: `mp4-h264`.
    - `mkv-ffv1-bt601` is the **FFV1 for VP415e** preset: lossless FFV1 resampled onto the ITU-R BT.601 13.5 MHz sampling grid instead of the 4fsc grid, for players driving real BT.601 hardware. 720 pixels wide, line count unchanged, horizontal-only so interlacing survives; the whole 720-sample window is taken from source, so nothing is cropped and the margins carry the source's own blanking. Defaults to 8-bit 4:2:2 and 16 slices. `output_padding` does not apply.

* `bt601_bit_depth` (string)
    - Output bit depth for `mkv-ffv1-bt601`. Values: `8` (yuv422p), `10` (yuv422p10le). Default: `8`.

* `ffv1_slices` (string)
    - FFV1 slices per frame (FFV1 formats only). Values: `auto`, `4`, `12`, `16`, `24`, `30`, `36`. Default: `auto` — 4 for `mkv-ffv1`, 16 for `mkv-ffv1-bt601`. The count is fixed at encode time and caps decoder parallelism.

* `chroma_gain` (double) / `chroma_phase` (double)
    - Chroma gain multiplier (0.0–10.0, default 1.0) and phase rotation in degrees (-180 to 180, default 0).

* `luma_nr` (double) / `chroma_nr` (double)
    - Luma / chroma noise reduction levels. Higher values reduce noise at the cost of sharpness or chroma resolution.

* `ntsc_phase_comp` (bool)
    - Enable NTSC phase compensation. NTSC sources only.

* `simple_pal` (bool)
    - Enable simple PAL chroma decoding (1D UV filter for Transform PAL). `transform2d`/`transform3d` decoders only.

* `transform_threshold` (double)
    - Similarity threshold for the Transform PAL decoder. Higher = more transform filtering. Range: 0.0–1.0. Default: 0.4. `transform2d`/`transform3d` decoders only.

* `chroma_weight` (double)
    - Chroma weight for the NTSC 3D adaptive filter. Higher = prefer more 2D result. Range: 0.0–10.0. Default: 1.0. `ntsc3d`/`ntsc3dnoadapt` decoders only.

* `adapt_threshold` (double)
    - NTSC 3D adaptive filter threshold. Higher = prefer more 3D result. Range: 0.0–10.0. Default: 1.0. `ntsc3d` decoder only.

* `output_padding` (int)
    - Alignment padding added to each output frame. Default: 8.

* `encoder_preset` (string)
    - FFmpeg mode only. Encoder speed/quality trade-off. Values: `fast`, `medium`, `slow`, `veryslow`.

* `encoder_crf` (int)
    - FFmpeg mode only. Constant Rate Factor for quality-based encoding. Range: 0–51 (lower = higher quality). Default: 18. Used when `encoder_bitrate` is 0.

* `encoder_bitrate` (int)
    - FFmpeg mode only. Target bitrate in bits per second. When non-zero, overrides CRF mode. Default: 0 (use CRF).

* `hardware_encoder` (string)
    - FFmpeg mode only. Hardware-accelerated encoding backend. Values: `none`, `vaapi`, `nvenc`, `qsv`, `amf`, `videotoolbox`. Default: `none`.

* `prores_profile` (string)
    - FFmpeg mode only, `mov-prores` format. ProRes quality profile: `proxy`, `lt`, `standard`, `hq`, `4444`, `4444xq`. Default: `hq`.

* `use_lossless_mode` (bool)
    - FFmpeg mode only. Enable mathematically lossless encoding (H.264/H.265/AV1 only, overrides CRF). Default: `false`.

* `apply_deinterlace` (bool)
    - FFmpeg mode only. Apply bwdif deinterlacing for progressive web playback. One frame is produced per field, so the output frame rate doubles (50 fps PAL, 59.94 fps NTSC). Default: `false`.

* `display_aspect_ratio` (string)
    - FFmpeg mode only. Display aspect ratio signalled to players. Metadata only — the video is not rescaled. Values: `auto` (square pixels), `4:3`, `16:9`. Most SD material should be played back at `4:3`. Default: `auto`.

* `video_filter` (string)
    - FFmpeg mode only. Custom FFmpeg video filter chain applied before encoding, using the same syntax as ffmpeg's `-vf` option (e.g. `fieldmatch,decimate` for inverse telecine, `crop=692:554`). Filters may change output dimensions and frame rate; the encoder follows the filter output automatically. An invalid filter string fails the export with the FFmpeg error message. Default: empty (no filtering).

* `embed_audio` (bool)
    - FFmpeg mode only. Embed pipeline audio into the output file, one output audio stream per selected channel pair. Requires audio in the pipeline. Default: `false`.

* `audio_channel_pairs` (string)
    - FFmpeg mode only; available only when `embed_audio` is enabled. Which audio channel pairs to embed: `all` (default) or a comma-separated list of 0-based channel pair indices, e.g. `0,2`. Indices match the CVBS container's `_audio_<p>.wav` numbering. The export fails if a listed channel pair does not exist.

* `audio_gain_db` (double)
    - FFmpeg mode only; available only when `embed_audio` is enabled. Gain applied to the embedded audio in decibels. `0` = unchanged; positive boosts (6 dB roughly doubles the amplitude), negative attenuates. Samples are clipped at full scale. Range: -24 to 24. Default: `0`.

* `embed_closed_captions` (bool)
    - FFmpeg mode only. Embed closed captions as mov_text subtitles. MP4/MOV output only. Default: `false`.
    - The container carries one subtitle track, so this embeds the primary caption service, CC1. Use the Closed Caption Sink to export CC2 or a text service.
    - Pairs are taken as the Closed Caption Sink takes them — both bytes' parity, the bytes with no function on their field ignored, the codes EIA-608 defines for their field, Line 21 data around them, and the field order — so the subtitles are the sink's CC1, and a capture whose first stored field is television field 2 still has its captions found.

* `embed_chapter_metadata` (bool)
    - FFmpeg mode only. Write chapter markers derived from VBI data into the output file. Default: `false`.

* `embed_disc_metadata` (bool)
    - FFmpeg mode, Matroska only. Attach the LaserDisc VBI metadata — picture numbers, programme time codes, chapters, stop codes, lead-in/lead-out and programme status — as a file inside the output, so a player emulator needs no sidecar. Default: `false`.
    - The attachment is `orc-disc-metadata.yaml`, and the disc-level summary is mirrored into container tags so `ffprobe` shows it without extracting anything. MP4 and MOV cannot carry attachments, so asking for this on another container fails the export rather than quietly writing a file without the document.
    - Enabling it adds a VBI pre-scan over the export range: Matroska writes attachments into the file header, so the whole map must exist before the first frame is encoded. `embed_chapter_metadata` shares that pass when both are on.
    - The format is documented in full in [Embedded Disc Metadata Format](../../technical/disc-metadata-format.md).

* `disc_metadata_detail` (string)
    - How much disc metadata to write: `map` (address map, events and disc status — a few kilobytes) or `full` (also the raw biphase words for every field, about 2.3 MB for a full side, so codes decode-orc does not yet interpret stay recoverable). Default: `map`.

**Stage tools**

* **FFmpeg Preset Config** — a preset helper dialog that applies well-tested encoder combinations without setting each parameter manually. Applying a preset switches the stage to FFmpeg output mode. The **FFV1 for VP415e** preset in the Lossless/Archive category also turns audio embedding on and returns the display aspect ratio to Auto, because that export signals the BT.601 sample aspect ratio itself. Because that preset exists to produce a self-contained file for a player emulator, it turns on disc metadata and chapter markers too — an emulator needs the picture-number map to seek at all, and chapters fall out of the same VBI pass. The Embed disc metadata option is disabled for MP4 and MOV presets, which cannot carry attachments.

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

---

## Removed stages

### HackDAC Sink (removed in v2.0)

The `hackdac_sink` stage was removed in Decode-Orc 2.0. It is no longer available in the plugin registry. Projects that referenced this stage must be recreated without it.
