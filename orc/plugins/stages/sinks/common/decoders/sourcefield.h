/*
 * File:        sourcefield.h
 * Module:      orc-core
 * Purpose:     Non-owning field view for CVBS_U10_4FSC decoder input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef SOURCEFIELD_H
#define SOURCEFIELD_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// A non-owning view of one interlaced field within a CVBS_U10_4FSC frame
// buffer.  All sample data remains owned by the VideoFrameRepresentation; the
// VFrameR must stay alive for the lifetime of any SourceField that references
// it.
//
// For composite sources, `data` points to the first sample of field 1 (i.e.
// the start of the frame buffer), or to the first sample of field 2 (offset by
// the total sample count of field 1).
//
// For YC sources, `luma_data` and `chroma_data` point into the respective
// luma and chroma frame buffers at the same field offsets.
//
// PAL non-orthogonal lines:
//   PAL field 1 has 313 lines and PAL field 2 has 312 lines.  Two lines per
//   field carry 1136 samples instead of 1135 (EBU Tech. 3280-E §1.3.1).
//   When `line_ptrs` is non-empty it contains one pointer per line pointing
//   directly into the frame buffer.  Decoders must use `line_ptrs[line]`
//   rather than `data + line * samples_per_line` when `line_ptrs` is
//   populated.  For orthogonal systems (NTSC, PAL_M) `line_ptrs` is empty
//   and `data + line * samples_per_line` is the correct access pattern.
struct SourceField {
  // 1-based sequence number within the source (frame-based, not field-based).
  int32_t seq_no = 0;

  // True for CVBS field 1 (the 313-line field in PAL; 263-line field in NTSC).
  bool is_first_field = true;

  // This field's position within the colour-field sequence, measured from the
  // burst: NTSC 1–4, PAL / PAL_M 1–8 (ld-decode fieldPhaseID convention).
  // Per field, not per frame: the NTSC comb derives each line's burst phase
  // from it.  std::nullopt when unknown.
  std::optional<int32_t> field_phase_id;

  // -------------------------------------------------------------------------
  // Composite path (is_yc == false)
  // -------------------------------------------------------------------------
  // Pointer to the first sample of this field in the frame buffer.
  // int16_t in the CVBS_U10_4FSC 10-bit domain.  nullptr if not populated.
  const int16_t* data = nullptr;

  // -------------------------------------------------------------------------
  // YC path (is_yc == true)
  // -------------------------------------------------------------------------
  const int16_t* luma_data = nullptr;    // Pointer into luma frame buffer.
  const int16_t* chroma_data = nullptr;  // Pointer into chroma frame buffer.

  // True when this field came from a YC source (separate Y and C files).
  bool is_yc = false;

  // -------------------------------------------------------------------------
  // Geometry
  // -------------------------------------------------------------------------
  // Number of lines in this field (e.g. 313 for PAL field 1, 263 for NTSC field
  // 1).
  size_t line_count = 0;

  // Nominal samples per line.  For PAL this is 1135; for NTSC 910; for PAL_M
  // 909. PAL non-orthogonal lines carry one extra sample — use line_ptrs in
  // that case.
  size_t samples_per_line = 0;

  // Per-line pointers for PAL non-uniform lines.  Non-empty only for PAL
  // composite and YC fields.  When non-empty, line k starts at line_ptrs[k]
  // (for both the composite `data` and YC `luma_data`/`chroma_data` paths).
  // luma_line_ptrs and chroma_line_ptrs are populated for YC PAL fields.
  std::vector<const int16_t*> line_ptrs;         // Composite or luma pointers.
  std::vector<const int16_t*> luma_line_ptrs;    // YC luma pointers (PAL).
  std::vector<const int16_t*> chroma_line_ptrs;  // YC chroma pointers (PAL).

  // -------------------------------------------------------------------------
  // Helpers
  // -------------------------------------------------------------------------

  // Return the vertical offset of this field within the interlaced frame
  // (0 for field 1, 1 for field 2).  Used by decoders to reconstruct frame-
  // flat line numbers for output.
  int32_t getOffset() const { return is_first_field ? 0 : 1; }

  // Return a pointer to composite line `line` (0-based within this field).
  // Uses line_ptrs when available (PAL), otherwise uses stride arithmetic.
  const int16_t* getLine(size_t line) const {
    if (!line_ptrs.empty()) {
      return line_ptrs[line];
    }
    return data + static_cast<ptrdiff_t>(line * samples_per_line);
  }

  // Return a pointer to luma line `line` (YC path only).
  const int16_t* getLumaLine(size_t line) const {
    if (!luma_line_ptrs.empty()) {
      return luma_line_ptrs[line];
    }
    return luma_data + static_cast<ptrdiff_t>(line * samples_per_line);
  }

  // Return a pointer to chroma line `line` (YC path only).
  const int16_t* getChromaLine(size_t line) const {
    if (!chroma_line_ptrs.empty()) {
      return chroma_line_ptrs[line];
    }
    return chroma_data + static_cast<ptrdiff_t>(line * samples_per_line);
  }
};

// Split each Y/C field in `inputFields` into a luma-only and a chroma-only
// composite field, writing them to `lumaFields` and `chromaFields`.  Each
// output field aliases the input's luma_data/chroma_data (and the matching
// PAL line pointers) as its composite `data`, with is_yc cleared, so a
// composite decoder can process the two channels separately.  The outputs are
// cleared first; they reference the same buffers as the inputs and must not
// outlive them.
void split_yc_fields(const std::vector<SourceField>& inputFields,
                     std::vector<SourceField>& lumaFields,
                     std::vector<SourceField>& chromaFields);

#endif  // SOURCEFIELD_H
