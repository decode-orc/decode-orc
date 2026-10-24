# Overview

Observers are components that analyze video fields and extract metadata, quality metrics, and encoded information from the video signal. They populate the observation context with their findings, which can then be used by other pipeline stages or displayed to the user.

Each observer specializes in extracting specific types of information from the video signal, ranging from VBI (Vertical Blanking Interval) data to quality measurements.

## Biphase Observer

**Purpose:** Extracts biphase-encoded (Manchester-encoded) VBI data from LaserDisc video signals.

**What it extracts:**
- CAV (Constant Angular Velocity) picture numbers
- Chapter numbers
- CLV (Constant Linear Velocity) timecodes (hours, minutes, seconds, picture)
- Lead-in and lead-out codes
- Picture stop codes
- User codes
- Programme status information (CX audio, disc size, side number, teletext, digital video, sound mode, FM multiplex, programme dump, parity)
- Amendment 2 status (copy permissions, video standard, sound mode)

**How it works:**
The observer reads VBI data from lines 16, 17, and 18 of the video field. It decodes the Manchester-encoded biphase data and interprets it according to the IEC 60856/60857 LaserDisc standards. The decoded data is validated using parity checks and BCD (Binary Coded Decimal) decoding.

**What it looks like:**
The extracted VBI data appears as metadata fields in the observation context, providing frame-accurate information about disc position, chapter markers, and disc configuration.

---

## Black PSNR Observer

**Purpose:** Measures the Peak Signal-to-Noise Ratio from VITS (Vertical Interval Test Signals) black level reference signals.

**What it measures:**
- Black level PSNR in decibels (dB)

**How it works:**
The observer analyzes specific regions of the VBI that contain black level test signals. It extracts sample data from these regions and calculates the PSNR by measuring the noise variance relative to the expected black level. The measurement helps assess the quality of the black level region of the video signal.

**What it looks like:**
A single numerical value in dB representing the signal quality in the black regions. Higher values indicate cleaner signals with less noise.

---

## White SNR Observer

**Purpose:** Measures Signal-to-Noise Ratio from VITS white flag test signals.

**What it measures:**
- White flag SNR in decibels (dB)

**How it works:**
The observer analyzes white flag test signals in the VBI to measure noise in the white level region of the video signal. It extracts samples from specific regions, uses the mean of the data as the signal reference, and calculates the SNR by comparing signal strength to noise variance. This measurement complements the Black PSNR measurement to provide a complete picture of signal quality across the luminance range.

**What it looks like:**
A numerical value in dB representing the signal-to-noise ratio in white regions. Higher values indicate cleaner signals with less noise at white levels.

---

## Burst Level Observer

**Purpose:** Analyzes the color burst signal amplitude.

**What it measures:**
- Median color burst peak amplitude in 10-bit sample units (CVBS_U10_4FSC domain)

**How it works:**
The observer examines the color burst signal (the reference signal for color decoding located after the horizontal sync pulse) on three sample lines in each field of the frame. For each line it computes the RMS of the burst window and converts it to a peak amplitude (RMS × √2 for a sinusoidal burst), rejecting outliers, then records the median across the sampled lines. The result is an AC amplitude — the peak excursion of the burst from its mean, not an absolute signal level. The burst level is important for color reproduction quality and can indicate signal degradation or processing artifacts.

**What it looks like:**
A numerical value stored in 10-bit sample units. The GUI (for example the Video Parameter Observer dialogue and the Burst Level Analysis chart) converts it for display into the project's amplitude unit — 10-bit samples, IRE, or mV. A standard-amplitude burst corresponds to a peak excursion of roughly 20 IRE: the burst swings ±20 IRE around blanking (40 IRE peak-to-peak for NTSC; 300 mV peak-to-peak for PAL).

---

## Closed Caption Observer

**Purpose:** Extracts EIA-608 closed caption data from the VBI.

**What it extracts:**
- Whether the field's line 21 carries line 21 data (the clock run-in and the start bits)
- First EIA-608 data byte (its 7 data bits)
- Second EIA-608 data byte (its 7 data bits)
- Whether each byte's odd parity over all eight bits held
- Where line 21 carried nothing: whether line 22 or line 20 did, and the same four things read there

**How it works:**
The observer reads line 21 of both fields of NTSC and PAL-M video: the first field's carries CC1, CC2, TEXT1 and TEXT2, the second field's CC3, CC4, TEXT3, TEXT4 and the Extended Data Service. It reads a line only when it finds the clock run-in — seven cycles of a sine at the bit rate, anywhere from 7 to 12 µs after the line's start, which must account for most of the line's variance where it lies; a weak one is read only when every data bit stands clear of the slicing level — takes the bit timing from the run-in's phase and the slicing level from its midpoint, and finds the two 0 start bits and the 1 start bit where the run-in puts them. It then reads the 16 NRZ bits, 7 data bits and an odd parity bit for each of the two bytes — the last from the early part of its cell, since the last bit ends where captures cut the line short — and checks each byte's parity over all eight bits. A line carrying noise, picture or another service's data is recorded as carrying nothing. Where line 21 carries nothing it reads line 22, then line 20, the same way, and records what it found there apart from line 21's reading: a recording whose time base slipped a line carries its captions next to line 21 — throughout, or on lines 20, 21 and 22 by turns. Whether such a line carries captions is a question about the whole recording rather than one field, so the Closed Caption Sink answers it (see its documentation); the Closed Captions dialog reads line 21 only.

**What it looks like:**
Two bytes of caption data per field, which can be decoded into text characters and control codes by caption decoders. 625-line PAL video has no line 21 service and records nothing.

The Closed Captions dialog (preview window, Observers menu) is where those bytes become readable. It is offered on NTSC projects only, and is greyed out on any other standard. EIA-608 carries two bytes per frame and builds a caption out of them over a second or more, so what is on screen at any one frame is the result of a run of frames before it, not of that frame's own data; the dialog therefore decodes a trailing window of frames ending at the current frame.

What it shows is a transcript of the captions decoded so far, each with the frame it appeared at, so a caption can be found again in the recording. The transcript follows the previewer: the caption that would be on screen at the preview frame is shown in bold and scrolled to as that frame moves, which is what tells you the captions are keeping step with the picture. It keeps accumulating as you step or play forward; jumping to an unrelated position starts it again from the frames preceding the new position.

A **Service** selector at the top of the dialog chooses which of Line 21's four services to decode. The two bytes a field carries are shared by **CC1** and **CC2** (captions) and **TEXT1** and **TEXT2** (pages of text such as schedules or scores), and which one a byte pair belongs to is decided by the control codes before it — so they can only be read one at a time. CC1 is what a captioned programme uses and is the default; the others are worth a look when a recording turns out to carry more than one. Changing the selection starts the decode again from the frames preceding the current position, because nothing decoded under the old selection describes the new one.

The status bar reports the caption mode the service is using — **Pop-on** (captions prepared off screen and shown complete), **Roll-up** (lines scrolling up as they arrive), **Paint-on** (text appearing character by character) or **Text service** (a page of text that scrolls) — and the byte pair recovered from the current frame, with the odd parity bit restored so it reads as it would in an SCC file. A byte whose parity check failed is marked with a question mark. The preview feeds its caption decoder the pairs the Closed Caption Sink takes: a pair whose first byte's parity failed is not fed at all, since it may have been a control code; one whose second byte failed is fed its first byte alone when that is a character; a byte with no function on its field is ignored alone; and a pair that is no code EIA-608 defines for its field is not fed. The music note and the other special and extended characters are shown as what they are. A recording whose first stored field is television field 2 is recognised from its control codes as the frames arrive and decoded the other way round, and the readout then numbers the fields as television fields. The sink's one further rule — that a pair needs Line 21 data around it — looks at frames after the one shown and is not applied, so a lone line of noise that reads as a defined code can still reach the preview. Most frames of a captioned recording carry no caption data at all — the caption data line is only written to while there is something to say — and the readout says so rather than leaving a gap.

The Closed Caption Sink stage exports the same data, one service at a time, as a Scenarist SCC, SubRip, plain text or HTML file.

---

## Colour Frame Phase Observer

**Purpose:** Determines each field's position within the repeating colour subcarrier sequence.

**What it measures:**
- Per-field colour-sequence phase ID (PAL/PAL-M: 1–8, NTSC: 1–4)
- Colour-frame sequence index derived from the phase ID (PAL/PAL-M: 1–4, NTSC: 0–1)

**How it works:**
The observer measures the colour burst phase of both fields in a frame and classifies each field's position within the colour subcarrier sequence: an 8-field sequence for PAL and PAL-M (EBU Tech. 3280-E, ITU-R BT.1700-1) or a 4-field sequence for NTSC (SMPTE 244M). Each field's phase ID is then reduced to a frame-level colour-frame index. A value of -1 is recorded when the burst is absent or too weak to classify (for example blank tape or a pre-programme leader).

**What it looks like:**
Two integer values per field in the observation context. The Video Parameter Observer dialogue shows the colour frame index for the currently previewed field or frame. Colour-frame information matters wherever colour sequence continuity does — for example validating source alignment or diagnosing chroma decoding issues.

---

## Field Quality Observer

**Purpose:** Calculates overall quality metrics for each video field.

**What it measures:**
- Field quality score (0.0-1.0)
- Dropout count
- Phase correctness indicator

**How it works:**
The observer combines multiple quality indicators to produce an overall quality score:
- **Dropout density**: Counts the number of dropouts detected in the field
- **Phase correctness**: Validates that the field's phase information is correct
- **Signal metrics**: Incorporates SNR and other signal quality measurements when available

The quality score is used by the disc mapping policy to select the best duplicate when multiple fields have the same VBI frame number (common when stacking multiple captures).

**What it looks like:**
A normalized quality score from 0.0 (worst) to 1.0 (best), along with individual metrics. Higher scores indicate better-quality fields with fewer dropouts and better signal characteristics.

---

## FM Code Observer

**Purpose:** Extracts FM code from NTSC LaserDisc signals.

**What it extracts:**
- FM code presence indicator
- 20-bit FM code data payload
- Field indicator flag

**How it works:**
The observer reads line 10 of NTSC video fields, where FM codes are encoded. It decodes the frequency-modulated signal to extract a 20-bit data value and a field flag bit. FM codes can carry various types of metadata specific to certain LaserDisc formats.

**What it looks like:**
A 20-bit data value and a boolean field flag when present. Not all LaserDiscs contain FM codes, so the presence indicator shows whether valid FM code data was decoded.

---

## White Flag Observer

**Purpose:** Detects the presence of white flag signals on NTSC LaserDisc video.

**What it detects:**
- White flag presence indicator

**How it works:**
The observer examines line 11 of NTSC video fields for the white flag signal. The white flag is a reference signal that can be used for various test and measurement purposes. It indicates the presence of this test signal for quality analysis.

**What it looks like:**
A simple boolean indicator showing whether a white flag was detected on the current field.
