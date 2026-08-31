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
| **Purpose** | Extract and write one NTSC Line 21 closed-caption (CC) service |

**Use this stage when:**

* Working with NTSC sources containing Line 21 closed captions
* You want to extract captions for archival or conversion
* You want to inspect CC data independently of video

**What it does**

Line 21 carries four services multiplexed into the same two bytes per field: **CC1** and **CC2** (captions) and **TEXT1** and **TEXT2** (pages of text such as schedules, scores or station information). Which service a byte pair belongs to is decided by the control codes before it, so a recording that used more than one produces garbled output if they are read together — a caption running through a page of listings.

The stage reads the two caption bytes from each field's VBI Line 21, keeps the pairs belonging to the service you select, and writes them in the chosen format.

**Parameters**

* `output_path` (string)
    - Path to the closed-caption output file. Use `.scc`, `.srt`, `.txt` or `.html` to match the format.
    - Required.

* `service` (string)
    - Which of the services multiplexed onto Line 21 to export.
    - Allowed values: `CC1` (the primary caption service), `CC2` (a second caption service, often a translation), `TEXT1`, `TEXT2`.
    - Default: `CC1`.

* `format` (string)
    - Export format.
    - Allowed values: `Scenarist SCC`, `SubRip SRT`, `Plain Text`, `HTML`.
    - Default: `Scenarist SCC`.

**Notes**

* Handles NTSC Line 21 only; PAL sources do not carry Line 21 CC data.
* CC3, CC4, TEXT3 and TEXT4 are carried on Line 21 of the second field, which is not decoded, so they are not offered.
* To export more than one service, add a second Closed Caption Sink with its own output path.
* `Scenarist SCC` records the selected service's byte pairs exactly as transmitted, including the duplicate copy of each control code; the tools that read an SCC file de-duplicate for themselves. The decoded formats keep the caption display's rows on separate lines with their indent, so a text service's columns still line up.
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

Frames are decoded on several threads while the stream is written from one, and the pass learns where in the line this recording puts its data and which lines carry it, so later frames cost far less than the first ones. None of it changes what is recovered: see `decode_threads`, `pin_data_phase` and `learn_active_lines` below.

The catalogue of records rides along with the packet stream, which is why this is a sink rather than an analysis sink. That is what the **NABTS Records** tool shows, and leaving `output_path` empty makes it the only thing the run produces.

!!! warning "Not World System Teletext"

    On a 525-line capture NABTS and WST share the clock run-in, the bit rate, the VBI lines and the data levels, and are told apart by the framing code alone — `0xE7` against `0xE4`. Use the **Teletext Sink** for WST. A stage pointed at the wrong service recovers nothing rather than recovering nonsense, so a run that finds almost no packets is the signal to try the other one.

Recovery quality tracks the source's luma bandwidth, and NABTS is far less forgiving of a marginal tape than WST is: a WST page is a grid of independent parity-coded bytes, so damage stays where it lands, while a NABTS record is a stateful byte stream in which one wrong byte changes how the several after it are read. The report's **mean decision confidence** is the number to read first — around 0.5 and above, expect readable pages; approaching 0.2, the recording is at the noise floor and no amount of decoding will recover a page from it. The two reference recordings bracket the range: a VHS SP capture of ExtraVision reads at 0.55 with 0.02 % of record bytes failing parity, while a VHS EP capture of NBC Teletext reads at 0.22 with 7.10 % failing, and browses as nonsense.

**Parameters**

* `output_path` (file path)
    - Path to the output packet stream; the `.t33` extension is appended if absent.
    - Optional. Left empty, the run decodes exactly as it would but writes no file, which is what to do when the records themselves are what you are after — the **NABTS Records** tool is filled either way. `write_report`, `export_records` and `export_captions` are all written beside the packet stream, so enabling any of them without a path fails the run.
* `first_vbi_line` (integer)
    - First candidate field line probed, 1-based, both fields.
    - Default: `10`.
* `last_vbi_line` (integer)
    - Last candidate field line probed, 1-based, both fields. With the above this gives the window of CEA-516 §1.1.1 and ITU-R BT.653 §2: broadcast lines 10–21 and 273–284. Narrowing it to the lines a service actually uses keeps noise out of the packet stream and helps the data-phase pin engage; the records catalogued come out the same either way.
    - Default: `21`.
* `keep_empty_packets` (boolean)
    - Emit a whole zero packet for candidate lines with no data so packet position maps 1:1 to (frame, field, line).
    - Default: `false`.
* `detector` (string)
    - How data bits are recovered: `Threshold` (slice at bit centres; exact on direct captures), `MLSE` (fit the recording's frequency response to the known start of each line and detect against it; recovers data from tape, where limited bandwidth smears bits into their neighbours), or `Automatic` (threshold first, MLSE only where it fails, so a clean source pays nothing extra).
    - Default: `Automatic`.
* `tolerant_framing` (boolean)
    - Accept a framing code with one bit error. Worth leaving off: the framing code is the only thing separating NABTS from 525-line WST, so tolerating an error in it weakens that separation as well as raising the false-positive rate on noise.
    - Default: `false`.
* `require_valid_prefix` (boolean)
    - Drop packets whose five prefix bytes — packet address, continuity index and packet structure, all under the same Hamming 8/4 code — do not survive correction. Random bytes clear all five about once in a million, and a packet whose prefix is unrecoverable cannot be placed in a data group anyway.
    - Default: `true`.
* `pin_data_phase` (boolean)
    - Narrow the search for where each line's data burst starts to where this recording's lines have already been seen to start. A narrowed search that finds nothing is repeated over the full window, so this cannot lose a packet.
    - Default: `true`.
* `learn_active_lines` (boolean)
    - After the first frames, read only the candidate lines this recording has been seen to carry data on, rechecking the full window periodically so a service starting part way into a recording is still picked up.
    - Default: `true`.
* `grammar_assisted_vote` (boolean)
    - A record no undamaged copy of ever arrived is recovered by voting its copies together byte by byte, and some positions the copies leave exactly level — two of them disagreeing once each, which the vote can only settle by taking the most recent. Ask the NAPLPS grammar about those positions instead: the record is read through the linter with each candidate in place, and the one that leaves it best formed wins, or none of them where the grammar has no preference. It is asked only about level positions, never about one the evidence settled, and only about presentation records — an application record's data is not NAPLPS. Unlike **Syntax repair** in the record viewer, this changes the recovered record data itself, and so the files `export_records` writes and every later reading of the record, which is why it is a parameter rather than a viewer control. The packet stream is untouched either way. The report says how many positions were level and how many the grammar settled.
    - Default: `true`.
* `decode_threads` (integer)
    - Threads to recover lines on; `0`, the default, uses one per processor. The recovered stream is identical whatever this is set to, so lower it only to leave the machine free for other work.
    - Default: `0`.
* `write_report` (boolean)
    - Write the run's diagnostic report next to the packet stream under its full name plus `.txt` (`mydata.t33` gives `mydata.t33.txt`), so it needs `output_path` set. Beyond the packet-level recovery profile it accounts for every layer above: packets orphaned rather than placed in a group, groups completed, record headers refused, linked series joined, records catalogued. The same report always goes to the log at debug level.
    - Default: `false`.
* `export_records` (boolean)
    - Write each record as its own file beside the packet stream, named for the channel, record address and version that identify it (`mydata.t33.000-1A4-v2.rec`). The file holds the record's data exactly as transmitted — NAPLPS presentation code, or application data for a type 2 record — which is what to hand to an external NAPLPS tool.
    - Default: `false`.
* `export_captions` (boolean)
    - Write the recording's captioning as a SubRip file beside the packet stream (`mydata.t33.srt`). The cues are the records the service marked with the caption flag of CEA-516 §5.2.7.3 — the flag, not the data channel, is what selects them — in transmission order, each running until the next replaces it, timed from the 59.94 fields per second of SMPTE 170M. Colour and positioning are dropped; SRT carries the plain text. A recording that carried no captioning writes no file.
    - Default: `false`.
**Stage tools**

* **NABTS Records** — the record viewer for this node. It lists every record the range carried, with its channel and record address, version, record type, how often it was seen and over which frames, and the classification flags the service set. It opens automatically when the node is triggered, which is how the records are reached: leave `output_path` empty and triggering the node is a decode-and-browse with no file written. The catalogue stays with the stage afterwards, so closing the viewer and picking **NABTS Records** from the **Stage Tools** menu re-opens it immediately rather than decoding the source again; on a node that has not been triggered that entry says there is nothing to show instead of starting the decode itself. Editing any stage's parameters rebuilds the graph and discards the catalogue with it, closing the viewer — trigger again to rebuild it from the new settings. Because the catalogue comes from a pass over the whole source rather than a window around the preview position, the list is the service's full carousel. **Save PNG…** writes the record on screen out as an image at the 4:3 shape of its display area, with blink processes held lit; it is offered for a drawn record and not for an application record, which is a listing rather than a picture. Every receiver saves at the same 1536 by 1152 — comparing one with another is what the choice of receiver is for, and images of different sizes would make the comparison about the sizes. That width is a whole multiple of all three grids, so every column of the receiver's raster gets the same number of image columns and the pixels stay hard-edged. **Save All PNGs…** writes every drawn record in the catalogue into a chosen folder in one pass, each the image **Save PNG…** would have written for it and named the same way, with a progress dialogue that can be cancelled partway; application records are listings and are skipped.
    - A **presentation record** is drawn as its NAPLPS display list, beside the plain text of the page in reading order — an index page is mostly words, and picking them off a rasterised page is tedious. **Show display area** outlines the lower 0.78125 of the unit screen that every receiver is guaranteed to show (ANSI X3.110 Table D1), which is how to tell a record drawn deliberately into one corner from one that was mis-scaled.
    - **Receiver** chooses which of four resolutions the page is drawn against, and is the only control for it — there is no parameter, on purpose. ANSI X3.110 draws into an abstract unit screen but sizes stroke width, line-texture dot and dash lengths, hatch spacing and the incremental raster in the physical pixels of the receiver displaying them (§5.3.2.2.6), so a page has no single correct appearance until a receiver is named. `256 x 200` is the grid Table D1 item 10 requires, which is what a set-top decoder of the period put on screen and what the window opens on; `512 x 400` and `768 x 600` are that grid at twice and three times, for a sharper reading of the same page; `512 x 400 (vector)` draws the same geometry as shapes rather than pixels, smooth at any zoom but without the pixel structure a receiver had. Only whole multiples of the reference grid are offered: a page is authored against it, and a fraction of it lands between pixels, thickening strokes unevenly and breaking letterforms. The built-in character patterns follow the receiver too — §5.1 leaves them to it "for a given display resolution", so `256 x 200` draws text from a 6 by 10 face, `512 x 400` from a 10 by 20 and `768 x 600` from a 9 by 15 at double size, all from the public-domain X11 misc-fixed family, rather than magnifying one coarse face at every resolution. Nothing in the recovery depends on the choice — the records are read off the recording, and the receiver decides only how they are drawn afterwards — so picking one redraws the page you are looking at and leaves you on it, without reading the recording again, and nothing is saved with the project. A parameter would instead rebuild the graph and discard every stage's results, which is a full re-run of a recovery pass that cannot see the setting.
    - **Syntax repair** presents a damaged page as recovered rather than as transmitted, and is on unless you clear it. NAPLPS has a published grammar (ANSI X3.110-1983), so a page recovered from a damaged recording can be checked against it: an operand byte knocked out of the columns numeric data occupies, an opcode knocked into them, a coordinate naming a point off the screen, a run of operands with bytes missing from the middle where a packet was lost. A byte is changed only where the recording independently says it is wrong — it failed the odd parity of CEA-516 §3.3, or the detector was unsure of it — and only where the grammar leaves exactly one thing it could have been; where it leaves more than one, nothing is changed and the count of what was left undecided is reported. A byte that never arrived is never filled in, because writing a plausible byte into a hole is inventing a page rather than recovering one. Clear the box to read the recording exactly as transmitted — having the two a click apart is how you judge whether a repair recovered the page or produced a tidier wrong one. It changes only what is drawn here: the packet stream, the record files and the report always carry the recording's own bytes.
    - **Show text** puts away the text pane beside the drawing, giving the page the whole width. It is offered for a drawn record that carries a text form of itself, which a presentation record does and an application record does not.
    - The panel along the foot of the window is where everything the viewer has to say is collected: what record you are looking at and what shape it is in, what the catalogue as a whole carries, and how the run went. The counts behind those lines — per record, per byte — go to the log and to the run's report rather than into the window.
    - An **application record** is shown as its function descriptors instead, each with its code in the code-table notation of §7.2.2 and its arguments.
    - **Enable animations** runs the blink processes the record set up. A blink process belongs to a colour map entry rather than to a figure, so what alternates is everything drawn in that one colour and nothing else — and it alternates with the second colour the record named, which may be another colour of the picture (a figure that twinkles) or the ground (one that vanishes and returns). The record's own ON/OFF intervals are not shown: every process alternates together on the same 0.75 Hz cycle the page viewer flashes at. Clear the box to hold the record still for reading or capture.
    - **Caption track** switches the pane to the recording's captioning as a list of cues with their timings. §7.3.10 carries captioning as a run of records that each replace the last, so the cues rather than the individual records are what the service says. The control is enabled only on a recording that carried captioning, and the line above the list says which records those were.

**Notes**

* NTSC and PAL-M sources are accepted. A 625-line source reports an error: CEA-516 §1.1.1 specifies NABTS on the 525-line signal, and no 625-line service exists to recover.
* This stage writes no CSV — its file output is the packet stream, and optionally the per-record files, the caption document and the report.
* Nothing is corrected in the packet stream. Where a packet carries the longitudinal check byte of §3.4, that byte and the per-byte parity form a product code and a single-bit error in the data block is located and corrected before the record layer sees it; whether a service sends it is the service's choice.
* Byte parity is not used to repair damaged bytes as it is for World System Teletext. CEA-516 §3.3 gives the data block odd parity only when its data group is of type 0, and a single packet does not say which type its group is.
* Only one copy of each record is kept — the intact copy if one arrives, otherwise the longest. A carousel transmits each record many times, and combining those copies the way the Teletext Sink combines repeated rows is not done here.

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

Triggering the stage makes one linear pass over the whole frame range. It probes the candidate VBI lines of both fields of every frame for teletext data lines, recovers the packets, and writes them as a flat, headerless packet stream in strictly temporal order (frame → field → ascending line). Frames are decoded on several threads while the stream is written from one, and the pass learns where in the line this particular recording puts its data and which lines carry it, so later frames cost far less than the first ones — together these take a pass over the reference captures from 1970 ms to 295 ms (625-line) and 946 ms to 156 ms (525-line). None of it changes what is recovered: see `decode_threads`, `pin_data_phase` and `learn_active_lines` below. Packets keep their transmission coding (Hamming 8/4 addressing, odd-parity display bytes), so consumers decode the stream exactly as a receiver decodes a live broadcast.

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
    - Default: `22` on a 625-line project, `21` on a 525-line one.
* `keep_empty_packets` (boolean)
    - Emit a whole zero packet for candidate lines with no data so packet position maps 1:1 to (frame, field, line) — the vhs-decode convention.
    - Default: `false`.
* `detector` (string)
    - How data bits are recovered: `Threshold` (slice at bit centres; exact on discs and direct captures), `MLSE` (fit the recording's frequency response to the known start of each line, detect against it, then refit that response to the whole packet just read and read it again; recovers teletext from tape, where limited bandwidth smears bits into their neighbours), or `Automatic` (threshold first, MLSE only where it fails — same behaviour and cost as threshold alone on a disc source).
    - Default: `Automatic`.
* `character_set` (string)
    - Which alphabet the page codes are read in: `Latin`, `Cyrillic (Russian/Bulgarian)`, `Cyrillic (Ukrainian)` or `Cyrillic (Serbian/Croatian)`. A display code means nothing on its own — 4/4 is `D` in the Latin set and `Д` in the Russian one — and a service that says which set it uses (a packet X/28/0 or M/29/0, ETSI EN 300 706 §15.2) is always believed, so this is consulted only for pages that designate nothing. That is most of what survives on tape: those packets are a Level 2.5 facility, and for Level 1 the standard falls back on "a local Code of Practice" — the region the receiver was sold in. Nothing in the recording can settle it, so the choice has to be yours. It affects only the pages you read and any subtitles exported from them; the exported packet stream is the transmitted bytes and carries no character set. Within Latin, the national option sub-set (`£` for a UK service, `é` for a French one) still comes from each page's own header bits and needs no setting.
    - Default: `Latin`.
* `second_character_set` (string)
    - The alphabet the `ESC` control character switches into, for services that mix two on one page — a Russian or Ukrainian service writes foreign names in Latin letters in among the Cyrillic, marking each switch with the code that toggles the rest of the display row into the other alphabet (ETSI EN 300 706 §12.2 Table 26 code 1/B, §15.3). `None` or one of the four sets `character_set` offers. As with `character_set`, a service that designates its own pair is always believed and this is ignored — including when it says it uses only one, which switches the control code off. `None` means the same for material that designates nothing: one alphabet throughout and the code inert, which is how pages were read before. Set it only where a recording really does mix alphabets; a wrong pairing turns the switched runs into nonsense rather than leaving them alone.
    - Default: `None`.
* `tolerant_framing` (boolean)
    - Accept framing codes with one bit error (more packets from noisy sources, higher false-positive rate).
    - Default: `false`.
* `require_valid_mrag` (boolean)
    - Drop packets whose magazine/row address fails Hamming 8/4 correction (suppresses false locks on noise).
    - Default: `true`.
* `repair_damaged_bytes` (boolean)
    - Every display byte carries a parity bit, so a byte that fails its parity check is known to be damaged. Restore it by flipping the bit the MLSE detector came closest to reading the other way. Recovers characters a difficult tape would otherwise lose; the cost is that a repaired byte can no longer be told from an undamaged one, so a repair that guessed wrong is no longer marked as damage. Applies to the MLSE detector only, so a disc or direct capture is unaffected.
    - Default: `true`.
* `pin_data_phase` (boolean)
    - Most of the work of reading a line is searching the whole of the standard's data-timing window for where the data burst starts. Every line of a time-base-corrected recording starts at very nearly the same place, so once enough lines have been read the search narrows to where they agreed. A narrowed search that finds nothing is repeated over the full window, so this cannot lose a packet; it costs a few percent on lines that carry no data.
    - Measured on the reference captures this is the larger of the two savings, and it also *recovers* packets an exhaustive search misses — narrowing the window rejects the false correlation peaks a whole-window search can settle on. The report says where the window was pinned, or why it was not.
    - Default: `true`.
* `learn_active_lines` (boolean)
    - A service uses a few of the lines its standard permits, but every line of the window is read on every frame, and on a line carrying picture content, VITS, VITC or captions that work is spent reaching a rejection already reached on the frame before. Read every line for the first 50 frames, then only the lines that have carried a packet, rechecking the full window every 50th frame so a service that starts part way into a recording is still picked up.
    - Unlike `pin_data_phase` this can lose a packet: a line that carries data exactly once, outside both the learning frames and a recheck frame, is not read. On the reference PAL capture that cost one packet in 3,964. Turn it off for an archival pass where every packet matters.
    - Default: `true`.
* `decode_threads` (integer)
    - Threads to recover lines on; `0`, the default, uses one per processor. Each line is recovered from its own samples, so frames are decoded several at a time while the packets are written from one thread in the order they were transmitted.
    - The recovered stream is identical whatever this is set to, so lower it only to leave the machine free for other work. Measured on the 625-line reference capture: 1406 ms on one thread, 442 on four, 295 on eight; past the processor's physical cores there is nothing more to win, because the run is by then waiting on the source stage to hand out frames.
    - Default: `0`.
* `squash_repeated_rows` (boolean)
    - Teletext pages are transmitted on a loop, so a recording holds several copies of every page row, damaged in different places. Combine them byte by byte — preferring values that pass their parity check, then weighting by how sure the detector was of each byte — and write the combined rows. Packet order, count and timing are unchanged; only damaged display bytes move. The pages shown in the viewer are built from the combined rows too. Needs a second pass over the recovered packets, held in memory (roughly 50 bytes each).
    - Copies are combined only within one run of a page: a header with the erase bit set (C4) says the content is being replaced, so what follows is a different page sharing a number. A service that erases on every transmission gives each one a run of its own, and nothing can be combined — the report says so, as a run count matching the transmission count.
    - Default: `true`.
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
    - Container and codec (FFmpeg mode only). Values include `mp4-h264`, `mkv-ffv1`, `mov-prores`, `mov-v210`, `mov-v410`, `mxf-mpeg2video`, `mov-h264`, `mp4-hevc`, `mov-hevc`, and `mp4-av1`. Default: `mp4-h264`.

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

* `embed_chapter_metadata` (bool)
    - FFmpeg mode only. Write chapter markers derived from VBI data into the output file. Default: `false`.

**Stage tools**

* **FFmpeg Preset Config** — a preset helper dialog that applies well-tested encoder combinations without setting each parameter manually. Applying a preset switches the stage to FFmpeg output mode.

**Notes**

* Raw mode does not support audio, closed caption, or chapter embedding; those options apply to FFmpeg output only.
* Raw output files can be very large; ensure sufficient disk space before triggering.
* The `y4m` raw format is directly readable by tools such as FFmpeg and rav1e without specifying the pixel format manually.
* CRF and bitrate modes are mutually exclusive; set `encoder_bitrate` to a non-zero value to switch from CRF mode.
* Video filtering (`apply_deinterlace` or `video_filter`) is not supported with hardware encoders that use GPU surfaces (`vaapi`, `qsv`, `videotoolbox`); the export automatically falls back to the software encoder in that case.
* When a video filter chain is active, interlaced coding flags are not forced on the encoder; the field structure of the filter output determines how frames are flagged.
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
