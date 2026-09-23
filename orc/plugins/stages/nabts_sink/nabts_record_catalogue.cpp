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
#include <map>
#include <set>
#include <string>
#include <utility>

#include "naplps_lint_repair.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

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
 * @brief The same-channel More Record address a header extension names, if any
 *
 * §5.2.8.2 meaning 001 is "redefinition of More record Address", and §5.2.8.4
 * sizes the data by what it carries: 0 is record address zero, 3 a short
 * address, 9 a long one — all in the same data channel. The channel-switching
 * forms (6 and 12) name a record this catalogue keys under another channel;
 * they are rare and are left unresolved rather than half-followed. The macro
 * form (2) designates code, not a record.
 */
bool more_extension_address(const std::vector<NabtsHeaderExtension>& extensions,
                            uint64_t& address) {
  for (const NabtsHeaderExtension& extension : extensions) {
    if (extension.meaning != 1) {
      continue;
    }
    switch (extension.size) {
      case 0:
        address = 0;
        return true;
      case 3:
      case 9: {
        uint64_t value = 0;
        for (const uint8_t nibble : extension.data) {
          value = (value << 4) | (nibble & 0xF);
        }
        // A short address PQR is the long 0000PQR00 (§5.2.5).
        address = extension.size == 3 ? value << 8 : value;
        return true;
      }
      default:
        break;
    }
  }
  return false;
}

/**
 * @brief The algorithmic More address of §5.2.7.6, or false at the range's end
 *
 * "Adding 1 to the long version of the current Record Address", where §7.3.4
 * adds: "For the purposes of incrementing, the last two digits shall be
 * regarded as decimal numbers" — so 09 steps to 10, not to 0A.
 */
bool algorithmic_more_address(uint64_t address, uint64_t& successor) {
  const unsigned tens = (address >> 4) & 0xF;
  const unsigned units = address & 0xF;
  if (tens > 9 || units > 9) {
    return false;  // not a decimal pair; the increment is undefined on it
  }
  const unsigned value = tens * 10 + units + 1;
  if (value > 99) {
    return false;
  }
  successor = (address & ~uint64_t{0xFF}) | ((value / 10) << 4) | (value % 10);
  return true;
}

/**
 * @brief What is known against |record|'s bytes, for the repair pass
 *
 * Parity is recomputable from the data alone, but what the recovery knew beyond
 * it is not: which bytes never arrived, and how sure the detector and the vote
 * ended up being of the ones that did. A record kept from a copy that arrived
 * whole carries neither mask, and falls back to the parity-only reading.
 */
NaplpsSuspectMap suspects_of(const NabtsCataloguedRecord& record) {
  return NaplpsSuspectMap::from_record(record.data, record.data_present,
                                       record.data_confidence);
}

/// One position the weights could not settle, and the candidates that were
/// level there. Held so the grammar can be asked about it once the whole
/// record has been voted — a candidate is graded by what it does to the record
/// around it, which is not known until the rest of the record is.
struct VoteContest {
  std::size_t position = 0;
  std::vector<uint8_t> candidates;
};

/**
 * @brief Ask the grammar about the positions the weights left level
 *
 * Each candidate is put in place and the whole record linted from the state it
 * will be read in; the candidate that leaves the record most grammatical wins,
 * and only where it is alone in doing so and does better than what the weights
 * chose. The discipline is the repair pass's (naplps_lint_repair.h), for the
 * same reason: a tie-break that guesses when the grammar has no preference is
 * inventing a record rather than recovering one.
 *
 * The damage evidence is computed once, from the record as the weights left it.
 * A candidate changes the parity of its own byte, so the map is a little stale
 * for that one position — which costs nothing, because every candidate for a
 * position is graded against the same map and only their difference matters.
 */
void adjudicate_contests(const std::vector<VoteContest>& contests,
                         const NabtsVoteOptions& options,
                         NabtsVoteResult& out) {
  NaplpsLinter unseeded;
  unseeded.reset_decoder();
  NaplpsLinter& context =
      options.context != nullptr ? *options.context : unseeded;

  const NaplpsSuspectMap suspects =
      NaplpsSuspectMap::from_record(out.data, out.present, out.confidence);
  const auto trial = [&](const std::vector<uint8_t>& data) {
    // Forked rather than advanced, exactly as the repair pass forks it: a
    // candidate byte may define a macro or designate a set, and none of that
    // may outlive the trial that considered it.
    NaplpsLinter scratch = context;
    return naplps_lint_grade(scratch.lint(data, suspects).findings);
  };

  NaplpsLintGrade baseline = trial(out.data);
  for (const VoteContest& contest : contests) {
    const uint8_t original = out.data[contest.position];
    uint8_t chosen = original;
    NaplpsLintGrade best;
    std::size_t attained_best = 0;

    for (const uint8_t candidate : contest.candidates) {
      if (candidate == original) {
        continue;  // What the weights chose is the baseline, already graded.
      }
      out.data[contest.position] = candidate;
      const NaplpsLintGrade grade = trial(out.data);
      out.data[contest.position] = original;

      if (attained_best == 0 || grade < best) {
        best = grade;
        chosen = candidate;
        attained_best = 1;
      } else if (grade == best) {
        ++attained_best;
      }
    }

    // Strictly better than what the weights chose, and better than every other
    // candidate: two readings the grammar likes equally are two readings it has
    // nothing to say about.
    if (attained_best != 1 || !(best < baseline)) {
      continue;
    }
    out.data[contest.position] = chosen;
    baseline = best;
    ++out.positions_adjudicated;
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
    out.present[to] = 1;
    if (from < copy.confidence.size()) {
      out.confidence[to] = copy.confidence[from];
    }
  }
  return out;
}

/// One pass of the vote. |included| is either null — every copy votes — or one
/// flag per copy, zero for the copies the outlier pass ruled out.
NabtsVoteResult vote_over(const std::vector<NabtsRecordCopy>& copies,
                          const uint8_t* included,
                          const NabtsVoteOptions& options) {
  NabtsVoteResult out;
  const std::size_t length = voted_length(copies, included);
  out.data.assign(length, 0);
  out.present.assign(length, 0);
  out.confidence.assign(length, 0);
  std::vector<VoteContest> contests;

  // One pass over the copies per position, tallying per byte value. The
  // accumulators are epoch-marked by position so none of them has to be cleared
  // between positions.
  std::array<uint64_t, 256> weight_of{};
  std::array<std::size_t, 256> support_of{};
  std::array<std::size_t, 256> newest_of{};
  std::array<std::size_t, 256> seen_at{};  // epoch: position + 1, 0 = never
  std::array<uint8_t, 256> distinct{};

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
        support_of[value] = 0;
        newest_of[value] = 0;
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
      ++support_of[value];
      newest_of[value] = copy;  // copies are held oldest first
    }

    bool best_clean = false;
    // NUL stands where no copy had this byte at all. X3.110 §6.1.4 makes it a
    // transparent control with no presentation effect, so a hole costs the
    // drawing nothing beyond the bytes that were actually lost — which is the
    // point of holding it open rather than letting the record close up over it.
    uint8_t best = 0;
    uint64_t best_weight = 0;
    std::size_t best_newest = 0;
    for (std::size_t k = 0; k < distinct_count; ++k) {
      const uint8_t value = distinct[k];
      const bool clean = teletext_odd_parity_valid(value);
      if (k > 0) {
        if (best_clean && !clean) {
          continue;  // a byte known corrupt never beats a parity-clean one
        }
        if (clean == best_clean && !(weight_of[value] > best_weight ||
                                     (weight_of[value] == best_weight &&
                                      newest_of[value] > best_newest))) {
          continue;
        }
      }
      best = value;
      best_clean = clean;
      best_weight = weight_of[value];
      best_newest = newest_of[value];
    }
    out.data[position] = best;

    if (distinct_count == 0) {
      // No copy delivered this byte. The NUL stands for it, and the mask says
      // so rather than letting a filler pass for a reading.
      continue;
    }
    out.present[position] = 1;

    // Weight behind a value the parity check has already ruled out is not
    // weight against the one that won, so both the confidence and the tie test
    // are taken within the winner's own class.
    uint64_t class_weight = 0;
    uint64_t rival_weight = 0;
    for (std::size_t k = 0; k < distinct_count; ++k) {
      const uint8_t value = distinct[k];
      if (teletext_odd_parity_valid(value) != best_clean) {
        continue;
      }
      class_weight += weight_of[value];
      if (value != best) {
        rival_weight = std::max(rival_weight, weight_of[value]);
      }
    }

    // What the recovery ends up making of the byte: the mean confidence of the
    // copies that voted for it, reduced by the share of the weight that voted
    // against it. A byte one copy decided on its own therefore carries that
    // detector's own figure through unchanged, and a byte every copy agreed on
    // comes out at whatever they agreed at.
    const uint64_t mean =
        best_weight / std::max<std::size_t>(1, support_of[best]);
    const uint64_t confidence =
        class_weight > 0 ? (mean * best_weight) / class_weight : 0;
    out.confidence[position] =
        static_cast<uint8_t>(std::min<uint64_t>(255, confidence));

    if (rival_weight * 100 < best_weight * kNabtsVoteTiePercent) {
      continue;
    }

    // Level: what stands here was settled by recency rather than by evidence,
    // so nothing actually chose it. Reported at no confidence whatever the
    // arithmetic above made of it, which is what puts it in front of the repair
    // pass later even if the grammar has nothing to say about it now.
    ++out.positions_contested;
    out.confidence[position] = 0;
    if (contests.size() >= kNabtsMaxAdjudicatedPositions) {
      continue;
    }

    std::vector<std::pair<uint64_t, uint8_t>> level;
    for (std::size_t k = 0; k < distinct_count; ++k) {
      const uint8_t value = distinct[k];
      if (teletext_odd_parity_valid(value) == best_clean &&
          weight_of[value] * 100 >= best_weight * kNabtsVoteTiePercent) {
        level.emplace_back(weight_of[value], value);
      }
    }
    // Heaviest first, and by value where even that is level, so which
    // candidates are put to the grammar does not depend on the order the copies
    // happened to arrive in.
    std::sort(level.begin(), level.end(), [](const auto& a, const auto& b) {
      return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    if (level.size() > kNabtsMaxVoteCandidates) {
      level.resize(kNabtsMaxVoteCandidates);
    }

    VoteContest contest;
    contest.position = position;
    contest.candidates.reserve(level.size());
    for (const auto& entry : level) {
      contest.candidates.push_back(entry.second);
    }
    contests.push_back(std::move(contest));
  }

  if (options.grammar_assisted && !contests.empty()) {
    adjudicate_contests(contests, options, out);
  }
  return out;
}

}  // namespace

NabtsVoteResult nabts_vote_record(const std::vector<NabtsRecordCopy>& copies,
                                  const NabtsVoteOptions& options) {
  if (copies.empty()) {
    return {};
  }
  if (copies.size() == 1) {
    // A vote of one is the copy itself, and what the recovery knew about that
    // copy is what it knows about the record.
    NabtsVoteResult out;
    out.data = copies.front().data;
    out.present = copies.front().present;
    out.confidence = copies.front().confidence;
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
  const NabtsVoteResult provisional = vote_over(copies, nullptr, {});
  std::vector<NabtsRecordCopy> slid;
  std::vector<uint8_t> included(copies.size(), 1);
  std::size_t dropped = 0;
  for (std::size_t index = 0; index < copies.size(); ++index) {
    const RecordAgreement unslid = agreement(copies[index], provisional, 0);
    if (!unslid.outlier()) {
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
      continue;
    }
    included[index] = 0;
    ++dropped;
  }
  const std::vector<NabtsRecordCopy>& voting = slid.empty() ? copies : slid;
  if (dropped == 0 || dropped * 2 >= copies.size()) {
    return vote_over(voting, nullptr, options);
  }
  return vote_over(voting, included.data(), options);
}

std::vector<uint8_t> nabts_vote_record_data(
    const std::vector<NabtsRecordCopy>& copies) {
  return nabts_vote_record(copies).data;
}

NabtsRecordCatalogue::NabtsRecordCatalogue(std::size_t max_records,
                                           std::size_t max_copies)
    : max_records_(std::max<std::size_t>(1, max_records)),
      max_copies_(std::max<std::size_t>(1, max_copies)) {}

bool NabtsRecordCatalogue::copy_is_better(const Entry& entry,
                                          const NabtsMessage& message) {
  const bool candidate_intact = message.complete && message.intact;
  if (candidate_intact != entry.kept_copy_intact) {
    // A clean copy always beats a damaged one, and never the other way round.
    return candidate_intact;
  }
  // Equal quality: the longer copy carries more of the record. Equal length
  // keeps what is already there rather than churning between identical copies.
  return message.data.size() > entry.record.data.size();
}

void NabtsRecordCatalogue::take_copy(Entry& entry,
                                     const NabtsMessage& message) {
  NabtsCataloguedRecord& record = entry.record;
  record.record_type = message.type;
  record.caption = message.classification.caption;
  record.cyclic_marker = message.classification.cyclic_marker;
  record.priority = message.classification.priority;
  record.alarm = message.classification.alarm;
  record.update = message.classification.update;
  record.support_record = message.classification.support_record;
  record.support_needed = message.classification.support_needed;
  record.index = message.classification.index;
  record.more = message.classification.more;
  record.records_in_message = message.records;
  record.complete = message.complete;
  record.data = message.data;

  record.functions = render_functions(message.functions);
  record.has_more_address =
      more_extension_address(message.extensions, record.more_address);
  entry.kept_copy_intact = message.complete && message.intact;
}

void NabtsRecordCatalogue::add_copy(Entry& entry,
                                    const NabtsMessage& message) const {
  if (message.complete && message.intact) {
    // The record has arrived whole, so there is nothing left for a vote to
    // improve on and the copies held for one can go.
    entry.copies.clear();
    entry.copies.shrink_to_fit();
    return;
  }
  if (entry.kept_copy_intact) {
    return;  // an undamaged copy arrived earlier and is what will be shown
  }
  if (!message.aligned) {
    // Bytes that have moved cannot be compared position for position with the
    // rest, and voting them in would corrupt every position after the hole.
    return;
  }
  if (entry.copies.size() >= max_copies_) {
    // Oldest out, so the copies held stay the most recent — which is what the
    // vote falls back to when a position has nothing to choose between.
    entry.copies.erase(entry.copies.begin());
  }
  entry.copies.push_back(
      NabtsRecordCopy{message.data, message.present, message.confidence});
}

void NabtsRecordCatalogue::tally_flags(Entry& entry,
                                       const NabtsMessage& message) {
  const NabtsClassification& flags = message.classification;
  const std::array<bool, kClassificationFlags> set{
      flags.caption,        flags.cyclic_marker, flags.priority,
      flags.alarm,          flags.update,        flags.support_record,
      flags.support_needed, flags.index,         flags.more};
  for (std::size_t flag = 0; flag < kClassificationFlags; ++flag) {
    if (!set[flag]) {
      continue;
    }
    ++entry.flags_set[flag];
    if (message.identity_attested) {
      ++entry.flags_set_attested[flag];
    }
  }
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
}

void NabtsRecordCatalogue::merge(const NabtsMessage& message,
                                 uint64_t frame_id) {
  const Key key{message.channel, message.address.value, message.version};
  ++touch_counter_;

  auto it = records_.find(key);
  if (it == records_.end()) {
    Entry entry;
    entry.record.channel = message.channel;
    entry.record.address = message.address.value;
    entry.record.address_text = message.address.text();
    entry.record.channel_text =
        fmt::format("{:03X}/{}", message.channel, message.address.text());
    entry.record.version = message.version;
    entry.record.reserved_purpose = message.reserved_purpose;
    entry.record.first_seen_frame = frame_id;
    take_copy(entry, message);
    it = records_.emplace(key, std::move(entry)).first;
  } else if (copy_is_better(it->second, message)) {
    take_copy(it->second, message);
  }

  Entry& entry = it->second;
  add_copy(entry, message);
  tally_flags(entry, message);
  entry.record.last_seen_frame = frame_id;
  ++entry.record.times_seen;
  if (message.complete && message.intact) {
    ++entry.record.times_intact;
  }
  if (message.identity_attested) {
    ++entry.record.times_attested;
  }
  entry.last_touched = touch_counter_;

  enforce_bounds();
}

void NabtsRecordCatalogue::enforce_bounds() {
  while (records_.size() > max_records_) {
    auto oldest = records_.begin();
    for (auto candidate = records_.begin(); candidate != records_.end();
         ++candidate) {
      if (candidate->second.last_touched < oldest->second.last_touched) {
        oldest = candidate;
      }
    }
    records_.erase(oldest);
    truncated_ = true;
  }
}

namespace {

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
  VbiIdentityReconciliation out;
  out.identities_seen = static_cast<uint32_t>(records_.size());

  std::vector<Key> attested_keys;
  std::vector<VbiIdentityDigits> attested_digits;
  std::vector<Key> unattested_keys;
  for (const auto& [key, entry] : records_) {
    if (entry.record.times_attested > 0) {
      attested_keys.push_back(key);
      attested_digits.push_back(identity_digits(entry.record));
    } else {
      unattested_keys.push_back(key);
    }
  }
  out.identities_unattested = static_cast<uint32_t>(unattested_keys.size());

  if (!vbi_identity_reconciliation_applies(attested_keys.size())) {
    // Nothing arrived as transmitted, so there is no baseline and the rule has
    // nothing to say. Leaving the catalogue as recovered is the honest answer;
    // emptying it would be the confident one.
    out.withheld = out.identities_unattested > 0;
    return out;
  }

  for (const Key& key : unattested_keys) {
    auto it = records_.find(key);
    if (it == records_.end()) {
      continue;
    }
    Entry& entry = it->second;
    const auto neighbour = vbi_single_digit_neighbour(
        identity_digits(entry.record), attested_digits);
    if (neighbour.has_value()) {
      auto target = records_.find(attested_keys[*neighbour]);
      if (target != records_.end()) {
        fold_into(target->second, entry);  // |entry| is erased just below
        ++out.identities_folded;
        out.appearances_folded += entry.record.times_seen;
      } else {
        ++out.identities_dropped;
        out.appearances_dropped += entry.record.times_seen;
      }
    } else {
      ++out.identities_dropped;
      out.appearances_dropped += entry.record.times_seen;
    }
    records_.erase(it);
  }
  return out;
}

void NabtsRecordCatalogue::fold_into(Entry& target, Entry& misread) const {
  // The appearances were appearances of the target: the service brought that
  // record round and this recording misread its name on the way past. Counting
  // them there is what keeps times_seen a property of the service rather than
  // of the damage.
  target.record.times_seen += misread.record.times_seen;
  target.record.times_intact += misread.record.times_intact;
  for (std::size_t flag = 0; flag < kClassificationFlags; ++flag) {
    target.flags_set[flag] += misread.flags_set[flag];
  }
  target.record.first_seen_frame =
      std::min(target.record.first_seen_frame, misread.record.first_seen_frame);
  target.record.last_seen_frame =
      std::max(target.record.last_seen_frame, misread.record.last_seen_frame);

  // The copies are the reason to fold rather than discard: they are bytes of
  // the target record that were received, and the vote in records() is where
  // they pay for themselves. A target that already holds an undamaged copy has
  // nothing for them to improve on, which is add_copy()'s rule as well.
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

std::vector<NabtsCataloguedRecord> NabtsRecordCatalogue::records(
    bool grammar_assisted_vote) const {
  std::vector<NabtsCataloguedRecord> out;
  std::vector<const Entry*> entries;
  out.reserve(records_.size());
  entries.reserve(records_.size());
  // The map is keyed on {channel, address, version}, so iteration is already
  // the order a reader would list a service in.
  for (const auto& entry : records_) {
    out.push_back(entry.second.record);
    voted_flags(entry.second, out.back());
    entries.push_back(&entry.second);
  }

  // The state each channel's pages are graded against, which is its Support
  // Record's (§5.2.7.9). Built as the support records are voted, and empty for
  // a channel that carried none.
  std::map<uint16_t, NaplpsLinter> context;

  const auto vote_one = [&](std::size_t index) {
    NabtsCataloguedRecord& record = out[index];
    // Copies are held only where no undamaged one ever arrived, so this is the
    // recording that never gave up a clean copy of the record: combine what did
    // arrive rather than show whichever copy happened to be longest.
    const std::vector<NabtsRecordCopy>& copies = entries[index]->copies;
    if (copies.empty()) {
      return;
    }

    NabtsVoteOptions options;
    // An application record's data is function descriptors (§7.2.2) rather than
    // NAPLPS, so there is no grammar here to appeal to.
    options.grammar_assisted =
        grammar_assisted_vote && nabts_type_is_presentation(record.record_type);
    if (options.grammar_assisted && !record.support_record) {
      // A Support Record is itself presented from a general reset (§8.5), so it
      // is graded from one too; everything else is graded with its channel's
      // support definitions in force.
      const auto found = context.find(record.channel);
      if (found != context.end()) {
        options.context = &found->second;
      }
    }

    NabtsVoteResult voted = nabts_vote_record(copies, options);
    record.data = std::move(voted.data);
    record.data_present = std::move(voted.present);
    record.data_confidence = std::move(voted.confidence);
    record.vote_positions_contested = voted.positions_contested;
    record.vote_positions_adjudicated = voted.positions_adjudicated;
    record.copies_voted = static_cast<uint32_t>(copies.size());
    if (record.record_type == kNabtsRecordTypeApplication) {
      // The descriptors were rendered from one copy; the vote may have
      // recovered bytes that copy had wrong (§7.2.2).
      record.functions =
          render_functions(nabts_decode_application_record(record.data));
    }
  };

  // Support Records first, per Data Channel, for the reason
  // nabts_interpret_records() presents them first: a page invoking a macro
  // §5.2.7.9 has defined there is otherwise graded as invoking an undefined
  // one. Each is read into its channel's state once it has been voted, so the
  // pages of that channel are weighed against the definitions they will
  // actually be presented with.
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (!out[i].support_record) {
      continue;
    }
    vote_one(i);
    if (grammar_assisted_vote &&
        nabts_type_is_presentation(out[i].record_type)) {
      NaplpsLinter& linter = context[out[i].channel];
      linter.reset_decoder();
      (void)linter.lint(out[i].data);
    }
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (!out[i].support_record) {
      vote_one(i);
    }
  }

  return out;
}

std::string NabtsLintTotals::summary() const {
  if (records_linted == 0) {
    return {};
  }
  if (records_faulted == 0 && suspect_bytes == 0) {
    return fmt::format(
        "NAPLPS lint\n"
        "  Records:       {} presentation record(s), none faulted\n",
        records_linted);
  }

  std::string out = "NAPLPS lint\n";
  out += fmt::format(
      "  Records:       {} presentation record(s), {} faulted before repair, "
      "{} after\n",
      records_linted, records_faulted, records_faulted_after);
  out += fmt::format(
      "  Findings:      {} error(s) and {} warning(s) before, {} and {} "
      "after\n",
      errors_before, warnings_before, errors_after, warnings_after);
  out += fmt::format(
      "  Repairs:       {} byte(s) corrected, {} run(s) resynchronised, {} "
      "coordinate word(s) dropped\n",
      bytes_repaired, pdis_resynchronised, coordinate_words_dropped);
  // The line to read first. Everything above counts what happened to the
  // records; this counts what happened to the pages, which is what a reader is
  // actually looking at and the only measure that can say the pass did harm.
  out += fmt::format(
      "  Pages:         {} record(s) altered, {} of them drawing differently\n",
      records_altered, records_drawing_changed);
  out += fmt::format(
      "  Left alone:    {} byte(s) in doubt, {} of them known wrong, of which "
      "{} the grammar could not decide",
      suspect_bytes, bytes_offered, bytes_ambiguous);
  if (changes_declined_by_reach > 0) {
    out += fmt::format(", and {} change(s) refused for redrawing the page",
                       changes_declined_by_reach);
  }
  out += "\n";
  return out;
}

NabtsLintTotals nabts_lint_records(
    const std::vector<NabtsCataloguedRecord>& records) {
  NabtsLintTotals totals;
  std::map<uint16_t, NaplpsLintRepairer> repairers;

  const auto lint_one = [&](const NabtsCataloguedRecord& record) {
    if (!nabts_type_is_presentation(record.record_type) ||
        record.data.empty()) {
      return;
    }
    const NaplpsRepairResult result =
        repairers[record.channel].repair(record.data, suspects_of(record));
    ++totals.records_linted;
    if (result.summary.errors_before > 0 ||
        result.summary.warnings_before > 0) {
      ++totals.records_faulted;
    }
    if (result.summary.errors_after > 0 || result.summary.warnings_after > 0) {
      ++totals.records_faulted_after;
    }
    totals.errors_before += result.summary.errors_before;
    totals.warnings_before += result.summary.warnings_before;
    totals.errors_after += result.summary.errors_after;
    totals.warnings_after += result.summary.warnings_after;
    totals.suspect_bytes += result.summary.suspect_bytes;
    totals.bytes_offered += result.summary.bytes_offered;
    totals.bytes_repaired += result.summary.bytes_repaired;
    totals.bytes_ambiguous += result.summary.bytes_ambiguous;
    totals.changes_declined_by_reach +=
        result.summary.changes_declined_by_reach;
    totals.pdis_resynchronised += result.summary.pdis_resynchronised;
    totals.coordinate_words_dropped += result.summary.coordinate_words_dropped;
    if (result.summary.total_repairs() > 0) {
      ++totals.records_altered;
    }
    if (result.summary.drawing_changed) {
      ++totals.records_drawing_changed;
    }
  };

  // Support Record first, per Data Channel, for the reason
  // nabts_interpret_records() does it: a repairer that has not seen the macros
  // §5.2.7.9 puts there reads every invocation of one as damage.
  for (const NabtsCataloguedRecord& record : records) {
    if (record.support_record) {
      lint_one(record);
    }
  }
  for (const NabtsCataloguedRecord& record : records) {
    if (!record.support_record) {
      lint_one(record);
    }
  }
  return totals;
}

void nabts_interpret_records(std::vector<NabtsCataloguedRecord>& records,
                             NaplpsRenderGrid grid, bool repair) {
  NaplpsInterpreter interpreter(grid);

  // Repaired presentation code per record, parallel to |records| and empty
  // where a record was not repaired. Done in one sweep before anything is
  // interpreted, because a record is run more than once — as itself, as a
  // member of a More chain, and as the Support Record another page needs — and
  // a page assembled from two readings of the same bytes would be neither.
  std::vector<std::vector<uint8_t>> repaired(repair ? records.size() : 0);
  std::vector<NaplpsRepairSummary> repairs(repair ? records.size() : 0);
  if (repair) {
    // One repairer per Data Channel, and its Support Record first: §5.2.7.9 has
    // a page's macros defined there, and a repairer that has not seen them
    // reads every invocation of one as damage.
    std::map<uint16_t, NaplpsLintRepairer> repairers;
    const auto repair_record = [&](std::size_t index) {
      NabtsCataloguedRecord& record = records[index];
      if (!nabts_type_is_presentation(record.record_type) ||
          record.data.empty()) {
        return;
      }
      NaplpsRepairResult result =
          repairers[record.channel].repair(record.data, suspects_of(record));
      repaired[index] = std::move(result.data);
      repairs[index] = result.summary;

      // Per record, and only where something was actually done to it: a reader
      // asking why a page looks the way it does wants to see which records the
      // grammar touched, not a line for every record it left alone.
      const NaplpsRepairSummary& did = repairs[index];
      if (did.total_repairs() > 0 || did.errors_before > 0) {
        ORC_LOG_DEBUG(
            "NAPLPS lint: {} — {} error(s)/{} warning(s) before, {}/{} after; "
            "{} byte(s) corrected, {} run(s) resynchronised, {} coordinate "
            "word(s) dropped, {} change(s) refused for redrawing the page; "
            "{} of {} doubtful byte(s) known wrong; page {} ({} -> {} "
            "drawing(s))",
            record.channel_text, did.errors_before, did.warnings_before,
            did.errors_after, did.warnings_after, did.bytes_repaired,
            did.pdis_resynchronised, did.coordinate_words_dropped,
            did.changes_declined_by_reach, did.bytes_offered, did.suspect_bytes,
            did.drawing_changed ? "draws differently" : "unchanged",
            did.primitives_before, did.primitives_after);
      }
    };
    for (std::size_t i = 0; i < records.size(); ++i) {
      if (records[i].support_record) {
        repair_record(i);
      }
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
      if (!records[i].support_record) {
        repair_record(i);
      }
    }

    // And one line for the sweep, so a run says plainly whether the linter ran
    // and what it came to. This is the line to look for when a page is being
    // read as repaired and it is not obvious that anything was.
    NaplpsRepairSummary sweep;
    uint32_t records_repaired = 0;
    for (const NaplpsRepairSummary& one : repairs) {
      if (!one.ran) {
        continue;
      }
      sweep.suspect_bytes += one.suspect_bytes;
      sweep.bytes_repaired += one.bytes_repaired;
      sweep.bytes_ambiguous += one.bytes_ambiguous;
      sweep.pdis_resynchronised += one.pdis_resynchronised;
      sweep.coordinate_words_dropped += one.coordinate_words_dropped;
      sweep.errors_before += one.errors_before;
      sweep.errors_after += one.errors_after;
      if (one.total_repairs() > 0) {
        ++records_repaired;
      }
    }
    ORC_LOG_INFO(
        "NAPLPS lint: read {} record(s) with syntax repair on — {} record(s) "
        "changed, {} byte(s) corrected, {} run(s) resynchronised, {} "
        "coordinate word(s) dropped; errors {} -> {}, {} of {} doubtful "
        "byte(s) left undecided",
        records.size(), records_repaired, sweep.bytes_repaired,
        sweep.pdis_resynchronised, sweep.coordinate_words_dropped,
        sweep.errors_before, sweep.errors_after, sweep.bytes_ambiguous,
        sweep.suspect_bytes);
  }

  // The bytes to interpret for |entry|: the repaired reading where there is
  // one, and what was recovered otherwise.
  const auto code_of =
      [&](const NabtsCataloguedRecord* entry) -> const std::vector<uint8_t>& {
    const std::size_t index = static_cast<std::size_t>(entry - records.data());
    if (index < repaired.size() && !repaired[index].empty()) {
      return repaired[index];
    }
    return entry->data;
  };

  // §8.7.1.4: one Support Record per Data Channel, at address FFF with the
  // Support Record Flag set — the record that "contain[s] one or more macro
  // definitions ... invoked directly by other Presentation Records in the same
  // Data Channel". Where versions differ the highest wins, which iteration
  // order delivers for free: same channel and address, ascending version.
  std::map<uint16_t, const NabtsCataloguedRecord*> support_by_channel;
  for (const NabtsCataloguedRecord& record : records) {
    if (record.support_record &&
        nabts_type_is_presentation(record.record_type)) {
      support_by_channel[record.channel] = &record;
    }
  }

  // The latest catalogued version at each {channel, address}, which is what a
  // More chain is followed through: §5.2.7.6 addresses the next record of a
  // chain, not a particular version of it. Ascending iteration leaves the
  // highest version in place.
  std::map<std::pair<uint16_t, uint64_t>, const NabtsCataloguedRecord*> latest;
  for (const NabtsCataloguedRecord& record : records) {
    if (nabts_type_is_presentation(record.record_type)) {
      latest[{record.channel, record.address}] = &record;
    }
  }

  // Who names each record as its More Record. §7.3.4's receiver algorithm
  // consults an explicit header extension address first (§5.2.8.4) and the
  // More Flag's algorithmic long-address-plus-one second (§5.2.7.6). The map
  // runs successor -> predecessor, which is the direction a chain is walked
  // to find what a record is presented over.
  std::map<std::pair<uint16_t, uint64_t>, const NabtsCataloguedRecord*>
      predecessor_of;
  for (const auto& entry : latest) {
    const NabtsCataloguedRecord* candidate = entry.second;
    uint64_t successor = 0;
    if (candidate->has_more_address) {
      successor = candidate->more_address;
    } else if (!candidate->more ||
               !algorithmic_more_address(candidate->address, successor)) {
      continue;
    }
    if (successor == candidate->address) {
      continue;  // a record naming itself is damage, not a chain
    }
    predecessor_of[{candidate->channel, successor}] = candidate;
  }

  // §6.1: a presentation record's data is NAPLPS, so this is where it becomes
  // something a viewer can draw — once per record, on whatever the vote
  // settled on. An application record's data is function descriptors (§7.2.2),
  // rendered above, and running it as presentation code would draw nonsense.
  //
  // Each record is presented the way a receiver would present the page: the
  // general reset of §8.5, then — for a record whose Support-Needed Flag is
  // set (§5.2.7.9) — the channel's Support Record, whose macros, DRCS, texture
  // masks and colour map persist into the page, then the caption preset of
  // §5.2.7.3 where the Caption Flag asks for it, and then the record itself.
  //
  // A More Record is presented last of all of its chain: §5.2.7.8 has it
  // "presented after the completion of the presentation of the current
  // Record" — over the standing display, not over a cleared one. So a
  // record's predecessors are walked back through the links above, executed
  // in order with the display carried between them, and the record itself
  // drawn on top of what they left.
  for (NabtsCataloguedRecord& record : records) {
    if (!nabts_type_is_presentation(record.record_type)) {
      continue;
    }
    if (record.data.empty()) {
      // Nothing to run, so nothing to draw: a record whose every copy was lost
      // has no presentation code, and running it would only replace the page it
      // already carries with the empty one it would have produced anyway.
      continue;
    }

    // The chain back to its base, oldest first, excluding |record| itself.
    // The visited set is what ends the walk around a closed loop of links.
    std::vector<const NabtsCataloguedRecord*> prefix;
    std::set<uint64_t> visited{record.address};
    uint64_t address = record.address;
    bool ring = false;
    while (true) {
      const auto predecessor = predecessor_of.find({record.channel, address});
      if (predecessor == predecessor_of.end()) {
        break;
      }
      if (!visited.insert(predecessor->second->address).second) {
        // After the de-duplication above every record has at most one
        // predecessor and one successor, so a revisit can only be the walk
        // arriving back where it started: the chain is a ring.
        ring = true;
        break;
      }
      prefix.insert(prefix.begin(), predecessor->second);
      address = predecessor->second->address;
    }

    if (ring) {
      // A ring — a rotating set the service cycles through (§7.3.4 note on
      // the Random More function) — has no transmitted base, and each member
      // walking to a different stop would scatter one set across as many
      // single-member "chains". The smallest address is taken as the base by
      // every member alike, and the prefix trimmed to the members between it
      // and this record. |prefix| holds the other members in link order
      // ending at this record's own predecessor, so the trim is a suffix.
      size_t base_index = 0;
      uint64_t base = record.address;
      for (size_t i = 0; i < prefix.size(); ++i) {
        if (prefix[i]->address < base) {
          base = prefix[i]->address;
          base_index = i;
        }
      }
      if (base == record.address) {
        prefix.clear();
      } else {
        prefix.erase(prefix.begin(),
                     prefix.begin() + static_cast<ptrdiff_t>(base_index));
      }
      address = base;
    }

    record.chain_base_address = address;
    record.chain_position = static_cast<uint32_t>(prefix.size());

    bool needs_support = record.support_needed;
    bool caption = record.caption;
    for (const NabtsCataloguedRecord* member : prefix) {
      needs_support = needs_support || member->support_needed;
      caption = caption || member->caption;
    }

    interpreter.reset_decoder();
    if (needs_support && !record.support_record) {
      const auto support = support_by_channel.find(record.channel);
      if (support != support_by_channel.end()) {
        // Run for its definitions; what the support record itself drew is not
        // part of this page.
        (void)interpreter.run(code_of(support->second));
      }
    }
    if (caption) {
      interpreter.apply_caption_state();
    }
    bool keep_display = false;
    for (const NabtsCataloguedRecord* member : prefix) {
      (void)interpreter.run(code_of(member), keep_display);
      keep_display = true;
    }
    record.page = interpreter.run(code_of(&record), keep_display);

    // What the repair did is the reader's business as much as what the decode
    // did: a page drawn partly from guesses should say so.
    const std::size_t index =
        static_cast<std::size_t>(&record - records.data());
    if (index < repairs.size()) {
      naplps_stamp_repair_diagnostics(repairs[index], record.page.diagnostics);
    }
  }
}

}  // namespace orc
