# Disc Mapping Design

This document describes a robust, deterministic design for reconstructing a
frame-accurate disc map from a noisy field capture in ld-decode. The design
explicitly prioritizes VBI-derived picture numbering (CAV / CLV), uses field
phase only as a rejection and tie-breaking mechanism, and cleanly separates
field pairing from frame numbering and gap detection.

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

### Redundancy and legal ranges

IEC 60856/60857 carries the number on two VBI lines, and both disc types are
decoded the same way: if lines 17 and 18 both decode and agree the number is
cross-validated; if they both decode and disagree it is discarded entirely; if
only one decodes the value is kept but flagged as unconfirmed for stage 1b.

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

Each stage shall produce a clear description of the decision making process/pipeline - with each stage description becoming part of a report to be given to the user alongside that actual outcome (which will be a field map stage parameter (the field map list)).