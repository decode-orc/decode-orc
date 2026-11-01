# Disc Mapping Design

This document describes a robust, deterministic design for reconstructing a
frame-accurate disc map from a noisy field capture in ld-decode. The design
explicitly prioritizes VBI-derived picture numbering (CAV / CLV), uses field
phase only as a rejection and tie-breaking mechanism, and cleanly separates
field pairing from frame numbering and gap detection.

It is implemented as the **Disc Mapper** tool of the Frame Map stage (see
[Transform stages](../gui-user-guide/stages/transform-stages.md)), whose
result is written back as the stage's `ranges` parameter. The design is
written in terms of fields; the implementation reads decode-orc's frame
stream, so the pairing of section 6 has already happened, and a frame whose
two fields carry different picture numbers (a player skip in mid-frame) is
dropped and the pictures it straddled padded.

---

## 1. Problem Summary

Input is a sequential capture of *fields* that may be:
- Missing, duplicated, reordered, or corrupted
- Carrying inconsistent or missing VBI data
- Affected by noise, dropouts, or capture glitches

Goal:
- Reconstruct an ordered sequence of *frames*
- Assign each frame a picture number (if possible)
- Detect and pad missing frames
- Choose the best instance among duplicates
- Preserve correct absolute positioning on the disc timeline

---

## 2. Definitions

### Field
A captured video field with:
- `FieldID` (capture order; not guaranteed contiguous)
- Parity (first / second field; may be unreliable)
- Phase hint (NTSC 1–4, PAL 1–8)
- Per-field VBI data (CAV PN, CLV timecode, chapter/program)
- Quality metrics

### Frame
A logical pairing of two related fields representing one picture on disc.

### Picture Number (PN)
A monotonically increasing integer index:
- CAV: directly from VBI picture number
- CLV: derived from timecode using format FPS
- `PN == 0` is invalid (lead-in/out)

---

## 3. Source-of-Truth Priority

1. **CAV picture number**
2. **CLV timecode → picture number**
3. Chapter / program number (weak hint only)
4. FieldID order, cadence, phase (supporting evidence only)

Phase and cadence must **never override valid VBI numbering**.

---

## 4. High-Level Pipeline

Fields
↓
Per-field VBI normalization
↓
Candidate field pairing
↓
Candidate frame construction
↓
VBI-driven frame selection & deduplication
↓
Gap detection & PAD insertion
↓
Final disc frame map


---

## 5. Per-Field Metadata Normalization

For each field:

1. Attempt to decode:
   - CAV picture number
   - CLV timecode → picture number
2. Validate:
   - PN ≠ 0
   - Every field of the code inside the range the standard gives it
   - The two VBI copies of the number agree
3. Assign confidence score and record whether the number was cross-validated
4. Mark lead-in/out or invalid data

All picture numbers are normalized to a single integer PN space.

### Gap plausibility

A gap between consecutive picture numbers is padded only when the capture can
account for it. The frames the capture holds between the two mapped frames are
pictures the mapping could not place, so each accounts for one missing picture;
beyond those, a fixed allowance covers the player mistracking forward over
pictures the capture never recorded.

    max paddable gap = frames captured across the gap + 100

A misplaced picture number fails this on both counts: the gap is wide and the
source distance is small, and frequently negative, because frames are ordered
by picture number at this point and the frame's true position lies elsewhere in
the capture.

A wider gap is still padded when the capture plays on from it: the 10 frames
after it move forward through the disc together, each later in the capture than
the last by enough frames to account for the pictures between. A mistracking
player can skip further than the allowance in a few frames, and go on skipping
before it settles (Dragon's Lair DS5 jumped 128 pictures in 5 frames, then about
64 at a time), and leaving that gap unpadded would put every later frame of the
capture out of step with the other captures of a stack. A misplaced number is
one frame on its own: the pictures after it in number order lie earlier in the
capture.

### Redundancy and legal ranges

IEC 60856/60857 carries the number on two VBI lines, and both disc types are
decoded the same way: if lines 17 and 18 both decode and agree the number is
cross-validated; if they both decode and disagree it is discarded and the
picture padded; if only one decodes the value is kept but flagged as
unconfirmed for stage 1b.

On a CAV disc one kind of disagreement is settled rather than padded. When the
nearest numbered frames before and after are two pictures apart, only the
picture between them can sit there, and a copy that names it is kept (with
reduced confidence; the report counts these as `correctedVBIErrors`). This
matters when the flipped bit is on the disc rather than in one capture: every
capture of Dragon's Lair side 1 reads line 17 of picture 18989 as 58989, so no
other source could supply the picture, and padding it lost it from the stack,
with the pulldown frame placed beside it. The rule needs no frame count, so
pulldown frames between the neighbours do not upset it. With room for more
than one picture, neither copy is trusted and the frame is padded.

| | Carried twice | Carried once |
|---|---|---|
| CAV | Picture number (§10.1.3, lines 17 and 18) | — |
| CLV | Hours and minutes (§10.1.6, lines 17 and 18) | Seconds and picture within the second (§10.1.10, line 16) |

No CLV reading is therefore fully protected. What cross-validation rules out
is the failure that displaces a frame furthest: the hours digit falls outside
the signature the decoder matches the line on, so a flipped bit there decodes
as a clean BCD digit and moves the picture by an hour of running time.

The legal maxima come from the field widths, not from the capture:

| | PAL (IEC 60856) | NTSC (IEC 60857) |
|---|---|---|
| CAV picture number (§10.1.3) | 99999 | 79999 |
| CLV picture number | 899999 (9:59:59.24) | 1079999 (9:59:59.29) |

The CLV ceiling is the largest running time §10.1.6 can express — hours is a
single BCD digit — plus the last picture of that second. §10.1.10 writes the
picture within the second as X4 = 0..2, X5 = 0..9 in both standards, but that
is the field's width rather than a frame count, so the bound applied is the
format's own frame rate.

---

## 6. Field Pairing (Candidate Frame Generation)

### 6.1 Pairing Strategy

- Fields are paired based on capture order (`FieldID`)
- Only pair within a small sliding window
- Prefer first → second parity but allow uncertainty

### 6.2 Frame Construction Rules

For each candidate pair:

- Select PN using priority:
  1. First field CAV PN
  2. Second field CAV PN
  3. First field CLV PN
  4. Second field CLV PN
- If both fields provide PN of same type:
  - Values must agree
  - Otherwise keep first-field value unless invalid
- Assign PN confidence
- Combine quality metrics
- Check phase consistency

### 6.3 Phase Handling

- Phase is used **only to veto obviously bad frames**
- Phase must not override trusted VBI PN
- Phase disagreement is recorded for later tie-breaking

Result: a set of candidate frames, some with PN, some without.

---

## 7. Frame Validation and Filtering

- Drop frames with PN == 0
- Retain frames with no PN for continuity
- Retain phase-invalid frames only if PN confidence is high
- Mark PN-disagreement and phase issues for later scoring

---

## 8. Deduplication and Frame Selection

### 8.1 Group by Picture Number

Frames with PN are grouped by PN value.

### 8.2 Duplicate Resolution

For each PN group, select the best frame using:

1. Highest PN confidence
2. Phase-valid preferred
3. Highest quality
4. Capture-order coherence with neighbors

All other frames for that PN are discarded.

---

## 9. Disc Timeline Construction

### 9.1 Ordered PN Sequence

- Sort selected frames by PN
- Enforce monotonic PN order
- Non-monotonic PN implies duplication or error

### 9.2 Gap Detection

For consecutive frames:

- If PN increments by 1 → OK
- If PN jumps forward:
  - Missing frames = ΔPN − 1
  - Insert PAD frames for missing PN values

Frames without PN:
- Do not block gap insertion
- Do not define numbering

### 9.3 Film with 3:2 pulldown

525-line discs (NTSC and PAL-M) carry film with 3:2 pulldown, and the disc
numbers film frames, not video frames. Four frames in every five carry a
number, on the field where their film frame starts (field 1 or field 2), and
the fifth carries none because the disc never gave it one. Its VBI is intact:
line 16 keeps its status code, or lines 17 and 18 a chapter number, which IEC
60857 10.1.5 places in the fields without a picture number. Dropping such a
frame would take one frame in five out of the film and the sound with it, so
the mapper keeps it (`pulldownFrames`), placed by its neighbours rather than
by a number it would have to invent:

- **CAV**: a frame with no number between source frames numbered *n* and
  *n* + 1 is the pulldown frame after picture *n*. A frame whose number exists
  but was not read sits between *n* and *n* + 2 instead (it is picture
  *n* + 1), so it is still dropped and padded, and a frame straddling a player
  skip never sits between consecutive pictures.
- **CLV**: the programme time counts video frames and skips the frame that
  starts no picture, so the pulldown frame sits between *n* and *n* + 2 and is
  frame *n* + 1. An unread number looks the same by the numbers alone, so the
  frame must also have read cleanly: every VBI line decoded, and one of them
  carrying a code.
- **The capture's first and last frames**, and frames beside its lead-in or
  lead-out, have one neighbour only. A frame that read cleanly is kept where
  that neighbour puts it when another pulldown frame lies within two cycles.

Duplicates of a pulldown frame (a replayed stretch) are resolved like
duplicates of a picture. Where a capture lacks a pulldown frame that the
cadence places (its frame was lost to a dropout, or it fell in a gap), the
place is padded (`pulldownFramesPadded`, also counted in `paddingFrames`), so
that every capture of the disc holds the same frames in the same places, as
stacking needs. The cadence is taken to run across a stretch when the kept
pulldown frames on both sides, a cycle away at most, fall on the same cycle;
across an edit the cycle changes and nothing is padded. Kept pulldown frames
further away vouch for it too, when they fall on the same cycle and nothing
captured between contradicts the cadence (two consecutive pictures in
consecutive frames where it puts a pulldown frame after the first). A player
that skips again and again lands too briefly to keep pulldown frames near the
gaps: Dragon's Lair DS5 crossed 980 pictures that way, and without this was 216
frames short of the other captures. A pulldown frame next
to a player skip has the skip as a neighbour, so it is dropped and its place
padded. Source Alignment then places captures in frames, by the frames at
which they read the same pictures, since a capture starting *k* pictures late
starts about 5*k*/4 frames late.

---

## 10. PAD Frame Semantics

PAD frames represent missing frames on disc:
- Carry a PN
- Contain no image data
- Preserve absolute disc positioning
- Enable correct downstream indexing

---

## 11. Handling Regions with No VBI

When no trusted PN exists for a region:

- Do not invent PN
- Optionally estimate missing frames using:
  - FieldID gaps
  - Expected cadence (2 fields/frame)
  - Phase continuity
- Resume PN-driven mapping when VBI reappears

---

## 12. Consistency Checks

### CAV
- PN must strictly increment by 1
- Any repeat is a duplicate
- Any jump implies missing frames

### CLV
- PN must be monotonic
- Large absolute PN values are expected
- Conversion must be consistent with disc format
- PN increments by 1 per frame exactly as CAV does, so the same
  bracket-and-deviation plausibility check applies to an unconfirmed reading

---

## 13. Algorithmic Components

| Stage                    | Technique                           |
|--------------------------|-------------------------------------|
| Field pairing            | Sliding-window matching / min-cost  |
| Frame deduplication      | Deterministic scoring               |
| Gap detection            | PN monotone scan                    |
| Optional robustness      | Viterbi / HMM over PN               |

---

## 14. Design Principles

- VBI numbering defines ground truth
- Phase and cadence are supporting evidence only
- Field pairing is local; numbering is global
- Missing data is explicit (PAD), not implicit
- Prefer determinism over heuristics where possible

---

## 15. Outcome

This design produces:
- A stable, frame-accurate disc map
- Correct absolute picture numbering
- Explicit representation of missing content
- Robust behavior under severe capture corruption

---

## 16. Implementation

Each step produces a description of its decisions, and those descriptions
form the report shown to the user alongside the outcome: a corrected `ranges`
specification for the Frame Map stage. Where a picture was captured more than
once (section 8.2), "highest quality" is scored from colour-burst level,
white-flag SNR, black-level PSNR and the share of the frame flagged as dropout,
and the report itemises each duplicate group with the readings behind the
choice.
---

## 17. Disc Identity

Beside the map, the analysis publishes what the disc says of itself, read over
every field of the side, as statistics a script can read from
`orc-cli tools run --json` or `tools refresh --json`. The values are the
disc-level vote of the disc metadata document the Video Sink embeds
([disc-metadata-format.md](disc-metadata-format.md), `disc:`), built by the
same code (`orc/plugins/stages/common/disc-metadata`, linked by the Video Sink
and orc-core), so the two cannot disagree. A value never recovered is left
out rather than guessed.

| Statistic | Document field | Meaning |
|---|---|---|
| `userCode` | `user_code` | The user code, as four hex digits |
| `discSide` | `side` | 1 or 2 |
| `discSizeInches` | `size_inches` | 12 or 8 |
| `cx` | `cx_enabled` | CX noise reduction |
| `teletext`, `digitalVideo`, `fmMultiplex`, `programmeDump` | `teletext`, `digital_video`, `fm_multiplex`, `programme_dump` | The programme status flags |
| `soundMode` | `sound_mode` | As the document spells it (`stereo`, `mono`, ...) |
| `amendment2` | `amendment2.present` | Whether any field carried the Amendment 2 status word |
| `amendment2CopyPermitted`, `amendment2VideoStandard`, `amendment2SoundMode` | `amendment2.copy_permitted`, `.video_standard`, `.sound_mode` | Its fields |
| `statusAgreement` | `confidence.agreement` | Fraction of status fields whose whole word agrees with the vote, to 4 places |
| `leadInSeen`, `leadOutSeen` | `events.lead_in`, `events.lead_out` | Whether the capture holds any lead-in or lead-out frame |
| `firstPicture`, `lastPicture` | — | The lowest and highest picture number the map placed (CAV only) |

`firstPicture` and `lastPicture` come from the map, not from the document's
address runs: over a whole side those include the odd number a frame misread,
which the map discards (section 5). A capture that stops short of the end of
the side shows it as a `lastPicture` below the disc's last picture, usually
with `leadOutSeen` false.
