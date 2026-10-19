/*
 * File:        naplps_reconstruct.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Rebuild the stretches of a NAPLPS record a recording lost, from
 *              other records and the service's own habits
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_reconstruct.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "naplps_interpreter.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

/// First byte of columns 2 to 7, where text, opcodes and operands lie; below
/// it are the C0 controls (X3.110 §4.1).
constexpr uint8_t kFirstGraphicColumnByte = 0x20;

enum class ReadState { kUnknown, kText, kPdi };

/// How the record was being read just before |offset|: from the last byte
/// before it the receiver read, transparent controls aside (X3.110
/// §6.1.4-6.1.6.1).
ReadState state_before(const std::vector<uint8_t>& data,
                       const std::vector<NaplpsByteRole>& roles,
                       size_t offset) {
  for (size_t i = std::min(offset, roles.size()); i-- > 0;) {
    switch (roles[i]) {
      case NaplpsByteRole::kUnread:
      case NaplpsByteRole::kTransparent:
        continue;
      case NaplpsByteRole::kText:
        return ReadState::kText;
      case NaplpsByteRole::kPdi:
        return ReadState::kPdi;
      case NaplpsByteRole::kControl: {
        const uint8_t byte = data[i] & 0x7F;
        if (byte == kNaplpsSo) {
          return ReadState::kPdi;
        }
        if (byte == kNaplpsSi) {
          return ReadState::kText;
        }
        return ReadState::kUnknown;
      }
      case NaplpsByteRole::kDefinition:
      case NaplpsByteRole::kOther:
        return ReadState::kUnknown;
    }
  }
  return ReadState::kUnknown;
}

}  // namespace

NaplpsReconstruction naplps_reconstruct_record(
    const std::vector<uint8_t>& data, const std::vector<uint8_t>& present,
    const NabtsRecordLibrary* library, size_t self,
    const NaplpsDialectModel* model,
    const NaplpsReconstructionOptions& options) {
  NaplpsReconstruction out;
  out.data = data;
  out.origin.assign(data.size(), NaplpsByteOrigin::kRecorded);
  std::set<std::string> sources;

  // 0. Bytes known to be wrong: they arrived, and failed the odd parity every
  //    data byte of a teletext record carries (CEA-516 §3.3). Each run of them
  //    is asked of the library as a lost stretch would be, the record's other
  //    known-wrong bytes stepped over in the context it is matched on — they
  //    vouch for nothing, and a recording's damage comes in bursts, so ending
  //    the context at them would leave a wrong byte beside another none.
  if (options.correct && library != nullptr) {
    std::vector<uint8_t> wrong(data.size(), 0);
    bool any = false;
    for (size_t i = 0; i < data.size(); ++i) {
      if (nabts_library_present(present, i) && data[i] != 0 &&
          !teletext_odd_parity_valid(data[i])) {
        wrong[i] = 1;
        ++out.summary.bytes_corrupt;
        any = true;
      }
    }
    for (size_t i = 0; any && i < data.size();) {
      if (wrong[i] == 0) {
        ++i;
        continue;
      }
      size_t j = i;
      while (j < data.size() && wrong[j] != 0) {
        ++j;
      }
      const NabtsLibraryAnswer answer =
          library->correct(data, present, wrong, i, j, self, options.policy);
      const bool clean =
          answer.fill &&
          std::all_of(answer.fill->bytes.begin(), answer.fill->bytes.end(),
                      [](uint8_t byte) {
                        return byte == 0 || teletext_odd_parity_valid(byte);
                      });
      if (clean) {
        for (size_t k = i; k < j; ++k) {
          out.data[k] = answer.fill->bytes[k - i];
          out.origin[k] = NaplpsByteOrigin::kCorrected;
        }
        out.summary.bytes_corrected += static_cast<uint32_t>(j - i);
        sources.insert(library->records()[answer.fill->record].source);
      }
      i = j;
    }
  }
  out.summary.sources.assign(sources.begin(), sources.end());
  if (present.empty()) {
    return out;  // Nothing was lost.
  }

  // The lost stretches, as the recovery left them.
  std::vector<std::pair<size_t, size_t>> holes;
  for (size_t i = 0; i < data.size();) {
    if (nabts_library_present(present, i)) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < data.size() && !nabts_library_present(present, j)) {
      out.origin[j] = NaplpsByteOrigin::kLost;
      ++j;
    }
    holes.emplace_back(i, j);
    out.summary.bytes_lost += static_cast<uint32_t>(j - i);
    i = j;
  }
  out.summary.holes = static_cast<uint32_t>(holes.size());
  if (holes.empty()) {
    return out;
  }

  // 1. Fill what the library can vouch for. Context is read from the bytes
  //    that arrived, never from another stretch's fill, so each fill stands on
  //    the recording alone.
  std::vector<bool> filled(holes.size(), false);
  if (options.fill && library != nullptr) {
    for (size_t h = 0; h < holes.size(); ++h) {
      const auto [start, end] = holes[h];
      const NabtsLibraryAnswer answer =
          library->fill(data, present, start, end, self, options.policy);
      if (!answer.fill) {
        continue;
      }
      std::copy(answer.fill->bytes.begin(), answer.fill->bytes.end(),
                out.data.begin() + static_cast<std::ptrdiff_t>(start));
      std::fill(out.origin.begin() + static_cast<std::ptrdiff_t>(start),
                out.origin.begin() + static_cast<std::ptrdiff_t>(end),
                NaplpsByteOrigin::kFilled);
      filled[h] = true;
      ++out.summary.holes_filled;
      out.summary.bytes_filled += static_cast<uint32_t>(end - start);
      sources.insert(library->records()[answer.fill->record].source);
    }
  }
  out.summary.sources.assign(sources.begin(), sources.end());

  // 2. A shift the loss took. Where the bytes after a stretch nothing could
  //    fill, up to the next control, read far more like text than PDI data
  //    (or the other way) than the record was being read before it, one is
  //    put back at its end. Everything after a shift is read by it, so the
  //    record is read again after each one put back.
  if (!options.read_holes || model == nullptr || model->records() == 0) {
    return out;
  }
  std::vector<NaplpsByteRole> roles = naplps_read_roles(out.data);
  for (size_t h = 0; h < holes.size(); ++h) {
    if (filled[h]) {
      continue;
    }
    const auto [start, end] = holes[h];
    const ReadState before = state_before(out.data, roles, start);
    if (before == ReadState::kUnknown) {
      continue;
    }
    size_t stop = end;
    while (stop < out.data.size() && stop - end < kNaplpsDialectWindowBytes &&
           (out.data[stop] & 0x7F) >= kFirstGraphicColumnByte) {
      ++stop;
    }
    if (stop - end < kNaplpsShiftEvidenceBytes) {
      continue;
    }
    const double text = model->text_score(&out.data[end], stop - end);
    const double pdi = model->pdi_score(&out.data[end], stop - end);
    const bool to_text =
        before == ReadState::kPdi && text - pdi >= options.shift_margin;
    const bool to_pdi =
        before == ReadState::kText && pdi - text >= options.shift_margin;
    if (!to_text && !to_pdi) {
      continue;
    }
    // SI invokes the primary set into GL and SO the PDI set, as every service
    // in the corpus leaves G0 and G1 (X3.110 §4.3.1), with the odd parity
    // CEA-516 §3.3 gives every data byte, so a byte put back reads as one
    // that arrived intact rather than as damage.
    out.data[end - 1] =
        teletext_odd_parity_encode(to_text ? kNaplpsSi : kNaplpsSo);
    out.origin[end - 1] = NaplpsByteOrigin::kInferred;
    ++out.summary.bytes_inferred;
    roles = naplps_read_roles(out.data);
  }
  return out;
}

void naplps_stamp_reconstruction_diagnostics(
    const NaplpsReconstructionSummary& summary,
    NabtsDecodeDiagnostics& diagnostics) {
  diagnostics.lost_bytes = summary.bytes_lost;
  diagnostics.filled_bytes = summary.bytes_filled;
  diagnostics.inferred_bytes = summary.bytes_inferred;
  diagnostics.corrupt_bytes = summary.bytes_corrupt;
  diagnostics.corrected_bytes = summary.bytes_corrected;
  diagnostics.fill_sources = summary.sources;
}

}  // namespace orc
