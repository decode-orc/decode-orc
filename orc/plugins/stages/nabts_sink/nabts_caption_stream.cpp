/*
 * File:        nabts_caption_stream.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The caption service as it arrived (CEA-516 §7.3.10)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_caption_stream.h"

#include <algorithm>
#include <utility>

#include "nabts_record_catalogue.h"

namespace orc {

NabtsCaptionStream::NabtsCaptionStream(std::size_t max_captions,
                                       std::size_t max_copies)
    : max_captions_(max_captions),
      max_copies_(std::max<std::size_t>(1, max_copies)) {}

bool NabtsCaptionStream::accepts(const NabtsMessage& message) const {
  if (!nabts_type_is_presentation(message.type)) {
    // An application record's data is function descriptors (§7.2.2), which no
    // receiver would show as a caption whatever its flags said.
    return false;
  }
  const bool noncyclic = message.type == kNabtsRecordTypeNoncyclicPresentation;
  const bool caption_address =
      caption_addresses_.count({message.channel, message.address.value}) > 0;
  if (message.classification.caption) {
    // §5.2.7.3. A flag read off a header that needed correcting is believed
    // where the rest of the header agrees with it — the record type captioning
    // is sent as (§5.2.2.3 makes it type 1), or an address the caption service
    // is using — and not otherwise: a burst sets a Hamming 8/4 bit as silently
    // as it clears one, and a cyclic page is not to leave the catalogue on one
    // misread bit.
    return message.identity_attested || noncyclic || caption_address;
  }
  // A caption whose flag byte was corrected clear: non-cyclic, from a damaged
  // header, at an address the caption service is already using.
  return noncyclic && !message.identity_attested && caption_address;
}

NabtsCaptionStream::Run NabtsCaptionStream::open_run(
    const NabtsMessage& message, uint64_t frame_id) {
  Run run;
  run.sequence = next_sequence_++;
  NabtsCataloguedRecord& record = run.record;
  nabts_take_identity(record, message, frame_id);
  record.chain_base_address = message.address.value;
  record.record_type = message.type;
  record.data = message.data;
  record.records_in_message = message.records;
  record.complete = message.complete;
  run.kept_copy_intact = message.complete && message.intact;
  return run;
}

void NabtsCaptionStream::extend_run(Run& run, const NabtsMessage& message,
                                    uint64_t frame_id) const {
  NabtsCataloguedRecord& record = run.record;
  const bool intact = message.complete && message.intact;

  // Which copy stands for the caption, as in the catalogue.
  if (record.times_seen > 0 &&
      nabts_copy_is_better(message, run.kept_copy_intact, record.data.size())) {
    record.record_type = message.type;
    record.data = message.data;
    record.records_in_message = message.records;
    record.complete = message.complete;
    run.kept_copy_intact = intact;
  }

  // The copies voted: this caption's own and no other's. §7.3.10.1 changes the
  // version with the content, so a copy of another version is another caption
  // however alike the two are.
  nabts_offer_copy(run.copies, message, run.kept_copy_intact, max_copies_);

  // The flags a caption carries are read off its arrivals: the Update Flag
  // §7.3.10.1 has every caption record set, and the others a reader may want.
  const NabtsClassification& flags = message.classification;
  record.caption = true;
  record.update = record.update || flags.update;
  record.last_seen_frame = frame_id;
  ++record.times_seen;
  record.times_intact += intact ? 1 : 0;
  record.times_attested += message.identity_attested ? 1 : 0;
}

NabtsCataloguedRecord NabtsCaptionStream::finish(const Run& run) {
  NabtsCataloguedRecord record = run.record;
  if (!run.copies.empty() && !run.kept_copy_intact) {
    // No copy of this caption arrived whole, so its data is what its own copies
    // agree on (see nabts_vote_record()).
    NabtsVoteResult voted = nabts_vote_record(run.copies);
    record.data = std::move(voted.data);
    record.data_present = std::move(voted.present);
    record.copies_voted = static_cast<uint32_t>(run.copies.size());
  }
  return record;
}

void NabtsCaptionStream::close(const Run& run) {
  closed_.push_back(finish(run));
}

void NabtsCaptionStream::add(const NabtsMessage& message, uint64_t frame_id) {
  const Address address{message.channel, message.address.value};
  caption_addresses_.insert(address);

  auto it = open_.find(address);
  if (it != open_.end() && it->second.record.version == message.version) {
    extend_run(it->second, message, frame_id);
    return;
  }

  // A new version at this address is a new caption (§7.3.10.1), and the one
  // before it is complete: nothing more of it will arrive.
  if (it != open_.end()) {
    close(it->second);
    open_.erase(it);
  } else if (open_.size() >= kMaxOpenAddresses) {
    auto oldest = open_.begin();
    for (auto candidate = open_.begin(); candidate != open_.end();
         ++candidate) {
      if (candidate->second.sequence < oldest->second.sequence) {
        oldest = candidate;
      }
    }
    close(oldest->second);
    open_.erase(oldest);
  }

  if (closed_.size() + open_.size() >= max_captions_) {
    // Past the bound. Each caption is counted once, however many copies of it
    // arrive after the first.
    const auto refused = refused_version_.find(address);
    if (refused == refused_version_.end() ||
        refused->second != message.version) {
      refused_version_[address] = message.version;
      ++dropped_;
    }
    return;
  }
  Run run = open_run(message, frame_id);
  extend_run(run, message, frame_id);
  open_.emplace(address, std::move(run));
}

std::vector<NabtsCataloguedRecord> NabtsCaptionStream::captions() const {
  std::vector<NabtsCataloguedRecord> out = closed_;
  out.reserve(closed_.size() + open_.size());
  for (const auto& entry : open_) {
    out.push_back(finish(entry.second));
  }
  // Transmission order. Captions at different addresses interleave, and one is
  // closed only when the next arrives at its own address, so the order they
  // were closed in is not the order they began in.
  std::stable_sort(
      out.begin(), out.end(),
      [](const NabtsCataloguedRecord& lhs, const NabtsCataloguedRecord& rhs) {
        return lhs.first_seen_frame < rhs.first_seen_frame;
      });
  return out;
}

}  // namespace orc
