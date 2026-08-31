# Source stages

Source stages are the **starting point of every decode-orc pipeline**. They load captured video (and any associated audio or disc data) from disk and make it available for processing.

You normally use **one source stage per capture**. If you have multiple captures of the same material, you add multiple source stages and combine them later using transform stages.

Source stages do not improve or modify the signal. Their purpose is to:

* Load captured files correctly
* Validate the video system (PAL, NTSC, or PAL-M)
* Keep video, audio, and disc data synchronised

> **Project format note:** Decode-Orc 2.0 requires that every project declares a `video_format` (PAL, NTSC, or PAL-M) and a `source_format` (Composite or YC) at creation time. These fields are read-only after the project is created. The stage picker shows only source stages that match the declared format.

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

Associated audio (analogue `.pcm`), EFM disc data (`.efm`), and AC3 RF symbols (`.ac3sym`) are attached if present alongside the `.tbc` file. When a `.pcm` sidecar is present it becomes **audio channel pair 0** (named from `pcm_name`, default `Analogue`). The `.pcm` sidecar — raw signed 16-bit little-endian stereo PCM, nominally 44100 Hz as written by ld-decode — is always converted on ingest to the pipeline's only audio form: 48 kHz synchronous (frame-locked) 24-bit stereo per SMPTE 272M (widened to 24-bit and resampled with SoXR HQ). The conversion is deferred until audio is first read, so video-only preview never pays for it.

**Composite variant user-facing inputs**

* **TBC file** (`.tbc`)
* Accompanying metadata database (`.tbc.db`)
* PCM audio file (`.pcm`, optional)
* EFM data file (`.efm`, optional)
* AC3 RF symbols file (`.ac3sym`, optional)

**Y/C variant user-facing inputs**

* **Luma (Y) file** (`.tbcy`)
* **Chroma (C) file** (`.tbcc`)
* Accompanying metadata database (auto-detected)
* PCM audio file (`.pcm`, optional)
* EFM data file (`.efm`, optional)
* AC3 RF symbols file (`.ac3sym`, optional)

**Parameters**

* `input_path` (file path) — Composite `.tbc` file. Composite captures only.
* `y_path` / `c_path` (file paths) — Luma `.tbcy` and chroma `.tbcc` files. Y/C captures only; set together.
* `pcm_path` (file path) — Analogue audio `.pcm` sidecar. Becomes channel pair 0, converted to 48 kHz frame-locked 24-bit stereo.
* `pcm_name` (string) — Name for the analogue audio channel pair (shown in the CVBS container and as the Video Sink stream title). Empty uses `Analogue`.
* `efm_path` (file path) — EFM t-value `.efm` sidecar.
* `ac3rf_path` (file path) — AC3 RF symbols `.ac3sym` sidecar.

**Notes**

* The stage validates that the Y/C colour-frame phase is aligned at open time. Misaligned Y/C files are rejected with a clear error.
* NTSC-J sources with a non-standard black level are detected automatically from metadata and exposed via a per-frame black level override.
* Legacy `.tbc.json` metadata produced by older ld-decode/vhs-decode versions is accepted with a warning; re-decoding with a current version (which produces `.tbc.db`) is recommended.

---

## CVBS Source

| | |
|-|-|
| **Stage id** | `PAL_CVBS_Source`, `NTSC_CVBS_Source`, or `PAL_M_CVBS_Source` (one variant per video system; the stage picker offers the one matching the project) |
| **Stage name** | CVBS Source |
| **Connections** | No inputs → 1 output |
| **Purpose** | Load CVBS captures stored in the CVBS file-format family |

**Use this stage when:**

* Your source is a CVBS file (`.cvbs`, or a `.cvbsy`/`.cvbsc` pair for Y/C projects) rather than a TBC capture

**What it does**

This stage reads CVBS payloads from `.cvbs` files (or `.cvbsy`/`.cvbsc` pairs) and normalises them to the CVBS_U10_4FSC 10-bit domain. By default the video system, sample encoding, and signal state are read from the `.meta` SQLite sidecar; because the CVBS file format declares metadata optional, the sample encoding can also be selected manually so that sources without a sidecar can be used.

The `STANDARD_STABLE_LOCKED` and `STANDARD_STABLE_UNLOCKED` signal-state presets are accepted. Files in any other state (including the pre-v1.6.0 `*_TBC_*` preset names) are rejected with a clear error before any frame data is returned, because the stage's frame geometry assumes time-base-stable samples at the standard 4fsc rate. When a sample encoding is selected manually the sidecar is ignored: the signal is assumed to be time-base stable and phase locked, and the frame count is measured from the file size.

Phase lock is not required. Colour-sequence phase is measured from each frame's burst rather than read from the sidecar, so an unlocked source (for example monochrome material) decodes normally. Whether the stored content is one unbroken sequence is declared separately by the `sequence_continuous` metadata field (CVBS file format spec v1.6.0). Opening a source marked `sequence_continuous = FALSE` shows an advisory and the source loads: the marker means the content contains at least one discontinuity, which for a LaserDisc source usually means the player skipped or jumped during the decode. Run the Disc Mapper (right-click a Frame Map stage, then **Stage Tools > Disc Mapper**) to put the frames back into their recorded order before exporting.

The following sample encodings are normalised automatically:

| Encoding | Normalisation |
|----------|---------------|
| `CVBS_U10_4FSC` | Identity (already 10-bit) |
| `CVBS_U16_4FSC` | `value = uint16_value / 64` |
| `CVBS_TPG21_4FSC` | `value = int16_value / 64 + 508` |
| `CVBS_S16_4FSC` | `value = int16_value / 32 + blanking_10bit` |

Associated dropout, audio, EFM, and AC3 sidecars are loaded automatically if present.

**Parameters**

The file-path parameters offered match the project's source type: a composite project shows only the CVBS file path, while a Y/C project shows only the Y (luma) and C (chroma) paths.

* `input_path` (file path)
    - Path to the composite data file (`.cvbs`). Composite projects only.

* `y_path` / `c_path` (file paths)
    - Paths to the luma (`.cvbsy`) and chroma (`.cvbsc`) channel files. Y/C projects only; set together.

* `sample_encoding` (string)
    - `From metadata` (default) reads the encoding from the `.meta` sidecar.
    - Selecting `CVBS_U10_4FSC`, `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, or `CVBS_S16_4FSC` manually makes the sidecar optional.

**Notes**

* Colour-frame index (PAL: 1–4, NTSC: 0–1, PAL-M: 1–4) is measured from the colour burst on each frame and stored in the frame descriptor. Frames where the burst is absent or unmeasurable carry `colour_frame_index = -1`.

---

## VBI Capture Source

| | |
|-|-|
| **Stage id** | `vbi_source` |
| **Stage name** | VBI Capture Source |
| **Connections** | No inputs → 1 output |
| **Purpose** | Ingest raw VBI teletext captures by laying their lines onto CVBS frames at the timing point and amplitude the standard puts them at |

**Use this stage when:**

* Your material is a raw VBI dump rather than a decoded capture — a bt8x8 card dump (`.vbi`, commonly FLAC-compressed as `.vbi.flac`) from a PAL, a SECAM or a 525-line source, a cx23885 card dump from a 525-line source, or the VBI lines cropped off a decoded `.tbc`

**What it does**

A raw VBI capture holds nothing but the vertical-blanking line records: no sync, no burst, no picture, and no timing reference of any kind. This stage reads those records and places them on otherwise blank CVBS frames at the data service's own time from 0H. The result is an ordinary CVBS_U10_4FSC representation, so the existing teletext decoders see exactly what they see from a native decode. Nothing else about the raster is manufactured: no stage that reads this output looks for sync, a vertical interval or a burst, so synthesising them would cost far more than placing the data does and be spent entirely on samples nobody reads.

Frames are built lazily, one at a time, as they are asked for: the full-frame expansion of a raw capture is 21.6× its size, so a four-hour bt8x8 capture would be 522 GB if it were materialised.

Records are level-mapped from the card's relative levels into the CVBS amplitude domain (logic 0 from the quiet region ahead of the clock run-in, logic 1 from the larger of the run-in peaks and the framing code's leading ones), resampled onto the 4×fsc lattice with a band-limited filter, and placed at the data service's nominal time from 0H. The mapping is linear and nothing else is done to the samples — a deconvolving slicer downstream recovers data by matching the blurred waveform it is given.

Because no capture format records the time from 0H to sample 0 of a record, and the card families' documented figures are unreliable, the stage measures it: records sampled from across the whole capture are correlated against a generated clock-run-in and framing-code template and the median becomes a single global offset. A fit that fails its health checks stops the run rather than decoding hours of material at a wrong offset.

A capture cropped from a decoded `.tbc` needs the opposite: its records start at 0H exactly, so its offset is known and never fitted, but the time at which the *broadcaster* transmitted the run-in is not — and the tabulated 525-line figures were measured on particular captures that others disagree with by up to a microsecond. The same survey is therefore run and read as the service anchor instead, so the written region is cut from where the run-in actually is rather than where the table predicts. Without it, a transmission a microsecond early has the head of every run-in replaced by blanking. Here a failed fit is not fatal — it leaves the tabulated figure standing, which is what earlier versions always used — and the log says which was used.

The last four bytes of every bt8x8 frame are the driver's frame sequence number. Comparing it at the two ends of the capture says how many frames were dropped across the whole of it without reading the capture through, which is what the `drops` policy acts on.

**The capture formats**

Everything about a capture — its geometry, its sampling rate, the data service it carries, where 0H is, what its logic levels mean, which field it starts on — is a property of the format rather than something you could be expected to know, so it all follows from the format you pick. Only the formats belonging to the project's television system are offered.

| Format | Projects | What it is |
|--------|----------|------------|
| `bt8x8 card dump, 8-bit (WST)` | PAL | A 625-line capture-card dump: 2048 samples per record of which 2044 are real, unsigned 8-bit, at 8×fsc (35 468 950 Hz); 16 records per field carrying field lines 7–22, the whole of the WST line list |
| `bt8x8 card dump, 8-bit (WST, SECAM source)` | PAL | The same container, byte for byte, from a SECAM source — see below |
| `bt8x8 card dump, 8-bit (WST, NTSC source)` and `(NABTS, NTSC source)` | NTSC | The same card on the driver's other television norm, which is not the same container: 2048 samples per record of which 1600 are real, unsigned 8-bit, at 8×fsc NTSC (28 636 363 Hz); 16 records per field of which the first 12 carry field lines 10–21, the whole of the 525-line teletext list — see below |
| `cx23885 card dump, 8-bit (WST)` and `(NABTS)` | NTSC | A 525-line capture-card dump from a Hauppauge HVR-1250 or sibling: 1440 samples per record with no padding, unsigned 8-bit, at 27 MHz; 12 records per field carrying field lines 10–21, the whole of the 525-line teletext list — see below |
| `.tbc VBI crop, 16-bit (WST)` and `(NABTS)` | NTSC | The first 16 line records of each field of a decoded 525-line luma `.tbc`: 910 samples per record with no padding, unsigned 16-bit, at 4×fsc, records 1–12 carrying field lines 10–21 |

There is deliberately no "custom" entry. Every fact behind these formats had to be measured off real captures — none of it is recoverable from the file, and guessing at it produces output that looks right and is not.

**bt8x8 dumps of a 525-line source**

The same card and the same driver as the PAL entries, and not the same container. The `bttv` driver's NTSC television norm has its own sampling clock (28 636 363 Hz, 8×fsc NTSC), its own `vbipack` of 144 — so 1600 of the 2048 samples per record are real rather than 2044 — and its own `vbistart` of `{10, 273}` rather than `{7, 320}`. What carries over is the card's geometry: the 2048-byte record stride, the sixteen records a field stores, the 65 536-byte frame and the driver's frame counter at the tail of it, which lands in the last record's padding here as it does on PAL. Unlike the other two 525-line containers, then, **a bt8x8 dump of a 525-line source can report dropped frames**.

`vbistart` opening at field line 10 is exactly where the 525-line teletext list starts, so records 0–11 carry field lines 10–21 and 273–284 and records 12–15 are the start of the picture. Two calibration thresholds differ from the PAL entries and no others: this entry expects the run-in on a tenth of the sampled records rather than a quarter, for the same reason the cx23885 entry does, and its spread limit is the PAL entry's 226 ns expressed at this norm's slower clock — 6,5 samples rather than 8. The reference capture, a 1984 off-air recording of TBS Keyfax, locks on 33,1% of its records with a spread of 3,3 samples and fits its offset at 249,1 samples against the driver's folkloric 244.

Choose the service to match the broadcast, as on any card capture: the offset is fitted against the service's own run-in template, so the wrong choice fails the calibration rather than quietly mis-slicing the data.

**cx23885 card dumps**

The other card container, and nothing like the bt8x8 one. Its figures are the Linux `cx23885` driver's own: 27 MHz, 1440 samples per record, 12 records from field line 10. 1440 samples at 27 MHz is exactly the ITU-R BT.601 digital active line written at twice the 13,5 MHz rate, so what the card hands over is the active line and nothing else — no sync, no burst, no padding, and therefore nowhere for a frame counter to live. A cx23885 dump cannot report dropped frames.

Where the window sits relative to 0H is neither in the file nor usefully in the driver, whose reported offset is a stub zero, so the configured figure is the window SMPTE 125M says the timing generator produces (122 samples at 13,5 MHz after 0H, 244 here) and the real value is measured from the clock run-in as on any card capture. A US network carried its magazine on two or three of the twelve lines, so this entry expects the run-in on a tenth of the sampled records rather than the quarter the PAL entries ask for; every other health check is the bt8x8 one expressed at this card's sampling rate.

Choose the service — WST or NABTS — to match the broadcast. Getting it wrong on a card capture fails the calibration outright and stops the run with a diagnostic, because the offset is fitted against the service's own run-in template; that is intended, and makes the wrong choice loud rather than silent.

**SECAM sources**

Pick the SECAM entry when the capture came from a SECAM broadcast or tape. The container is identical — the driver's SECAM television norm shares the PAL one's sampling clock, its `vbipack` and its `vbistart` — and post-decode SECAM is a 625-line signal carrying the same World System Teletext, which is why it is placed on PAL frames.

What differs is how much of the line list can carry teletext. A SECAM transmission with vertical colour identification puts the identification signal (the "green bottles") on field lines 8–15 and 321–328, so half the records of every stored field are spoken for before any teletext is inserted, and broadcasters using those idents typically left the remainder to test signals and a very few teletext lines. The SECAM entry therefore expects the clock run-in on far fewer of the sampled records before it trusts the calibration; every other health check is the same. Using the PAL entry on such a capture gets a perfectly good fit rejected on the line count alone.

**Parameters**

* `input_path` (file path) — Path to the raw capture. FLAC-wrapped captures are unwrapped transparently; the wrapper's declared sample rate is a placeholder and is never used for timing.
* `format` (string) — Which of the capture formats above the file is.
* `drops` (string) — `preserve` (default) emits only the frames present; `pad` synthesises blank frames so output frame *n* stays aligned with source frame *n*. A format carrying no frame counter cannot report drops at all.

**Notes**

* Nothing is written to disk by this stage. To export the frames as a `.composite` + `.meta` pair, connect a CVBS Sink. Bear in mind that what is exported is a legal CVBS file carrying teletext on an otherwise blank raster, not a reconstruction of the broadcast the capture was cut from.
* A capture that ends short of a whole frame — on an odd field, as the `.tbc` crops usually do, or part-way through a line record, as a card dump stopped at the keyboard does — is loaded normally. The trailing bytes cannot make a frame, so they are not emitted, and the log says what was dropped. Only a file too short to hold a single whole frame is refused.
* Which frame lines carry data follows from the television and teletext systems (WST occupies broadcast frame lines 7–22 and 320–335) and from which stored records the container declares as data records; it is not configured directly. A capture holding more data records than the standard defines is reported as an error rather than truncated.
