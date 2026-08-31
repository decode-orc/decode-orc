# Teletext Sink

Recovers World System Teletext (WST) data lines from the VBI, writes the packets as a flat stream, and presents the pages it found in a page viewer. Both television systems ITU-R BT.653 defines System B on are covered:

* **625 lines** (PAL) — ETSI EN 300 706, 6.9375 Mbit/s, 42-byte packets, written as `.t42`.
* **525 lines** (NTSC, PAL-M) — BT.653 Table 1b, 5.727272 Mbit/s, 34-byte packets, written as `.t34`.

North American NABTS (System C) is covered by the **NABTS Sink** instead. That matters on a 525-line capture, because the two services share the clock run-in, the bit rate, the VBI lines and the data levels and are told apart by the framing code alone — `0xE4` for WST here against `0xE7` for NABTS. A stage pointed at the wrong one recovers nothing rather than recovering nonsense, so if this sink finds almost no packets on a 525-line source, try the other.

French System A and Japanese System D transmissions are not supported, and NTSC line-21 captions are covered by the Closed Caption Sink instead.

## When to use

Add this sink when a LaserDisc, CVBS capture, or tape source carries teletext you want to preserve or inspect. The packet stream can be browsed and decoded with external tools such as vhs-teletext (`teletext filter` / `teletext interactive`) and wxTED — see the [T42 packet stream description](https://teletext.wiki.zxnet.co.uk/wiki/T42_packet_stream) on the zxnet teletext wiki.

Expected recovery quality depends on the source's luma bandwidth at the teletext bit rate:

* **LaserDisc and broadcast-quality CVBS captures** carry the full teletext spectrum; recovery is expected to perform like a hardware decoder.
* **Consumer VHS** truncates the upper half of the teletext spectrum, causing heavy intersymbol interference. Threshold slicing cannot recover anything at all from such a recording — the clock run-in is the first thing the tape loses — so the stage carries a second bit detector for this case; see `detector` below. With it, PAL SP and LP recordings measured here yield readable pages. Stacking multiple captures and dropout correction upstream both improve the odds further.

## What it does

Trigger the node and it makes one linear pass over the whole frame range. For each frame it probes the candidate VBI lines of both fields for a teletext data line and extracts the payload bytes (magazine/row address plus the service's display bytes). Recovered packets are written to the output file in strictly temporal order — frame, then field 1 and field 2, then ascending line — exactly as a receiver would see them broadcast.

The output is a flat, headerless sequence of whole packets in **transmission coding**: Hamming 8/4 on addressing bytes and odd parity on display bytes are preserved, so consumers decode the stream exactly as they would a live transmission. What the coding does not promise is that every byte is the one this copy of the line carried. By default the stage mends display bytes that fail their parity check and combines the repeated transmissions of each page row (`repair_damaged_bytes` and `squash_repeated_rows` below), which is most of what makes a tape readable; turn both off and the packets are written exactly as recovered.

The same pass assembles the pages, so the run produces a catalogue of every page the recording carried — where each was first and last seen, how often the carousel brought it round, and its best assembly from every copy recovered. A page number transmitted as a sequence of sub-pages is catalogued as the whole sequence, one assembly per sub-page, since each carries different content under its own sub-code. That is what the **Teletext Pages** tool shows. The packet stream is the stage's product and the catalogue rides along with it, which is why this is a sink rather than an analysis sink; leaving `output_path` empty makes the catalogue the only thing the run produces, and that is supported.

The pass also learns about the recording as it goes, and spends less on later frames than on the first ones: where in the line this recording's data bursts start, and which of the candidate lines carry them at all (`pin_data_phase` and `learn_active_lines` below).

Recovering a line reads that line's samples and nothing else, so frames are decoded on several threads at once while the packets are written from one, in the order they were transmitted (`decode_threads`). The recovered stream does not depend on how many threads were used — see that parameter for what makes that true. Learning and threading together take a pass over the 625-line reference capture from 1970 ms to 295 ms, and the 525-line one from 946 ms to 156 ms.

On a 625-line source the stage can additionally decode the subtitle page (pages flagged C6 in the header control bits, conventionally page 888 in the UK) and write the recovered subtitles as a SubRip (`.srt`) file next to the packet stream. Cue timing derives from the field number at 50 fields/s, which is why the option is not offered on 525-line sources; colour, positioning, and other Level 1 presentation attributes are dropped — SRT carries the plain text. Text is written as UTF-8 through the page's own G0 character set — the alphabet of `character_set` below, and within Latin the national option sub-set the page header selects (ETSI EN 300 706 §15.6.2 Table 36) — so a UK page's `£`, `½` and `¼` reach the file as themselves rather than as the ASCII the transmitted codes would be, and a Cyrillic service's cues reach it in Cyrillic. Where a page carries two alphabets, each cue follows the row's `ESC` switches from one to the other (see `second_character_set`).

## Tools

### Teletext Pages

The page viewer for this node. Triggering the node opens it automatically, which is how the pages are reached — leave `output_path` empty and triggering is a decode-and-browse that writes no file.

The catalogue stays with the stage after the run, so closing the viewer and picking **Teletext Pages** from the **Stage Tools** menu re-opens it immediately, reading what the last trigger produced rather than decoding the source again. That menu entry only ever reads: on a node that has not been triggered it says there is nothing to show instead of starting the decode, because deciding when to spend that time is what **Trigger Stage** is for. Editing any stage's parameters rebuilds the graph and discards every stage's results, closing the open viewers with them — trigger again for a catalogue that matches the new settings.

The viewer lists every page the range carried — page number, how many times it was seen, and the frames it was first and last seen at — and renders the selected page as a Level 1 display, together with the run's recovery summary. Because the catalogue comes from a pass over the whole source rather than a window around the preview position, the list is the service's full carousel rather than whatever happened to be on screen.

A page number is not always one page. A service can transmit a sequence of **sub-pages** under it and cycle through them — the multi-page set of ETSI EN 300 706 Annex A.1, which on a receiver is a page that changes on its own every few seconds. Each sub-page is catalogued separately, and the control under the page display says how many the page has, which is on screen, and its sub-code as Annex A.1 writes it (`0001`, `0002`, …); the arrows either side step through the sequence and wrap, as the carousel itself does. A page with no sub-pages says so and the arrows are inert. The status line under the display describes the sub-page on screen — its own transmissions and the frames it was seen at — while the page list counts the whole set together, because a row there is about the page number.

Which pages are sequences is otherwise invisible in the list, so the page's row says so in its tooltip.

**Enable animations** flashes the characters the service marked to flash. ETSI EN 300 706 §12.2 gives code 0/8 the job of alternating the foreground pixels of the characters that follow it between the foreground and background colours, cancelled by Steady (0/9) or by the start of the next row; the rate is the receiver's own, and this is the 0.75 Hz cycle a World System Teletext receiver used — three quarters of it showing the character and the last quarter blank. Only the characters alternate: their background stays put, and a concealed character stays concealed. Clear the box to hold the page still, which is what reading a flashing headline, or capturing the display, wants.

**Save PNG…** writes the page on screen to an image file. The page is drawn on its own, at whole pixels per character rectangle and at the aspect of the character rectangle the service uses, so a 40 x 25 page comes out 960 x 1000 with no border around it and every column landing on a pixel boundary. Flashing characters are saved lit whatever phase the display happens to be in, because a still caught in the blank phase would be missing the very text the service chose to flash; **Show data errors** is honoured, so a page saved with the overlay on records what was lost as well as what arrived. The name offered is the page and its sub-code — `Page-100-0002.png`. The packet stream the stage writes is the data rather than the picture, so this is the only place the assembled page exists as one.

**Save All PNGs…** writes every page and sub-page of the catalogue in one pass, into a folder chosen once. Each image is exactly what **Save PNG…** would have written with that display on screen — the same size, flashing held lit, **Show data errors** honoured — under the same name, so a whole carousel lands as `Page-100-0001.png`, `Page-100-0002.png`, and so on. A service can carry hundreds of pages, so a progress dialogue says how far the batch has got and cancelling it keeps what was already written. The button stands whenever the catalogue holds anything drawn, whatever happens to be on screen.

## Parameters

### output_path (file path)
Path to the output packet stream. The service's extension is appended if absent: `.t42` on a 625-line source, `.t34` on a 525-line one.

Optional. Leave it empty and the run decodes exactly as it would but writes no file, which is what to do when the pages themselves are what you are after — the **Teletext Pages** tool is filled either way. `write_report` and `export_subtitles` both write beside the packet stream, so they need a path and the run is refused if either is enabled without one.

### first_vbi_line (integer)
First candidate field line probed for teletext, 1-based and applied to both fields. Default: `6` on a 625-line project, `10` on a 525-line one. The 625-line default window 6–22 covers broadcast lines 6–22 (field 1) and 318–335 (field 2) permitted to carry teletext by ETSI EN 300 706; the 525-line window 10–21 covers broadcast lines 10–21 and 273–284 (ITU-R BT.653 §2).

### last_vbi_line (integer)
Last candidate field line probed for teletext, 1-based and applied to both fields. Default: `22` on a 625-line project, `21` on a 525-line one.

### keep_empty_packets (boolean)
When enabled, every candidate line with no recoverable teletext emits a whole zero packet instead of being skipped, so each packet position in the file maps 1:1 back to a specific (frame, field, line) — the vhs-decode convention, useful for packet-for-packet comparison against other decoders. Default: `false`.

### detector (string)
How data bits are recovered from each line. Default: `Automatic`.

* `Threshold` — locks to the clock run-in and slices at bit centres. Exact on a source that passes the whole data band, which is what a disc or a direct capture gives you, and the cheapest option.
* `MLSE` — fits the recording's frequency response to the 24 known bits every teletext line starts with, then picks the payload bit sequence that response most likely produced. This is what recovers teletext from tape, where the limited bandwidth smears each bit into its neighbours. Nothing is trained or configured beforehand: each line carries the reference used to fit its own channel. It then refits that response against the whole packet it has just read and reads the packet again, which is where most of its accuracy on a tape comes from: 24 known bits pin a frequency response far less well than 360 do. Costs roughly ten times the work per line and, because it fits rather than matching an exact framing code, it locks onto a noise-only line a few times in a hundred (5 of 64 synthesized noise lines here, and none at all on the tens of thousands of real non-teletext data lines measured) — `require_valid_mrag` and an internal parity plausibility check are what hold that down.
* `Automatic` — try `Threshold` first and fall back to `MLSE` only on lines it could not lock. A disc source therefore behaves and costs exactly as it did before, and a tape source gets the fallback without being told to.

### character_set (string)
Which alphabet the recovered page codes are read in. Default: `Latin`. The other three are `Cyrillic (Russian/Bulgarian)`, `Cyrillic (Ukrainian)` and `Cyrillic (Serbian/Croatian)`.

A page's forty display codes are seven bits each, and what they mean depends entirely on which G0 character set the service used: code 4/4 is `D` in the Latin set and `Д` in the Russian one. ETSI EN 300 706 §15.2 lets a service say which, in a packet X/28/0 or M/29/0, **and this stage always believes it when it does** — the setting here is consulted only for pages that designate nothing. That is most of what survives on tape: those packets are a Level 2.5 facility and a Level 1 service transmits neither, which is why the standard falls back on "a local Code of Practice", meaning the region the receiver was sold in. Nothing in a recording distinguishes the alphabets, so there is nothing this stage could measure and the choice has to be yours.

Get it wrong on a Cyrillic service and the pages come out as readable-looking Latin gibberish — `Петербург` arrives as `Peterburg` — which is exactly what a western receiver showed on the same broadcast. Get it wrong the other way and a Latin service comes out as Cyrillic gibberish. It changes nothing about what is *recovered*: the exported packet stream is the transmitted bytes and carries no character set at all, so a `.t42` written under the wrong setting is not damaged and never needs exporting again. What it does affect is the pages you read and any subtitles exported from them, and those come from the decode — so correcting the setting means triggering the node again.

Within the Latin set the *national option sub-set* — which puts `£` at 2/3 for a UK service and `é` for a French one — still comes from each page's own header bits (C12–C14) and needs no setting. The Cyrillic sets reserve no such positions, so those bits are ignored for them.

Only the three Cyrillic sets are implemented alongside Latin. The standard also defines Greek, Arabic and Hebrew G0 sets; a page designating one of those is shown as Latin.

### second_character_set (string)
The alphabet the ESC control character switches into. Default: `None`; the other four are the same sets `character_set` offers.

Some services need two alphabets on one page. A Russian or Ukrainian broadcaster writes foreign names, titles and station idents in Latin letters in among the Cyrillic, and marks each switch with a control code in the row itself: ETSI EN 300 706 §12.2 Table 26 code 1/B, `ESC` (or *Switch*), which **toggles** the rest of the display row between the page's first and second G0 sets. §15.3 designates the pair in a packet X/28/0, X/28/4, M/29/0 or M/29/4, and — exactly as for `character_set` — a service that designates its own is always believed and this setting ignored. That includes a service saying it needs only one: the standard reserves the value `1111111` for "no second G0 set required", and a page carrying it has `ESC` switched off no matter what is set here.

`None`, the default, means the same thing for material that designates nothing: one alphabet throughout, `ESC` inert, which is how every page was read before this parameter existed. Set it only where a recording really does mix alphabets — the pairing is a claim about the transmission, and a wrong one turns the switched runs into nonsense instead of leaving them alone. For a Russian service the pair is `character_set` = `Cyrillic (Russian/Bulgarian)` with `second_character_set` = `Latin`.

The two sets are re-selected at the start of every display row, which is what keeps the damage bounded: a lost `ESC` corrupts the remainder of one row and can never carry into the next. Two other things work in your favour. Combining repeated transmissions (`squash_repeated_rows`) votes on the `ESC` byte along with every other byte of the row, so a copy that lost one is normally outvoted before the page is assembled; and a byte that fails its parity check never toggles, so a page's alphabet cannot be flipped by damage that the stage has already spotted. Where damage *and* a second set coincide, the decoder marks every cell after the damaged byte on that row as being in one of the two sets rather than definitely one: a byte lost to damage cannot be told from the `ESC` it might have been, and there is no honest way to say which alphabet the rest of the row is in.

Within a second Latin set the national option sub-set is English. Unlike the first set, it cannot come from the page header: §15.3 says in as many words that the C12–C14 bits "[are] not relevant to the secondary set", and with no packet X/28 or M/29 to designate it there is nothing else to read.

### tolerant_framing (boolean)
Accept framing codes with one bit error. Recovers more packets from noisy sources at the cost of a higher false-positive rate. Default: `false`.

### require_valid_mrag (boolean)
Drop packets whose magazine/row address bytes fail Hamming 8/4 correction. This suppresses false framing-code locks on noise while still passing single-bit-damaged packets through to downstream tools. Default: `true`.

### repair_damaged_bytes (boolean)
Restore odd parity on damaged display bytes by flipping the bit the MLSE detector was least sure of. Default: `true`.

Every display byte carries a parity bit (ETSI EN 300 706 §8.1), so a byte that fails its parity check is *known* to be damaged — but parity says only that, not which of the eight bits is wrong. The MLSE detector does know: it chose the packet's bits by finding the most likely sequence, and it can say, for each bit, how much more likely that choice was than the opposite one. Flipping the bit it came closest to reading the other way is the best available repair of a single-bit error, and it restores parity, so the emitted packet is still valid transmission coding.

What it costs is the distinction between a byte that arrived intact and a byte that has been guessed. Downstream — the page view's damaged-byte readout, and the parity-first rule when repeated copies of a row are combined — a wrong repair looks exactly like clean data. On a recording where most bytes come back damaged that trade is worth making; on one where few do, it is not. A repaired byte does carry the low confidence of the bit that was flipped, so combining repeated rows still prefers a copy that arrived intact.

Applies only to the data bytes of parity-coded rows (0–25), and only under the MLSE detector — a disc or a direct capture, which the threshold detector reads exactly, is untouched whatever this is set to. The magazine/row address, and the header's page number and control bytes, are Hamming 8/4 coded and carry their own correction, which page decoding already applies; rows above 25 are not byte-wise parity coded at all.

Measured on the reference VHS captures, packets whose 40 data bytes all satisfy parity rise from 70.4 % to 87.9 % (LP) and from 78.7 % to 91.6 % (SP) — though on a real recording that figure is partly manufactured by the repair itself. Against synthesized lines with known payloads, where the answer can be checked, 714 repairs across 2438 recovered packets corrected 598 bytes and damaged none.

### pin_data_phase (boolean)
Narrow the search for where each line's data burst starts to where this recording's lines have already been seen to start. Default: `true`.

Neither detector is told where in the line the data is. Each sweeps the whole data-timing window of ETSI EN 300 706 §6.3 looking for it — 284 candidate positions for the threshold detector at the PAL sample rate, 355 for MLSE, every one of them on every candidate line of every field. That sweep is most of what a pass costs.

It is also almost entirely redundant. The position of the data burst in the line is fixed by the standard, and a time-base-corrected recording has already had the transport's timing variation taken out of it, so every data line of a recording starts within a sample or two of every other. Once 24 lines have yielded packets this takes the middle of the positions they locked at, widens it to cover their spread, and sweeps only that. The locks are kept as a running window of the last 64, so a recording whose data start moves part way through — a tape spliced from two transfers — follows it rather than averaging across it. If the locks disagree by more than about seven samples they are not describing one position, and the full sweep is used instead.

This cannot lose a packet. A narrowed sweep that recovers nothing is repeated over the full window, so the worst case is the few percent the narrow sweep cost — paid on lines that carry no data, which is where a pass has time to spare. What it does change is *which* lock is chosen on a line where both windows find one, and that turns out to favour the narrow window: an exhaustive sweep can settle on a false correlation peak elsewhere in the line, and pinning rejects those by construction. On the reference captures it recovers 4 % more packets on the 625-line sample and 0.7 % more on the 525-line one, while cutting the decode time by roughly a third.

The report says where the window was pinned and from how many locks, or why it was not pinned.

### learn_active_lines (boolean)
After the first frames, read only the candidate lines this recording has been seen to carry teletext on. Default: `true`.

The VBI window is what a service *may* use, not what it does: 17 field lines on 625 and 12 on 525, of which a real service uses a handful. The rest are read on every frame regardless, and where one carries picture content, VITS, VITC or line-21 captions rather than nothing, that work runs the full detection chain — under the `Automatic` detector, including the MLSE fallback — to reach a rejection it reached on the frame before.

The first 50 frames are read in full and what each line yielded is counted; after that only the lines that have produced a packet are read. Because a service can start part way into a recording, every 50th frame reads the full window again, so a line that comes alive is picked back up within one interval. The mask is kept per field, so a service using different lines in each field is not flattened.

Unlike `pin_data_phase` this can lose a packet: a line that carries data exactly once, on a frame that is neither a learning frame nor a recheck frame, is not read. On the reference 625-line capture that cost one packet in 3,964. The saving depends entirely on what the dead lines hold — on the 625-line sample, whose unused lines are blank and rejected instantly by the amplitude gate, it is worth about 3 %; on the 525-line tape, whose unused lines carry enough signal to reach the MLSE fallback, about 20 %. Turn it off for an archival pass where every packet matters.

### decode_threads (integer)
Threads to recover lines on. Default: `0`, meaning one per processor.

Each line is recovered from its own samples, so the frames of a recording are independent and are decoded several at a time. Writing the packets is not independent — a teletext stream is strictly ordered, and the page assembly and row combining downstream of it both depend on that order — so emission stays on one thread and the decoding runs ahead of it.

**The recovered stream is identical whatever this is set to.** That is not something that comes for free with threading, and it is worth saying how it is arranged. The pass learns as it goes (`pin_data_phase` and `learn_active_lines` above), so what a thread is allowed to know while it works has to be fixed in advance, or a frame's packets would depend on how far ahead of it the other threads had got. Frames are therefore decoded in blocks: every frame of a block is read against one frozen view of what the pass has learned, and the block is then emitted in order, which is what advances that view for the next one. A worker never reads the live state. The functional tests decode the reference captures at one, three and eight threads and require the same SHA-256 from each.

Lower it only to leave the machine free for other work. Measured on the 625-line reference capture: 1406 ms on one thread, 442 on four, 295 on eight. Beyond the processor's physical cores there is nothing further to win — the run is by then waiting on the source stage, which serves frames from a single file position and so hands them out one at a time.

### squash_repeated_rows (boolean)
Combine repeated transmissions of each page row and write the combined form. Default: `true`.

Teletext is a carousel, so any recording longer than one cycle holds several copies of every row, damaged in different places. Comparing them byte by byte recovers a row cleaner than any single copy of it. The vote goes first to values that pass their parity check — a byte known to be corrupt never wins over one that is not, however often it was seen — and then by how sure the detector was of each byte, so a copy read cleanly outweighs the same number of copies of one it nearly misread. Packet order, count and timing are unchanged; only damaged display bytes move. Page headers are left alone: their display bytes carry a clock that legitimately differs between transmissions.

Copies are only combined within one *run* of one sub-page. A page number transmitted as a sequence of sub-pages carries different content under each sub-code, so each sub-page's copies are compared only against each other — the sub-code is part of the identity a copy is filed under, which is also why the viewer's sub-pages hold exactly the copies that were combined into them. A header with the erase bit set (C4, ETSI EN 300 706 §9.3.1.3 Table 2) says the page's content is being replaced, so what follows it is a different page that happens to share a number, and combining across it would blend the two. A service that sets C4 on every transmission therefore gives every transmission a run of its own and nothing can be combined — the report below says so directly, as a run count equal to the transmission count and a copies-per-row distribution that is entirely single-copy.

Combining assumes the copies are copies of the *same* row, and two things put packets of other pages among them. The magazine and row address is carried in two Hamming 8/4 bytes, and that code has minimum distance 4: it corrects one bit error and detects two, but a burst of three resolves silently to a neighbouring codeword, so the packet is not damaged but *moved* — onto another magazine's open page, or onto another row of this one. Separately, a page whose header was lost leaves the page before it open, and its rows are filed against that one. Either way the intruder is a clean read of real data, only not of this row, and enough of them turn a page into a per-character blend of every page mis-addressed into it — worse the more copies are combined, which is the opposite of what combining them is for.

Two rules keep them out. A packet other than a header is only attributed to a page when its address arrived as a codeword rather than being corrected into one: a corrected address is one bit error away from having been mis-corrected instead, and it is not evidence of where the packet belongs. Such a packet is still written to the output stream — the stream is a record of what was recovered — but no page votes it in, and no vote mends it either, so a difficult recording shows a *higher* damage figure for having refused to guess. Then the vote itself is taken twice: once to find what the copies mostly say, and again without the copies that agree with less than half of it. Only a minority can be dropped that way, since where most copies disagree there is no row for the rest to be outliers of — a "page" assembled entirely out of intruders is left as it was rather than being tidied into something that looks decided.

The pages shown in the viewer are built from the combined rows, so this improves what the viewer displays as well as what the file holds.

Costs a second pass over the recovered packets, which are held in memory (roughly 50 bytes each).

### write_report (boolean)
Write the run's diagnostic report next to the packet stream, named after it with a `.txt` extension — `mydata.t42` gives `mydata.t42.txt`, `mydata.t34` gives `mydata.t34.txt`. Needs `output_path` set. Default: `false`.

The same report is always written to the log at debug level; this only keeps a copy somewhere a reader can go back to.

It opens with the result in one line — how much of what came out is damaged — and the same figure appears in the stage's status when the run finishes:

```
Teletext analysis report
  Data loss 1.14% — 30 of 2,640 recovered characters are damaged
  Combining repeated rows mended 470 of the 500 characters that arrived
  damaged (94.0%); without it the loss would be 18.94%
```

Damage is counted by the odd parity the standard already puts on every display byte (§8.1), over the display rows of the stream as written. Read it as *of the characters this export produced, this share are known wrong*. Two caveats, both in the conservative direction: it is a floor, because a byte damaged in two bits passes parity and is counted as good; and it says nothing about rows that never arrived, which are absent from both sides of the ratio.

Below the headline the report gives what was exported (output path, frames, VBI window, detector, character set, packet and field counts, pages catalogued), how recovery went (the profile described under Notes below), and the detail behind the headline — the share of rows the vote changed, and how many copies each row was combined from:

```
Teletext squashing: 264 row packets over 1 page run; 246 rewritten (93.2%),
  660 of 10,560 display bytes replaced (6.25%)
  Odd-parity failures: 647 before (6.13%), 0 after (0.00%)
  Copies per row packet: 8+ copies 264 (100.0%)
```

The copies-per-row line is what separates a run that could not correct anything from one that had nothing to correct: a row transmitted once cannot be improved however good the vote is.

### export_subtitles (boolean)
Decode the subtitle page alongside the packet export and write timed subtitle cues to a `.srt` file next to the output (same name, `.srt` extension), which is why `output_path` must be set. A subtitle is displayed when the page arrives, replaced when its text changes, and cleared when the page is erased or loses its subtitle flag. Default: `false`. Offered on 625-line projects only.

### subtitle_page (string)
The teletext page carrying the subtitles: a magazine digit (1–8) followed by two hexadecimal page digits, e.g. `888` (the UK convention). Only used when `export_subtitles` is enabled. Default: `888`.

### subtitle_format (string)
Subtitle output format. Currently only `SRT` (SubRip) is offered — the least lossy portable target for teletext subtitle text; colour and positioning are dropped at this level. Default: `SRT`.

## Page numbers the recording named but the service never sent

A damaged recording does not only lose pages — it invents them, and the invention is convincing.

The magazine and packet number (ETSI EN 300 706 §7.1.2), the page number (§9.3.1.1) and the sub-code (§9.3.1.2) each arrive in their own Hamming 8/4 byte. That code has minimum distance 4 (§8.2), so it corrects one bit error and detects two, but a burst of **three** lands inside a neighbouring codeword's correction sphere and is resolved there silently, with nothing to say a guess was made. On a band-limited recording that is the ordinary error rather than the exotic one: the MLSE detector's characteristic failure is a run of alternating bit inversions, and the codeword for digit 0 is itself an alternating pattern, so digit 0 is carried onto digit 7 over and over.

The damage that does is not to the page. The page still decodes; it is simply catalogued at a number the service never transmitted, where it sits beside the real one holding the same content.

So the run counts, per sub-page, how often its number arrived **as transmitted** rather than corrected into shape, and prunes the catalogue on it:

* a page no appearance ever named as transmitted is not a page. Where exactly one attested page number differs from it in a single hexadecimal digit, it is a misreading of that one, and its appearances are added to it. Its rows are not: rows are combined by the squasher under the misread number long before the catalogue sees them, so there is nothing left to merge.
* where several attested numbers are a single digit away, the misreading has two explanations and neither is evidence. A magazine carrying both 1/03 and 1/07 is ordinary. The entry is removed rather than attributed.
* where none is, it was a false lock on noise. Removed.

The run's report says how many page numbers were folded and how many dropped, so a shorter list than an earlier run gave is explained rather than mysterious.

On an undamaged source every byte arrives as transmitted, the pass finds nothing, and nothing changes. The one case it declines outright is a recording where *no* page number was ever attested: there is then no baseline to judge the rest against, and the catalogue is left exactly as recovered.

## Notes

* PAL, NTSC and PAL-M sources are accepted; any other video system is reported as an error.
* Empty VBI lines are cheap to probe under every detector: a line that never rises meaningfully above black is rejected before any detection runs.
* Beyond the two options above, nothing is corrected in the packet stream: the addressing bytes are written as recovered and left to their own Hamming coding, and further correction is the consumer's job (vhs-teletext, wxTED, and similar tools).
* A 525-line service sends the last eight columns of its rows in separate row-extension packets, which the page viewer reassembles; the packet stream holds them as transmitted. The service addresses them to a magazine it is not using for pages, so which magazines carry extensions is read from the recording: one that opens a page with a header carries pages, and one that sends nothing but the six block numbers carries extensions.
* The same "arrived as a codeword" test gates individual packets, not only page numbers. A display row or row-extension packet whose address had to be corrected is attributed to no page at all, so it neither reaches the viewer nor joins a vote. A header is the exception: the page it opens is reconciled afterwards by the pass above, which a row filed against the wrong page cannot be. The exception stops at the reading above, which no later pass can undo — a corrupted packet correcting into a header would otherwise take a magazine's extensions out of the recording for good, and a page of that magazine keeps 32 of its 40 columns for as long as the reading stands.
* Combining repeated rows ("squashing") is an idea taken from [vhs-teletext](https://github.com/ali1234/vhs-teletext) by Alistair Buxton, with thanks. A row transmitted only once cannot be corrected, so the benefit grows with how long the recording runs and how often each page comes round.
* Every run logs a recovery profile at debug level, as part of the report described under `write_report`. It covers how many candidate lines carried a data burst, how many packets came out of each detector, which gate discarded the rest, how the odd-parity failures of the recovered packets are spread across the data-byte positions, and, for the MLSE detector, how its reconstruction error is spread along the packet, how sure it was of the bytes it emitted, and how many of them parity repair mended. That last profile is the timing reading: an error that grows from the first byte to the last means the bit clock is running at the wrong rate across the packet, while a level profile means noise and intersymbol interference. This is a diagnostic for judging a difficult tape; the packet stream is unaffected.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
