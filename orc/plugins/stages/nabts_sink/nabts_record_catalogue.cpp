/*
 * File:        nabts_record_catalogue.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Bounded NABTS record catalogue implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_record_catalogue.h"

#include <orc/support/logging.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include "nabts_caption_stream.h"
#include "nabts_data_group.h"
#include "nabts_navigation.h"
#include "nabts_packet.h"
#include "nabts_record_library.h"
#include "naplps_dialect_model.h"
#include "naplps_reconstruct.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

// The first bytes of a page, where it clears the screen if it does: services
// open a page with a few set-up codes and RESET within its first dozen bytes
// (SO CAN NSR, a WAIT and RESET, say). A page none of whose copies delivered
// them may have lost its clear, so it is not taken for one drawn over the page
// before it.
constexpr std::size_t kNabtsContextStartBytes = 16;

/// Function descriptor arguments as text: printable code-table characters as
/// themselves, everything else as its hexadecimal value.
std::string arguments_text(const std::vector<uint8_t>& arguments) {
  std::string out;
  out.reserve(arguments.size());
  for (const uint8_t argument : arguments) {
    // §7.2.2 confines arguments to 2/0 through 7/15, which is exactly the
    // printable range of the code table, so anything outside it is damage.
    if (argument >= 0x20 && argument < 0x7F) {
      out.push_back(static_cast<char>(argument));
    } else {
      out += fmt::format("<{:02X}>", argument);
    }
  }
  return out;
}

/// §7.2.2's descriptors as the catalogue reports them.
std::vector<NabtsRecordFunction> render_functions(
    const std::vector<NabtsFunctionDescriptor>& functions) {
  std::vector<NabtsRecordFunction> out;
  out.reserve(functions.size());
  for (const NabtsFunctionDescriptor& function : functions) {
    NabtsRecordFunction rendered;
    rendered.code = function.code_text();
    rendered.control = function.is_control();
    rendered.arguments = arguments_text(function.arguments);
    out.push_back(std::move(rendered));
  }
  return out;
}

/// The length most copies agree on; the longest of them where none has more
/// support than another, since a copy is far likelier to have been cut short
/// than to have grown.
std::size_t voted_length(const std::vector<NabtsRecordCopy>& copies,
                         const uint8_t* included) {
  std::size_t best = 0;
  std::size_t best_support = 0;
  for (std::size_t index = 0; index < copies.size(); ++index) {
    if (included != nullptr && included[index] == 0) {
      continue;
    }
    const NabtsRecordCopy& candidate = copies[index];
    std::size_t support = 0;
    for (std::size_t other_index = 0; other_index < copies.size();
         ++other_index) {
      if (included != nullptr && included[other_index] == 0) {
        continue;
      }
      const NabtsRecordCopy& other = copies[other_index];
      support += (other.data.size() == candidate.data.size()) ? 1 : 0;
    }
    if (support > best_support ||
        (support == best_support && candidate.data.size() > best)) {
      best = candidate.data.size();
      best_support = support;
    }
  }
  return best;
}

/**
 * @brief Take what a copy's header extension makes its More Record (§7.3.4)
 *
 * §5.2.8.2 meaning 001 is "redefinition of More record Address", and §5.2.8.4
 * gives its data a sense by its size (see nabts_extension_target()): a record
 * in the same data channel or a named one, or a macro. All of them are kept,
 * including a record in another data channel, which the catalogue does not
 * present as continuing into — but whose naming still means the More Flag's
 * algorithmic address is not the More Record.
 */
void take_more_designation(NabtsCataloguedRecord& record,
                           const NabtsMessage& message) {
  const NabtsExtensionTarget target =
      nabts_more_target(message.extensions, message.channel);
  record.more_redefined = target.kind != NabtsExtensionTarget::Kind::kNothing;
  record.has_more_address = target.kind == NabtsExtensionTarget::Kind::kRecord;
  record.more_channel = record.has_more_address ? target.channel : 0;
  record.more_address = record.has_more_address ? target.address : 0;
  record.more_macro = target.kind == NabtsExtensionTarget::Kind::kMacro
                          ? target.macro
                          : uint8_t{0};
}

// §5.2.8.2: header extension meanings 010 and 011, the redefinition of the
// Next and the Index Record addresses.
constexpr uint8_t kExtensionMeaningNext = 2;
constexpr uint8_t kExtensionMeaningIndex = 3;

/**
 * @brief Take the records a copy's header extension names for NEXT and INDEX
 *
 * A service can name them on most of its pages — a story's NEXT the next
 * story, a weather page's INDEX the weather index — and both of the period's
 * receivers fetched them in place of their own defaults
 * (see NabtsCataloguedRecord::has_next_address). The first field of each
 * meaning that names a record in the page's own data channel is kept; a macro
 * or a record in another channel is a key the catalogue cannot follow, as for
 * More.
 */
void take_link_designations(NabtsCataloguedRecord& record,
                            const NabtsMessage& message) {
  record.has_next_address = false;
  record.next_address = 0;
  record.has_index_address = false;
  record.index_address = 0;
  for (const NabtsHeaderExtension& extension : message.extensions) {
    const bool next = extension.meaning == kExtensionMeaningNext;
    const bool index = extension.meaning == kExtensionMeaningIndex;
    if (!next && !index) {
      continue;
    }
    const NabtsExtensionTarget target =
        nabts_extension_target(extension, message.channel);
    if (target.kind != NabtsExtensionTarget::Kind::kRecord ||
        target.channel != message.channel) {
      continue;
    }
    if (next && !record.has_next_address) {
      record.has_next_address = true;
      record.next_address = target.address;
    } else if (index && !record.has_index_address) {
      record.has_index_address = true;
      record.index_address = target.address;
    }
  }
}

/// How much of a provisional vote one copy agrees with.
struct RecordAgreement {
  std::size_t judged = 0;
  std::size_t agreed = 0;

  /// Agreeing with too little of the vote to be a copy of the record.
  bool outlier() const {
    return judged >= kNabtsOutlierMinJudgedPositions &&
           agreed * 100 < judged * kNabtsOutlierAgreementPercent;
  }

  /// Agreeing with so nearly all of the vote that the copy's suffix check is
  /// to be believed over the other copies (kNabtsVouchAgreementPercent).
  bool vouches() const {
    return judged > 0 && agreed * 100 >= judged * kNabtsVouchAgreementPercent;
  }

  /// A larger share agreed, compared without division.
  bool better_than(const RecordAgreement& other) const {
    return agreed * std::max<std::size_t>(1, other.judged) >
           other.agreed * std::max<std::size_t>(1, judged);
  }
};

/// Whether a copy's byte |position| arrived.
bool copy_has(const NabtsRecordCopy& copy, std::size_t position) {
  return position < copy.data.size() &&
         (position >= copy.present.size() || copy.present[position] != 0);
}

/// How far |copy| agrees with |vote| when its byte |position| + |slip| is read
/// as the record's byte |position|, over the positions both have a byte for.
RecordAgreement agreement(const NabtsRecordCopy& copy,
                          const NabtsVoteResult& vote, std::ptrdiff_t slip) {
  RecordAgreement out;
  for (std::size_t position = 0; position < vote.data.size(); ++position) {
    const std::ptrdiff_t source = static_cast<std::ptrdiff_t>(position) + slip;
    if (vote.present[position] == 0 || source < 0 ||
        !copy_has(copy, static_cast<std::size_t>(source))) {
      continue;
    }
    ++out.judged;
    out.agreed +=
        copy.data[static_cast<std::size_t>(source)] == vote.data[position] ? 1
                                                                           : 0;
  }
  return out;
}

/// |copy| with its bytes moved so byte |position| + |slip| lands at |position|.
/// Bytes slid in from outside the copy are holes, which is what they are.
NabtsRecordCopy slide(const NabtsRecordCopy& copy, std::ptrdiff_t slip) {
  const std::ptrdiff_t length = std::max<std::ptrdiff_t>(
      0, static_cast<std::ptrdiff_t>(copy.data.size()) - slip);
  NabtsRecordCopy out;
  out.data.assign(static_cast<std::size_t>(length), 0);
  out.present.assign(static_cast<std::size_t>(length), 0);
  if (!copy.confidence.empty()) {
    out.confidence.assign(static_cast<std::size_t>(length), 0);
  }
  for (std::ptrdiff_t position = 0; position < length; ++position) {
    const std::ptrdiff_t source = position + slip;
    if (source < 0 || !copy_has(copy, static_cast<std::size_t>(source))) {
      continue;
    }
    const auto from = static_cast<std::size_t>(source);
    const auto to = static_cast<std::size_t>(position);
    out.data[to] = copy.data[from];
    // What the suffix check made of the byte goes with it.
    out.present[to] =
        from < copy.present.size() ? copy.present[from] : kNabtsByteArrived;
    if (from < copy.confidence.size()) {
      out.confidence[to] = copy.confidence[from];
    }
  }
  return out;
}

/// Whether a copy's byte |position| arrived as the detector's wide reading
/// changed it (kNabtsByteReread): read to pass its parity, which is then no
/// evidence that it is right.
bool copy_reread(const NabtsRecordCopy& copy, std::size_t position) {
  return position < copy.present.size() &&
         copy.present[position] == kNabtsByteReread;
}

/// Whether a copy's byte |position| arrived passing its parity, and its parity
/// says so: a byte the wide reading changed passes because it was read to.
bool copy_usable(const NabtsRecordCopy& copy, std::size_t position) {
  return copy_has(copy, position) &&
         teletext_odd_parity_valid(copy.data[position]) &&
         !copy_reread(copy, position);
}

/// A copy's bytes as the content comparisons read them: whether byte
/// |position| arrived passing its parity, arrived failing it, and was vouched
/// for by the suffix check. A byte the wide reading changed is read as one
/// failing its parity: the stretch it is in arrived damaged, and the reading
/// made it pass (kNabtsByteReread). Worked out once a comparison, which reads
/// every byte at every slip and window it tries.
struct ByteReading {
  explicit ByteReading(const NabtsRecordCopy& read) : copy(read) {
    usable_bytes.resize(copy.data.size());
    for (std::size_t position = 0; position < copy.data.size(); ++position) {
      usable_bytes[position] = copy_usable(copy, position)
                                   ? static_cast<int16_t>(copy.data[position])
                                   : int16_t{-1};
    }
  }

  const NabtsRecordCopy& copy;
  // Each position's byte where it arrived passing its parity, else -1.
  std::vector<int16_t> usable_bytes;

  std::ptrdiff_t size() const {
    return static_cast<std::ptrdiff_t>(usable_bytes.size());
  }
  bool arrived(std::ptrdiff_t position) const {
    return position >= 0 && copy_has(copy, static_cast<std::size_t>(position));
  }
  uint8_t at(std::ptrdiff_t position) const {
    return copy.data[static_cast<std::size_t>(position)];
  }
  /// The byte at |position| where it arrived passing its parity, else -1.
  int usable_byte(std::ptrdiff_t position) const {
    return position >= 0 && position < size()
               ? usable_bytes[static_cast<std::size_t>(position)]
               : -1;
  }
  bool usable(std::ptrdiff_t position) const {
    return usable_byte(position) >= 0;
  }
  bool failing(std::ptrdiff_t position) const {
    return arrived(position) && !usable(position);
  }
  bool vouched(std::ptrdiff_t position) const {
    const auto index = static_cast<std::size_t>(position);
    return usable(position) && index < copy.present.size() &&
           copy.present[index] == kNabtsByteVouched;
  }
};

/// How far |copy| agrees with |held| when its byte |position| + |slip| is read
/// as |held|'s byte |position|, over the positions where both hold a byte that
/// passes its parity: a byte known to be damaged says nothing about which
/// content its copy carries. Over |held|'s positions |from| to |to|, or all of
/// them.
RecordAgreement content_agreement_at(
    const ByteReading& held, const ByteReading& copy, std::ptrdiff_t slip,
    std::size_t from = 0,
    std::size_t to = std::numeric_limits<std::size_t>::max()) {
  RecordAgreement out;
  const auto end = static_cast<std::ptrdiff_t>(
      std::min(to, static_cast<std::size_t>(held.size())));
  for (auto position = static_cast<std::ptrdiff_t>(from); position < end;
       ++position) {
    const int a = held.usable_byte(position);
    const int b = copy.usable_byte(position + slip);
    if (a < 0 || b < 0) {
      continue;
    }
    ++out.judged;
    out.agreed += a == b ? 1 : 0;
  }
  return out;
}

/// Whether |copy|'s byte |q|, read as |held|'s byte |p|, moved rather than
/// changed: the copy's bytes around it that pass their parity
/// (kNabtsDisplacementContext either side, at least four of them) agree, four
/// in five, with |held|'s at some other offset within
/// kNabtsDisplacementReach.
///
/// Whether any offset does is the answer, whichever is found, so |hint| — the
/// offset that last explained a byte, which the next byte of the same moved
/// stretch shares — is tried first, and is set to the offset found.
bool displaced(const ByteReading& held, std::ptrdiff_t p,
               const ByteReading& copy, std::ptrdiff_t q,
               std::ptrdiff_t& hint) {
  constexpr std::size_t kMinJudged = 4;
  constexpr std::ptrdiff_t kWidth = 2 * kNabtsDisplacementContext + 1;
  std::array<int, static_cast<std::size_t>(kWidth)> context{};
  std::size_t usable_context = 0;
  for (std::ptrdiff_t k = 0; k < kWidth; ++k) {
    context[static_cast<std::size_t>(k)] =
        copy.usable_byte(q + k - kNabtsDisplacementContext);
    usable_context += context[static_cast<std::size_t>(k)] >= 0 ? 1 : 0;
  }
  if (usable_context < kMinJudged) {
    return false;
  }
  const auto agrees_at = [&](std::ptrdiff_t offset) {
    std::size_t judged = 0;
    std::size_t agreed = 0;
    for (std::ptrdiff_t k = 0; k < kWidth; ++k) {
      const int byte = context[static_cast<std::size_t>(k)];
      const int other =
          held.usable_byte(p + k - kNabtsDisplacementContext + offset);
      if (byte < 0 || other < 0) {
        continue;
      }
      ++judged;
      agreed += byte == other ? 1 : 0;
    }
    return judged >= kMinJudged && agreed * 5 >= judged * 4;
  };
  if (hint != 0 && agrees_at(hint)) {
    return true;
  }
  for (std::ptrdiff_t offset = -kNabtsDisplacementReach;
       offset <= kNabtsDisplacementReach; ++offset) {
    if (offset != 0 && offset != hint && agrees_at(offset)) {
      hint = offset;
      return true;
    }
  }
  return false;
}

/// Whether |copy|, its byte p + |slip| read as |held|'s byte p, carries
/// something |held| does not (kNabtsPageDifferenceBytes): runs of other bytes
/// in a stretch both arrived with cleanly, or other bytes where the suffix
/// check vouched for both.
bool carries_more_than(const ByteReading& held, const ByteReading& copy,
                       std::ptrdiff_t slip) {
  const std::ptrdiff_t n = held.size();
  // The displacement that last explained a byte (see displaced()).
  std::ptrdiff_t displacement = 0;
  const auto differs = [&](std::ptrdiff_t p) {
    return held.usable(p) && copy.usable(p + slip) &&
           held.at(p) != copy.at(p + slip);
  };

  // Bytes the suffix check vouched for in both copies, where they differ.
  std::size_t vouched_judged = 0;
  std::vector<std::ptrdiff_t> vouched_differing;
  for (std::ptrdiff_t p = 0; p < n; ++p) {
    if (!held.vouched(p) || !copy.vouched(p + slip)) {
      continue;
    }
    ++vouched_judged;
    if (held.at(p) != copy.at(p + slip)) {
      vouched_differing.push_back(p);
    }
  }
  const auto vouched_enough = [&](std::size_t count) {
    return count >= kNabtsPageDifferenceVouchedBytes &&
           count * 100 >= vouched_judged * kNabtsPageDifferenceVouchedPercent;
  };
  if (vouched_enough(vouched_differing.size())) {
    std::size_t foreign = 0;
    for (const std::ptrdiff_t p : vouched_differing) {
      foreign += displaced(held, p, copy, p + slip, displacement) ? 0 : 1;
    }
    if (vouched_enough(foreign)) {
      return true;
    }
  }

  // Runs of differing bytes, where neither copy fails parity around them.
  const auto quiet = [&](std::ptrdiff_t from, std::ptrdiff_t to) {
    const auto reach =
        static_cast<std::ptrdiff_t>(kNabtsPageDifferenceQuietReach);
    std::size_t failing = 0;
    for (std::ptrdiff_t p = from - reach; p < to + reach; ++p) {
      failing += held.failing(p) ? 1 : 0;
      failing += copy.failing(p + slip) ? 1 : 0;
    }
    return failing <= kNabtsPageDifferenceQuietFailures;
  };
  const auto run_min = static_cast<std::ptrdiff_t>(kNabtsPageDifferenceRun);
  std::size_t foreign = 0;
  for (std::ptrdiff_t p = 0; p < n && foreign < kNabtsPageDifferenceBytes;) {
    if (!differs(p)) {
      ++p;
      continue;
    }
    std::ptrdiff_t end = p;
    while (end < n && differs(end)) {
      ++end;
    }
    if (end - p >= run_min && quiet(p, end)) {
      // Within the run, only the bytes no displacement explains, still
      // running.
      std::ptrdiff_t run = 0;
      for (std::ptrdiff_t k = p; k <= end; ++k) {
        if (k < end && !displaced(held, k, copy, k + slip, displacement)) {
          ++run;
          continue;
        }
        if (run >= run_min) {
          foreign += static_cast<std::size_t>(run);
        }
        run = 0;
      }
    }
    p = end;
  }
  return foreign >= kNabtsPageDifferenceBytes;
}

/// Whether |a| and |b|, |b|'s byte p + |slip| read as |a|'s byte p, are two
/// pages built on one layout rather than copies of one page
/// (kNabtsPageDifferenceBytes): each carries something the other does not.
bool different_pages(const ByteReading& a, const ByteReading& b,
                     std::ptrdiff_t slip) {
  return carries_more_than(a, b, slip) && carries_more_than(b, a, -slip);
}

/// How far |copy| agrees with |held| window by window
/// (kNabtsContentWindowBytes), each window at the slip within |max_slip| that
/// agrees best: the agreement of a copy whose bytes moved part way through.
RecordAgreement windowed_agreement(const ByteReading& held,
                                   const ByteReading& copy,
                                   std::ptrdiff_t max_slip) {
  RecordAgreement out;
  for (std::size_t from = 0; from < static_cast<std::size_t>(held.size());
       from += kNabtsContentWindowBytes) {
    const std::size_t to = from + kNabtsContentWindowBytes;
    std::optional<RecordAgreement> best;
    for (std::ptrdiff_t slip = -max_slip; slip <= max_slip; ++slip) {
      const RecordAgreement window =
          content_agreement_at(held, copy, slip, from, to);
      if (window.judged >= kNabtsContentWindowMinJudged &&
          (!best || window.better_than(*best))) {
        best = window;
      }
    }
    if (best) {
      out.judged += best->judged;
      out.agreed += best->agreed;
    }
  }
  return out;
}

/// Whether |copy| agrees with |held| as a copy of its content does
/// (kNabtsSameContentPercent), and how far, at the slip that agrees best,
/// which |slip| says. Whether each arrived intact is what says whether its
/// length is its record's. The window by window comparison looks as far as
/// |window_slip|.
bool agrees_as_one_content(const ByteReading& held, bool held_intact,
                           const ByteReading& copy, bool copy_intact,
                           RecordAgreement& agreement,
                           std::ptrdiff_t window_slip, std::ptrdiff_t& slip) {
  // A record's length is its content's, give or take the few bytes a misread
  // header moves the start of its data by (kNabtsMaxRecordSlip) — but only a
  // copy that arrived intact is known to be its record's length (see
  // kNabtsSameContentPercent).
  const std::size_t longer =
      std::max(held.copy.data.size(), copy.copy.data.size());
  const std::size_t shorter =
      std::min(held.copy.data.size(), copy.copy.data.size());
  if (held_intact && copy_intact && longer - shorter > kNabtsMaxRecordSlip) {
    return false;
  }
  const auto same = [](const RecordAgreement& a) {
    return a.judged < kNabtsOutlierMinJudgedPositions ||
           a.agreed * 100 >= a.judged * kNabtsSameContentPercent;
  };
  agreement = content_agreement_at(held, copy, 0);
  slip = 0;
  if (same(agreement)) {
    return true;
  }
  // A misread header moves every byte of a copy (kNabtsMaxRecordSlip), which
  // is not a new content: looked for only where the copy disagrees as it
  // stands, as the vote does.
  for (std::ptrdiff_t candidate_slip =
           -static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip);
       candidate_slip <= static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip);
       ++candidate_slip) {
    if (candidate_slip == 0) {
      continue;
    }
    const RecordAgreement candidate =
        content_agreement_at(held, copy, candidate_slip);
    if (candidate.better_than(agreement)) {
      agreement = candidate;
      slip = candidate_slip;
    }
  }
  if (same(agreement)) {
    return true;
  }
  // Nor where its bytes moved part way through, by a block it lost without a
  // hole or read at another width: that is what the windows find
  // (kNabtsContentWindowBytes).
  // The windows are laid over |held|, each at its own slip, so a copy that
  // kept few of its bytes lends the same ones to several windows at slips that
  // do not line them up. Laid over the copy instead, each of its bytes is
  // read once, at the slip of the window it is in.
  const auto line_up = [](const RecordAgreement& windows) {
    return windows.judged >= kNabtsOutlierMinJudgedPositions &&
           windows.agreed * 100 >= windows.judged * kNabtsContentWindowPercent;
  };
  const RecordAgreement over_held = windowed_agreement(held, copy, window_slip);
  if (line_up(over_held)) {
    agreement = over_held;
    return true;
  }
  const RecordAgreement over_copy = windowed_agreement(copy, held, window_slip);
  if (line_up(over_copy)) {
    agreement = over_copy;
    return true;
  }
  return false;
}

/// Whether |copy| carries the content |held| does: it agrees with it as a copy
/// of its content does (agrees_as_one_content()), and the two are not two
/// pages built on one layout, which agree on that layout as copies of one page
/// do (kNabtsPageDifferenceBytes).
bool same_content(const NabtsRecordCopy& held, bool held_intact,
                  const NabtsRecordCopy& copy, bool copy_intact,
                  RecordAgreement& agreement,
                  std::ptrdiff_t window_slip = kNabtsContentWindowSlip) {
  std::ptrdiff_t slip = 0;
  const ByteReading held_bytes{held};
  const ByteReading copy_bytes{copy};
  return agrees_as_one_content(held_bytes, held_intact, copy_bytes, copy_intact,
                               agreement, window_slip, slip) &&
         !different_pages(held_bytes, copy_bytes, slip);
}

/// One pass of the vote. |included| is either null — every copy votes — or one
/// flag per copy, zero for the copies the outlier pass ruled out. |vouching|
/// is null — no byte is taken as vouched for, which is the vote parity alone
/// can hold — or one flag per copy, non-zero for the copies whose vouched bytes
/// (kNabtsByteVouched) outrank the rest.
NabtsVoteResult vote_over(const std::vector<NabtsRecordCopy>& copies,
                          const uint8_t* included, const uint8_t* vouching) {
  NabtsVoteResult out;
  const std::size_t length = voted_length(copies, included);
  out.data.assign(length, 0);
  out.present.assign(length, 0);

  // One pass over the copies per position, tallying per byte value. The
  // accumulators are epoch-marked by position so none of them has to be cleared
  // between positions.
  std::array<uint64_t, 256> weight_of{};
  std::array<std::size_t, 256> newest_of{};
  // The same, counting only the copies whose block the suffix check vouched
  // for (kNabtsByteVouched).
  std::array<uint64_t, 256> vouched_weight_of{};
  std::array<std::size_t, 256> vouched_support_of{};
  std::array<std::size_t, 256> vouched_newest_of{};
  // Copies holding each value as it arrived rather than as the detector's wide
  // reading changed it (kNabtsByteReread).
  std::array<std::size_t, 256> plain_support_of{};
  std::array<std::size_t, 256> seen_at{};  // epoch: position + 1, 0 = never
  std::array<uint8_t, 256> distinct{};

  // Which class a value votes in: vouched for by the suffix check of some
  // copy's block, parity-clean, or known corrupt. A class outranks every class
  // below it whatever the weight behind them, and weight is counted within it.
  // A byte the detector's wide reading changed (kNabtsByteReread) is never
  // vouched for, but votes with its weight among the parity-clean ones: the
  // wide reading is a better reading of the tapes it is taken on, and the
  // errors it mends are the ones every copy shares. Ranked below every byte a
  // copy received passing its parity instead, it left more bytes wrong against
  // the pages' content and split no page the fewer.
  enum Tier { kCorrupt = 0, kParityClean = 1, kVouched = 2 };
  const auto tier_of = [&](uint8_t value) {
    if (vouched_support_of[value] > 0) {
      return kVouched;
    }
    return teletext_odd_parity_valid(value) ? kParityClean : kCorrupt;
  };
  const auto weight_in = [&](uint8_t value, Tier tier) {
    return tier == kVouched ? vouched_weight_of[value] : weight_of[value];
  };

  for (std::size_t position = 0; position < length; ++position) {
    const std::size_t epoch = position + 1;
    std::size_t distinct_count = 0;
    for (std::size_t copy = 0; copy < copies.size(); ++copy) {
      if (included != nullptr && included[copy] == 0) {
        continue;  // not a copy of this record (see nabts_vote_record())
      }
      const NabtsRecordCopy& candidate = copies[copy];
      if (position >= candidate.data.size()) {
        continue;  // a copy cut short votes only over the bytes it has
      }
      if (position < candidate.present.size() &&
          candidate.present[position] == 0) {
        // A byte a lost packet carried. The hole is held open so the bytes
        // after it keep their offsets, but there is nothing here to vote with.
        continue;
      }
      const uint8_t value = candidate.data[position];
      if (seen_at[value] != epoch) {
        seen_at[value] = epoch;
        weight_of[value] = 0;
        newest_of[value] = 0;
        vouched_weight_of[value] = 0;
        vouched_support_of[value] = 0;
        vouched_newest_of[value] = 0;
        plain_support_of[value] = 0;
        distinct[distinct_count++] = value;
      }
      // What the detector made of this byte, or full weight where nothing
      // measured it — see NabtsRecordCopy::confidence. One is the floor, so a
      // byte the detector could not decide at all still counts for more than a
      // copy that has no byte here at all.
      const uint64_t weight =
          position < candidate.confidence.size()
              ? std::max<uint64_t>(1, candidate.confidence[position])
              : 255;
      weight_of[value] += weight;
      newest_of[value] = copy;  // copies are held oldest first
      if (!copy_reread(candidate, position)) {
        ++plain_support_of[value];
      }
      if (vouching != nullptr && vouching[copy] != 0 &&
          position < candidate.present.size() &&
          candidate.present[position] == kNabtsByteVouched) {
        vouched_weight_of[value] += weight;
        ++vouched_support_of[value];
        vouched_newest_of[value] = copy;
      }
    }

    // NUL stands where no copy had this byte at all. X3.110 §6.1.4 makes it a
    // transparent control with no presentation effect, so a hole costs the
    // drawing nothing beyond the bytes that were actually lost — which is the
    // point of holding it open rather than letting the record close up over it.
    //
    // A byte whose block the suffix check vouched for beats any byte of a block
    // it did not, however many copies agree on that one: the damage a
    // recording does is not random — the same pattern of bits misread the same
    // way, copy after copy — so agreement among damaged copies is not evidence
    // against the one the check says is right. Below that, a byte known
    // corrupt never beats a parity-clean one.
    uint8_t best = 0;
    Tier best_tier = kCorrupt;
    uint64_t best_weight = 0;
    std::size_t best_newest = 0;
    for (std::size_t k = 0; k < distinct_count; ++k) {
      const uint8_t value = distinct[k];
      const Tier tier = tier_of(value);
      const uint64_t weight = weight_in(value, tier);
      const std::size_t newest =
          tier == kVouched ? vouched_newest_of[value] : newest_of[value];
      if (k > 0) {
        if (tier < best_tier) {
          continue;
        }
        if (tier == best_tier &&
            !(weight > best_weight ||
              (weight == best_weight && newest > best_newest))) {
          continue;
        }
      }
      best = value;
      best_tier = tier;
      best_weight = weight;
      best_newest = newest;
    }
    out.data[position] = best;
    // Zero where no copy delivered this byte: the NUL stands for it, and the
    // mask says so rather than letting a filler pass for a reading. Where the
    // value won among bytes the suffix check vouched for, the mask says that
    // too, for what reads the record afterwards to tell a byte the check
    // vouched for from one that merely arrived; and where every copy holding
    // it holds it as the wide reading made it pass its parity, that.
    if (distinct_count == 0) {
      out.present[position] = kNabtsByteLost;
    } else if (best_tier == kVouched) {
      out.present[position] = kNabtsByteVouched;
    } else if (plain_support_of[best] == 0 && teletext_odd_parity_valid(best)) {
      out.present[position] = kNabtsByteReread;
    } else {
      out.present[position] = kNabtsByteArrived;
    }
  }
  return out;
}

}  // namespace

namespace {

/// How far a copy's bytes move when its group closes up over packets it lost
/// without a hole: one to three packets' data blocks, of 28, 27 or 26 bytes
/// as the packets carry no suffix, a one-byte or a two-byte one (§3.4).
constexpr std::array<std::size_t, 9> kClosedUpShifts{26, 27, 28, 52, 54,
                                                     56, 78, 81, 84};

/// Each copy's positions to leave out of the vote (see nabts_vote_record()):
/// those it holds a byte at where fewer than half the copies reaching the
/// position hold one, and where, at the nearest kNabtsAlignmentWitnesses
/// settled positions on one side, it disagrees with |provisional| but agrees
/// with it a block or more further on (kClosedUpShifts). Empty when there are
/// none. |included| as for vote_over().
std::vector<std::vector<std::size_t>> closed_up_bytes(
    const std::vector<NabtsRecordCopy>& copies,
    const NabtsVoteResult& provisional, const uint8_t* included) {
  const std::size_t length = provisional.data.size();
  std::vector<uint32_t> holding(length, 0);
  std::vector<uint32_t> reaching(length, 0);
  for (std::size_t c = 0; c < copies.size(); ++c) {
    if (included != nullptr && included[c] == 0) {
      continue;
    }
    const std::size_t reach = std::min(length, copies[c].data.size());
    for (std::size_t position = 0; position < reach; ++position) {
      ++reaching[position];
      holding[position] += copy_has(copies[c], position) ? 1 : 0;
    }
  }
  // Where most of the copies that reach a position hold a byte, what they say
  // there is settled whichever way one copy votes. Where none is unsettled and
  // held, no copy can decide a position alone and there is nothing to look at.
  const auto settled = [&](std::size_t position) {
    return provisional.present[position] != 0 && holding[position] >= 2 &&
           holding[position] * 2 >= reaching[position];
  };
  bool any = false;
  for (std::size_t position = 0; position < length && !any; ++position) {
    any = holding[position] > 0 && !settled(position);
  }
  std::vector<std::vector<std::size_t>> out;
  if (!any) {
    return out;
  }

  // Per copy: the settled positions it holds a byte at (its witnesses), and
  // counted from the first of them, where it agrees with the vote as it
  // stands (lane 0) and a shift further on (lane k + 1), and where that
  // shifted position is settled so as to be judged.
  constexpr std::size_t kLanes = kClosedUpShifts.size() + 1;
  std::vector<std::size_t> witnesses;
  std::array<std::vector<uint32_t>, kLanes> agreed;
  std::array<std::vector<uint32_t>, kLanes> judged;
  for (std::size_t c = 0; c < copies.size(); ++c) {
    if (included != nullptr && included[c] == 0) {
      continue;
    }
    const NabtsRecordCopy& copy = copies[c];
    const std::size_t reach = std::min(length, copy.data.size());
    witnesses.clear();
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      agreed[lane].assign(1, 0);
      judged[lane].assign(1, 0);
    }
    for (std::size_t position = 0; position < reach; ++position) {
      if (!copy_has(copy, position) || !settled(position)) {
        continue;
      }
      witnesses.push_back(position);
      for (std::size_t lane = 0; lane < kLanes; ++lane) {
        const std::size_t at =
            position + (lane == 0 ? 0 : kClosedUpShifts[lane - 1]);
        const bool counts = at < length && settled(at);
        judged[lane].push_back(judged[lane].back() + (counts ? 1 : 0));
        agreed[lane].push_back(
            agreed[lane].back() +
            (counts && copy.data[position] == provisional.data[at] ? 1 : 0));
      }
    }
    // Whether the witnesses from..to show the copy holding the vote's bytes a
    // block or more later than the vote has them: it disagrees as it stands
    // and agrees once shifted.
    const auto majority = [&](std::size_t lane, std::size_t from,
                              std::size_t to) {
      const uint32_t count = judged[lane][to] - judged[lane][from];
      return count >= kNabtsAlignmentWitnesses / 2 &&
             (agreed[lane][to] - agreed[lane][from]) * 2 >= count;
    };
    const auto moved = [&](std::size_t from, std::size_t to) {
      const uint32_t count = judged[0][to] - judged[0][from];
      if (count < kNabtsAlignmentWitnesses / 2 || majority(0, from, to)) {
        return false;
      }
      for (std::size_t lane = 1; lane < kLanes; ++lane) {
        if (majority(lane, from, to)) {
          return true;
        }
      }
      return false;
    };
    for (std::size_t position = 0; position < reach; ++position) {
      if (!copy_has(copy, position) || settled(position)) {
        continue;
      }
      const auto next = static_cast<std::size_t>(
          std::lower_bound(witnesses.begin(), witnesses.end(), position) -
          witnesses.begin());
      const std::size_t before =
          next > kNabtsAlignmentWitnesses ? next - kNabtsAlignmentWitnesses : 0;
      const std::size_t after =
          std::min(witnesses.size(), next + kNabtsAlignmentWitnesses);
      if (!moved(before, next) && !moved(next, after)) {
        continue;
      }
      if (out.empty()) {
        out.resize(copies.size());
      }
      out[c].push_back(position);
    }
  }
  return out;
}

}  // namespace

NabtsVoteResult nabts_vote_record(const std::vector<NabtsRecordCopy>& copies) {
  if (copies.empty()) {
    return {};
  }
  if (copies.size() == 1) {
    // A vote of one is the copy itself, and what the recovery knew about that
    // copy is what it knows about the record: whether each byte arrived, and
    // whether the suffix check vouched for its block.
    NabtsVoteResult out;
    out.data = copies.front().data;
    out.present = copies.front().present;
    return out;
  }

  // Not every copy filed under a record is a copy of that record. §3.2.2 codes
  // the packet address in Hamming 8/4 and §8.2 of ETSI EN 300 706 gives that
  // code minimum distance 4, so a burst resolves silently to a neighbouring
  // codeword and a packet of some other record is assembled into this one; a
  // record identity the recording never named can also be folded in here (see
  // reconcile_identities()). Such a copy is a clean read of real data, only not
  // of this record, so neither parity nor confidence can tell it apart — and
  // voting it in makes the record a per-byte blend of both, which for a
  // stateful opcode stream is worse than either alone and worse the more copies
  // there are.
  //
  // What tells it apart is how much of the record it agrees with. Copies of one
  // record differ only where they were damaged; a copy of something else
  // disagrees nearly everywhere. So the vote is taken twice: once to find what
  // the copies mostly say, and again without the copies that hardly say it.
  // Only a minority may be dropped — where most of them disagree there is no
  // record for the rest to be outliers of.
  //
  // A copy of the record can also disagree nearly everywhere because it has
  // slipped: a misread header starts its data a few bytes early or late (see
  // kNabtsMaxRecordSlip). So before a copy is ruled out it is tried slid either
  // way, and one that agrees once slid votes slid. That is only ever tried on a
  // copy that disagrees as it stands — a slide is looked for to explain an
  // outlier, never to improve on a copy that already agrees.
  //
  // The suffix check vouches for a block as it was received, not for where the
  // record layer placed it or which record it is a copy of, so what the copies
  // mostly say is found by parity and weight alone. Only a copy that agrees
  // with nearly all of it (kNabtsVouchAgreementPercent) has its vouched bytes
  // outrank the others in the vote that counts.
  const NabtsVoteResult provisional = vote_over(copies, nullptr, nullptr);
  std::vector<NabtsRecordCopy> slid;
  std::vector<uint8_t> included(copies.size(), 1);
  std::vector<uint8_t> vouching(copies.size(), 0);
  std::size_t dropped = 0;
  for (std::size_t index = 0; index < copies.size(); ++index) {
    const RecordAgreement unslid = agreement(copies[index], provisional, 0);
    if (!unslid.outlier()) {
      vouching[index] = unslid.vouches() ? 1 : 0;
      continue;
    }
    RecordAgreement best = unslid;
    std::ptrdiff_t best_slip = 0;
    for (std::ptrdiff_t slip =
             -static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip);
         slip <= static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip); ++slip) {
      if (slip == 0) {
        continue;
      }
      const RecordAgreement candidate =
          agreement(copies[index], provisional, slip);
      if (!candidate.outlier() && candidate.better_than(best)) {
        best = candidate;
        best_slip = slip;
      }
    }
    if (best_slip != 0) {
      if (slid.empty()) {
        slid = copies;
      }
      slid[index] = slide(copies[index], best_slip);
      vouching[index] = best.vouches() ? 1 : 0;
      continue;
    }
    included[index] = 0;
    ++dropped;
  }
  const std::vector<NabtsRecordCopy>& voting = slid.empty() ? copies : slid;
  const uint8_t* voters =
      dropped == 0 || dropped * 2 >= copies.size() ? nullptr : included.data();

  // A copy whose group closed up over a block it lost without a hole — a
  // continuity index read wrong where lines went by (§3.2.4) — agrees with the
  // rest as far as the lost block and holds the block after it from there on,
  // so it is no outlier. Where every other copy lost that block, as on a tape
  // that loses one line's packets every field, it is the only voter and the
  // record holds the next block there: text such as "Press 16 for Joe
  // MontPress 16 for", where a recording of the page whole shows the block
  // belongs 27 bytes further on. Where few copies hold a byte, a copy that
  // holds the vote's bytes a block late beside it (closed_up_bytes()) abstains
  // there, and the stretch stays lost — which Reconstruct lost data can fill.
  const std::vector<std::vector<std::size_t>> closed_up =
      closed_up_bytes(voting, provisional, voters);
  if (closed_up.empty()) {
    return vote_over(voting, voters, vouching.data());
  }
  std::vector<NabtsRecordCopy> placed = voting;
  for (std::size_t c = 0; c < closed_up.size(); ++c) {
    if (closed_up[c].empty()) {
      continue;
    }
    NabtsRecordCopy& copy = placed[c];
    copy.present.resize(copy.data.size(), kNabtsByteArrived);
    for (const std::size_t position : closed_up[c]) {
      copy.present[position] = 0;
    }
  }
  return vote_over(placed, voters, vouching.data());
}

NabtsRecordCatalogue::NabtsRecordCatalogue(std::size_t max_records,
                                           std::size_t max_copies)
    : max_records_(std::max<std::size_t>(1, max_records)),
      max_copies_(std::max<std::size_t>(1, max_copies)),
      captions_(std::make_unique<NabtsCaptionStream>()) {}

NabtsRecordCatalogue::~NabtsRecordCatalogue() = default;
NabtsRecordCatalogue::NabtsRecordCatalogue(
    NabtsRecordCatalogue&& other) noexcept = default;
NabtsRecordCatalogue& NabtsRecordCatalogue::operator=(
    NabtsRecordCatalogue&& other) noexcept = default;

std::vector<NabtsCataloguedRecord> NabtsRecordCatalogue::captions() const {
  return captions_->captions();
}

uint64_t NabtsRecordCatalogue::captions_dropped() const {
  return captions_->dropped();
}

void nabts_take_identity(NabtsCataloguedRecord& record,
                         const NabtsMessage& message, uint64_t frame_id) {
  record.channel = message.channel;
  record.address = message.address.value;
  record.address_text = message.address.text();
  record.channel_text =
      fmt::format("{:03X}/{}", message.channel, message.address.text());
  record.version = message.version;
  record.reserved_purpose = message.reserved_purpose;
  record.first_seen_frame = frame_id;
}

bool nabts_copy_is_better(const NabtsMessage& message, bool kept_intact,
                          std::size_t kept_bytes) {
  const bool candidate_intact = message.complete && message.intact;
  if (candidate_intact != kept_intact) {
    return candidate_intact;
  }
  return message.data.size() > kept_bytes;
}

void nabts_offer_copy(std::vector<NabtsRecordCopy>& copies,
                      const NabtsMessage& message, bool kept_intact,
                      std::size_t max_copies) {
  if (message.complete && message.intact) {
    // The record has arrived whole, so there is nothing left for a vote to
    // improve on and the copies held for one can go.
    copies.clear();
    copies.shrink_to_fit();
    return;
  }
  if (kept_intact) {
    return;  // an undamaged copy arrived earlier and is what will be shown
  }
  if (!message.aligned) {
    // Bytes that have moved cannot be compared position for position with the
    // rest, and voting them in would corrupt every position after the hole.
    return;
  }
  if (copies.size() >= max_copies) {
    // Oldest out, so the copies held stay the most recent — which is what the
    // vote falls back to when a position has nothing to choose between.
    copies.erase(copies.begin());
  }
  copies.push_back(
      NabtsRecordCopy{message.data, message.present, message.confidence});
}

void NabtsRecordCatalogue::take_copy(Entry& entry,
                                     const NabtsMessage& message) {
  NabtsCataloguedRecord& record = entry.record;
  // The kept copy's type is the tie the type vote falls back on
  // (voted_type()); its flags are voted afresh by records() (voted_flags()).
  record.record_type = message.type;
  record.records_in_message = message.records;
  record.complete = message.complete;

  record.functions = render_functions(message.functions);
  take_more_designation(record, message);
  take_link_designations(record, message);
  entry.kept_copy_intact = message.complete && message.intact;
  // The kept copy's bytes are held here alone, not in record.data as well;
  // records() lists them from here.
  entry.representative = NabtsRecordCopy{message.data, message.present, {}};
}

void NabtsRecordCatalogue::tally_flags(Entry& entry,
                                       const NabtsMessage& message,
                                       bool attested) {
  const NabtsClassification& flags = message.classification;
  const std::array<bool, kClassificationFlags> set{
      flags.caption, flags.cyclic_marker,  flags.priority,       flags.alarm,
      flags.update,  flags.support_record, flags.support_needed, flags.index,
      flags.more,    flags.auto_acquire};
  for (std::size_t flag = 0; flag < kClassificationFlags; ++flag) {
    if (!set[flag]) {
      continue;
    }
    ++entry.flags_set[flag];
    if (attested) {
      ++entry.flags_set_attested[flag];
    }
  }
  const ClassificationForm form = form_of(flags);
  ++entry.forms[form];
  if (attested) {
    ++entry.forms_attested[form];
  }
  ++entry.types[message.type & 0xFu];
  if (attested) {
    ++entry.types_attested[message.type & 0xFu];
  }
}

NabtsRecordCatalogue::ClassificationForm NabtsRecordCatalogue::form_of(
    const NabtsClassification& flags) {
  ClassificationForm form{flags.pointer_bytes, {}};
  form.second.reserve(flags.reserved_flags.size());
  for (const NabtsReservedFlag& flag : flags.reserved_flags) {
    // YNm, named as §5.2.7.1 numbers them, with the byte's information bits
    // as the hexadecimal digit a Hamming 8/4 byte decodes to.
    form.second.push_back(
        fmt::format("Y{}{} = {:X}", flag.group, flag.position, flag.value));
  }
  return form;
}

uint8_t NabtsRecordCatalogue::voted_type(const Entry& entry) {
  const auto& types =
      entry.record.times_attested > 0 ? entry.types_attested : entry.types;
  uint8_t chosen = entry.record.record_type;
  uint32_t most = types[chosen & 0xFu];
  for (size_t type = 0; type < types.size(); ++type) {
    if (types[type] > most) {
      most = types[type];
      chosen = static_cast<uint8_t>(type);
    }
  }
  return chosen;
}

void NabtsRecordCatalogue::voted_flags(const Entry& entry,
                                       NabtsCataloguedRecord& record) {
  const bool attested = entry.record.times_attested > 0;
  const uint64_t voters =
      attested ? entry.record.times_attested : entry.record.times_seen;
  const auto voted = [&](ClassificationFlag flag) {
    const uint64_t set =
        attested ? entry.flags_set_attested[flag] : entry.flags_set[flag];
    return set * 2 > voters;
  };
  record.caption = voted(kCaption);
  record.cyclic_marker = voted(kCyclicMarker);
  record.priority = voted(kPriority);
  record.alarm = voted(kAlarm);
  record.update = voted(kUpdate);
  record.support_record = voted(kSupportRecord);
  record.support_needed = voted(kSupportNeeded);
  record.index = voted(kIndex);
  record.more = voted(kMore);
  record.auto_acquire = voted(kAutoAcquire);
  record.record_type = voted_type(entry);

  // The form most of the same appearances carried; among forms carried
  // equally often, the first in the map's order, so the choice does not
  // depend on the order the copies arrived in.
  const auto& forms = attested ? entry.forms_attested : entry.forms;
  const ClassificationForm* chosen = nullptr;
  uint32_t most = 0;
  for (const auto& [form, count] : forms) {
    if (count > most) {
      most = count;
      chosen = &form;
    }
  }
  record.classification_pointers = chosen != nullptr ? chosen->first : 0;
  record.reserved_flags =
      chosen != nullptr ? chosen->second : std::vector<std::string>{};
}

uint32_t NabtsRecordCatalogue::content_of(const NabtsMessage& message) const {
  const NabtsRecordCopy copy{message.data, message.present, {}};
  const bool copy_intact = message.complete && message.intact;
  uint32_t next = 0;
  std::optional<uint32_t> best;
  RecordAgreement best_agreement;
  for (auto it = records_.lower_bound(
           Key{message.channel, message.address.value, message.version, 0});
       it != records_.end() && std::get<0>(it->first) == message.channel &&
       std::get<1>(it->first) == message.address.value &&
       std::get<2>(it->first) == message.version;
       ++it) {
    const uint32_t content = std::get<3>(it->first);
    next = std::max(next, content + 1);
    if (!message.aligned) {
      // Bytes that have moved cannot be compared position for position, so
      // they say nothing about which content they carry.
      if (!best) {
        best = content;
      }
      continue;
    }
    // The kept copy is the longest rather than the best, so a copy that
    // disagrees with it is tried against every copy held for the content's
    // vote as well (kNabtsSameContentPercent). A content whose kept copy
    // arrived intact holds no other.
    const Entry& entry = it->second;
    RecordAgreement agreement;
    bool same = same_content(entry.representative, entry.kept_copy_intact, copy,
                             copy_intact, agreement);
    for (std::size_t held = 0; !same && held < entry.copies.size(); ++held) {
      same = same_content(entry.copies[held], false, copy, false, agreement);
    }
    if (!same) {
      continue;
    }
    if (!best || agreement.better_than(best_agreement)) {
      best = content;
      best_agreement = agreement;
    }
  }
  return best.value_or(next);
}

void NabtsRecordCatalogue::merge(const NabtsMessage& message,
                                 uint64_t frame_id) {
  // §7.3.10.1: a caption's version names it only while it is on air, so the
  // caption service is kept as it arrived rather than keyed on its versions.
  if (captions_->accepts(message)) {
    captions_->add(message, frame_id);
    return;
  }

  // §5.2.2 defines record types 0 to 3 and reserves the rest, and no service
  // sent one: a copy carrying a reserved type had its header misread, and
  // does not name its identity as transmitted however its bytes arrived.
  const bool attested = message.identity_attested &&
                        message.type <= kNabtsRecordTypePriorityPresentation;

  // A copy that carries none of the contents held under its identity is a
  // content of its own (kNabtsSameContentPercent), catalogued apart rather
  // than voted into one it is not a copy of.
  const Key key{message.channel, message.address.value, message.version,
                content_of(message)};
  ++touch_counter_;

  auto it = records_.find(key);
  if (it == records_.end()) {
    Entry entry;
    nabts_take_identity(entry.record, message, frame_id);
    entry.record.content = std::get<3>(key);
    take_copy(entry, message);
    it = records_.emplace(key, std::move(entry)).first;
  } else if (nabts_copy_is_better(message, it->second.kept_copy_intact,
                                  it->second.representative.data.size())) {
    take_copy(it->second, message);
  }

  Entry& entry = it->second;
  nabts_offer_copy(entry.copies, message, entry.kept_copy_intact, max_copies_);
  tally_flags(entry, message, attested);
  entry.record.last_seen_frame = frame_id;
  ++entry.record.times_seen;
  if (message.complete && message.intact) {
    ++entry.record.times_intact;
  }
  if (attested) {
    ++entry.record.times_attested;
  }
  if (message.header_attested &&
      message.type <= kNabtsRecordTypePriorityPresentation) {
    ++entry.times_header_attested;
  }
  entry.last_touched = touch_counter_;

  enforce_bounds();
}

void NabtsRecordCatalogue::enforce_bounds() {
  // An identity no appearance named as transmitted goes before any that one
  // did, the least recently seen of them first (see kMaxCataloguedRecords):
  // dropping the least recently seen of all, a damaged tape dropped pages named
  // as transmitted while it held misread identities.
  const auto goes_first = [](const Entry& a, const Entry& b) {
    const bool a_attested = a.record.times_attested > 0;
    const bool b_attested = b.record.times_attested > 0;
    if (a_attested != b_attested) {
      return !a_attested;
    }
    return a.last_touched < b.last_touched;
  };
  while (records_.size() > max_records_) {
    auto victim = records_.begin();
    for (auto candidate = records_.begin(); candidate != records_.end();
         ++candidate) {
      if (goes_first(candidate->second, victim->second)) {
        victim = candidate;
      }
    }
    records_.erase(victim);
    truncated_ = true;
  }
}

namespace {

/// The share of the positions where both |a| and |b| hold a byte passing its
/// parity that hold the same one, at the slip within kNabtsMaxRecordSlip that
/// agrees best; zero where that slip judged fewer than kNabtsIdentityMinJudged
/// positions.
double vote_agreement(const NabtsRecordCopy& a, const NabtsRecordCopy& b) {
  const ByteReading a_bytes{a};
  const ByteReading b_bytes{b};
  RecordAgreement best = content_agreement_at(b_bytes, a_bytes, 0);
  std::ptrdiff_t best_slip = 0;
  for (std::ptrdiff_t slip = -static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip);
       slip <= static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip); ++slip) {
    const RecordAgreement candidate =
        content_agreement_at(b_bytes, a_bytes, slip);
    if (candidate.better_than(best)) {
      best = candidate;
      best_slip = slip;
    }
  }
  if (best.judged < kNabtsIdentityMinJudged) {
    return 0.0;
  }
  const double share =
      static_cast<double>(best.agreed) / static_cast<double>(best.judged);
  // Two pages built on one layout agree on it as a page's misread copies do
  // (kNabtsPageDifferenceBytes), and folded together they vote into a blend.
  if (share >= kVbiSameContent &&
      different_pages(b_bytes, a_bytes, best_slip)) {
    return 0.0;
  }
  return share;
}

/// Whether |a| and |b| are two pages built on one layout, at the slip within
/// kNabtsMaxRecordSlip that agrees best (kNabtsPageDifferenceBytes).
bool views_differ_as_pages(const NabtsRecordCopy& a, const NabtsRecordCopy& b) {
  const ByteReading a_bytes{a};
  const ByteReading b_bytes{b};
  RecordAgreement best = content_agreement_at(a_bytes, b_bytes, 0);
  std::ptrdiff_t best_slip = 0;
  for (std::ptrdiff_t slip = -static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip);
       slip <= static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip); ++slip) {
    const RecordAgreement candidate =
        content_agreement_at(a_bytes, b_bytes, slip);
    if (candidate.better_than(best)) {
      best = candidate;
      best_slip = slip;
    }
  }
  return different_pages(a_bytes, b_bytes, best_slip);
}

/// Whether two copies of one record are copies of each other: at least half of
/// the kNabtsIdentityMinJudged or more positions both received agree, at the
/// slip within kNabtsMaxRecordSlip that agrees best.
bool copies_agree(const NabtsRecordCopy& a, const NabtsRecordCopy& b) {
  const ByteReading a_bytes{a};
  const ByteReading b_bytes{b};
  RecordAgreement best = content_agreement_at(a_bytes, b_bytes, 0);
  for (std::ptrdiff_t slip = -static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip);
       slip <= static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip); ++slip) {
    const RecordAgreement candidate =
        content_agreement_at(a_bytes, b_bytes, slip);
    if (candidate.better_than(best)) {
      best = candidate;
    }
  }
  return best.judged >= kNabtsIdentityMinJudged &&
         best.agreed * 2 >= best.judged;
}

// §5.2.1's identity written out digit by digit, in transmission order: the
// three packet-address digits (§3.2.3), the nine record-address digits of the
// long form §5.2.5 makes every address equivalent to, and the version (§5.2.7.2
// Y16). Thirteen digits, each of which arrived in its own Hamming 8/4 byte and
// so can be moved on its own.
VbiIdentityDigits identity_digits(const NabtsCataloguedRecord& record) {
  VbiIdentityDigits digits;
  digits.reserve(13);
  for (int shift = 8; shift >= 0; shift -= 4) {
    digits.push_back(static_cast<uint8_t>((record.channel >> shift) & 0xF));
  }
  for (int shift = 32; shift >= 0; shift -= 4) {
    digits.push_back(static_cast<uint8_t>((record.address >> shift) & 0xF));
  }
  digits.push_back(static_cast<uint8_t>(record.version & 0xF));
  return digits;
}

}  // namespace

VbiIdentityReconciliation NabtsRecordCatalogue::reconcile_identities() {
  join_contents();
  fold_misread_versions();

  VbiIdentityReconciliation out;
  out.identities_seen = static_cast<uint32_t>(records_.size());

  std::vector<Key> keys;
  std::vector<VbiIdentity> identities;
  std::size_t attested = 0;
  for (const auto& [key, entry] : records_) {
    keys.push_back(key);
    identities.push_back({identity_digits(entry.record),
                          entry.record.times_attested > 0,
                          entry.record.times_seen});
    attested += identities.back().attested ? 1 : 0;
  }
  out.identities_unattested =
      static_cast<uint32_t>(identities.size() - attested);

  if (!vbi_identity_reconciliation_applies(attested)) {
    // Nothing arrived as transmitted, so there is no baseline and the rule has
    // nothing to say. Leaving the catalogue as recovered is the honest answer;
    // emptying it would be the confident one.
    out.withheld = out.identities_unattested > 0;
    return out;
  }

  // What each identity's copies say its record is, compared over the
  // positions both received (kNabtsIdentityMinJudged): see the header.
  std::vector<NabtsRecordCopy> views;
  views.reserve(keys.size());
  for (const Key& key : keys) {
    views.push_back(view_of(records_.at(key)));
  }
  auto verdicts =
      vbi_reconcile_identities(identities, [&](std::size_t a, std::size_t b) {
        const NabtsRecordCopy& view = views[a];
        if (view.data.empty() || views[b].data.empty()) {
          return std::numeric_limits<double>::quiet_NaN();
        }
        if (a == b) {
          // Asked of an identity before it is kept as a page of its own.
          return keepable(records_.at(keys[a]), view)
                     ? 1.0
                     : std::numeric_limits<double>::quiet_NaN();
        }
        return vote_agreement(view, views[b]);
      });
  // The data channel most of the identities named as transmitted are on: the
  // service's own. And the identities a content of which was named as
  // transmitted: another content of one of them is not a page its header
  // can vouch for, since the identity is already the service's.
  std::map<uint16_t, std::size_t> attested_on_channel;
  std::set<std::tuple<uint16_t, uint64_t, uint8_t>> named;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (identities[i].attested) {
      ++attested_on_channel[std::get<0>(keys[i])];
      named.insert(
          {std::get<0>(keys[i]), std::get<1>(keys[i]), std::get<2>(keys[i])});
    }
  }
  uint16_t service_channel = 0;
  std::size_t most_attested = 0;
  for (const auto& [channel, count] : attested_on_channel) {
    if (count > most_attested) {
      service_channel = channel;
      most_attested = count;
    }
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (identities[i].attested) {
      continue;
    }
    VbiIdentityVerdict& verdict = verdicts[i];
    // A single sighting goes to its one neighbour a digit away whatever it
    // carries; where its content is another page built on that page's layout
    // (kNabtsPageDifferenceBytes), folding it in would only blend the two.
    if (verdict.action == VbiIdentityVerdict::Action::kFold &&
        views_differ_as_pages(views[verdict.into], views[i])) {
      verdict = {VbiIdentityVerdict::Action::kDrop, 0};
    }
    if (verdict.action == VbiIdentityVerdict::Action::kDrop &&
        std::get<0>(keys[i]) == service_channel &&
        named.count({std::get<0>(keys[i]), std::get<1>(keys[i]),
                     std::get<2>(keys[i])}) == 0 &&
        kept_by_its_header(records_.at(keys[i]), views[i])) {
      verdict = {VbiIdentityVerdict::Action::kKeep, 0};
    }
  }
  // A record on a data channel other than the service's that carries the
  // address and version of a page of the service's channel seen far more
  // often, and none of whose copies arrived intact, is that page's packets
  // misaddressed (kNabtsMisaddressedSeenRatio). Judged of the identities the
  // verdicts above leave standing — named as transmitted, or kept — against
  // namesakes they leave standing too.
  const auto standing = [&](std::size_t i) {
    return identities[i].attested ||
           verdicts[i].action == VbiIdentityVerdict::Action::kKeep;
  };
  std::map<Key, std::size_t> index_of;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    index_of.emplace(keys[i], i);
  }
  std::vector<bool> misaddressed(keys.size(), false);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const NabtsCataloguedRecord& record = records_.at(keys[i]).record;
    if (std::get<0>(keys[i]) == service_channel || !standing(i) ||
        record.times_intact > 0) {
      continue;
    }
    uint64_t namesake_seen = 0;
    for (auto it = records_.lower_bound(Key{
             service_channel, std::get<1>(keys[i]), std::get<2>(keys[i]), 0});
         it != records_.end() && std::get<0>(it->first) == service_channel &&
         std::get<1>(it->first) == std::get<1>(keys[i]) &&
         std::get<2>(it->first) == std::get<2>(keys[i]);
         ++it) {
      if (standing(index_of.at(it->first))) {
        namesake_seen += it->second.record.times_seen;
      }
    }
    misaddressed[i] =
        namesake_seen >= kNabtsMisaddressedSeenRatio * record.times_seen;
  }
  // A misreading folds into an identity left standing; one that would fold
  // into a misaddressed record is that record's like, and goes with it.
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (!identities[i].attested &&
        verdicts[i].action == VbiIdentityVerdict::Action::kFold &&
        misaddressed[verdicts[i].into]) {
      verdicts[i] = {VbiIdentityVerdict::Action::kDrop, 0};
    }
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (identities[i].attested || misaddressed[i]) {
      continue;
    }
    Entry& entry = records_.at(keys[i]);
    const uint64_t seen = entry.record.times_seen;
    switch (verdicts[i].action) {
      case VbiIdentityVerdict::Action::kKeep:
        ++out.identities_kept;
        out.appearances_kept += seen;
        continue;
      case VbiIdentityVerdict::Action::kFold:
        // Into an attested or kept identity, neither of which is erased here.
        fold_into(records_.at(keys[verdicts[i].into]), entry);
        ++out.identities_folded;
        out.appearances_folded += seen;
        break;
      case VbiIdentityVerdict::Action::kDrop:
        ++out.identities_dropped;
        out.appearances_dropped += seen;
        break;
    }
    records_.erase(keys[i]);
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (misaddressed[i]) {
      ++out.identities_misaddressed;
      out.appearances_misaddressed += records_.at(keys[i]).record.times_seen;
      records_.erase(keys[i]);
    }
  }
  return out;
}

void NabtsRecordCatalogue::fold_misread_versions() {
  // §5.2.7.2 has a service change a record's version (Y16) when it revises the
  // record, and the revision then replaces it: a version sent a handful of
  // times while another version of the same address went on being sent
  // before and after, and carrying that version's bytes, is that version with
  // its version nibble misread. Hamming 8/4's minimum distance of 4 lets a
  // burst resolve silently into another codeword, so the misreading can arrive
  // with every byte of its header a codeword — named as transmitted, which
  // keeps it out of the fold below. Listed on its own, such a version is a page
  // drawn from a couple of damaged sightings in the middle of hundreds, and
  // smeared by their damaged bytes.
  std::vector<Key> keys;
  keys.reserve(records_.size());
  for (const auto& [key, entry] : records_) {
    keys.push_back(key);
  }
  for (const Key& rare_key : keys) {
    const auto rare_it = records_.find(rare_key);
    if (rare_it == records_.end()) {
      continue;  // folded already
    }
    const Entry& rare = rare_it->second;
    const NabtsCataloguedRecord& seen = rare.record;
    std::optional<Key> into;
    double into_share = 0.0;
    for (auto it = records_.lower_bound(
             Key{std::get<0>(rare_key), std::get<1>(rare_key), 0, 0});
         it != records_.end() &&
         std::get<0>(it->first) == std::get<0>(rare_key) &&
         std::get<1>(it->first) == std::get<1>(rare_key);
         ++it) {
      const NabtsCataloguedRecord& main = it->second.record;
      if (std::get<2>(it->first) == std::get<2>(rare_key) ||
          main.times_seen < kNabtsMisreadVersionMinMain ||
          main.times_seen < kNabtsMisreadVersionRatio * seen.times_seen ||
          seen.first_seen_frame < main.first_seen_frame ||
          seen.last_seen_frame > main.last_seen_frame) {
        continue;
      }
      const double share = vote_agreement(view_of(rare), view_of(it->second));
      if (share >= kNabtsMisreadVersionAgreement && share > into_share) {
        into = it->first;
        into_share = share;
      }
    }
    if (!into) {
      continue;
    }
    fold_into(records_.at(*into), rare_it->second);
    records_.erase(rare_it);
  }
}

void NabtsRecordCatalogue::fold_into(Entry& target, Entry& misread) const {
  // The appearances were appearances of the target: the service brought that
  // record round and this recording misread its name on the way past. Counting
  // them there is what keeps times_seen a property of the service rather than
  // of the damage.
  add_tallies(target, misread);

  // The copies are the reason to fold rather than discard: they are bytes of
  // the target record that were received, and the vote in records() is where
  // they pay for themselves. A target that already holds an undamaged copy has
  // nothing for them to improve on, which is nabts_offer_copy()'s rule as well.
  if (target.kept_copy_intact) {
    return;
  }
  // Only into the room that is left. The target's own copies came in under an
  // identity the recording did name as transmitted, which is better evidence
  // than anything folded in here — so a misreading fills the vote out rather
  // than displacing what is already in it.
  for (NabtsRecordCopy& copy : misread.copies) {
    if (target.copies.size() >= max_copies_) {
      break;
    }
    target.copies.push_back(std::move(copy));
  }
}

NabtsRecordCopy NabtsRecordCatalogue::view_of(const Entry& entry) {
  if (entry.copies.empty()) {
    return entry.representative;
  }
  NabtsVoteResult voted = nabts_vote_record(entry.copies);
  return NabtsRecordCopy{std::move(voted.data), std::move(voted.present), {}};
}

namespace {

/// Bytes of |view| that arrived passing their parity, other than those the
/// wide reading made pass (kNabtsByteReread).
std::size_t usable_bytes(const NabtsRecordCopy& view) {
  std::size_t usable = 0;
  for (std::size_t position = 0; position < view.data.size(); ++position) {
    usable += copy_usable(view, position) ? 1 : 0;
  }
  return usable;
}

}  // namespace

bool NabtsRecordCatalogue::keepable(const Entry& entry,
                                    const NabtsRecordCopy& view) {
  // §5.2.2 reserves types 4 to 15 and both decoders of the day refuse them; a
  // record no copy named as transmitted and most of whose copies read as one
  // is not a page anyone was shown.
  if (voted_type(entry) > kNabtsRecordTypePriorityPresentation) {
    return false;
  }
  if (usable_bytes(view) < kNabtsIdentityMinJudged) {
    return false;
  }
  if (entry.kept_copy_intact) {
    return true;
  }
  // Copies of several pages misread onto one name each agree with the vote
  // they make up between them, but not with one another; the copies of one
  // page misread the same way every time do.
  for (std::size_t a = 0; a < entry.copies.size(); ++a) {
    for (std::size_t b = a + 1; b < entry.copies.size(); ++b) {
      if (copies_agree(entry.copies[a], entry.copies[b])) {
        return true;
      }
    }
  }
  return false;
}

bool NabtsRecordCatalogue::kept_by_its_header(const Entry& entry,
                                              const NabtsRecordCopy& view) {
  return entry.times_header_attested > 0 &&
         voted_type(entry) <= kNabtsRecordTypePriorityPresentation &&
         usable_bytes(view) >= kNabtsIdentityMinJudged;
}

void NabtsRecordCatalogue::add_tallies(Entry& target, const Entry& source) {
  target.record.times_seen += source.record.times_seen;
  target.record.times_intact += source.record.times_intact;
  target.times_header_attested += source.times_header_attested;
  for (std::size_t flag = 0; flag < kClassificationFlags; ++flag) {
    target.flags_set[flag] += source.flags_set[flag];
  }
  for (const auto& [form, count] : source.forms) {
    target.forms[form] += count;
  }
  for (std::size_t type = 0; type < target.types.size(); ++type) {
    target.types[type] += source.types[type];
  }
  target.record.first_seen_frame =
      std::min(target.record.first_seen_frame, source.record.first_seen_frame);
  target.record.last_seen_frame =
      std::max(target.record.last_seen_frame, source.record.last_seen_frame);
}

void NabtsRecordCatalogue::absorb(Entry& target, Entry& source) const {
  // The appearances were appearances of the target's content, filed apart
  // while too few copies were in to tell.
  add_tallies(target, source);
  target.record.times_attested += source.record.times_attested;
  for (std::size_t flag = 0; flag < kClassificationFlags; ++flag) {
    target.flags_set_attested[flag] += source.flags_set_attested[flag];
  }
  for (const auto& [form, count] : source.forms_attested) {
    target.forms_attested[form] += count;
  }
  for (std::size_t type = 0; type < target.types.size(); ++type) {
    target.types_attested[type] += source.types_attested[type];
  }

  if (source.kept_copy_intact && !target.kept_copy_intact) {
    // An undamaged copy is what is shown, as merge() would have had it had
    // the copy been filed under the target to begin with.
    NabtsCataloguedRecord counts = target.record;
    target.record = source.record;
    target.record.content = counts.content;
    target.record.times_seen = counts.times_seen;
    target.record.times_intact = counts.times_intact;
    target.record.times_attested = counts.times_attested;
    target.record.first_seen_frame = counts.first_seen_frame;
    target.record.last_seen_frame = counts.last_seen_frame;
    target.kept_copy_intact = true;
    target.representative = source.representative;
    target.copies.clear();
  } else if (!target.kept_copy_intact) {
    // The most recent copies are the ones kept, as nabts_offer_copy() keeps
    // them.
    for (NabtsRecordCopy& copy : source.copies) {
      target.copies.push_back(std::move(copy));
    }
    if (target.copies.size() > max_copies_) {
      target.copies.erase(
          target.copies.begin(),
          target.copies.begin() +
              static_cast<std::ptrdiff_t>(target.copies.size() - max_copies_));
    }
  }
}

void NabtsRecordCatalogue::join_contents() {
  std::map<std::tuple<uint16_t, uint64_t, uint8_t>, std::vector<Key>>
      by_identity;
  for (const auto& held : records_) {
    by_identity[{std::get<0>(held.first), std::get<1>(held.first),
                 std::get<2>(held.first)}]
        .push_back(held.first);
  }
  for (auto& [identity, keys] : by_identity) {
    // Keys ascend by content, which is the order the contents first arrived.
    std::vector<std::optional<NabtsRecordCopy>> views(keys.size());
    const auto view = [&](std::size_t index) -> const NabtsRecordCopy& {
      if (!views[index]) {
        views[index] = view_of(records_.at(keys[index]));
      }
      return *views[index];
    };
    std::vector<bool> joined(keys.size(), false);
    for (std::size_t later = 1; later < keys.size(); ++later) {
      Entry& source = records_.at(keys[later]);
      for (std::size_t earlier = 0; earlier < later; ++earlier) {
        if (joined[earlier]) {
          continue;
        }
        Entry& target = records_.at(keys[earlier]);
        RecordAgreement agreement;
        // A copy given holes for blocks it never lost runs two or three
        // blocks late, so the windows look that far as well
        // (kNabtsJoinWindowSlip).
        if (same_content(view(earlier), target.kept_copy_intact, view(later),
                         source.kept_copy_intact, agreement) ||
            same_content(view(earlier), target.kept_copy_intact, view(later),
                         source.kept_copy_intact, agreement,
                         kNabtsJoinWindowSlip)) {
          absorb(target, source);
          views[earlier].reset();
          joined[later] = true;
          break;
        }
      }
    }
    uint32_t number = 0;
    for (std::size_t index = 0; index < keys.size(); ++index) {
      if (joined[index]) {
        records_.erase(keys[index]);
        continue;
      }
      if (std::get<3>(keys[index]) != number) {
        auto node = records_.extract(keys[index]);
        std::get<3>(node.key()) = number;
        node.mapped().record.content = number;
        records_.insert(std::move(node));
      }
      ++number;
    }
  }
}

std::vector<NabtsCataloguedRecord> NabtsRecordCatalogue::records() const {
  std::vector<NabtsCataloguedRecord> out;
  out.reserve(records_.size());
  // The map is keyed on {channel, address, version, content}, so iteration is
  // already the order a reader would list a service in.
  for (const auto& held : records_) {
    const Entry& entry = held.second;
    out.push_back(entry.record);
    NabtsCataloguedRecord& record = out.back();
    voted_flags(entry, record);

    // Copies are held only where no undamaged one ever arrived, so this is the
    // recording that never gave up a clean copy of the record: combine what did
    // arrive rather than show whichever copy happened to be longest.
    if (entry.copies.empty()) {
      record.data = entry.representative.data;
      continue;
    }
    NabtsVoteResult voted = nabts_vote_record(entry.copies);
    record.data = std::move(voted.data);
    record.data_present = std::move(voted.present);
    record.copies_voted = static_cast<uint32_t>(entry.copies.size());
    if (record.record_type == kNabtsRecordTypeApplication) {
      // The descriptors were rendered from one copy; the vote may have
      // recovered bytes that copy had wrong (§7.2.2).
      record.functions =
          render_functions(nabts_decode_application_record(record.data));
    }
  }
  return out;
}

std::string nabts_record_key(const NabtsCataloguedRecord& record) {
  std::string key = fmt::format("{:03X}-{}-v{:X}", record.channel,
                                record.address_text, record.version & 0xFu);
  if (record.content > 0) {
    key += fmt::format("-{}", record.content + 1);
  }
  return key;
}

std::string nabts_record_identity(const NabtsCataloguedRecord& record) {
  std::string identity = record.channel_text;
  std::transform(
      identity.begin(), identity.end(), identity.begin(), [](char c) {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
      });
  identity += fmt::format(" v{:X}", record.version & 0xFu);
  if (record.content > 0) {
    identity += fmt::format(" ({})", record.content + 1);
  }
  return identity;
}

std::vector<NabtsCataloguedRecord> nabts_catalogue_stream(
    const std::vector<uint8_t>& stream) {
  NabtsGroupAssembler groups;
  groups.set_lines_unknown();
  NabtsRecordAssembler assembler;
  NabtsRecordCatalogue catalogue;
  // The frame a packet was on is not in the stream. Six packets a frame is the
  // order of what a service sends, which is all the first- and last-seen frame
  // numbers of a reference are used for.
  constexpr uint64_t kPacketsPerFrameEstimate = 6;
  uint64_t frame = 0;
  groups.set_group_callback(
      [&](const NabtsDataGroup& group) { assembler.add_group(group); });
  assembler.set_message_callback(
      [&](const NabtsMessage& message) { catalogue.merge(message, frame); });
  for (size_t offset = 0; offset + kNabtsPacketBytes <= stream.size();
       offset += kNabtsPacketBytes) {
    frame = offset / kNabtsPacketBytes / kPacketsPerFrameEstimate;
    groups.add_packet(
        nabts_decode_packet(&stream[offset], kNabtsPacketBytes, nullptr));
  }
  groups.flush();
  assembler.flush();
  catalogue.reconcile_identities();
  return catalogue.records();
}

namespace {

/// A record's address within its data channel.
using AddressKey = std::pair<uint16_t, uint64_t>;

/**
 * @brief The presentation code a reconstruction rebuilt, per record
 *
 * Parallel to the records, and empty where a record lost nothing or
 * reconstruction is off. Done in one sweep before anything is interpreted,
 * because a record is run more than once — as itself, as a member of a More
 * chain, and as the Support Record another page needs — and a page assembled
 * from two readings of the same bytes would be neither.
 */
struct RebuiltCode {
  std::vector<std::vector<uint8_t>> code;
  std::vector<NaplpsReconstructionSummary> summaries;
};

RebuiltCode rebuild_records(const std::vector<NabtsCataloguedRecord>& records,
                            const NabtsReconstructionInput& reconstruction) {
  RebuiltCode out;
  if (!reconstruction.enabled) {
    return out;
  }
  out.code.resize(records.size());
  out.summaries.resize(records.size());

  // The library and the model are the recording's own presentation records
  // and those of the references: the templates a lost stretch is looked up
  // in, and the habits the reading of what is left leans on.
  NabtsRecordLibrary library;
  NaplpsDialectModel model;
  std::vector<size_t> library_index(records.size(), SIZE_MAX);
  for (std::size_t i = 0; i < records.size(); ++i) {
    const NabtsCataloguedRecord& record = records[i];
    if (!nabts_type_is_presentation(record.record_type) ||
        record.data.empty()) {
      continue;
    }
    library_index[i] = library.records().size();
    library.add(NabtsLibraryRecord{"this recording", nabts_record_key(record),
                                   record.data, record.data_present});
    model.learn(record.data, record.data_present);
  }
  if (reconstruction.references != nullptr) {
    for (const NabtsReferenceRecord& reference : *reconstruction.references) {
      library.add(NabtsLibraryRecord{reference.source, reference.key,
                                     reference.data, reference.present});
      model.learn(reference.data, reference.present);
    }
  }
  library.build();

  NaplpsReconstructionSummary sweep;
  uint32_t records_rebuilt = 0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const NabtsCataloguedRecord& record = records[i];
    if (library_index[i] == SIZE_MAX) {
      continue;
    }
    // Every record, not only those that lost bytes: one that lost nothing can
    // still hold bytes it received failing their parity.
    NaplpsReconstruction rebuilt_record = naplps_reconstruct_record(
        record.data, record.data_present, &library, library_index[i], &model,
        reconstruction.options);
    sweep.bytes_corrupt += rebuilt_record.summary.bytes_corrupt;
    if (rebuilt_record.summary.holes == 0 &&
        rebuilt_record.summary.bytes_corrected == 0) {
      continue;
    }
    out.code[i] = std::move(rebuilt_record.data);
    out.summaries[i] = std::move(rebuilt_record.summary);
    const NaplpsReconstructionSummary& did = out.summaries[i];
    sweep.bytes_lost += did.bytes_lost;
    sweep.bytes_filled += did.bytes_filled;
    sweep.bytes_inferred += did.bytes_inferred;
    sweep.bytes_corrected += did.bytes_corrected;
    if (did.changed()) {
      ++records_rebuilt;
    }
  }
  ORC_LOG_INFO(
      "NAPLPS reconstruction: {} record(s) rebuilt from {} library "
      "record(s) — {} of {} lost byte(s) filled, {} inferred; "
      "{} of {} byte(s) received failing parity corrected",
      records_rebuilt, library.records().size(), sweep.bytes_filled,
      sweep.bytes_lost, sweep.bytes_inferred, sweep.bytes_corrected,
      sweep.bytes_corrupt);
  return out;
}

/// Whether |next| takes over from |held| as the record a channel or an
/// address is presented by: the higher version, which ascending iteration
/// delivers for free — except that a version no appearance ever named as
/// transmitted, kept because it recurred with content of its own
/// (reconcile_identities()), does not displace one that was: a page filed under
/// a misread version would otherwise take a page's place at the head of its
/// More chain, and the story the page continues would stand alone.
bool displaces(const NabtsCataloguedRecord* held,
               const NabtsCataloguedRecord& next) {
  return held == nullptr || next.times_attested > 0 ||
         held->times_attested == 0;
}

/// §8.7.1.4: one Support Record per Data Channel, at address FFF with the
/// Support Record Flag set — the record that "contain[s] one or more macro
/// definitions ... invoked directly by other Presentation Records in the same
/// Data Channel". Where versions differ the highest wins (displaces()).
std::map<uint16_t, const NabtsCataloguedRecord*> support_records(
    const std::vector<NabtsCataloguedRecord>& records) {
  std::map<uint16_t, const NabtsCataloguedRecord*> support_by_channel;
  for (const NabtsCataloguedRecord& record : records) {
    if (record.support_record &&
        nabts_type_is_presentation(record.record_type)) {
      const NabtsCataloguedRecord*& held = support_by_channel[record.channel];
      if (displaces(held, record)) {
        held = &record;
      }
    }
  }
  return support_by_channel;
}

/// The record |record| continues into as its More Record, in |successor|.
/// §7.3.4's receiver algorithm consults a header extension redefining the More
/// Record first (§5.2.8.4) and the More Flag's algorithmic
/// long-address-plus-one (§5.2.7.6) only where there is none.
bool continues_into(const NabtsCataloguedRecord& record, uint64_t& successor) {
  successor = 0;
  if (record.more_redefined) {
    // The extension decides, even where it cannot be followed here: a macro
    // is code, not a record, and a record in another data channel is one
    // this page is not presented as continuing into. Either way §7.3.4 never
    // reaches the More Flag.
    if (!record.has_more_address || record.more_channel != record.channel) {
      return false;
    }
    successor = record.more_address;
  } else if (!record.more ||
             !nabts_algorithmic_more_address(record.address, successor)) {
    return false;
  }
  // Only a link forward continues a page. A link back to the same or an
  // earlier address — an index page naming itself, a story naming the Master
  // Index (000), the last page of a rotating set naming the first (§7.3.4's
  // note on the Random More function) — returns the reader to a page that
  // stands on its own and is reached by its number as well as by MORE, so it is
  // presented as that page rather than over the one that named it. Every chain
  // then runs forward and ends: its base is the lowest address it passes
  // through.
  return successor > record.address;
}

/**
 * @brief Who names each record as its More Record
 *
 * The map runs successor -> predecessor, which is the direction a chain is
 * walked to find what a record is presented over. A chain is followed through
 * the latest catalogued version at each address: §5.2.7.6 addresses the next
 * record of a chain, not a particular version of it.
 *
 * A record that several others name keeps the last of them in address order.
 * A service can name a section's index page from every page of the section,
 * and such an index clears the screen before it draws, so
 * which of them it is presented over does not change what it shows.
 */
std::map<AddressKey, const NabtsCataloguedRecord*> more_predecessors(
    const std::vector<NabtsCataloguedRecord>& records) {
  // The latest catalogued version at each {channel, address}. Ascending
  // iteration leaves the highest version in place, confirmed versions before
  // unconfirmed ones (displaces()).
  std::map<AddressKey, const NabtsCataloguedRecord*> latest;
  // The most seen content of each identity that continues into a record.
  std::map<std::tuple<uint16_t, uint64_t, uint8_t>,
           const NabtsCataloguedRecord*>
      continuing;
  for (const NabtsCataloguedRecord& record : records) {
    if (!nabts_type_is_presentation(record.record_type)) {
      continue;
    }
    const NabtsCataloguedRecord*& held =
        latest[{record.channel, record.address}];
    if (displaces(held, record)) {
      held = &record;
    }
    uint64_t successor = 0;
    if (continues_into(record, successor)) {
      const NabtsCataloguedRecord*& linking =
          continuing[{record.channel, record.address, record.version}];
      if (linking == nullptr || record.times_seen > linking->times_seen) {
        linking = &record;
      }
    }
  }

  // An identity can carry more than one content (see merge()), and where the
  // latest one at an address does not continue into a record but another
  // content of the same identity does, the chain is followed through that one.
  // A content left in the carousel from a page's earlier design can still be
  // sent under its identity beside a later one whose More Flag carries the
  // story on: taken for the chain, the older content would leave the pages
  // after it drawn without the frame and colours of the page they continue.
  for (auto& entry : latest) {
    uint64_t successor = 0;
    if (continues_into(*entry.second, successor)) {
      continue;
    }
    const auto linking = continuing.find(
        {entry.second->channel, entry.second->address, entry.second->version});
    if (linking != continuing.end()) {
      entry.second = linking->second;
    }
  }

  std::map<AddressKey, const NabtsCataloguedRecord*> predecessor_of;
  for (const auto& entry : latest) {
    const NabtsCataloguedRecord* candidate = entry.second;
    uint64_t successor = 0;
    if (continues_into(*candidate, successor)) {
      predecessor_of[{candidate->channel, successor}] = candidate;
    }
  }
  return predecessor_of;
}

/// The chain back to |record|'s base (more_predecessors()), oldest first,
/// excluding the record itself. Every link runs forward, so each step back is
/// to a lower address.
std::vector<const NabtsCataloguedRecord*> chain_before(
    const std::map<AddressKey, const NabtsCataloguedRecord*>& predecessor_of,
    const NabtsCataloguedRecord& record) {
  std::vector<const NabtsCataloguedRecord*> prefix;
  uint64_t address = record.address;
  for (auto predecessor = predecessor_of.find({record.channel, address});
       predecessor != predecessor_of.end();
       predecessor = predecessor_of.find({record.channel, address})) {
    prefix.insert(prefix.begin(), predecessor->second);
    address = predecessor->second->address;
  }
  return prefix;
}

/**
 * @brief Runs records into pages the way a receiver presents a page
 *
 * The general reset of §8.5, then — for a record whose Support-Needed Flag is
 * set (§5.2.7.9) — the channel's Support Record, whose macros, DRCS, texture
 * masks and colour map persist into the page, then the caption preset of
 * §5.2.7.3 where the Caption Flag asks for it, and then the record itself.
 * Each record's code is the rebuilt reading where there is one, and what was
 * recovered otherwise.
 */
class PagePresenter {
 public:
  PagePresenter(NaplpsInterpreter& interpreter,
                const std::vector<NabtsCataloguedRecord>& records,
                const RebuiltCode& rebuilt, const NabtsKeyPlay* play)
      : interpreter_(interpreter),
        records_(records),
        rebuilt_(rebuilt),
        play_(play),
        support_by_channel_(support_records(records)) {}

  /// Present |record| over the pages a viewer came to it from, |context|, and
  /// then over its chain, |prefix|.
  void present(NabtsCataloguedRecord& record,
               const std::vector<const NabtsCataloguedRecord*>& context,
               const std::vector<const NabtsCataloguedRecord*>& prefix) {
    bool needs_support = record.support_needed;
    bool caption = record.caption;
    for (const NabtsCataloguedRecord* member : prefix) {
      needs_support = needs_support || member->support_needed;
      caption = caption || member->caption;
    }
    for (const NabtsCataloguedRecord* page : context) {
      needs_support = needs_support || page->support_needed;
    }
    prepare(record, needs_support, caption);
    bool keep_display = false;
    for (const NabtsCataloguedRecord* page : context) {
      (void)interpreter_.run(code_of(page), keep_display);
      keep_display = true;
    }
    for (const NabtsCataloguedRecord* member : prefix) {
      (void)interpreter_.run(code_of(member), keep_display);
      keep_display = true;
    }
    record.page = interpreter_.run(code_of(&record), keep_display);
    // A Support Record's macros are there for the pages that invoke them
    // (§8.7.1.4), not for a viewer's keys.
    record.linked_keys =
        record.support_record ? uint16_t{0} : interpreter_.linked_keys();
    const std::size_t index = index_of(&record);
    if (play_ != nullptr && index == play_->record) {
      // Table D1 item 7: each key's macro runs from the state the page and
      // the keys before it left, over the display they left.
      for (const uint8_t key : play_->keys) {
        record.page = interpreter_.press_key(key);
      }
    }

    // What the reconstruction did is the reader's business as much as what
    // the decode did: a page drawn partly from rebuilt bytes should say so.
    if (index < rebuilt_.summaries.size()) {
      naplps_stamp_reconstruction_diagnostics(rebuilt_.summaries[index],
                                              record.page.diagnostics);
    }
  }

  /// Whether |record| starts a screen of its own when it is drawn by itself.
  /// A More Record that does hides whatever its chain was drawn over, so the
  /// page a viewer came to the chain from is no part of how it looks.
  bool stands_alone(const NabtsCataloguedRecord* record) {
    const auto known = starts_afresh_.find(record);
    if (known != starts_afresh_.end()) {
      return known->second;
    }
    prepare(*record, record->support_needed, record->caption);
    const bool alone =
        !nabts_draws_over_previous(interpreter_.run(code_of(record)));
    starts_afresh_.emplace(record, alone);
    return alone;
  }

 private:
  /// Where a receiver starts a page from: the general reset, the channel's
  /// Support Record where |needs_support|, the caption preset where
  /// |caption|.
  void prepare(const NabtsCataloguedRecord& record, bool needs_support,
               bool caption) {
    interpreter_.reset_decoder();
    if (needs_support && !record.support_record) {
      const auto support = support_by_channel_.find(record.channel);
      if (support != support_by_channel_.end()) {
        // Run for its definitions; what the support record itself drew is
        // not part of this page.
        (void)interpreter_.run(code_of(support->second));
      }
    }
    if (caption) {
      interpreter_.apply_caption_state();
    }
  }

  std::size_t index_of(const NabtsCataloguedRecord* entry) const {
    return static_cast<std::size_t>(entry - records_.data());
  }

  const std::vector<uint8_t>& code_of(
      const NabtsCataloguedRecord* entry) const {
    const std::size_t index = index_of(entry);
    if (index < rebuilt_.code.size() && !rebuilt_.code[index].empty()) {
      return rebuilt_.code[index];
    }
    return entry->data;
  }

  NaplpsInterpreter& interpreter_;
  const std::vector<NabtsCataloguedRecord>& records_;
  const RebuiltCode& rebuilt_;
  const NabtsKeyPlay* play_;
  const std::map<uint16_t, const NabtsCataloguedRecord*> support_by_channel_;
  std::map<const NabtsCataloguedRecord*, bool> starts_afresh_;
};

/// |record| as the navigation (nabts_navigation.h) sees a page, which also
/// records the pages it offers on |record| (offered_pages).
NabtsNavigationPage navigation_page(NabtsCataloguedRecord& record) {
  NabtsNavigationPage page;
  page.channel = record.channel;
  page.address = record.address;
  page.version = record.version;
  page.first_frame = record.first_seen_frame;
  page.last_frame = record.last_seen_frame;
  page.has_more = record.more_redefined && record.has_more_address &&
                  record.more_channel == record.channel;
  page.more = page.has_more ? record.more_address : 0;
  page.has_next = record.has_next_address;
  page.next = record.next_address;
  page.has_index = record.has_index_address;
  page.index = record.index_address;
  page.has_next_by_number =
      nabts_next_page_address(record.address, page.next_by_number);
  for (const std::string& number :
       nabts_offered_numbers(nabts_page_lines(record.page))) {
    uint64_t offered = 0;
    if (nabts_keypad_address(number, offered) && offered != record.address) {
      page.offered.push_back(offered);
    }
  }
  record.offered_pages = page.offered;
  page.drawn = !record.page.empty();
  page.draws_over = nabts_draws_over_previous(record.page);
  bool start_arrived = true;
  for (std::size_t i = 0;
       i < std::min(record.data_present.size(), kNabtsContextStartBytes); ++i) {
    start_arrived = start_arrived && record.data_present[i] != 0;
  }
  page.wants_context = page.draws_over && start_arrived &&
                       record.times_intact > 0 && record.chain_position == 0 &&
                       !record.caption && record.classification_pointers <= 1;
  return page;
}

}  // namespace

void nabts_interpret_records(std::vector<NabtsCataloguedRecord>& records,
                             NaplpsRenderGrid grid,
                             const NabtsReconstructionInput& reconstruction,
                             NaplpsReceiverPace receiver_pace,
                             const NabtsKeyPlay* play) {
  NaplpsInterpreter interpreter(grid);
  interpreter.set_receiver_pace(receiver_pace);
  const RebuiltCode rebuilt = rebuild_records(records, reconstruction);
  PagePresenter presenter(interpreter, records, rebuilt, play);
  const std::map<AddressKey, const NabtsCataloguedRecord*> predecessor_of =
      more_predecessors(records);

  // §6.1: a presentation record's data is NAPLPS, so this is where it becomes
  // something a viewer can draw — once per record, on whatever the vote
  // settled on. An application record's data is function descriptors (§7.2.2),
  // rendered above, and running it as presentation code would draw nonsense.
  //
  // A More Record is presented last of all of its chain: §5.2.7.8 has it
  // "presented after the completion of the presentation of the current
  // Record" — over the standing display, not over a cleared one. So a
  // record's predecessors are walked back through the links above, executed
  // in order with the display carried between them, and the record itself
  // drawn on top of what they left.
  for (NabtsCataloguedRecord& record : records) {
    const std::size_t index =
        static_cast<std::size_t>(&record - records.data());
    if (!nabts_type_is_presentation(record.record_type) ||
        (play != nullptr && index != play->record)) {
      continue;
    }
    if (record.data.empty()) {
      // Nothing to run, so nothing to draw: a record whose every copy was lost
      // has no presentation code, and running it would only replace the page it
      // already carries with the empty one it would have produced anyway.
      continue;
    }
    const std::vector<const NabtsCataloguedRecord*> prefix =
        chain_before(predecessor_of, record);
    record.chain_base_address =
        prefix.empty() ? record.address : prefix.front()->address;
    record.chain_position = static_cast<uint32_t>(prefix.size());
    record.presented_over.clear();
    presenter.present(record, {}, prefix);
  }
  if (play != nullptr) {
    // A page played on its keys runs them over the page as it was sent.
    return;
  }

  // A page that starts on the screen it was asked from — no clear, no ground
  // over the display area before it draws — was drawn over the page the viewer
  // came from: the Panasonic TU-2000x does not clear its screen when a new page
  // is asked for, and a service can write pages to be drawn over others (a
  // quiz's questions and answers over its board, rotating highlight cards over
  // a highlights page). Such a page is
  // presented over the pages that lead to it (nabts_presentation_contexts()),
  // each as the viewer saw it — a More Record over its chain — and so are the
  // members of its More chain, up to one that starts a screen of its own.
  // Pages with two classification pointer bytes, which no surviving receiver
  // accepts (§8.5.2.7.1), are left as drawn on their own, since how a receiver
  // changed page under them is not known; so is
  // a page no copy of which arrived whole with its check passed, or whose first
  // bytes no copy delivered, which may have lost its clear rather than been
  // sent without one.
  std::vector<NabtsNavigationPage> navigation;
  std::vector<NabtsCataloguedRecord*> navigation_record;
  std::map<const NabtsCataloguedRecord*, size_t> navigation_index;
  for (NabtsCataloguedRecord& record : records) {
    if (!nabts_type_is_presentation(record.record_type) ||
        record.data.empty()) {
      continue;
    }
    navigation_index[&record] = navigation.size();
    navigation.push_back(navigation_page(record));
    navigation_record.push_back(&record);
  }
  const std::vector<std::vector<std::size_t>> contexts =
      nabts_presentation_contexts(navigation);

  for (NabtsCataloguedRecord& record : records) {
    const auto self = navigation_index.find(&record);
    if (self == navigation_index.end()) {
      continue;
    }
    const std::vector<const NabtsCataloguedRecord*> prefix =
        chain_before(predecessor_of, record);
    const auto base =
        navigation_index.find(prefix.empty() ? &record : prefix.front());
    if (base == navigation_index.end() || contexts[base->second].empty()) {
      continue;
    }
    bool hidden = false;
    for (std::size_t i = 1; i < prefix.size() && !hidden; ++i) {
      hidden = presenter.stands_alone(prefix[i]);
    }
    if (hidden || (!prefix.empty() && presenter.stands_alone(&record))) {
      continue;
    }
    std::vector<const NabtsCataloguedRecord*> context;
    for (const std::size_t index : contexts[base->second]) {
      // A More Record was on screen over the members of its chain before it.
      const NabtsCataloguedRecord* page = navigation_record[index];
      for (const NabtsCataloguedRecord* member :
           chain_before(predecessor_of, *page)) {
        context.push_back(member);
      }
      context.push_back(page);
      record.presented_over.push_back(nabts_record_identity(*page));
    }
    presenter.present(record, context, prefix);
  }
}

}  // namespace orc
