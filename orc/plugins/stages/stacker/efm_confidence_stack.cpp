/*
 * File:        efm_confidence_stack.cpp
 * Module:      orc-core
 * Purpose:     Sync-anchored confidence-weighted combining of EFM t-values
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "efm_confidence_stack.h"

#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace orc {
namespace {

// IEC 60908 Section 20.2: an EFM channel frame is 588 channel bits long and
// opens with the frame sync pattern, two consecutive T11 runs.
constexpr int kChannelFrameBits = 588;
constexpr uint8_t kSyncSymbolT11 = 11;

// IEC 60908 Section 20.1: the (2,10) run-length limit of EFM bounds every run
// to T3..T11.
constexpr int kMinRunBits = 3;
constexpr int kMaxRunBits = 11;

// Whole-slot alignment offsets tried either side of the computed one, to
// absorb a source whose own first sync was missed or spurious.
constexpr int64_t kAlignmentSearchRadius = 1;

// Repair is a fixed-point loop over at most a few hundred boundaries; the cap
// only exists so a pathological slot cannot spin.
constexpr int kMaxRepairPasses = 64;

// A source's weight for one run, from the producer's doubt nibble. The band is
// deliberately narrow - 32 for a fully trusted run down to 17 for a fully
// distrusted one - so that two sources which agree (34) always outweigh one
// that does not (32), however sure of itself it is. A producer's doubt is one
// capture's opinion of its own reading; two independent captures agreeing is
// an observation, and the better evidence of the two. With a wider band a
// single confident capture can delete a transition that two doubtful ones both
// saw, which restructures the whole channel frame around it.
constexpr int kMinRunWeight = 17;
constexpr int kMaxRunWeight = kMinRunWeight + static_cast<int>(kEfmDoubtMax);

int run_weight(uint8_t packed) {
  return kMaxRunWeight - static_cast<int>(efm_doubt(packed));
}

// One complete 588-bit channel frame within a source's t-value stream.
struct Segment {
  size_t begin = 0;  // index of the first sync symbol
  size_t count = 0;  // t-values in the channel frame
};

// A source's t-values cut on its own sync grid. |slots| is ascending by slot
// number and holds only channel frames that measured exactly 588 bits.
struct SourceGrid {
  std::vector<std::pair<int64_t, Segment>> slots;
  int64_t first_sync_bit = 0;
  bool has_sync = false;
};

// Cut |packed| on its T11+T11 syncs and number the resulting channel frames
// from the source's own first sync. Slot numbers advance by the rounded
// number of channel frames between consecutive syncs, so a missed sync
// consumes the slot it should have occupied instead of shifting every later
// frame.
SourceGrid build_grid(const std::vector<uint8_t>& packed) {
  SourceGrid grid;
  const size_t count = packed.size();

  std::vector<int64_t> sync_bit;
  std::vector<size_t> sync_index;
  int64_t bit = 0;
  size_t i = 0;
  while (i + 1 < count) {
    if (efm_tvalue(packed[i]) == kSyncSymbolT11 &&
        efm_tvalue(packed[i + 1]) == kSyncSymbolT11) {
      sync_index.push_back(i);
      sync_bit.push_back(bit);
      // The sync pattern is two runs, so it cannot overlap the next one.
      bit += static_cast<int64_t>(efm_tvalue(packed[i])) +
             static_cast<int64_t>(efm_tvalue(packed[i + 1]));
      i += 2;
    } else {
      bit += static_cast<int64_t>(efm_tvalue(packed[i]));
      ++i;
    }
  }

  if (sync_index.empty()) {
    return grid;
  }

  grid.has_sync = true;
  grid.first_sync_bit = sync_bit.front();

  int64_t slot = 0;
  for (size_t j = 0; j + 1 < sync_index.size(); ++j) {
    const int64_t span = sync_bit[j + 1] - sync_bit[j];
    if (span == kChannelFrameBits) {
      grid.slots.push_back(
          {slot, Segment{sync_index[j], sync_index[j + 1] - sync_index[j]}});
    }
    slot += std::max<int64_t>(
        1, (span + kChannelFrameBits / 2) / kChannelFrameBits);
  }
  return grid;
}

// Locate the channel frame a source holds for |slot|, or nullptr.
const Segment* find_slot(const SourceGrid& grid, int64_t offset, int64_t slot) {
  const int64_t local = slot - offset;
  const auto it =
      std::lower_bound(grid.slots.begin(), grid.slots.end(), local,
                       [](const std::pair<int64_t, Segment>& entry,
                          int64_t value) { return entry.first < value; });
  if (it == grid.slots.end() || it->first != local) {
    return nullptr;
  }
  return &it->second;
}

// Signature of a channel frame: its first few data runs, one nibble each. Two
// captures of the same channel frame nearly always agree on these, and with
// only a few hundred slots in a video frame the collisions that do occur are
// swamped by the offset histogram below.
constexpr size_t kSyncRuns = 2;
constexpr int kSignatureRuns = 6;

// Fewest agreeing slots before a histogram peak is believed rather than the
// bit-position estimate.
constexpr size_t kMinAlignmentVotes = 8;

uint32_t slot_signature(const std::vector<uint8_t>& packed,
                        const Segment& segment) {
  if (segment.count < kSyncRuns + kSignatureRuns) {
    return 0;
  }
  uint32_t signature = 0;
  for (size_t k = 0; k < kSignatureRuns; ++k) {
    signature =
        (signature << 4) | efm_tvalue(packed[segment.begin + kSyncRuns + k]);
  }
  return signature;
}

// Whole-slot offset that maps a source's slot numbers onto the reference's.
//
// Captures of the same disc do not start at the same place on it. Aligning the
// sources by video frame gets the offset down to a fraction of a frame, but a
// fraction of a frame is still hundreds of channel frames - two Domesday
// captures of the same side sit a steady 1.123 frames apart, which is around
// 36 channel frames even after the whole-frame part is taken out. So the
// offset is found rather than assumed: every slot the two sides sign
// identically votes for the offset it implies, and the winning offset is the
// one the most slots agree on. That is one pass over each source's slots, not
// a search over candidate offsets.
int64_t align_to_reference(const std::vector<uint8_t>& source,
                           const SourceGrid& grid,
                           const std::vector<uint8_t>& reference,
                           const SourceGrid& reference_grid,
                           std::vector<std::pair<uint32_t, int64_t>>& index,
                           std::vector<int64_t>& votes) {
  const auto bit_gap =
      static_cast<double>(grid.first_sync_bit - reference_grid.first_sync_bit);
  const auto estimate = static_cast<int64_t>(
      std::llround(bit_gap / static_cast<double>(kChannelFrameBits)));

  index.clear();
  index.reserve(reference_grid.slots.size());
  for (const auto& entry : reference_grid.slots) {
    index.emplace_back(slot_signature(reference, entry.second), entry.first);
  }
  std::sort(index.begin(), index.end());

  votes.clear();
  for (const auto& entry : grid.slots) {
    const uint32_t signature = slot_signature(source, entry.second);
    if (signature == 0) {
      continue;
    }
    auto it =
        std::lower_bound(index.begin(), index.end(), signature,
                         [](const std::pair<uint32_t, int64_t>& row,
                            uint32_t value) { return row.first < value; });
    for (; it != index.end() && it->first == signature; ++it) {
      votes.push_back(it->second - entry.first);
    }
  }
  if (votes.size() < kMinAlignmentVotes) {
    return estimate;
  }

  std::sort(votes.begin(), votes.end());
  int64_t best = estimate;
  size_t best_count = 0;
  size_t run_start = 0;
  for (size_t i = 1; i <= votes.size(); ++i) {
    if (i == votes.size() || votes[i] != votes[run_start]) {
      const size_t count = i - run_start;
      if (count > best_count) {
        best_count = count;
        best = votes[run_start];
      }
      run_start = i;
    }
  }
  return best_count >= kMinAlignmentVotes ? best : estimate;
}

// One source's contribution to a slot.
struct Contribution {
  const std::vector<uint8_t>* packed = nullptr;
  Segment segment;
  bool is_reference = false;
};

using BitTally = std::array<int, kChannelFrameBits + 1>;

// Tally the votes for and against a transition at each interior bit offset of
// a channel frame. A source votes for an offset it has a transition at, with
// the weight of the weaker of the two runs that meet there, and against an
// offset that falls strictly inside one of its runs, with that run's weight.
void tally_votes(const std::vector<Contribution>& contributions, BitTally& yes,
                 BitTally& no, std::vector<bool>& reference_transition) {
  yes.fill(0);
  no.fill(0);
  reference_transition.assign(kChannelFrameBits + 1, false);

  for (const auto& contribution : contributions) {
    const std::vector<uint8_t>& packed = *contribution.packed;
    const Segment& segment = contribution.segment;
    int bit = 0;
    for (size_t k = 0; k < segment.count; ++k) {
      const uint8_t byte = packed[segment.begin + k];
      const int length = static_cast<int>(efm_tvalue(byte));
      const int weight = run_weight(byte);

      if (k > 0 && bit > 0 && bit < kChannelFrameBits) {
        const int previous = run_weight(packed[segment.begin + k - 1]);
        yes[static_cast<size_t>(bit)] += std::min(previous, weight);
        if (contribution.is_reference) {
          reference_transition[static_cast<size_t>(bit)] = true;
        }
      }
      for (int inside = bit + 1; inside < bit + length; ++inside) {
        if (inside > 0 && inside < kChannelFrameBits) {
          no[static_cast<size_t>(inside)] += weight;
        }
      }
      bit += length;
    }
  }
}

int vote_margin(const BitTally& yes, const BitTally& no, int bit) {
  return yes[static_cast<size_t>(bit)] - no[static_cast<size_t>(bit)];
}

// Doubt implied by how strongly a transition won its vote: none when the
// sources were unanimous, total when it scraped in on a bare majority or had
// to be forced in by run-length repair.
uint8_t agreement_doubt(const BitTally& yes, const BitTally& no, int bit) {
  const int total =
      yes[static_cast<size_t>(bit)] + no[static_cast<size_t>(bit)];
  if (total <= 0) {
    return kEfmDoubtMax;
  }
  const double margin = static_cast<double>(vote_margin(yes, no, bit)) /
                        static_cast<double>(total);
  const auto doubt = static_cast<int>(
      std::lround(static_cast<double>(kEfmDoubtMax) * (1.0 - margin)));
  return static_cast<uint8_t>(
      std::clamp(doubt, 0, static_cast<int>(kEfmDoubtMax)));
}

// Working buffers reused across every channel frame of a video frame. There
// are around 290 of them per frame and several thousand frames in a decode, so
// nothing in the per-slot path should be allocating.
struct VoteScratch {
  std::vector<bool> accepted = std::vector<bool>(kChannelFrameBits + 1, false);
  std::vector<int> boundaries;
  std::vector<bool> reference_transition;
  std::vector<size_t> cursor;
  std::vector<int> cursor_bit;
  std::vector<std::pair<uint32_t, int64_t>> signature_index;
  std::vector<int64_t> offset_votes;
};

// Force the accepted transitions into a legal T3..T11 run sequence. A run that
// came out too short loses its weaker bounding transition; one that came out
// too long regains the best transition the vote rejected inside it, if there
// is one that leaves both halves legal.
void repair_run_lengths(const BitTally& yes, const BitTally& no,
                        std::vector<bool>& accepted,
                        std::vector<int>& boundaries) {
  for (int pass = 0; pass < kMaxRepairPasses; ++pass) {
    boundaries.clear();
    boundaries.push_back(0);
    for (int bit = 1; bit < kChannelFrameBits; ++bit) {
      if (accepted[static_cast<size_t>(bit)]) {
        boundaries.push_back(bit);
      }
    }
    boundaries.push_back(kChannelFrameBits);

    bool changed = false;
    for (size_t r = 0; r + 1 < boundaries.size(); ++r) {
      const int start = boundaries[r];
      const int end = boundaries[r + 1];
      const int length = end - start;

      if (length < kMinRunBits) {
        // Drop whichever bounding transition the sources believed least. The
        // channel-frame boundaries are grid anchors and are never dropped.
        const bool can_drop_start = start > 0;
        const bool can_drop_end = end < kChannelFrameBits;
        if (!can_drop_start && !can_drop_end) {
          continue;
        }
        int victim = can_drop_start ? start : end;
        if (can_drop_start && can_drop_end) {
          victim = vote_margin(yes, no, start) <= vote_margin(yes, no, end)
                       ? start
                       : end;
        }
        accepted[static_cast<size_t>(victim)] = false;
        changed = true;
        break;
      }

      if (length > kMaxRunBits) {
        int best = -1;
        int best_margin = 0;
        for (int bit = start + kMinRunBits; bit <= end - kMinRunBits; ++bit) {
          if (accepted[static_cast<size_t>(bit)]) {
            continue;
          }
          const int margin = vote_margin(yes, no, bit);
          if (best < 0 || margin > best_margin) {
            best = bit;
            best_margin = margin;
          }
        }
        if (best < 0) {
          continue;
        }
        accepted[static_cast<size_t>(best)] = true;
        changed = true;
        break;
      }
    }

    if (!changed) {
      return;
    }
  }
}

// Walks each source's runs forward alongside the combined run sequence, so the
// question "which sources reported exactly this run, and how sure were they?"
// is answered in one pass over the channel frame rather than a search per run.
class ReportedDoubt {
 public:
  ReportedDoubt(const std::vector<Contribution>& contributions,
                VoteScratch& scratch)
      : contributions_(contributions),
        cursor_(scratch.cursor),
        cursor_bit_(scratch.cursor_bit) {
    cursor_.assign(contributions.size(), 0);
    cursor_bit_.assign(contributions.size(), 0);
  }

  // Lowest doubt among the sources that reported exactly this run. A run no
  // source reported - one the vote or the repair pass constructed - is
  // maximally doubtful, which is the honest answer and the one the CIRC
  // erasure machinery can act on. |start| must not decrease between calls.
  uint8_t of(int start, int end) {
    int lowest = -1;
    for (size_t c = 0; c < contributions_.size(); ++c) {
      const std::vector<uint8_t>& packed = *contributions_[c].packed;
      const Segment& segment = contributions_[c].segment;
      while (cursor_[c] < segment.count && cursor_bit_[c] < start) {
        cursor_bit_[c] +=
            static_cast<int>(efm_tvalue(packed[segment.begin + cursor_[c]]));
        ++cursor_[c];
      }
      if (cursor_[c] >= segment.count || cursor_bit_[c] != start) {
        continue;
      }
      const uint8_t byte = packed[segment.begin + cursor_[c]];
      if (cursor_bit_[c] + static_cast<int>(efm_tvalue(byte)) != end) {
        continue;
      }
      const int doubt = static_cast<int>(efm_doubt(byte));
      if (lowest < 0 || doubt < lowest) {
        lowest = doubt;
      }
    }
    return lowest < 0 ? kEfmDoubtMax : static_cast<uint8_t>(lowest);
  }

 private:
  const std::vector<Contribution>& contributions_;
  std::vector<size_t>& cursor_;
  std::vector<int>& cursor_bit_;
};

// Combine the channel frames two or more sources hold for one slot, appending
// the combined runs to |out|. Returns false, having appended nothing, when the
// vote could not be repaired into a legal T3..T11 channel frame; the caller
// then keeps the reference's own frame rather than emitting an illegal one.
bool vote_slot(const std::vector<Contribution>& contributions,
               VoteScratch& scratch, std::vector<uint8_t>& out) {
  BitTally yes;
  BitTally no;
  std::vector<bool>& reference_transition = scratch.reference_transition;
  tally_votes(contributions, yes, no, reference_transition);

  // A tie is no evidence either way, so it is settled the only honest way
  // available: keep what the source with the fewest dropouts read. Dropping
  // the transition instead would leave a run neither side reported.
  std::vector<bool>& accepted = scratch.accepted;
  accepted.assign(kChannelFrameBits + 1, false);
  for (int bit = 1; bit < kChannelFrameBits; ++bit) {
    const int margin = vote_margin(yes, no, bit);
    accepted[static_cast<size_t>(bit)] =
        margin > 0 ||
        (margin == 0 && reference_transition[static_cast<size_t>(bit)]);
  }
  repair_run_lengths(yes, no, accepted, scratch.boundaries);

  std::vector<int>& boundaries = scratch.boundaries;
  boundaries.clear();
  boundaries.push_back(0);
  for (int bit = 1; bit < kChannelFrameBits; ++bit) {
    if (accepted[static_cast<size_t>(bit)]) {
      boundaries.push_back(bit);
    }
  }
  boundaries.push_back(kChannelFrameBits);

  for (size_t r = 0; r + 1 < boundaries.size(); ++r) {
    const int length = boundaries[r + 1] - boundaries[r];
    if (length < kMinRunBits || length > kMaxRunBits) {
      return false;
    }
  }

  ReportedDoubt reported(contributions, scratch);
  for (size_t r = 0; r + 1 < boundaries.size(); ++r) {
    const int start = boundaries[r];
    const int end = boundaries[r + 1];

    // A run is trusted only where the sources agreed on both of its ends and
    // at least one of them vouched for the run itself.
    uint8_t doubt = reported.of(start, end);
    if (start > 0) {
      doubt = std::max(doubt, agreement_doubt(yes, no, start));
    }
    if (end < kChannelFrameBits) {
      doubt = std::max(doubt, agreement_doubt(yes, no, end));
    }

    out.push_back(efm_pack(static_cast<uint8_t>(end - start), doubt));
  }
  return true;
}

}  // namespace

std::vector<uint8_t> stack_efm_confidence(
    const std::vector<std::vector<uint8_t>>& sources, size_t reference) {
  if (sources.empty()) {
    return {};
  }
  if (reference >= sources.size()) {
    reference = 0;
  }
  // Nothing was stacked, so the source's own doubt still stands.
  if (sources.size() == 1) {
    return sources.front();
  }

  std::vector<SourceGrid> grids;
  grids.reserve(sources.size());
  for (const auto& source : sources) {
    grids.push_back(build_grid(source));
  }

  const SourceGrid& reference_grid = grids[reference];
  if (!reference_grid.has_sync || reference_grid.slots.empty()) {
    // Without a sync grid in the reference there is no axis to combine on.
    return sources[reference];
  }

  VoteScratch scratch;
  std::vector<int64_t> offsets(sources.size(), 0);
  for (size_t s = 0; s < sources.size(); ++s) {
    if (s == reference || !grids[s].has_sync) {
      continue;
    }
    offsets[s] = align_to_reference(sources[s], grids[s], sources[reference],
                                    reference_grid, scratch.signature_index,
                                    scratch.offset_votes);
  }

  // The output follows the reference stream run for run, with each of its
  // complete channel frames replaced by the combined one. Everything the sync
  // grid does not cover - the partial frames at each end, and any stretch
  // where the grid broke down - is carried over from the reference unchanged,
  // so the output is always a continuous stream of the reference's bit length.
  const std::vector<uint8_t>& base = sources[reference];
  std::vector<uint8_t> out;
  out.reserve(base.size());

  std::vector<Contribution> contributions;
  contributions.reserve(sources.size());

  size_t copied = 0;
  for (const auto& entry : reference_grid.slots) {
    const Segment& segment = entry.second;
    if (segment.begin < copied) {
      continue;
    }
    out.insert(out.end(), base.begin() + static_cast<std::ptrdiff_t>(copied),
               base.begin() + static_cast<std::ptrdiff_t>(segment.begin));

    contributions.clear();
    for (size_t s = 0; s < sources.size(); ++s) {
      if (s == reference) {
        contributions.push_back({&base, segment, true});
        continue;
      }
      if (!grids[s].has_sync) {
        continue;
      }
      const Segment* candidate = find_slot(grids[s], offsets[s], entry.first);
      if (candidate != nullptr) {
        contributions.push_back({&sources[s], *candidate, false});
      }
    }

    if (contributions.size() < 2 || !vote_slot(contributions, scratch, out)) {
      out.insert(
          out.end(), base.begin() + static_cast<std::ptrdiff_t>(segment.begin),
          base.begin() +
              static_cast<std::ptrdiff_t>(segment.begin + segment.count));
    }
    copied = segment.begin + segment.count;
  }

  out.insert(out.end(), base.begin() + static_cast<std::ptrdiff_t>(copied),
             base.end());
  return out;
}

}  // namespace orc
