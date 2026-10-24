/*
 * File:        naplps_reconstruct.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Rebuild the stretches of a NAPLPS record a recording lost, from
 *              other records and the service's own habits
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NAPLPS_RECONSTRUCT_H
#define ORC_NAPLPS_RECONSTRUCT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nabts_record_library.h"
#include "naplps_dialect_model.h"
#include "vbi-services/nabts_page.h"

namespace orc {

/// A shift is inferred after a lost stretch where the bytes after it read at
/// least this much more likely the other way, in average log-likelihood per
/// byte — about three times as likely per byte.
constexpr double kNaplpsShiftMargin = 1.1;

/// A shift needs at least this many bytes after the lost stretch to weigh.
constexpr size_t kNaplpsShiftEvidenceBytes = 6;

/// What one byte of a reconstructed record is.
enum class NaplpsByteOrigin : uint8_t {
  /// Arrived, and is read as it arrived.
  kRecorded,
  /// Never arrived; copied from another record whose bytes on both sides of
  /// it agree with this one's (NabtsRecordLibrary).
  kFilled,
  /// Never arrived; a shift the reading of the bytes after it calls for.
  kInferred,
  /// Never arrived, and nothing stands in for it.
  kLost,
  /// Arrived failing the odd parity CEA-516 §3.3 gives every data byte of a
  /// teletext record — so known to be wrong — and replaced by the byte another
  /// record has there, agreeing with this one on both sides of it
  /// (NabtsRecordLibrary), the same test a fill is held to.
  kCorrected,
};

/// What a reconstruction may do. Everything on is the records viewer's
/// "Reconstruct lost data"; the holdout that chose the defaults turns the
/// layers on one at a time.
struct NaplpsReconstructionOptions {
  /// Fill lost stretches from the library.
  bool fill = true;
  /// Infer a shift the bytes after a lost stretch call for.
  bool read_holes = true;
  /// Replace bytes that arrived failing their parity with what the library
  /// can vouch for in their place.
  bool correct = true;
  NabtsLibraryPolicy policy;
  double shift_margin = kNaplpsShiftMargin;
};

/// What a reconstruction did to one record.
struct NaplpsReconstructionSummary {
  /// Lost stretches, and the bytes in them, before anything was done.
  uint32_t holes = 0;
  uint32_t bytes_lost = 0;
  uint32_t holes_filled = 0;
  uint32_t bytes_filled = 0;
  uint32_t bytes_inferred = 0;
  /// Bytes that arrived failing their parity, and of those the ones replaced
  /// from the library.
  uint32_t bytes_corrupt = 0;
  uint32_t bytes_corrected = 0;
  /// Where the fills and corrections came from: the library records'
  /// sources, each once.
  std::vector<std::string> sources;

  bool changed() const {
    return bytes_filled > 0 || bytes_inferred > 0 || bytes_corrected > 0;
  }
};

struct NaplpsReconstruction {
  std::vector<uint8_t> data;
  /// One entry per byte of @ref data.
  std::vector<NaplpsByteOrigin> origin;
  NaplpsReconstructionSummary summary;
};

/**
 * @brief Rebuild what a recording lost of one record
 *
 * A byte that never arrived stands in a record as the 0x00 of a lost packet's
 * filler, which X3.110 §6.1.6.1 makes a control with no presentation effect:
 * the receiver reads straight on past it, so the bytes after the loss are
 * read as the rest of whatever the loss cut, and a colour, a DOMAIN or a
 * shift the loss took is simply never there. This reads the record as a
 * receiver with the lost bytes back would have, as far as they can be known:
 *
 *   1. Fill (options.fill). Each lost stretch the library can vouch for
 *      (NabtsRecordLibrary::fill) is copied in.
 *   2. Infer a shift (options.read_holes). Where the bytes after a stretch
 *      nothing could fill read far more like text than PDI data (or the other
 *      way) than the record was being read before it, the loss took a shift,
 *      and one is put back at its end.
 *
 * Before either, and whether or not the record lost anything, a byte that
 * arrived failing the odd parity CEA-516 §3.3 gives every data byte of a
 * teletext record is known to be wrong — the vote had no copy with a clean
 * value for it — and where the library vouches for what stood there
 * (options.correct), with the recording's other known-wrong bytes kept out of
 * the context it is matched on, it is replaced: the same test a fill passes,
 * applied to one byte the recording got wrong rather than a stretch it lost.
 *
 * Nothing here changes a byte that arrived and passed its parity, and every
 * byte's origin is reported, so a page drawn from the result can say what it
 * is drawn from.
 *
 * @param present  Parallel to @p data; empty when every byte arrived.
 * @param library  May be null, which skips filling.
 * @param self     Index of this record in @p library, whose own bytes are
 *                 never offered back to it; library->records().size() or more
 *                 excludes nothing.
 * @param model    May be null, which skips shift inference.
 *
 * Linear in the record's length for each shift put back (one reading of the
 * record each, and one to start), plus the library's fill cost.
 */
NaplpsReconstruction naplps_reconstruct_record(
    const std::vector<uint8_t>& data, const std::vector<uint8_t>& present,
    const NabtsRecordLibrary* library, size_t self,
    const NaplpsDialectModel* model,
    const NaplpsReconstructionOptions& options = {});

/// Put what a reconstruction did into a page's diagnostics, which is where a
/// page drawn from rebuilt bytes says so (NabtsDecodeDiagnostics).
void naplps_stamp_reconstruction_diagnostics(
    const NaplpsReconstructionSummary& summary,
    NabtsDecodeDiagnostics& diagnostics);

}  // namespace orc

#endif  // ORC_NAPLPS_RECONSTRUCT_H
