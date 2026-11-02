/*
 * File:        nabts_salvage.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Salvage of the groups whose synchronizing packet was lost
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_salvage.h"

#include <orc/support/logging.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

#include "nabts_record.h"
#include "nabts_record_catalogue.h"
#include "naplps_dialect_model.h"
#include "naplps_interpreter.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

// The stretch of bytes an alignment is proposed by. Six seven-bit bytes and a
// twelve-bit channel make a 54-bit key.
constexpr std::size_t kStretch = 6;

// A stretch found in more places than this proposes nothing: a template's
// frame, a row of spaces, a run of NULs held open for a lost block.
constexpr std::size_t kMaxStretchHits = 32;

// Alignments checked in full for each run, the most proposed first.
constexpr std::size_t kAlignmentsTried = 4;

// Where two records both pass as a run's, the better must agree on this many
// percentage points more for the run to be the better's: two versions of a
// page differ in a handful of bytes, and a run one of them holds is as good a
// match for the other until a byte they differ in is reached.
constexpr double kClearMarginPercent = 2.0;

// A run shorter than this is not judged: a packet or two is as likely a
// misaddressed packet as the end of a group.
constexpr uint32_t kMinRunPackets = 3;

// A run no record holds and no other run matches is listed as a page of its
// own only when this much of it passes its parity: a page seen once must
// carry its own evidence of being one. Length is no part of it past the
// packets any run needs (kMinRunPackets): a run of three clean packets on a
// damaged recording is as often a story's paragraph or a forecast as a run of
// eight, and a run of misaddressed packets or of a record that is no
// presentation code fails its parity in places, or reads unlike the
// service's pages (kDialectFloorPercentile).
constexpr std::size_t kLonePageMinUsablePercent = 90;

// How far into a salvaged page its presentation code is searched for the
// first shift, which says what set the bytes before it were in: four blocks.
constexpr std::size_t kResumeSearchBytes = 112;

// A page none of the records holds is listed only where its code reads, byte
// pair by byte pair, about as a page of the service reads (see
// NaplpsDialectModel): at least as well as the record at this share of the
// way up the channel's records, less this margin in nats a byte. Fragments of
// a badly damaged tape that only agree with each other — operand bytes read
// as text, a run of noise both copies took — read far worse than any page.
constexpr std::size_t kDialectFloorPercentile = 10;
constexpr double kDialectFloorMargin = 0.25;
// Records a channel needs before its dialect is a floor to judge by.
constexpr std::size_t kDialectMinRecords = 8;

// SO and SI (X3.110 §6.1.3): SO invokes the picture description set and SI
// the primary set, so the first of them a page holds says which set the bytes
// before it were read in. SO has odd parity as it stands (§3.3).
constexpr uint8_t kShiftOut = 0x0E;
constexpr uint8_t kShiftIn = 0x0F;
// ESC (X3.110 §6.1.3), which with the bytes after it designates a set.
constexpr uint8_t kEscape = 0x1B;

/// Whether byte |i| arrived and passes §3.3's odd parity.
bool usable(const std::vector<uint8_t>& data,
            const std::vector<uint8_t>& present, std::size_t i) {
  return (present.empty() || present[i] != kNabtsByteLost) &&
         teletext_odd_parity_valid(data[i]);
}

/// Whether the |kStretch| bytes from |i| are all usable().
bool usable_stretch(const std::vector<uint8_t>& data,
                    const std::vector<uint8_t>& present, std::size_t i) {
  for (std::size_t k = 0; k < kStretch; ++k) {
    if (!usable(data, present, i + k)) {
      return false;
    }
  }
  return true;
}

uint64_t stretch_key(uint16_t channel, const uint8_t* bytes) {
  uint64_t key = channel & 0xFFFu;
  for (std::size_t k = 0; k < kStretch; ++k) {
    key = (key << 7) | (bytes[k] & 0x7Fu);
  }
  return key;
}

/// Where a run sits in something it is set against: which one, and the
/// position of the run's first byte there.
struct Placement {
  uint32_t target = 0;
  int64_t offset = 0;
};

/// The agreement of a run with something at one placement.
struct Agreement {
  std::size_t judged = 0;
  std::size_t agreed = 0;

  bool passes() const {
    return judged >= kNabtsSalvageMinJudged &&
           agreed * 100 >= judged * kNabtsSalvageMinAgreePercent;
  }
  double percent() const {
    return judged == 0 ? 0.0
                       : 100.0 * static_cast<double>(agreed) /
                             static_cast<double>(judged);
  }
};

/// Stretches of the things runs are set against, keyed by channel and bytes.
using StretchIndex =
    std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, int64_t>>>;

/// The placements |index| proposes for |run|, the most proposed first.
std::vector<Placement> proposals(const StretchIndex& index,
                                 const NabtsLooseRun& run) {
  std::map<std::pair<uint32_t, int64_t>, uint32_t> votes;
  if (run.data.size() < kStretch) {
    return {};
  }
  for (std::size_t i = 0; i + kStretch <= run.data.size(); ++i) {
    if (!usable_stretch(run.data, run.present, i)) {
      continue;
    }
    const auto found = index.find(stretch_key(run.channel, &run.data[i]));
    if (found == index.end() || found->second.size() > kMaxStretchHits) {
      continue;
    }
    for (const auto& [target, position] : found->second) {
      ++votes[{target, position - static_cast<int64_t>(i)}];
    }
  }
  std::vector<std::pair<uint32_t, Placement>> ranked;
  ranked.reserve(votes.size());
  for (const auto& [placement, count] : votes) {
    ranked.push_back({count, Placement{placement.first, placement.second}});
  }
  // Most proposed first; equal counts in placement order, so the choice does
  // not depend on the order the map was filled in.
  std::stable_sort(
      ranked.begin(), ranked.end(),
      [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<Placement> out;
  for (std::size_t i = 0; i < ranked.size() && i < kAlignmentsTried; ++i) {
    out.push_back(ranked[i].second);
  }
  return out;
}

/// |run| against |data| (with |present|) at |offset|, over the bytes both hold
/// passing their parity, compared as seven-bit values.
Agreement agreement_at(const NabtsLooseRun& run,
                       const std::vector<uint8_t>& data,
                       const std::vector<uint8_t>& present, int64_t offset) {
  Agreement out;
  for (std::size_t i = 0; i < run.data.size(); ++i) {
    const int64_t at = offset + static_cast<int64_t>(i);
    if (at < 0 || at >= static_cast<int64_t>(data.size()) ||
        !usable(run.data, run.present, i)) {
      continue;
    }
    const auto p = static_cast<std::size_t>(at);
    if (!usable(data, present, p)) {
      continue;
    }
    ++out.judged;
    if ((run.data[i] & 0x7Fu) == (data[p] & 0x7Fu)) {
      ++out.agreed;
    }
  }
  return out;
}

/// The log-likelihood per byte pair |model| gives |data| read as a receiver
/// reads it — text where it is text, picture description where it is that —
/// over the stretches of one kind whose bytes all arrived; minus infinity
/// where no such stretch holds a pair.
double dialect_score(const NaplpsDialectModel& model,
                     const std::vector<uint8_t>& data,
                     const std::vector<uint8_t>& present) {
  const std::vector<NaplpsByteRole> roles = naplps_read_roles(data);
  const auto arrived = [&](std::size_t i) {
    return data[i] != 0 && (present.empty() || present[i] != kNabtsByteLost);
  };
  double total = 0.0;
  std::size_t pairs = 0;
  std::size_t i = 0;
  while (i < roles.size()) {
    const NaplpsByteRole role = roles[i];
    if ((role != NaplpsByteRole::kText && role != NaplpsByteRole::kPdi) ||
        !arrived(i)) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < roles.size() && roles[j] == role && arrived(j)) {
      ++j;
    }
    const std::size_t count = j - i;
    if (count >= 2) {
      const double mean = role == NaplpsByteRole::kText
                              ? model.text_score(&data[i], count)
                              : model.pdi_score(&data[i], count);
      total += mean * static_cast<double>(count - 1);
      pairs += count - 1;
    }
    i = j;
  }
  return pairs == 0 ? -std::numeric_limits<double>::infinity()
                    : total / static_cast<double>(pairs);
}

/// What setting a run against its candidate placements came to.
struct Placed {
  /// The placement that agrees best, of those that pass.
  std::optional<Placement> best;
  /// Whether another target passes within kClearMarginPercent of the best.
  bool ambiguous = false;
  /// Every placement that passes, the best among them.
  std::vector<Placement> passing;
};

/// |run| set against each of |candidates|.
template <typename Bytes>
Placed place_run(const NabtsLooseRun& run,
                 const std::vector<Placement>& candidates,
                 const Bytes& bytes_of) {
  Placed out;
  Agreement best_agreement;
  std::optional<double> runner_up;
  for (const Placement& candidate : candidates) {
    const auto& [data, present] = bytes_of(candidate.target);
    const Agreement agreement =
        agreement_at(run, data, present, candidate.offset);
    if (!agreement.passes()) {
      continue;
    }
    out.passing.push_back(candidate);
    if (!out.best || agreement.percent() > best_agreement.percent()) {
      if (out.best && out.best->target != candidate.target) {
        runner_up = best_agreement.percent();
      }
      out.best = candidate;
      best_agreement = agreement;
    } else if (candidate.target != out.best->target) {
      runner_up = std::max(runner_up.value_or(0.0), agreement.percent());
    }
  }
  out.ambiguous = out.best && runner_up &&
                  best_agreement.percent() - *runner_up < kClearMarginPercent;
  return out;
}

/// Clusters found to be one page, each with where its frame sits in its
/// root's: a union-find whose links carry an offset, and which keeps each
/// page's span so that no merge makes one longer than a data group can be.
class ClusterUnion {
 public:
  /// A new cluster, spanning |lo| to |hi| of its own frame.
  void add(int64_t lo, int64_t hi) {
    parent_.push_back(parent_.size());
    shift_.push_back(0);
    lo_.push_back(lo);
    hi_.push_back(hi);
  }
  /// |cluster|'s root, and |shift| set to where position 0 of its frame sits
  /// in the root's.
  std::size_t find(std::size_t cluster, int64_t& shift) const {
    shift = 0;
    std::size_t at = cluster;
    while (parent_[at] != at) {
      shift += shift_[at];
      at = parent_[at];
    }
    return at;
  }
  /// |cluster| now also spans |lo| to |hi| of its own frame.
  void extend(std::size_t cluster, int64_t lo, int64_t hi) {
    int64_t shift = 0;
    const std::size_t root = find(cluster, shift);
    lo_[root] = std::min(lo_[root], lo + shift);
    hi_[root] = std::max(hi_[root], hi + shift);
  }
  /// Make |b| one page with |a|, position x of |b|'s frame being x + |delta|
  /// of |a|'s — unless the page would then span more than |longest| bytes,
  /// which only two pages' runs strung together can. A link the two already
  /// have another way is left as it is.
  void join(std::size_t a, std::size_t b, int64_t delta, int64_t longest) {
    int64_t shift_a = 0;
    int64_t shift_b = 0;
    const std::size_t root_a = find(a, shift_a);
    const std::size_t root_b = find(b, shift_b);
    if (root_a == root_b) {
      return;
    }
    // x in b's frame is x + shift_b in root_b's; x + delta in a's frame is
    // x + delta + shift_a in root_a's.
    const int64_t moved = delta + shift_a - shift_b;
    const int64_t lo = std::min(lo_[root_a], lo_[root_b] + moved);
    const int64_t hi = std::max(hi_[root_a], hi_[root_b] + moved);
    if (hi - lo > longest) {
      return;
    }
    parent_[root_b] = root_a;
    shift_[root_b] = moved;
    lo_[root_a] = lo;
    hi_[root_a] = hi;
  }

 private:
  std::vector<std::size_t> parent_;
  std::vector<int64_t> shift_;
  std::vector<int64_t> lo_;
  std::vector<int64_t> hi_;
};

/// Runs judged to be one page none of the catalogued records holds, set out
/// in one frame of reference: what the runs say at each position, as far as
/// they have said it, and where each run starts.
struct Cluster {
  uint16_t channel = 0;
  /// The position of |consensus|'s first byte.
  int64_t base = 0;
  /// The first usable byte any run gave each position, and a mask of which
  /// positions have one (the runs' own masks, so agreement_at() reads it).
  std::vector<uint8_t> consensus;
  std::vector<uint8_t> known;
  /// Runs, by their index among those held, and where each starts.
  std::vector<std::pair<std::size_t, int64_t>> members;
  /// The positions whose stretch is in the index already: each is indexed
  /// once, however many runs hold it, or a page sent more often than
  /// kMaxStretchHits times would stop proposing itself to its own copies.
  std::set<int64_t> indexed;
};

/// Take |run|, held as run |held|, into |cluster| (number |id|) at |offset|:
/// extend the consensus with what it says where nothing has been said yet, and
/// index the stretches it holds at positions not yet indexed.
void take_into(Cluster& cluster, uint32_t id, std::size_t held,
               const NabtsLooseRun& run, int64_t offset, StretchIndex& index) {
  const int64_t start = std::min(cluster.base, offset);
  const int64_t end =
      std::max(cluster.base + static_cast<int64_t>(cluster.consensus.size()),
               offset + static_cast<int64_t>(run.data.size()));
  if (start < cluster.base) {
    const auto grow = static_cast<std::size_t>(cluster.base - start);
    cluster.consensus.insert(cluster.consensus.begin(), grow, 0);
    cluster.known.insert(cluster.known.begin(), grow, kNabtsByteLost);
    cluster.base = start;
  }
  cluster.consensus.resize(static_cast<std::size_t>(end - cluster.base), 0);
  cluster.known.resize(cluster.consensus.size(), kNabtsByteLost);
  for (std::size_t i = 0; i < run.data.size(); ++i) {
    const auto p = static_cast<std::size_t>(offset + static_cast<int64_t>(i) -
                                            cluster.base);
    if (cluster.known[p] == kNabtsByteLost &&
        usable(run.data, run.present, i)) {
      cluster.consensus[p] = run.data[i];
      cluster.known[p] = kNabtsByteArrived;
    }
  }
  for (std::size_t i = 0; i + kStretch <= run.data.size(); ++i) {
    const int64_t at = offset + static_cast<int64_t>(i);
    if (usable_stretch(run.data, run.present, i) &&
        cluster.indexed.insert(at).second) {
      index[stretch_key(run.channel, &run.data[i])].push_back({id, at});
    }
  }
  cluster.members.push_back({held, offset});
}

/// The share of |run|'s bytes that are usable(), in percent.
std::size_t usable_percent(const NabtsLooseRun& run) {
  if (run.data.empty()) {
    return 0;
  }
  std::size_t count = 0;
  std::size_t arrived = 0;
  for (std::size_t i = 0; i < run.data.size(); ++i) {
    if (run.present[i] == kNabtsByteLost) {
      continue;
    }
    ++arrived;
    if (teletext_odd_parity_valid(run.data[i])) {
      ++count;
    }
  }
  return arrived == 0 ? 0 : count * 100 / arrived;
}

/// The record types the presentation records of each channel a record was
/// named on as transmitted carry, counted: a page salvaged there is listed
/// with the commonest.
using NamedChannels = std::map<uint16_t, std::map<uint8_t, uint32_t>>;

NamedChannels named_channels_of(
    const std::vector<NabtsCataloguedRecord>& records) {
  NamedChannels named;
  for (const NabtsCataloguedRecord& record : records) {
    if (record.times_attested > 0 && !record.identity_lost) {
      ++named[record.channel][record.record_type];
    }
  }
  return named;
}

/**
 * @brief The runs that are copies of a listed record, put to use
 *
 * Each run on a named channel is placed against every stretch of the
 * records' bytes that arrived passing parity. A copy of a listed record,
 * whichever, is taken — never a page of its own — and its bytes fill only the
 * one record it is clearly a copy of, and in it only the positions no copy
 * delivered: each such position takes the reading the runs give of it most,
 * weighted by the detector's confidence, and the surest of the runs holding
 * it (NabtsCataloguedRecord::data_confidence).
 *
 * @return The runs on a named channel no record holds.
 */
std::vector<std::size_t> fill_records(
    const std::vector<NabtsHeldRun>& runs,
    std::vector<NabtsCataloguedRecord>& records, const NamedChannels& named,
    std::vector<bool>& taken, NabtsSalvageStats& stats) {
  StretchIndex record_index;
  for (std::size_t r = 0; r < records.size(); ++r) {
    const NabtsCataloguedRecord& record = records[r];
    if (named.count(record.channel) == 0 || record.data.size() < kStretch) {
      continue;
    }
    for (std::size_t p = 0; p + kStretch <= record.data.size(); ++p) {
      if (usable_stretch(record.data, record.data_present, p)) {
        record_index[stretch_key(record.channel, &record.data[p])].push_back(
            {static_cast<uint32_t>(r), static_cast<int64_t>(p)});
      }
    }
  }
  const auto record_bytes = [&records](uint32_t r) {
    return std::pair<const std::vector<uint8_t>&, const std::vector<uint8_t>&>(
        records[r].data, records[r].data_present);
  };

  struct Reading {
    uint32_t weight = 0;
    uint8_t surest = 0;
  };
  std::map<std::pair<uint32_t, std::size_t>, std::map<uint8_t, Reading>>
      readings;
  std::vector<std::size_t> unmatched;
  for (std::size_t h = 0; h < runs.size(); ++h) {
    const NabtsLooseRun& run = runs[h].run;
    if (named.count(run.channel) == 0) {
      continue;  // see the class comment: no record of the channel was named
    }
    const Placed placed_run =
        place_run(run, proposals(record_index, run), record_bytes);
    if (placed_run.passing.empty()) {
      unmatched.push_back(h);
      continue;
    }
    // Two versions of a page agree on all but the bytes they differ in: a run
    // that fits more than one record fills neither.
    taken[h] = true;
    ++stats.runs_matched;
    // A run that fits one record unambiguously has its best placement.
    if (placed_run.ambiguous || !placed_run.best) {
      continue;
    }
    const Placement& placed = *placed_run.best;
    NabtsCataloguedRecord& record = records[placed.target];
    if (record.data_present.empty()) {
      continue;  // the record arrived whole: nothing to fill
    }
    for (std::size_t i = 0; i < run.data.size(); ++i) {
      const int64_t at = placed.offset + static_cast<int64_t>(i);
      if (at < 0 || at >= static_cast<int64_t>(record.data.size()) ||
          !usable(run.data, run.present, i)) {
        continue;
      }
      const auto p = static_cast<std::size_t>(at);
      if (record.data_present[p] != kNabtsByteLost) {
        continue;  // the vote settled this byte, and a run is one reading
      }
      Reading& reading = readings[{placed.target, p}][run.data[i]];
      reading.weight += 1u + run.confidence[i];
      reading.surest = std::max(reading.surest, run.confidence[i]);
    }
  }
  std::map<uint32_t, uint32_t> filled_per_record;
  for (const auto& [where, values] : readings) {
    const auto chosen = std::max_element(
        values.begin(), values.end(), [](const auto& a, const auto& b) {
          return a.second.weight < b.second.weight;
        });
    NabtsCataloguedRecord& record = records[where.first];
    record.data[where.second] = chosen->first;
    record.data_present[where.second] = kNabtsByteArrived;
    if (where.second < record.data_confidence.size()) {
      record.data_confidence[where.second] = chosen->second.surest;
    }
    ++filled_per_record[where.first];
  }
  for (const auto& [r, count] : filled_per_record) {
    records[r].bytes_salvaged += count;
    stats.bytes_filled += count;
  }
  stats.records_filled = filled_per_record.size();
  stats.runs_unmatched = unmatched.size();
  return unmatched;
}

/// A page none of the records holds: its channel, and its runs with where
/// each starts in the page's frame.
struct RunPage {
  uint16_t channel = 0;
  std::vector<std::pair<std::size_t, int64_t>> members;
};

/// The pages the runs |unmatched| make. Each run joins the cluster it agrees
/// with best, set against what the runs already taken say, or starts one of
/// its own; a run that agrees with more than one makes them one page, since a
/// page's first runs need not overlap one another. The pages are by their
/// root cluster, each run placed in its frame.
std::map<std::size_t, RunPage> pages_of_runs(
    const std::vector<NabtsHeldRun>& runs,
    const std::vector<std::size_t>& unmatched) {
  std::vector<Cluster> clusters;
  StretchIndex cluster_index;
  ClusterUnion pages_of;
  for (const std::size_t h : unmatched) {
    const NabtsLooseRun& run = runs[h].run;
    const auto cluster_bytes = [&clusters](uint32_t c) {
      return std::pair<const std::vector<uint8_t>&,
                       const std::vector<uint8_t>&>(clusters[c].consensus,
                                                    clusters[c].known);
    };
    // Placements in a cluster's frame are offsets into its consensus, which
    // agreement_at() reads from zero.
    std::vector<Placement> candidates = proposals(cluster_index, run);
    for (Placement& candidate : candidates) {
      candidate.offset -= clusters[candidate.target].base;
    }
    const Placed placed = place_run(run, candidates, cluster_bytes);
    if (placed.best) {
      Cluster& cluster = clusters[placed.best->target];
      const int64_t start = placed.best->offset + cluster.base;
      take_into(cluster, placed.best->target, h, run, start, cluster_index);
      pages_of.extend(placed.best->target, start,
                      start + static_cast<int64_t>(run.data.size()));
      for (const Placement& other : placed.passing) {
        if (other.target == placed.best->target) {
          continue;
        }
        // The run starts at |start| in the best cluster's frame and at
        // other.offset + base in the other's: position x of the other's frame
        // is x + start - (other.offset + base) of the best's.
        const int64_t other_start = other.offset + clusters[other.target].base;
        pages_of.join(placed.best->target, other.target, start - other_start,
                      static_cast<int64_t>(kNabtsMaxGroupBytes));
      }
      continue;
    }
    if (clusters.size() >= kNabtsSalvageMaxRuns) {
      continue;
    }
    Cluster fresh;
    fresh.channel = run.channel;
    const auto id = static_cast<uint32_t>(clusters.size());
    clusters.push_back(std::move(fresh));
    pages_of.add(0, static_cast<int64_t>(run.data.size()));
    take_into(clusters.back(), id, h, run, 0, cluster_index);
  }

  std::map<std::size_t, RunPage> by_root;
  for (std::size_t c = 0; c < clusters.size(); ++c) {
    int64_t shift = 0;
    const std::size_t root = pages_of.find(c, shift);
    RunPage& page = by_root[root];
    page.channel = clusters[c].channel;
    for (const auto& [h, offset] : clusters[c].members) {
      page.members.push_back({h, offset + shift});
    }
  }
  return by_root;
}

/// How each named channel writes NAPLPS, from its presentation records, and
/// how well its records read by that: the floor a page none of them holds
/// must reach, where enough of them say.
struct ChannelDialects {
  std::map<uint16_t, NaplpsDialectModel> models;
  std::map<uint16_t, double> floors;
};

ChannelDialects channel_dialects(
    const std::vector<NabtsCataloguedRecord>& records,
    const NamedChannels& named) {
  ChannelDialects out;
  for (const NabtsCataloguedRecord& record : records) {
    if (named.count(record.channel) > 0 &&
        nabts_type_is_presentation(record.record_type)) {
      out.models[record.channel].learn(record.data, record.data_present);
    }
  }
  for (auto& [channel, model] : out.models) {
    std::vector<double> scores;
    for (const NabtsCataloguedRecord& record : records) {
      if (record.channel == channel &&
          nabts_type_is_presentation(record.record_type)) {
        const double score =
            dialect_score(model, record.data, record.data_present);
        if (std::isfinite(score)) {
          scores.push_back(score);
        }
      }
    }
    if (scores.size() < kDialectMinRecords) {
      continue;
    }
    std::sort(scores.begin(), scores.end());
    ORC_LOG_DEBUG(
        "NabtsSalvage: channel {:03X} dialect over {} records, scores min "
        "{:.3f} p10 {:.3f} p25 {:.3f} median {:.3f}",
        channel, scores.size(), scores.front(), scores[scores.size() / 10],
        scores[scores.size() / 4], scores[scores.size() / 2]);
    out.floors[channel] =
        scores[scores.size() * kDialectFloorPercentile / 100] -
        kDialectFloorMargin;
  }
  return out;
}

/// A page's runs voted over the span they cover, and the frames they arrived
/// in.
struct VotedPage {
  NabtsVoteResult voted;
  uint64_t first_frame = UINT64_MAX;
  uint64_t last_frame = 0;
};

VotedPage vote_page(const std::vector<NabtsHeldRun>& runs,
                    const RunPage& page) {
  int64_t span_start = INT64_MAX;
  int64_t span_end = INT64_MIN;
  for (const auto& [h, offset] : page.members) {
    span_start = std::min(span_start, offset);
    span_end = std::max(span_end,
                        offset + static_cast<int64_t>(runs[h].run.data.size()));
  }
  const auto span = static_cast<std::size_t>(span_end - span_start);
  std::vector<NabtsRecordCopy> copies;
  copies.reserve(page.members.size());
  VotedPage out;
  for (const auto& [h, offset] : page.members) {
    const NabtsHeldRun& held = runs[h];
    NabtsRecordCopy copy;
    copy.data.assign(span, 0);
    copy.present.assign(span, kNabtsByteLost);
    copy.confidence.assign(span, 0);
    const auto at = static_cast<std::size_t>(offset - span_start);
    for (std::size_t i = 0; i < held.run.data.size(); ++i) {
      copy.data[at + i] = held.run.data[i];
      copy.present[at + i] = held.run.present[i];
      copy.confidence[at + i] = held.run.confidence[i];
    }
    copies.push_back(std::move(copy));
    out.first_frame = std::min(out.first_frame, held.frame);
    out.last_frame = std::max(out.last_frame, held.frame);
  }
  out.voted = nabts_vote_record(copies);
  return out;
}

/**
 * @brief Where a voted page's bytes begin, and whether in picture description
 *
 * The record's head went with its synchronizing packet, and with it the
 * shift that put the bytes after it in the set they are in (X3.110 §6.1.3).
 * Where the first shift the page holds is SI, the bytes before it were
 * picture description code — the tail of a command, then whole commands that
 * place and colour what follows — and the page starts with the SO they must
 * have followed. Otherwise the bytes before the first shift read as the
 * service's own records, by |dialect| where it is known, say they read better.
 * Nothing where no byte of the page arrived.
 */
std::optional<std::pair<std::size_t, bool>> page_start(
    const NabtsVoteResult& voted, const NaplpsDialectModel* dialect) {
  std::size_t start = voted.data.size();
  for (std::size_t i = 0; i < voted.data.size(); ++i) {
    if (voted.present[i] != kNabtsByteLost) {
      start = i;
      break;
    }
  }
  if (start >= voted.data.size()) {
    return std::nullopt;
  }
  bool resumes_in_pdi = false;
  std::size_t first_shift = start;
  bool shift_found = false;
  for (std::size_t i = start;
       i < voted.data.size() && i < start + kResumeSearchBytes; ++i) {
    const uint8_t code = voted.data[i] & 0x7Fu;
    if (voted.present[i] == kNabtsByteLost ||
        !teletext_odd_parity_valid(voted.data[i])) {
      continue;
    }
    if (code == kShiftOut || code == kShiftIn) {
      first_shift = i;
      shift_found = true;
      resumes_in_pdi = code == kShiftIn;
      break;
    }
  }
  if (!resumes_in_pdi && dialect != nullptr) {
    const std::size_t end =
        shift_found ? first_shift
                    : std::min(voted.data.size(), start + kResumeSearchBytes);
    if (end > start + 1) {
      resumes_in_pdi = dialect->pdi_score(&voted.data[start], end - start) >
                       dialect->text_score(&voted.data[start], end - start);
    }
  }
  return std::make_pair(start, resumes_in_pdi);
}

/// The record a salvaged page is listed as: under the |number|th address of
/// its channel's lost identities, with the record type |record_type|, its
/// head held open as lost — a block of it, and the SO it must have followed
/// where it resumes in picture description — then the voted bytes from
/// |start| on.
NabtsCataloguedRecord lost_identity_page(const RunPage& runs_page,
                                         const VotedPage& voted_page,
                                         uint32_t number, uint8_t record_type,
                                         std::size_t start,
                                         bool resumes_in_pdi) {
  const NabtsVoteResult& voted = voted_page.voted;
  NabtsCataloguedRecord page;
  page.channel = runs_page.channel;
  page.address = kNabtsIdentityLostAddressBase + number;
  page.address_text = fmt::format("LOST{}", number);
  page.channel_text =
      fmt::format("{:03X}/{}", runs_page.channel, page.address_text);
  page.identity_lost = true;
  page.record_type = record_type;
  page.chain_base_address = page.address;
  page.records_in_message = 1;
  page.first_seen_frame = voted_page.first_frame;
  page.last_seen_frame = voted_page.last_frame;
  page.times_seen = runs_page.members.size();
  page.copies_voted = static_cast<uint32_t>(runs_page.members.size());
  page.data.assign(kNabtsMaxDataBlockBytes, 0);
  page.data_present.assign(kNabtsMaxDataBlockBytes, kNabtsByteLost);
  if (resumes_in_pdi) {
    page.data.push_back(kShiftOut);
    page.data_present.push_back(kNabtsByteLost);
  }
  page.data.insert(page.data.end(),
                   voted.data.begin() + static_cast<std::ptrdiff_t>(start),
                   voted.data.end());
  page.data_present.insert(
      page.data_present.end(),
      voted.present.begin() + static_cast<std::ptrdiff_t>(start),
      voted.present.end());
  if (voted.confidence.size() == voted.data.size()) {
    // Nothing is known of the lost head, and the rest is what the vote made
    // of it.
    page.data_confidence.assign(page.data.size() - (voted.data.size() - start),
                                0);
    page.data_confidence.insert(
        page.data_confidence.end(),
        voted.confidence.begin() + static_cast<std::ptrdiff_t>(start),
        voted.confidence.end());
  }
  page.bytes_salvaged = static_cast<uint32_t>(voted.data.size() - start);
  return page;
}

}  // namespace

std::string NabtsSalvageStats::summary() const {
  return fmt::format(
      "{} run(s) of a teletext channel's packets whose group had lost its "
      "synchronizing packet: {} found in a listed record, filling {} byte(s) "
      "no copy had delivered in {} record(s); {} no record holds, {} of them "
      "making up {} page(s) listed without their identity ({} more refused, "
      "their code reading unlike the service's pages{}){}",
      runs_held, runs_matched, bytes_filled, records_filled, runs_unmatched,
      runs_in_pages, pages_found, pages_refused,
      pages_fill > 0
          ? fmt::format(", and {} the encoder's idle fill", pages_fill)
          : std::string{},
      runs_refused > 0
          ? fmt::format("; {} more refused past the bounds", runs_refused)
          : std::string{});
}

void NabtsSalvage::add(const NabtsLooseRun& run, uint64_t frame) {
  if (run.packets < kMinRunPackets || run.data.empty()) {
    return;
  }
  if (runs_.size() >= kNabtsSalvageMaxRuns ||
      bytes_ + run.data.size() > kNabtsSalvageMaxBytes) {
    ++stats_.runs_refused;
    return;
  }
  Held held{run, frame};
  if (held.run.starts_synchronizing) {
    // The group header that did not decode is no record data, and the record
    // header after it, Hamming 8/4 throughout (§5.2.1), passes odd parity as
    // every codeword does: neither may be compared or kept as presentation
    // code. Where the record header decodes, its length is known; otherwise
    // only the group header is set aside, and what follows is judged as it
    // stands.
    std::size_t skip = kNabtsGroupHeaderBytes;
    if (held.run.data.size() > skip) {
      const NabtsRecordHeader header = nabts_decode_record_header(
          held.run.data.data() + skip, held.run.data.size() - skip);
      if (header.valid) {
        skip += header.header_bytes;
      }
    }
    skip = std::min(skip, held.run.data.size());
    std::fill(held.run.present.begin(),
              held.run.present.begin() + static_cast<std::ptrdiff_t>(skip),
              kNabtsByteLost);
  }
  bytes_ += held.run.data.size();
  runs_.push_back(std::move(held));
}

NabtsSalvageStats NabtsSalvage::apply(
    std::vector<NabtsCataloguedRecord>& records) {
  NabtsSalvageStats stats = stats_;
  stats.runs_held = runs_.size();
  if (runs_.empty()) {
    return stats;
  }

  const NamedChannels named = named_channels_of(records);
  // The runs found to be a copy of a listed record or part of a listed page;
  // the rest are kept as they arrived (unplaced()).
  std::vector<bool> taken(runs_.size(), false);
  const std::vector<std::size_t> unmatched =
      fill_records(runs_, records, named, taken, stats);
  const std::map<std::size_t, RunPage> by_root =
      pages_of_runs(runs_, unmatched);
  const ChannelDialects dialects = channel_dialects(records, named);

  // A page of two or more runs, or of one that carries its own evidence, is
  // voted and listed.
  std::map<uint16_t, uint32_t> next_number;
  std::vector<NabtsCataloguedRecord> pages;
  for (const auto& entry : by_root) {
    const RunPage& cluster = entry.second;
    const NabtsLooseRun& first = runs_[cluster.members.front().first].run;
    const bool stands_alone =
        cluster.members.size() == 1 &&
        usable_percent(first) >= kLonePageMinUsablePercent;
    if (cluster.members.size() < 2 && !stands_alone) {
      continue;
    }
    const VotedPage voted = vote_page(runs_, cluster);
    // The encoder's idle fill sends groups like any other channel's, and they
    // lose their synchronizing packets as often: no page either.
    if (nabts_serial_evidence(voted.voted.data, voted.voted.present)
            .is_pattern()) {
      ++stats.pages_fill;
      continue;
    }
    const auto dialect = dialects.models.find(cluster.channel);
    const bool dialect_known = dialect != dialects.models.end() &&
                               dialect->second.records() >= kDialectMinRecords;
    const std::optional<std::pair<std::size_t, bool>> start =
        page_start(voted.voted, dialect_known ? &dialect->second : nullptr);
    if (!start) {
      continue;
    }
    const auto& types = named.at(cluster.channel);
    const uint8_t record_type =
        std::max_element(
            types.begin(), types.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; })
            ->first;
    NabtsCataloguedRecord page =
        lost_identity_page(cluster, voted, ++next_number[cluster.channel],
                           record_type, start->first, start->second);
    if (dialect_known) {
      ORC_LOG_DEBUG(
          "NabtsSalvage: page of {} run(s), {} bytes, score {:.3f}",
          cluster.members.size(), page.data.size(),
          dialect_score(dialect->second, page.data, page.data_present));
    }
    const auto floor = dialects.floors.find(cluster.channel);
    if (dialect_known && floor != dialects.floors.end() &&
        !(dialect_score(dialect->second, page.data, page.data_present) >=
          floor->second)) {
      ++stats.pages_refused;
      continue;
    }
    stats.runs_in_pages += cluster.members.size();
    for (const auto& member : cluster.members) {
      taken[member.first] = true;
    }
    pages.push_back(std::move(page));
  }
  std::stable_sort(
      pages.begin(), pages.end(),
      [](const NabtsCataloguedRecord& a, const NabtsCataloguedRecord& b) {
        return a.channel < b.channel;
      });
  stats.pages_found = pages.size();
  for (NabtsCataloguedRecord& page : pages) {
    records.push_back(std::move(page));
  }
  for (std::size_t h = 0; h < runs_.size(); ++h) {
    if (!taken[h]) {
      unplaced_.push_back(std::move(runs_[h]));
    }
  }
  runs_.clear();
  bytes_ = 0;
  return stats;
}

std::string nabts_unplaced_runs_text(const std::vector<NabtsHeldRun>& runs) {
  constexpr std::size_t kCharactersPerLine = 64;
  std::string out;
  for (const NabtsHeldRun& held : runs) {
    const NabtsLooseRun& run = held.run;
    std::size_t lost = 0;
    for (const uint8_t present : run.present) {
      lost += present == kNabtsByteLost ? 1 : 0;
    }
    out += fmt::format(
        "{}Channel {:03X}, ended in frame {}: {} packet{}, {} byte{} ({} "
        "lost)\n",
        out.empty() ? "" : "\n", run.channel, held.frame, run.packets,
        run.packets == 1 ? "" : "s", run.data.size(),
        run.data.size() == 1 ? "" : "s", lost);
    std::string line;
    std::size_t width = 0;
    const auto put = [&](const std::string& token) {
      if (width + token.size() > kCharactersPerLine && width > 0) {
        out += "  " + line + "\n";
        line.clear();
        width = 0;
      }
      line += token;
      width += token.size();
    };
    for (std::size_t i = 0; i < run.data.size(); ++i) {
      const uint8_t byte = run.data[i];
      const auto code = static_cast<uint8_t>(byte & 0x7FU);
      if (i < run.present.size() && run.present[i] == kNabtsByteLost) {
        put("_");
      } else if (!teletext_odd_parity_valid(byte)) {
        put("~");
      } else if (code == kShiftOut) {
        put("{SO}");
      } else if (code == kShiftIn) {
        put("{SI}");
      } else if (code == kEscape) {
        put("{ESC}");
      } else if (code < 0x20 || code == 0x7F) {
        put(".");
      } else {
        put(std::string(1, static_cast<char>(code)));
      }
    }
    if (width > 0) {
      out += "  " + line + "\n";
    }
  }
  return out;
}

}  // namespace orc
