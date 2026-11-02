/*
 * File:        teletext_page_catalogue.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Bounded teletext page catalogue implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_catalogue.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace orc {

uint64_t teletext_version_lost_packets(const TeletextPageSnapshot& page,
                                       uint64_t times_seen) {
  uint64_t lost = 0;
  const auto& arrivals = page.row_arrivals;
  // Row 0 is the header, which carries a live clock and is never squashed, so
  // its count is always zero and says nothing.
  for (std::size_t row = 1; row < arrivals.size(); ++row) {
    const auto arrived = static_cast<uint64_t>(std::max(0, arrivals[row]));
    // A row that never arrived is unknowable; more copies than appearances is
    // a row re-sent inside one transmission, not negative loss.
    if (arrived == 0 || arrived >= times_seen) {
      continue;
    }
    lost += times_seen - arrived;
  }
  return lost;
}

uint64_t teletext_subpage_lost_packets(
    const TeletextCataloguedSubPage& subpage) {
  // The assembly is the current version's, so its rows are measured against
  // that version's appearances rather than the sub-page's over every version.
  return teletext_version_lost_packets(subpage.page,
                                       subpage.version_times_seen);
}

TeletextPageCatalogue::TeletextPageCatalogue(std::size_t max_subpages,
                                             std::size_t max_subpages_per_page)
    : max_subpages_(max_subpages == 0 ? 1 : max_subpages),
      max_subpages_per_page_(
          max_subpages_per_page == 0 ? 1 : max_subpages_per_page) {}

void TeletextPageCatalogue::merge(const TeletextPageSnapshot& snapshot,
                                  uint64_t frame_id) {
  const std::pair<int, int> page_key{snapshot.magazine, snapshot.page_number};
  Entry& entry = pages_[page_key];

  // Sticky subtitle flag — the service telling the receiver which page carries
  // the subtitles. A page transmitted without C6 between captions is still the
  // subtitle page, and the flag belongs to the page rather than to whichever
  // sub-page of it happened to carry a caption.
  if (snapshot.subtitle) {
    entry.subtitle = true;
  }

  // The sub-code identifies which sub-page of the set this is (ETSI EN 300 706
  // §9.3.1.2, Annex A.1). It is the identity the row squasher combined its
  // copies under, so a sub-page here holds exactly what was combined for it.
  auto sub_it = entry.subpages.find(snapshot.subcode);
  const bool is_new = (sub_it == entry.subpages.end());
  if (is_new) {
    SubEntry fresh;
    fresh.subpage.subcode = snapshot.subcode;
    fresh.subpage.first_seen_frame = frame_id;
    sub_it = entry.subpages.emplace(snapshot.subcode, std::move(fresh)).first;
    ++subpage_count_;
  }

  SubEntry& sub_entry = sub_it->second;
  sub_entry.last_touched = ++touch_counter_;

  // The snapshot's content run is the version it shows (see
  // TeletextPageSnapshot::content_run).
  const auto [version_it, new_version] =
      sub_entry.versions.try_emplace(snapshot.content_run);
  VersionState& version = version_it->second;
  if (new_version) {
    version.version.first_seen_frame = frame_id;
    if (sub_entry.versions.size() > 1) {
      ++earlier_versions_;
    }
  }
  version.last_touched = touch_counter_;
  version.version.last_seen_frame = frame_id;
  version.version.page = snapshot;
  if (snapshot.header_field_index != version.counted_header_field) {
    version.counted_header_field = snapshot.header_field_index;
    ++version.version.times_seen;
  }
  sub_entry.current_run = snapshot.content_run;

  // The version's content is always replaced: with a row squasher attached
  // the decoder renders every snapshot from every copy of its run recovered so
  // far, so the newest assembly is the best one.
  sub_entry.subpage.last_seen_frame = frame_id;

  // One appearance per distinct header transmission. A rolling header (ETSI EN
  // 300 706 §9.3.1.4) closes the page mid-transmission and re-emits it under
  // the same header field index, which is what keeps this from counting the
  // same appearance twice.
  if (snapshot.header_field_index != sub_entry.counted_header_field) {
    sub_entry.counted_header_field = snapshot.header_field_index;
    ++sub_entry.subpage.times_seen;
    if (snapshot.identity_attested) {
      ++sub_entry.subpage.times_attested;
    }
  }

  if (new_version) {
    enforce_version_bounds(sub_entry);
  }

  if (is_new) {
    enforce_subpage_bounds(entry);
  }
}

void TeletextPageCatalogue::enforce_version_bounds(SubEntry& sub_entry) {
  // The least recently shown version of |entry| other than its current one.
  const auto oldest_version = [](SubEntry& entry) {
    auto oldest = entry.versions.end();
    for (auto it = entry.versions.begin(); it != entry.versions.end(); ++it) {
      if (it->first == entry.current_run) {
        continue;
      }
      if (oldest == entry.versions.end() ||
          it->second.last_touched < oldest->second.last_touched) {
        oldest = it;
      }
    }
    return oldest;
  };

  while (sub_entry.versions.size() > kMaxEarlierVersionsPerSubPage + 1) {
    sub_entry.versions.erase(oldest_version(sub_entry));
    ++sub_entry.subpage.versions_dropped;
    --earlier_versions_;
  }

  while (earlier_versions_ > kMaxEarlierVersions) {
    SubEntry* victim = nullptr;
    std::map<int, VersionState>::iterator victim_version;
    for (auto& [page_key, page_entry] : pages_) {
      for (auto& [subcode, candidate] : page_entry.subpages) {
        const auto version = oldest_version(candidate);
        if (version != candidate.versions.end() &&
            (victim == nullptr || version->second.last_touched <
                                      victim_version->second.last_touched)) {
          victim = &candidate;
          victim_version = version;
        }
      }
    }
    if (victim == nullptr) {
      break;  // cannot happen: the count is of versions some sub-page holds
    }
    victim->versions.erase(victim_version);
    ++victim->subpage.versions_dropped;
    --earlier_versions_;
  }
}

std::map<int, TeletextPageCatalogue::SubEntry>::iterator
TeletextPageCatalogue::oldest_subpage(Entry& entry) {
  auto oldest = entry.subpages.end();
  auto oldest_unattested = entry.subpages.end();
  for (auto it = entry.subpages.begin(); it != entry.subpages.end(); ++it) {
    if (oldest == entry.subpages.end() ||
        it->second.last_touched < oldest->second.last_touched) {
      oldest = it;
    }
    if (it->second.subpage.times_attested == 0 &&
        (oldest_unattested == entry.subpages.end() ||
         it->second.last_touched < oldest_unattested->second.last_touched)) {
      oldest_unattested = it;
    }
  }
  return oldest_unattested != entry.subpages.end() ? oldest_unattested : oldest;
}

void TeletextPageCatalogue::enforce_subpage_bounds(Entry& page_entry) {
  // Per-page cap first: a page whose sub-codes churn loses its own oldest
  // sub-pages rather than the rest of the service's.
  while (page_entry.subpages.size() > max_subpages_per_page_) {
    const auto oldest = oldest_subpage(page_entry);
    earlier_versions_ -= oldest->second.versions.size() - 1;
    page_entry.subpages.erase(oldest);
    --subpage_count_;
    ++subpages_dropped_;
    truncated_ = true;
  }

  while (subpage_count_ > max_subpages_) {
    auto oldest_page = pages_.begin();
    auto oldest_sub = oldest_subpage(oldest_page->second);
    for (auto page_it = pages_.begin(); page_it != pages_.end(); ++page_it) {
      if (page_it->second.subpages.empty()) {
        continue;
      }
      const auto candidate = oldest_subpage(page_it->second);
      if (candidate->second.last_touched < oldest_sub->second.last_touched) {
        oldest_page = page_it;
        oldest_sub = candidate;
      }
    }
    earlier_versions_ -= oldest_sub->second.versions.size() - 1;
    oldest_page->second.subpages.erase(oldest_sub);
    --subpage_count_;
    ++subpages_dropped_;
    truncated_ = true;
    // A page number is only in the catalogue for the sub-pages it carried.
    if (oldest_page->second.subpages.empty()) {
      pages_.erase(oldest_page);
      ++pages_dropped_;
    }
  }
}

namespace {

// A page's identity written out digit by digit, in transmission order: the
// magazine, the two page-number digits (ETSI EN 300 706 §9.3.1.1) and the four
// sub-code digits S1 to S4 (§9.3.1.2). Seven digits, each of which arrived in
// its own Hamming 8/4 byte and so can be moved on its own.
//
// The sub-code is unpacked back into the four fields the header carries rather
// than compared as the packed value TeletextPageSnapshot holds: S2 occupies
// three bits and S4 two, so a single digit of the packed form spans two of the
// transmitted fields and a single-digit test over it would be testing something
// the transmission never had.
VbiIdentityDigits page_identity_digits(int magazine, int page_number,
                                       int subcode) {
  VbiIdentityDigits digits;
  digits.reserve(7);
  digits.push_back(static_cast<uint8_t>(magazine & 0xF));
  digits.push_back(static_cast<uint8_t>((page_number >> 4) & 0xF));
  digits.push_back(static_cast<uint8_t>(page_number & 0xF));
  digits.push_back(static_cast<uint8_t>(subcode & 0xF));          // S1
  digits.push_back(static_cast<uint8_t>((subcode >> 4) & 0x7));   // S2
  digits.push_back(static_cast<uint8_t>((subcode >> 7) & 0xF));   // S3
  digits.push_back(static_cast<uint8_t>((subcode >> 11) & 0x3));  // S4
  return digits;
}

// Fraction of the characters both pages hold cleanly, on the display rows both
// received, that are the same — cells blank in both left aside — or NaN where
// there are none. The header row carries the page number and a clock.
double content_agreement(const TeletextPageSnapshot& a,
                         const TeletextPageSnapshot& b) {
  std::size_t compared = 0;
  std::size_t same = 0;
  for (std::size_t row = 1; row < TeletextPageSnapshot::kRows; ++row) {
    if (!a.row_received[row] || !b.row_received[row]) {
      continue;
    }
    for (std::size_t column = 0; column < TeletextPageSnapshot::kColumns;
         ++column) {
      const TeletextPageCell& x = a.cells[row][column];
      const TeletextPageCell& y = b.cells[row][column];
      if (x.parity_error || y.parity_error ||
          (x.character == 0x20 && y.character == 0x20)) {
        continue;
      }
      ++compared;
      same += x.character == y.character ? 1 : 0;
    }
  }
  return compared == 0
             ? std::numeric_limits<double>::quiet_NaN()
             : static_cast<double>(same) / static_cast<double>(compared);
}

}  // namespace

std::size_t TeletextPageCatalogue::drop_unrepeated_versions() {
  std::size_t dropped = 0;
  for (auto& [page_key, entry] : pages_) {
    for (auto& [subcode, sub_entry] : entry.subpages) {
      const bool any_repeated =
          std::any_of(sub_entry.versions.begin(), sub_entry.versions.end(),
                      [](const auto& version) {
                        return version.second.version.times_seen > 1;
                      });
      if (!any_repeated) {
        continue;
      }
      for (auto it = sub_entry.versions.begin();
           it != sub_entry.versions.end();) {
        if (it->second.version.times_seen > 1) {
          ++it;
          continue;
        }
        it = sub_entry.versions.erase(it);
        --earlier_versions_;
        ++dropped;
      }
      // The version shown last, of those left, is the current one.
      if (sub_entry.versions.count(sub_entry.current_run) == 0) {
        const auto latest = std::max_element(
            sub_entry.versions.begin(), sub_entry.versions.end(),
            [](const auto& a, const auto& b) {
              return a.second.last_touched < b.second.last_touched;
            });
        sub_entry.current_run = latest->first;
      }
    }
  }
  return dropped;
}

VbiIdentityReconciliation TeletextPageCatalogue::reconcile_identities() {
  // One identity per sub-page: the sub-code is part of what a header names, so
  // a misread S1 duplicates a sub-page inside a page number that is itself
  // perfectly attested.
  std::vector<std::pair<std::pair<int, int>, int>> located;
  std::vector<VbiIdentity> identities;
  std::size_t attested = 0;
  for (const auto& [page_key, entry] : pages_) {
    for (const auto& [subcode, sub_entry] : entry.subpages) {
      located.emplace_back(page_key, subcode);
      identities.push_back(
          {page_identity_digits(page_key.first, page_key.second, subcode),
           sub_entry.subpage.times_attested > 0, sub_entry.subpage.times_seen});
      attested += identities.back().attested ? 1 : 0;
    }
  }
  VbiIdentityReconciliation out;
  out.identities_seen = static_cast<uint32_t>(identities.size());
  out.identities_unattested =
      static_cast<uint32_t>(identities.size() - attested);

  if (!vbi_identity_reconciliation_applies(attested)) {
    // Nothing arrived as transmitted, so there is no baseline and the rule has
    // nothing to say. Leaving the catalogue as recovered is the honest answer;
    // emptying it would be the confident one.
    out.withheld = out.identities_unattested > 0;
    return out;
  }

  const auto sub_entry = [&](std::size_t i) -> SubEntry& {
    return pages_.at(located[i].first).subpages.at(located[i].second);
  };
  // A misreading carries whichever version of its page was on air when it was
  // made, so two sub-pages agree as closely as their closest versions do.
  const auto verdicts =
      vbi_reconcile_identities(identities, [&](std::size_t a, std::size_t b) {
        double best = std::numeric_limits<double>::quiet_NaN();
        for (const auto& [run_a, version_a] : sub_entry(a).versions) {
          for (const auto& [run_b, version_b] : sub_entry(b).versions) {
            const double agree = content_agreement(version_a.version.page,
                                                   version_b.version.page);
            if (!(agree <= best)) {
              best = agree;  // a measured agreement always replaces NaN
            }
          }
        }
        return best;
      });
  for (std::size_t i = 0; i < located.size(); ++i) {
    if (identities[i].attested) {
      continue;
    }
    auto page_it = pages_.find(located[i].first);
    auto sub_it = page_it->second.subpages.find(located[i].second);
    const TeletextCataloguedSubPage& misread = sub_it->second.subpage;
    switch (verdicts[i].action) {
      case VbiIdentityVerdict::Action::kKeep:
        ++out.identities_kept;
        out.appearances_kept += misread.times_seen;
        continue;
      case VbiIdentityVerdict::Action::kFold: {
        // The appearances were appearances of the target: the carousel brought
        // that page round and this recording misread its number on the way
        // past. Its rows stay behind — the squasher combined them under the
        // misread number, so there is nothing here to merge — but counting the
        // appearances keeps times_seen a property of the service rather than
        // of the damage. The target is attested or kept, so never erased here.
        TeletextCataloguedSubPage& kept = sub_entry(verdicts[i].into).subpage;
        kept.times_seen += misread.times_seen;
        kept.first_seen_frame =
            std::min(kept.first_seen_frame, misread.first_seen_frame);
        kept.last_seen_frame =
            std::max(kept.last_seen_frame, misread.last_seen_frame);
        ++out.identities_folded;
        out.appearances_folded += misread.times_seen;
        break;
      }
      case VbiIdentityVerdict::Action::kDrop:
        ++out.identities_dropped;
        out.appearances_dropped += misread.times_seen;
        break;
    }
    earlier_versions_ -= sub_it->second.versions.size() - 1;
    page_it->second.subpages.erase(sub_it);
    --subpage_count_;
    // A page number is only in the catalogue for the sub-pages it carried.
    if (page_it->second.subpages.empty()) {
      pages_.erase(page_it);
    }
  }
  return out;
}

std::vector<TeletextCataloguedPage> TeletextPageCatalogue::pages() const {
  std::vector<TeletextCataloguedPage> out;
  out.reserve(pages_.size());
  for (const auto& [key, entry] : pages_) {
    if (entry.subpages.empty()) {
      continue;  // cannot happen: a page is erased with its last sub-page
    }
    TeletextCataloguedPage page;
    page.magazine = key.first;
    page.page_number = key.second;
    page.subtitle = entry.subtitle;
    page.subpages.reserve(entry.subpages.size());

    // The page-level figures cover the whole set, so that a table of pages
    // reads the same whether or not a page turns out to be a multi-page set.
    bool first = true;
    for (const auto& [subcode, sub_entry] : entry.subpages) {
      TeletextCataloguedSubPage subpage = sub_entry.subpage;
      // The page on show is the version shown last; the others are listed
      // oldest first.
      const auto current = sub_entry.versions.find(sub_entry.current_run);
      if (current != sub_entry.versions.end()) {
        subpage.page = current->second.version.page;
        subpage.version_first_seen_frame =
            current->second.version.first_seen_frame;
        subpage.version_times_seen = current->second.version.times_seen;
      }
      subpage.earlier_versions.clear();
      for (const auto& [run, version] : sub_entry.versions) {
        if (run == sub_entry.current_run) {
          continue;
        }
        TeletextPageVersion earlier = version.version;
        earlier.lost_packets =
            teletext_version_lost_packets(earlier.page, earlier.times_seen);
        subpage.earlier_versions.push_back(std::move(earlier));
      }
      std::sort(
          subpage.earlier_versions.begin(), subpage.earlier_versions.end(),
          [](const TeletextPageVersion& lhs, const TeletextPageVersion& rhs) {
            return lhs.first_seen_frame < rhs.first_seen_frame;
          });
      page.times_seen += subpage.times_seen;
      page.times_attested += subpage.times_attested;
      page.first_seen_frame =
          first ? subpage.first_seen_frame
                : std::min(page.first_seen_frame, subpage.first_seen_frame);
      page.last_seen_frame =
          first ? subpage.last_seen_frame
                : std::max(page.last_seen_frame, subpage.last_seen_frame);
      first = false;
      page.subpages.push_back(std::move(subpage));
    }
    out.push_back(std::move(page));
  }
  return out;
}

}  // namespace orc
