# VBI Capture Source

Reads a raw VBI capture — a file holding nothing but the vertical-blanking line records a capture card or a cropped `.tbc` wrote — and lays those lines onto CVBS frames at the timing point and the amplitude the standard puts them at. The result is an ordinary CVBS_U10_4FSC VideoFrameRepresentation, so the teletext decoders see the data exactly as they see it on a native decode.

The rest of each frame is blanking. A VBI capture carries no sync, no vertical interval and no colour burst, and nothing that reads this stage's output looks for them: the teletext slicer locks to the clock run-in inside the line it is handed. Manufacturing a whole television signal around the data would cost far more than placing the data does, and would be spent entirely on samples nobody reads.

## When to use

Add VBI Capture Source as the first stage of a pipeline when your material is a third-party VBI dump rather than a decoded capture: a capture-card dump (`.vbi`, often FLAC-compressed as `.vbi.flac`), or the VBI lines cropped off a decoded `.tbc` (which circulate as `.flac`, `.u16` or `.tbc`, and as the pictures of an FFV1 video, `vbi.mkv`). If you have a whole `.tbc` or a `.composite` file, use the TBC Source or CVBS Source stage instead — those carry a picture.

Nothing is written to disk by this stage. To export the frames as a `.composite` + `.meta` pair, connect a CVBS Sink. Bear in mind that what is exported is a legal CVBS file carrying teletext on an otherwise blank raster, not a reconstruction of the broadcast the capture was cut from.

## Parameters

There are three. Everything else about a capture — its geometry, its sampling rate, the data service it carries, where 0H is, what its logic levels mean, which field it starts on — is a property of the format rather than something you could be expected to know, so it all follows from the **Capture Format** you pick.

| Parameter | Meaning |
|-----------|---------|
| VBI Capture Path (`input_path`) | Path to the capture. FLAC-wrapped files are unwrapped transparently; the wrapper's declared sample rate is a conventional placeholder and is never used for timing. So is a crop kept as the pictures of a lossless video (see [A crop kept as a video](#a-crop-kept-as-a-video)). The browse dialog filters on the extensions these captures arrive under — `.vbi`, `.flac`, `.u8`, `.u16`, `.tbc`, `.mkv` — but nothing about a capture is read from its name, so anything else can still be picked through the All Files filter. |
| Capture Format (`format`) | What the capture is. Only the formats belonging to the project's television system are offered, so a PAL project is offered the one card container in its two source flavours and an NTSC project its two card containers and its `.tbc` crop, each in the two services its captures might carry. |
| Dropped Frames (`drops`) | `preserve` (default) emits only the frames present; `pad` emits a blank frame in each gap so output frame *n* stays aligned with source frame *n*. A format carrying no frame counter cannot report drops at all, so neither policy has anything to act on. |

## The capture formats

### `bt8x8 card dump, 8-bit (WST)` — PAL projects

A 625-line capture-card dump. 2048 samples per record of which 2044 are real, unsigned 8-bit, at 8×fsc (35 468 950 Hz); 16 records per field carrying field lines 7–22, which is the whole of the WST line list.

The card's own time from 0H is documented but unreliable — the driver's source calls its own datasheet figure wrong — so it is measured from the clock run-in when the capture is opened, and the run stops with a diagnostic if that measurement cannot be trusted. Logic levels are measured per line, from each line's own data, because a card's levels move with its gain control. The last four bytes of every frame are the driver's frame sequence number, which is what makes dropped frames detectable at all.

### `bt8x8 card dump, 8-bit (WST, SECAM source)` — PAL projects

The same container, byte for byte, from a SECAM source. The driver's SECAM television norm shares the PAL one's 8×fsc sampling clock, its `vbipack` and its `vbistart`, so the record stride, the field stride, the frame trailer and the lines the sixteen records cover are all unchanged — and post-decode SECAM is a 625-line signal carrying the same World System Teletext, which is why it is placed on PAL frames.

What differs is how much of the line list can carry teletext. A SECAM transmission with vertical colour identification puts the identification signal — a continuous 4,4 MHz burst, the "green bottles" — on field lines 8–15 and 321–328, which is records 1 to 8 of every stored field. Half the records are spoken for before any teletext is inserted, and broadcasters using those idents typically left the rest to test signals and a very few teletext lines: the Russian tape this entry was measured on carries teletext on records 12–14 only (field lines 19–21 and 332–334), with a VITS and a white bar on records 9–11.

That is a fifth of the records at best, where a PAL capture uses most of them, and the run-in is looked for on every stored record. So this entry expects the run-in on far fewer of them before it trusts a fit. Nothing else about the calibration is relaxed: the same search window, the same acceptance correlation, the same spread and drift limits, and the same absolute floor of accepted records. Pick this entry when your capture came from a SECAM broadcast or tape; picking the PAL one instead gets you a healthy fit rejected on the line count alone, and picking this one for PAL material only makes the least important of the checks less likely to fire.

### `bt8x8 card dump, 8-bit (WST, NTSC source)` and `(NABTS, NTSC source)` — NTSC projects

The same card and the same driver as the two PAL entries above, and **not** the same container. `bttv_tvnorms[]` gives its NTSC television norm its own sampling clock — 28 636 363 Hz, 8×fsc NTSC rather than 8×fsc PAL — its own `vbipack` of 144, so 1600 of the 2048 samples per record are real rather than 2044, and its own `vbistart` of `{10, 273}` rather than `{7, 320}`. What does carry over is the card's own geometry: the 2048-byte record stride, the sixteen records a field stores, the 65 536-byte frame, and the four bytes of driver frame counter at the tail of it — which land in the last record's padding here as they do on PAL, there being 448 bytes of it rather than four. **A bt8x8 dump of a 525-line source can report dropped frames**, which the other 525-line containers cannot.

`vbistart` opening at field line 10 is exactly where the 525-line teletext list starts, so records 0–11 carry field lines 10–21 and 273–284, the whole of it, and records 12–15 are the start of the picture and carry nothing. As on the PAL entries the card's time from 0H is measured from the clock run-in when the capture is opened, and the logic levels are measured per line from the line's own data. A dump that carries no teletext but does carry line-21 captions is opened on its caption run-in instead (see [A card capture that carries no teletext](#a-card-capture-that-carries-no-teletext)).

Two calibration thresholds differ from the PAL entries, and only two. A US broadcaster carried its magazines on a handful of the twelve lines where a 625-line one filled most of the list, so this entry expects the run-in on a tenth of the sampled records rather than a quarter. And the spread limit is a tolerance in *time* — 226 ns, the PAL entry's 8 samples at 35,47 MHz — so at this norm's slower clock it is 6,5 samples rather than 8. The reference capture, a 1984 off-air recording of TBS Keyfax, locks on 33,1% of its records with a spread of 3,3 samples and no drift, fits its capture offset at 249,1 samples against the driver's folkloric 244, and carries its magazines on field lines 14–18 of the first field and 14, 17 and 18 of the second.

**Which of the two do I pick?** As with the cx23885 entries below, the one matching the service the broadcast carried — and here it matters, because a card capture's offset is fitted against the service's own run-in template, so the wrong choice fails the calibration outright, with a diagnostic naming the service the capture does carry, rather than merely mis-slicing the data.

### `cx23885 card dump, 8-bit (WST)` and `(NABTS)` — NTSC projects

A 525-line capture-card dump from a Hauppauge HVR-1250 or one of its siblings — the other card container in circulation, and nothing like the bt8x8 one. 1440 samples per record with no padding, unsigned 8-bit, at 27 MHz; 12 records per field carrying field lines 10–21 and 273–284, which is the whole of the 525-line teletext list, so every stored record is a data line.

The figures are the Linux `cx23885` driver's own, from `cx23885_vbi_fmt()`: `sampling_rate = 27000000`, `samples_per_line = VBI_LINE_LENGTH` (1440), 12 records from `V4L2_VBI_ITU_525_F1_START + 9`. 1440 samples at 27 MHz is 53,33 µs, which is exactly the ITU-R BT.601 digital active line written at twice the 13,5 MHz rate — so what the card hands over is the active line and nothing else. There is no sync in it, no colour burst, no padding, and therefore nowhere for a frame counter to live: **a cx23885 dump cannot report dropped frames**, and the stage says so rather than implying continuity.

Where the window sits relative to 0H is not in the file and not usefully in the driver either — its `f->fmt.vbi.offset` is a stub 0, and the hardware plainly does not hand over the sync pulse. The configured figure is the window SMPTE 125M says the timing generator produces, 122 samples at 13,5 MHz after 0H, which is 244 here; as with any card capture it is a starting point and the real value is measured from the clock run-in when the capture is opened.

A US network carried its magazine on two or three of the twelve lines where a 625-line broadcaster filled most of the list, so this entry expects the run-in on a smaller share of the records before it trusts a fit — a tenth, against the quarter the PAL entries ask for. Nothing else is relaxed: the search window, the acceptance correlation and the spread and drift limits are the bt8x8 ones expressed at this card's sampling rate. The reference capture — a 1987 CBS off-tape recording — locks on 24,7% of its records with a spread of 1,6 samples, carries its packets on field lines 15 and 16 of both fields, and yields about five and a half NABTS packets per frame.

**Which of the two do I pick?** The same question the `.tbc` crop entries below pose, with the same answer, and here it is worth getting right: a card capture's offset is fitted against the service's own run-in template, so choosing the wrong service does not merely mis-slice the data, it fails the calibration outright and stops the run with a diagnostic that names the service the capture does carry. That is the intended behaviour — a fit that cannot find its own service is not a fit — and it makes the wrong choice loud rather than silent.

### `.tbc VBI crop, 16-bit (WST)` and `(NABTS)` — NTSC projects

The first 16 line records of each field of a decoded 525-line luma `.tbc`, which is the shape of every circulating NTSC teletext capture. 910 samples per record with no padding, unsigned 16-bit, at 4×fsc (14 318 182 Hz).

Records 1–12 carry field lines 10–21, the twelve lines the 525-line standard defines. Record 0 is the last post-equalising line and records 13–15 are the start of the picture; none of them carries data. Record 13, line 22, is placed all the same, on line 22 of the frame, for the Closed Caption Observer alone: a recording whose time base slipped a line carries its captions there, and nothing else reads it, the calibration included. The crop's levels are fixed, so the record changes no other line's. That numbering was measured on the captures rather than assumed: record 0 carries a 2,3 µs equalising pulse where every other record carries a 4,7 µs line sync, and on a captioned recording record 12 carries the line 21 caption run-in.

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

1. **Level-mapped.** A card capture's levels are relative and move with the card's gain control, so they are measured on every line, and from the data itself: logic 0 and logic 1 are the lowest and the highest tenth of the line's payload samples, which sit on the payload's runs of zeros and ones — logic 1 from the clock run-in's highest tenth where that is higher, as on a caption line of padding, whose payload is an eighth ones. Nothing ahead of the payload is a reference. The card's window opens wherever its hardware opens it — on a bt8x8 dump, a microsecond ahead of the run-in, on whatever the card's clamp left there — and on a tape the run-in itself has collapsed to the midpoint of the data. On a card dump whose record head sits well above the data's logic 0, reading the head as logic 0, as the stage once did, put the lower third of most data lines below the output's floor.

   A payload quantile reads any line, blank or not, so what decides whether a line keeps its own levels is how far it swings against the lines that carry the data service. A line carries the service when its clock run-in and framing code are found where the capture-offset fit put them, to the correlation the fit accepted them at; the frame's levels are the median of those lines', the card's gain at that moment. Every line that swings at least half as far keeps its own levels — a data line, a test signal, a caption line. A line that swings less — a blank line, whose payload quantiles span only its noise — is mapped through the frame's levels rather than having its noise stretched to the data amplitude, and a frame in which no line carries the service is mapped at the levels the capture's data lines were measured at when it was opened.

   A capture cropped from a decoded `.tbc` is different: the decoder has already normalised blanking and white to fixed values, so its levels are absolute and are mapped straight onto the output's rather than estimated again. The mapping is linear and nothing else is done to the samples: a deconvolving slicer downstream recovers data by matching the blurred waveform it is given, so any sharpening or slicing here would destroy what it depends on.
2. **Resampled** onto the 4×fsc output lattice with a band-limited filter — not by dropping samples, which would fold the 6,9375 MHz teletext carrier back onto itself. A capture already sampled at 4×fsc with its records starting at 0H is on the output's own lattice, so it is copied rather than filtered: there is nothing to interpolate and nothing that could alias.
3. **Placed** at the data service's nominal time from 0H, corrected by the calibrated capture offset. Both fold into the resampler's filter phase as one pass, so nothing is interpolated twice and no error accumulates.

Only the data region of the line is written — the region the standard gives the packet, plus one bit period of guard at each end so the leading edge of the first run-in bit and the trailing edge of the last payload bit survive. A record covers more of the line than that at both ends (a bt8x8 record opens inside the colour burst window and runs on past the end of the packet), and the rest of it is discarded rather than laid over the blanking. Every sample is clamped into the legal 10-bit range as the last step.

Because the placement is the same on every data line of every frame, all of it — the filter taps included — is resolved once when the capture is opened. Building a frame is then a fill and one filtering pass per data line: thirty-two lines of the six hundred and twenty-five on PAL, twenty-four of the five hundred and twenty-five on NTSC, and nothing at all written to the rest.

Frames follow the host's flat-frame convention: 1135 samples per PAL line, with the two extra samples of each field taken up on its last line, and 910 on every NTSC line, whose lattice is orthogonal and needs no such correction. Every teletext line therefore starts within a sixth of a sample of its true 0H, which is also how a real time-base corrected capture is stored — so a line read out of this stage sits where a line read out of any other source does.

### Serving a decoder working on many threads

A decoder downstream — the Teletext Sink, the NABTS Sink — reads a dozen or two lines of every frame and nothing else, on as many threads as the machine has. The stage is built so that it keeps up with them rather than being what they wait on:

- **Lines are served without building frames.** Each frame's records are placed once, into the lines they land on and nothing else — 44 kB a frame on NTSC and 73 kB on PAL, against the 955 kB and 1,4 MB of a whole frame — and every line read is answered from that. A line read is sample for sample the line a whole-frame read shows. Each reading thread keeps the frame it is reading at hand until it moves on to another, so the placements kept besides are only the few that two readers may share, and the memory they take follows the number of threads reading. Whole frames are still built, from the same placement, for a consumer that asks for one (the preview, the CVBS Sink).
- **A pass through the capture is decoded ahead of it.** A FLAC-wrapped capture decodes strictly forward from a stream position, so several threads asking for frames a little out of order would otherwise make it seek and re-decode for every frame behind the one it last decoded, and hand the frames out one at a time. When the stage sees requests moving through the capture in order, it opens the capture again on threads of its own and decodes ahead of them into a window of frames. It starts with one decoder and adds more — each with its own copy of the file, each taking its own stretch of the capture — only while readers are found waiting, and never more than one for every three threads reading, half the processor count, or sixteen: threads working at the front of the window wait on the frames being decoded there, which another decoder would not bring them any sooner, and each decoder holds a stretch of the window of its own. A pass on a thread or two rarely needs more than one. The window holds a few megabytes per decoder and follows the readers: a frame's bytes are let go once its reader has them, and the buffers they were decoded into are used again for later frames rather than freed, so a pass allocates no more of them than the window has held at once.
- **A line nobody reads is not filtered.** The stage notes which lines its readers ask for and places only those: a closed-caption pass reads line 21 of each field, two of a frame's twenty-four on NTSC, and has only those two filtered, while a decoder reading every data line has every frame placed whole once it has read its first. Each frame's levels are still measured on all its lines, so a line reads the same whichever others were placed with it. The lines a frame does place are filtered together, four abreast — in one AVX2 register where the processor has it — with every sum taken in the order a line alone would take it.
- **Random access is not disturbed by it.** A single frame asked for out of sequence — the preview jumping about the capture — is read as before, seeking where the capture has seek points, and never moves the window away from a pass it is not part of.
- Only reading the capture's own stream position is serialised. Decoding the samples, level mapping, resampling and placing them run on the reading threads, all at once.

None of this changes a single sample: a frame's bytes are the capture's whichever decoder produced them, and the placement is the frame builder's own.

### Capture offset calibration

The single most important value in the stage is the time from 0H to sample 0 of each stored record, and no capture format records it. For the card-capture families the documented figure is explicitly unreliable — the driver's own comment calls the datasheet value wrong and says the real one differs between chip revisions — so for those formats the stage measures it instead: records sampled from across the whole capture (not from its opening minutes, which may be a tape settling or a mistracked lead-in) are correlated against a generated clock-run-in and framing-code template, accepted peaks are refined to a fraction of a sample, and the median becomes a single global offset. It is applied globally and never per line: a per-line correction would erase real timing information and shift lines that were already right.

The fit is reported with its spread, its acceptance fraction and any drift. A monotonic drift across the capture is diagnostic of a wrong sampling rate specifically, and the slope gives the corrected rate directly, so it is reported rather than absorbed. If the fit fails its health checks the run stops with a diagnostic: a wrong global offset silently mis-places every line of a capture that may run for hours.

A teletext line carries two marks a fit can check where it locked: its framing code, which reads with at most one bit wrong, and the alternation of its clock run-in. A tape can take either away — the run-in collapses towards the data midpoint long before the data does, and a service framed otherwise than the one configured reads its framing code wrong — but a line that carries the service keeps one of them. A fit most of whose accepted records show neither is refused whatever its spread: it locked on something shaped enough like the run-in and framing code to correlate, but not on them. On a card dump that carries teletext, three quarters or more of the accepted records show a mark; a line carrying another code with no run-in ahead of it can lock a NABTS fit at one place on a tenth of the records, a third of which show one. Such a capture then opens on its captions (below).

The same survey reads the levels the capture's data lines carry their data at — the median of the accepted records' payload levels — and the log reports them. They are what a frame with no data line of its own is mapped at.

### A card capture that carries no teletext

A 525-line card dump need not carry teletext at all — a recording with nothing in its vertical interval but line-21 captions is an ordinary thing to have captured — and the teletext fit cannot tell such a capture from a wrong configuration: it finds no run-in either way. So when that fit fails on a 525-line card capture, the stage looks further before it stops:

- If the capture carries **the other 525-line teletext service** — NABTS opened as WST, or the reverse — it is refused, and the diagnostic names the service it does carry. Choosing the wrong service is meant to be loud, captions or no captions.
- Otherwise the offset is fitted on the **line-21 caption run-in** instead (CTA-608-E: seven cycles of a sine at 32 × the line rate from 10,5 µs after 0H, then the two 0 start bits, with the 1 start bit 8,75 bit periods after the run-in begins), correlating line 21 of both fields across the capture. If it passes, the capture opens with a warning to expect little or no teletext from it. Every record is then written over the caption line, which runs a microsecond and a half past the end of a teletext packet, and a line with no level reference of its own is mapped on the scale the caption data sets — its 0 bits at blanking and its 1 bits at 50 IRE — so a Closed Caption Sink reads the captions, the text services and the XDS. A card dump that carries line 21's caption or text services and no teletext opens this way.
  - **Each field is fitted on its own.** Line 21's two fields are two caption streams — CC1, CC2 and the first two text services in field 1, CC3, CC4, the other two and XDS in field 2 — and the encoders that insert them need not time their run-ins alike: on a card dump the two fields' run-ins can sit 8 to 17 samples apart at 27 MHz (0,3 to 0,6 µs), each field tight to within a sample, while the teletext lines of the same captures show no such split. Pooled, such a capture looks like one that was never time-base corrected. So each field must pass the spread, drift and acceptance checks on its own records; a field that carries no captions — many recordings carry them on one field only — is left out with a note in the log; and the offset puts the midpoint of the two fields' run-ins on the anchor, which places each within a third of a microsecond of it for the caption reader to find. Two fields further apart than the format's search tolerance cannot both be placed by one offset, and the capture is refused.
  - **A run-in the card clipped is still found.** A card's window can open after the caption run-in has begun: a card dump can start most of a cycle into every run-in, far ahead of where the preset predicts it. The caption fit looks from a run-in that began up to one and a half of its cycles before the record — correlating only the part of the template inside the record — to the format's search tolerance past the predicted position.
  - **A caption line has to look like one.** The template is the waveform CTA-608-E draws, and a caption line matches it at 0,8 to 0,97; a line-21 record counts at 0,7 or better, where the teletext fit takes 0,5 for run-ins a tape has all but erased. Lines that carry no captions reach 0,6 at most, and counted they mislead the fit: a few of them in the hours before a recording's captions begin can make a drift of tens of samples out of a field whose captions sit within a few samples of one place.
  - **A hundred and twenty-eight frames are sampled**, across the whole capture as for the teletext fit, where the teletext fit samples sixteen. A captions-only capture has two records a frame to fit on rather than twenty-four, and its captions can come and go: captions on one field in the last quarter of a recording are sampled by sixteen frames only four times, against the eight each field needs.
- If neither fit passes, the capture is refused as before.

A 625-line capture carries no line-21 captions, so it is refused as before.

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
| Red | Not configured, or unusable: no path is set, or the path does not point to an accessible file. |

Parameters can be set via **Edit Parameters...** in the node context menu.
