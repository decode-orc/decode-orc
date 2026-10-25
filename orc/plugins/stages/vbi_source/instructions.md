# VBI Capture Source

Reads a raw VBI capture — a file holding nothing but the vertical-blanking line records a capture card or a cropped `.tbc` wrote — and lays those lines onto CVBS frames at the timing point and the amplitude the standard puts them at. The result is an ordinary CVBS_U10_4FSC VideoFrameRepresentation, so the teletext decoders see the data exactly as they see it on a native decode.

The rest of each frame is blanking. A VBI capture carries no sync, no vertical interval and no colour burst, and nothing that reads this stage's output looks for them: the teletext slicer locks to the clock run-in inside the line it is handed. Manufacturing a whole television signal around the data would cost far more than placing the data does, and would be spent entirely on samples nobody reads.

## When to use

Add VBI Capture Source as the first stage of a pipeline when your material is a third-party VBI dump rather than a decoded capture: a capture-card dump (`.vbi`, often FLAC-compressed as `.vbi.flac`), or the VBI lines cropped off a decoded `.tbc` (which circulate as `.flac`, `.u16` or `.tbc`). If you have a whole `.tbc` or a `.cvbs` file, use the TBC Source or CVBS Source stage instead — those carry a picture.

Nothing is written to disk by this stage. To export the frames as a `.cvbs` + `.meta` pair, connect a CVBS Sink. Bear in mind that what is exported is a legal CVBS file carrying teletext on an otherwise blank raster, not a reconstruction of the broadcast the capture was cut from.

## Parameters

There are three. Everything else about a capture — its geometry, its sampling rate, the data service it carries, where 0H is, what its logic levels mean, which field it starts on — is a property of the format rather than something you could be expected to know, so it all follows from the **Capture Format** you pick.

| Parameter | Meaning |
|-----------|---------|
| VBI Capture Path (`input_path`) | Path to the capture. FLAC-wrapped files are unwrapped transparently; the wrapper's declared sample rate is a conventional placeholder and is never used for timing. The browse dialog filters on the extensions these captures arrive under — `.vbi`, `.flac`, `.u8`, `.u16`, `.tbc` — but nothing about a capture is read from its name, so anything else can still be picked through the All Files filter. |
| Capture Format (`format`) | What the capture is. Only the formats belonging to the project's television system are offered, so a PAL project is offered the one card container in its two source flavours and an NTSC project its three card containers and its `.tbc` crop, each in the two services its captures might carry. |
| Dropped Frames (`drops`) | `preserve` (default) emits only the frames present; `pad` emits a blank frame in each gap so output frame *n* stays aligned with source frame *n*. A format carrying no frame counter cannot report drops at all, so neither policy has anything to act on. |

## The capture formats

### `bt8x8 card dump, 8-bit (WST)` — PAL projects

A 625-line capture-card dump. 2048 samples per record of which 2044 are real, unsigned 8-bit, at 8×fsc (35 468 950 Hz); 16 records per field carrying field lines 7–22, which is the whole of the WST line list.

The card's own time from 0H is documented but unreliable — the driver's source calls its own datasheet figure wrong — so it is measured from the clock run-in when the capture is opened, and the run stops with a diagnostic if that measurement cannot be trusted. Logic levels are estimated per line, because a card's levels move with its gain control. The last four bytes of every frame are the driver's frame sequence number, which is what makes dropped frames detectable at all.

### `bt8x8 card dump, 8-bit (WST, SECAM source)` — PAL projects

The same container, byte for byte, from a SECAM source. The driver's SECAM television norm shares the PAL one's 8×fsc sampling clock, its `vbipack` and its `vbistart`, so the record stride, the field stride, the frame trailer and the lines the sixteen records cover are all unchanged — and post-decode SECAM is a 625-line signal carrying the same World System Teletext, which is why it is placed on PAL frames.

What differs is how much of the line list can carry teletext. A SECAM transmission with vertical colour identification puts the identification signal — a continuous 4,4 MHz burst, the "green bottles" — on field lines 8–15 and 321–328, which is records 1 to 8 of every stored field. Half the records are spoken for before any teletext is inserted, and broadcasters using those idents typically left the rest to test signals and a very few teletext lines: the Russian tape this entry was measured on carries teletext on records 12–14 only (field lines 19–21 and 332–334), with a VITS and a white bar on records 9–11.

That is a fifth of the records at best, where a PAL capture uses most of them, and the run-in is looked for on every stored record. So this entry expects the run-in on far fewer of them before it trusts a fit. Nothing else about the calibration is relaxed: the same search window, the same acceptance correlation, the same spread and drift limits, and the same absolute floor of accepted records. Pick this entry when your capture came from a SECAM broadcast or tape; picking the PAL one instead gets you a healthy fit rejected on the line count alone, and picking this one for PAL material only makes the least important of the checks less likely to fire.

### `bt8x8 card dump, 8-bit (WST, NTSC source)` and `(NABTS, NTSC source)` — NTSC projects

The same card and the same driver as the two PAL entries above, and **not** the same container. `bttv_tvnorms[]` gives its NTSC television norm its own sampling clock — 28 636 363 Hz, 8×fsc NTSC rather than 8×fsc PAL — its own `vbipack` of 144, so 1600 of the 2048 samples per record are real rather than 2044, and its own `vbistart` of `{10, 273}` rather than `{7, 320}`. What does carry over is the card's own geometry: the 2048-byte record stride, the sixteen records a field stores, the 65 536-byte frame, and the four bytes of driver frame counter at the tail of it — which land in the last record's padding here as they do on PAL, there being 448 bytes of it rather than four. **A bt8x8 dump of a 525-line source can report dropped frames**, which the other 525-line containers cannot.

`vbistart` opening at field line 10 is exactly where the 525-line teletext list starts, so records 0–11 carry field lines 10–21 and 273–284, the whole of it, and records 12–15 are the start of the picture and carry nothing. As on the PAL entries the card's time from 0H is measured from the clock run-in when the capture is opened, and the logic levels are estimated per line.

Two calibration thresholds differ from the PAL entries, and only two. A US broadcaster carried its magazines on a handful of the twelve lines where a 625-line one filled most of the list, so this entry expects the run-in on a tenth of the sampled records rather than a quarter. And the spread limit is a tolerance in *time* — 226 ns, the PAL entry's 8 samples at 35,47 MHz — so at this norm's slower clock it is 6,5 samples rather than 8. The reference capture, a 1984 off-air recording of TBS Keyfax, locks on 33,1% of its records with a spread of 3,3 samples and no drift, fits its capture offset at 249,1 samples against the driver's folkloric 244, and carries its magazines on field lines 14–18 of the first field and 14, 17 and 18 of the second.

**Which of the two do I pick?** As with the cx23885 entries below, the one matching the service the broadcast carried — and here it matters, because a card capture's offset is fitted against the service's own run-in template, so the wrong choice fails the calibration outright rather than merely mis-slicing the data.

### `cx2388x card dump, 8-bit (WST, NTSC source)` and `(NABTS, NTSC source)` — NTSC projects

A 525-line capture-card dump from a Conexant CX23880/1/2/3 card, the chip family the Linux `cx88` driver serves. The container is very nearly the bt8x8 NTSC one: the same 8×fsc NTSC sampling clock — 28 636 363 Hz, which the CX2388x data sheet (§2.6.16) gives as the VBI sampling rate and which is its crystal frequency, so no rate conversion is involved — the same 2048-byte record and the same 1600 real samples at the head of it. What differs is the field: the driver stores `VBI_LINE_NTSC_COUNT` records, twelve, from field lines 10 and 273, so every stored record is one of field lines 10–21 and 273–284 — the whole of the 525-line teletext list and nothing more. A frame is 49 152 bytes.

The 448 bytes of padding after the real samples hold a constant rather than a sequence number, and there is no other trailer, so **a cx2388x dump cannot report dropped frames**.

Measured on the reference capture rather than only read off the driver: each record runs through the next line's sync pulse, whose leading edge falls at about sample 1578 of the 1820-sample line, and is constant from sample 1600 to the end of the stride; record 9 carries the vertical interval reference signal that belongs on field line 19. The driver's `f->fmt.vbi.offset` of 244 is configured as the starting point and the real value is measured from the clock run-in, as on any card capture. Its calibration thresholds are the bt8x8 NTSC entry's, for the same reasons — the same clock, the same kind of material and the same handful of lines in use. The reference capture — an off-tape recording of CBS ExtraVision — carries NABTS on field lines 15–18 of both fields, locks on 36,2% of its records with a spread of 2,6 samples and fits its offset at 230,1 samples.

**Which of the two do I pick?** As with the bt8x8 and cx23885 entries, the one matching the service the broadcast carried.

There is no 625-line cx2388x entry yet: the driver captures a different number of records on that norm, and no 625-line dump has been measured to confirm the container.

### `cx23885 card dump, 8-bit (WST)` and `(NABTS)` — NTSC projects

A 525-line capture-card dump from a Hauppauge HVR-1250 or one of its siblings — the other card container in circulation, and nothing like the bt8x8 one. 1440 samples per record with no padding, unsigned 8-bit, at 27 MHz; 12 records per field carrying field lines 10–21 and 273–284, which is the whole of the 525-line teletext list, so every stored record is a data line.

The figures are the Linux `cx23885` driver's own, from `cx23885_vbi_fmt()`: `sampling_rate = 27000000`, `samples_per_line = VBI_LINE_LENGTH` (1440), 12 records from `V4L2_VBI_ITU_525_F1_START + 9`. 1440 samples at 27 MHz is 53,33 µs, which is exactly the ITU-R BT.601 digital active line written at twice the 13,5 MHz rate — so what the card hands over is the active line and nothing else. There is no sync in it, no colour burst, no padding, and therefore nowhere for a frame counter to live: **a cx23885 dump cannot report dropped frames**, and the stage says so rather than implying continuity.

Where the window sits relative to 0H is not in the file and not usefully in the driver either — its `f->fmt.vbi.offset` is a stub 0, and the hardware plainly does not hand over the sync pulse. The configured figure is the window SMPTE 125M says the timing generator produces, 122 samples at 13,5 MHz after 0H, which is 244 here; as with any card capture it is a starting point and the real value is measured from the clock run-in when the capture is opened.

A US network carried its magazine on two or three of the twelve lines where a 625-line broadcaster filled most of the list, so this entry expects the run-in on a smaller share of the records before it trusts a fit — a tenth, against the quarter the PAL entries ask for. Nothing else is relaxed: the search window, the acceptance correlation and the spread and drift limits are the bt8x8 ones expressed at this card's sampling rate. The reference capture — a 1987 CBS off-tape recording — locks on 24,7% of its records with a spread of 1,6 samples, carries its packets on field lines 15 and 16 of both fields, and yields about five and a half NABTS packets per frame.

**Which of the two do I pick?** The same question the `.tbc` crop entries below pose, with the same answer, and here it is worth getting right: a card capture's offset is fitted against the service's own run-in template, so choosing the wrong service does not merely mis-slice the data, it fails the calibration outright and stops the run with a diagnostic. That is the intended behaviour — a fit that cannot find its own service is not a fit — and it makes the wrong choice loud rather than silent.

### `.tbc VBI crop, 16-bit (WST)` and `(NABTS)` — NTSC projects

The first 16 line records of each field of a decoded 525-line luma `.tbc`, which is the shape of every circulating NTSC teletext capture. 910 samples per record with no padding, unsigned 16-bit, at 4×fsc (14 318 182 Hz).

Records 1–12 carry field lines 10–21, the twelve lines the 525-line standard defines. Record 0 is the last post-equalising line and records 13–15 are the start of the picture; none of them carries data. That numbering was measured on the captures rather than assumed: record 0 carries a 2,3 µs equalising pulse where every other record carries a 4,7 µs line sync, and on a captioned recording record 12 carries the line 21 caption run-in.

Sample 0 of every record is already 0H and the levels are already the decoder's own fixed domain, so neither is measured — the record is copied onto the output line index for index and its samples are scaled by the one fixed factor between the two domains. What *is* measured is where the broadcaster put the clock run-in; see [Capture offset calibration](#capture-offset-calibration) below.

**Which of the two do I pick?** The one matching the service the broadcast carried. Nothing in the file records it: the two share the same lines and the same 5,7273 Mbit/s bit rate and differ only in framing code (`0xE4` against `0xE7`) and packet length (34 bytes against 33). Of the material in circulation, TBS Electra is **WST** and CBS ExtraVision and NBC Teletext are **NABTS** — US network teletext was NABTS almost throughout, and Electra is the exception. The filename is usually the best evidence you have.

Picking the wrong one is not a disaster here. Because the record is copied index for index, the data is not moved and not rescaled; all that changes is how much of the line is copied, by about fourteen samples at the front and six at the back. The choice is carried through to the stage's output either way, so a downstream decoder is told which service it has been handed rather than left to assume — and whether it can decode what was placed is the slicer's business, not this stage's.

### Captures that end short of a whole frame

A capture stops when it stops, and nothing that writes one of these files rounds it off first. The `.tbc` crops end on an odd field about as often as not; a card dump ends wherever the writer was when it was stopped, which is as likely to be part-way through a line record as on a record boundary. Neither is a misconfiguration and neither is refused. Whatever follows the last whole frame is short of a frame, so it is not emitted, and the stage says in the log what it dropped — distinguishing an odd field, which is how a capture ordinarily ends, from a ragged tail, which means the capture was cut mid-record.

The one thing that *is* refused is a file too short to hold a single whole frame, since there is then nothing to emit at all.

A ragged tail is also what a wrong container geometry would leave behind, so the log says so when it reports one. It is not treated as evidence of one, because it never was: a wrong container and an interrupted capture both leave an arbitrary number of bytes over, and nothing in the length can tell them apart. The check that does catch a wrong geometry is the capture-offset fit — a wrong stride, sampling rate or television system scatters the clock run-in far beyond the spread and drift limits, and the run stops with a diagnostic naming the cause.

### Adding a format

There is deliberately no "custom" entry. Every fact in the table above had to be measured off real captures — none of it is recoverable from the file, and guessing at it produces output that looks right and is not. A format that *has* been measured is a single data entry in the stage's preset table.

## What it does

The capture is read as fixed-stride line records — the container descriptor gives the stride, the padding, which records carry data, and where the frame trailer is — and every record keeps its `(frame, field, record)` position exactly as stored. Records are never dropped or reordered, so a packet recovered downstream resolves back to the source bytes it came from as a pure index relation.

Each record is then:

1. **Level-mapped.** A card capture's levels are relative and move with the card's gain control, so logic 0 is read from the quiet region ahead of the clock run-in and logic 1 from the larger of the run-in's peaks and the framing code's leading run of ones. Taking the larger is what stops a band-limited source (a tape, where the run-in has been very nearly filtered away) from being scaled to a fraction of the correct amplitude. A capture cropped from a decoded `.tbc` is different: the decoder has already normalised blanking and white to fixed values, so its levels are absolute and are mapped straight onto the output's rather than estimated again. Estimating them would also be wrong on such a capture, whose records start at 0H and so open with the line's sync pulse and colour burst, neither of which is the blanking a quiet-region estimate assumes it is reading. The mapping is linear and nothing else is done to the samples: a deconvolving slicer downstream recovers data by matching the blurred waveform it is given, so any sharpening or slicing here would destroy what it depends on.
2. **Resampled** onto the 4×fsc output lattice with a band-limited filter — not by dropping samples, which would fold the 6,9375 MHz teletext carrier back onto itself. A capture already sampled at 4×fsc with its records starting at 0H is on the output's own lattice, so it is copied rather than filtered: there is nothing to interpolate and nothing that could alias.
3. **Placed** at the data service's nominal time from 0H, corrected by the calibrated capture offset. Both fold into the resampler's filter phase as one pass, so nothing is interpolated twice and no error accumulates.

Only the data region of the line is written — the region the standard gives the packet, plus one bit period of guard at each end so the leading edge of the first run-in bit and the trailing edge of the last payload bit survive. A record covers more of the line than that at both ends (a bt8x8 record opens inside the colour burst window and runs on past the end of the packet), and the rest of it is discarded rather than laid over the blanking. Every sample is clamped into the legal 10-bit range as the last step.

Because the placement is the same on every data line of every frame, all of it — the filter taps included — is resolved once when the capture is opened. Building a frame is then a fill and one filtering pass per data line: thirty-two lines of the six hundred and twenty-five on PAL, twenty-four of the five hundred and twenty-five on NTSC, and nothing at all written to the rest.

Frames follow the host's flat-frame convention: 1135 samples per PAL line, with the two extra samples of each field taken up on its last line, and 910 on every NTSC line, whose lattice is orthogonal and needs no such correction. Every teletext line therefore starts within a sixth of a sample of its true 0H, which is also how a real time-base corrected capture is stored — so a line read out of this stage sits where a line read out of any other source does.

### Capture offset calibration

The single most important value in the stage is the time from 0H to sample 0 of each stored record, and no capture format records it. For the card-capture families the documented figure is explicitly unreliable — the driver's own comment calls the datasheet value wrong and says the real one differs between chip revisions — so for those formats the stage measures it instead: records sampled from across the whole capture (not from its opening minutes, which may be a tape settling or a mistracked lead-in) are correlated against a generated clock-run-in and framing-code template, accepted peaks are refined to a fraction of a sample, and the median becomes a single global offset. It is applied globally and never per line: a per-line correction would erase real timing information and shift lines that were already right.

The fit is reported with its spread, its acceptance fraction and any drift. A monotonic drift across the capture is diagnostic of a wrong sampling rate specifically, and the slope gives the corrected rate directly, so it is reported rather than absorbed. If the fit fails its health checks the run stops with a diagnostic: a wrong global offset silently mis-places every line of a capture that may run for hours.

Captures cropped from a time-base corrected decode have sample 0 of every record at 0H by construction. Their offset is exactly zero and is never fitted — there is nothing to measure, and the measurement would be taken against a record whose head is the line's sync pulse rather than the back porch a card capture opens in.

### The service anchor, on a time-base corrected capture

A capture of that family still has one thing unmeasured, and it is not the offset: it is when the broadcaster transmitted. The stage places the data at the service's nominal time from 0H — the anchor — and cuts the written region one bit period either side of it. That anchor is tabulated, but the tabulated 525-line figures were measured on particular captures, and transmissions disagree with them by up to a microsecond. A broadcaster who transmitted a microsecond early puts the head of every run-in outside the region cut from the tabulated figure, and it is replaced by blanking.

A card capture never meets this, because its fitted offset absorbs the disagreement: the fit lands the run-in on the tabulated anchor by construction. Only a TBC-derived capture, which is never fitted, takes the anchor at face value.

So for those captures the stage runs the same clock-run-in survey and reads the answer the other way round — not as an offset, which is known to be zero, but as the anchor, which is not known at all. The window is then cut from where the run-in actually is. The measurement is reported in the log next to the offset it did not fit, together with the difference from the tabulated figure.

Unlike a card capture's offset, a fit that fails its health checks here does not stop the run. The two failures are not comparable: a wrong global offset mis-places every line of the capture, where a missing anchor measurement only leaves the tabulated figure standing, which is what the stage used for these sources before it measured anything and is right for the captures that figure was taken from. The log says which happened.

### Dropped frames

The last four bytes of every bt8x8 frame are the driver's own frame sequence number, and a container can declare such a trailer whatever wrote it. Because that counter advances once per captured frame whether or not the frame reached the file, comparing it at the ends of the capture says exactly how many frames were dropped across the whole of it — without reading the capture through. Frames are built one at a time as they are asked for, so this is the difference between opening a four-hour capture instantly and decoding 10 GB before the first frame appears.

- `preserve` emits only the frames present. Output frame numbering no longer matches the capture's own, so frame-boundary integrity is lost.
- `pad` emits a blank frame in each gap, so output frame *n* stays aligned with source frame *n*. Padded frames carry no source data and are flagged as such.

A format with no frame counter cannot report drops at all, and the stage says so in as many words rather than implying continuity.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured: the capture is accessible and a format is chosen. The capture itself is checked against that format — its length, its word size — when the stage runs. |
| Red | Not configured, or unusable: no path is set, the path does not point to an accessible file, or the format is not one of the entries listed above. |

Parameters can be set via **Edit Parameters...** in the node context menu.
