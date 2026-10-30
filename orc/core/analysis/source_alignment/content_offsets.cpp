/*
 * File:        content_offsets.cpp
 * Module:      orc-core/analysis
 * Purpose:     Check aligned captures' picture numbers against their content
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "content_offsets.h"

#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/stack_measurement.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace orc {

namespace {

// Block size of the luma map frames are compared on: 8 samples, two cycles
// of the subcarrier at 4fsc, so chroma averages out, by 2 lines of a field.
constexpr size_t kBlockSamples = 8;
constexpr size_t kBlockLines = 2;

// Blocks two maps must share for a correlation to count.
constexpr size_t kMinSharedBlocks = 64;

// Candidate frames tried per window for one whose picture differs from both
// neighbours', and how many such frames are kept.
constexpr size_t kProbeCandidates = 6;
constexpr size_t kProbesPerWindow = 2;

// A frame's picture must differ from its neighbours' by at least this (one
// minus their correlation) to show an offset: on a still or a slow fade
// every offset matches.
constexpr double kMinMotion = 0.02;

// A source holds none of the reference's pictures only where the
// reference's picture changes by at least this from one frame to the next.
// Below it a capture of another pressing can differ from the reference by
// nearly as much as the picture does: on Community South the map sheets
// change by 0.08-0.10 from one frame to the next, and captures of other
// pressings matched their own sheet at 0.05 and its neighbours at 0.09.
constexpr double kMismatchMotion = 0.3;

// While bisecting, how far either side of the midpoint a frame with motion is
// looked for.
constexpr int64_t kBisectSearch = 6;

// One frame's luma, as block means; |valid| is false for a block holding a
// flagged sample.
struct BlockMap {
  std::vector<float> value;
  std::vector<uint8_t> valid;
};

// One minus the correlation of |a| and |b| over the blocks valid in both.
std::optional<double> distance(const BlockMap& a, const BlockMap& b) {
  if (a.value.size() != b.value.size()) return std::nullopt;
  double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
  size_t count = 0;
  for (size_t k = 0; k < a.value.size(); ++k) {
    if (!a.valid[k] || !b.valid[k]) continue;
    const double x = a.value[k];
    const double y = b.value[k];
    sa += x;
    sb += y;
    saa += x * x;
    sbb += y * y;
    sab += x * y;
    ++count;
  }
  if (count < kMinSharedBlocks) return std::nullopt;
  const auto n = static_cast<double>(count);
  const double va = saa - sa * sa / n;
  const double vb = sbb - sb * sb / n;
  if (va <= 0.0 || vb <= 0.0) return std::nullopt;
  return 1.0 - (sab - sa * sb / n) / std::sqrt(va * vb);
}

// What one probe of one source found.
struct Reading {
  enum class Kind { kUnusable, kOffset, kMismatch };
  Kind kind = Kind::kUnusable;
  int32_t offset = 0;
  bool operator==(const Reading& other) const {
    return kind == other.kind &&
           (kind != Kind::kOffset || offset == other.offset);
  }
  bool operator!=(const Reading& other) const { return !(*this == other); }
};

// A stretch of output frames over which one source reads the same.
struct Piece {
  int64_t from = 0;
  int64_t earliest_from = 0;
  Reading reading;
};

// Reads frames of one source as block maps, each once.  Not thread-safe;
// the reference's is shared behind a mutex.
class MapReader {
 public:
  MapReader(const VideoFrameRepresentation* source, int64_t start,
            std::pair<int64_t, int64_t> coverage)
      : source_(source), start_(start), coverage_(coverage) {}

  // The map of output frame |k|; nullptr where the source has no real frame
  // of the full size.
  const BlockMap* at(int64_t k) {
    if (k < coverage_.first || k > coverage_.second) return nullptr;
    auto it = maps_.find(k);
    if (it == maps_.end()) it = maps_.emplace(k, read(k)).first;
    return it->second ? &*it->second : nullptr;
  }

  std::pair<int64_t, int64_t> coverage() const { return coverage_; }

 private:
  std::optional<BlockMap> read(int64_t k) const {
    const auto id = static_cast<FrameID>(start_ + k);
    if (!source_->has_frame(id)) return std::nullopt;
    const auto desc = source_->get_frame_descriptor(id);
    if (!desc || desc->is_padding_frame || desc->height == 0 ||
        desc->samples_per_line_nominal == 0) {
      return std::nullopt;
    }
    const VideoSystem system = desc->system;
    const size_t width = desc->samples_per_line_nominal;
    const size_t height = desc->height;
    const size_t total = frame_line_sample_offset(system, width, height);
    std::vector<StackSample> plane;
    if (source_->has_separate_channels()) {
      const StackSample* luma = source_->get_frame_luma(id);
      if (luma) plane.assign(luma, luma + total);
    } else {
      plane = source_->get_frame_copy(id);
    }
    if (plane.size() < total) return std::nullopt;
    const std::vector<uint8_t> mask =
        dropout_mask(source_->get_dropout_hints(id), total);

    BlockMap map;
    const size_t field1_lines = std::min(field1_line_count(system), height);
    for (size_t field = 0; field < 2; ++field) {
      const size_t from = (field == 0 ? 0 : field1_lines) + kFieldBlankingLines;
      const size_t to = field == 0 ? field1_lines : height;
      for (size_t line = from; line + kBlockLines <= to; line += kBlockLines) {
        const size_t length = frame_line_sample_count(system, width, line);
        const size_t first = length / 5;
        const size_t last = length > 10 ? length - 10 : length;
        for (size_t x = first; x + kBlockSamples <= last; x += kBlockSamples) {
          double sum = 0.0;
          bool flagged = false;
          for (size_t l = 0; l < kBlockLines; ++l) {
            const size_t base =
                frame_line_sample_offset(system, width, line + l) + x;
            for (size_t n = 0; n < kBlockSamples; ++n) {
              sum += plane[base + n];
              flagged = flagged || mask[base + n] != 0;
            }
          }
          map.value.push_back(static_cast<float>(
              sum / static_cast<double>(kBlockSamples * kBlockLines)));
          map.valid.push_back(flagged ? 0 : 1);
        }
      }
    }
    return map;
  }

  const VideoFrameRepresentation* source_;
  int64_t start_;
  std::pair<int64_t, int64_t> coverage_;
  std::map<int64_t, std::optional<BlockMap>> maps_;
};

// The reference's maps, read by every source's thread in turn.
class SharedReference {
 public:
  explicit SharedReference(MapReader reader) : reader_(std::move(reader)) {}

  // How far output frame |k|'s picture differs from both neighbours', when
  // it differs by at least kMinMotion, and a copy of its map.
  std::optional<std::pair<double, BlockMap>> motion(int64_t k) {
    std::lock_guard<std::mutex> lock(mutex_);
    const BlockMap* here = reader_.at(k);
    const BlockMap* before = reader_.at(k - 1);
    const BlockMap* after = reader_.at(k + 1);
    if (!here || !before || !after) return std::nullopt;
    const std::optional<double> a = distance(*here, *before);
    const std::optional<double> b = distance(*here, *after);
    if (!a || !b) return std::nullopt;
    const double least = std::min(*a, *b);
    if (least < kMinMotion) return std::nullopt;
    return std::make_pair(least, *here);
  }

  std::pair<int64_t, int64_t> coverage() const { return reader_.coverage(); }

 private:
  std::mutex mutex_;
  MapReader reader_;
};

// Source |reader| at output frame |k|, against the reference's map there,
// whose picture differs from its neighbours' by |motion|: the offset whose
// frame matches it, at most half as far as any other; failing that, a
// mismatch when none comes within half the reference's own change from one
// frame to the next and that change is at least kMismatchMotion.
Reading read_at(MapReader& reader, int64_t k, double motion,
                const BlockMap& reference) {
  std::vector<double> distances;
  for (int32_t d = -kMaxContentOffset; d <= kMaxContentOffset; ++d) {
    const BlockMap* map = reader.at(k + d);
    if (!map) return {};
    const std::optional<double> dist = distance(*map, reference);
    if (!dist) return {};
    distances.push_back(*dist);
  }
  ORC_LOG_DEBUG(
      "Content offsets: frame {}: reference change {:.4f}, distances {:.4f} "
      "{:.4f} {:.4f} {:.4f} {:.4f}",
      k + 1, motion, distances[0], distances[1], distances[2], distances[3],
      distances[4]);
  const auto best_it = std::min_element(distances.begin(), distances.end());
  const double best = *best_it;
  double second = std::numeric_limits<double>::infinity();
  for (auto it = distances.begin(); it != distances.end(); ++it) {
    if (it != best_it) second = std::min(second, *it);
  }
  if (second >= 2.0 * best && second >= 0.5 * motion) {
    return {
        Reading::Kind::kOffset,
        static_cast<int32_t>(best_it - distances.begin()) - kMaxContentOffset};
  }
  if (motion >= kMismatchMotion && best > 0.5 * motion) {
    return {Reading::Kind::kMismatch, 0};
  }
  return {};
}

// One source checked against the reference: the pieces it reads as, over
// the output frames both cover.
std::vector<Piece> check_source(
    MapReader& reader, SharedReference& reference,
    const std::vector<std::vector<int64_t>>& window_probes,
    const std::function<bool()>& cancelled) {
  const auto [lo, hi] = reader.coverage();
  auto usable = [&](int64_t k) {
    return k - kMaxContentOffset >= lo && k + kMaxContentOffset <= hi;
  };
  auto reading_at = [&](int64_t k) -> Reading {
    if (!usable(k)) return {};
    const auto motion = reference.motion(k);
    if (!motion) return {};
    return read_at(reader, k, motion->first, motion->second);
  };

  // One reading per window: the first probe that shows an offset, or a
  // mismatch only when every probe of the window shows one, as a single
  // frame torn by dropouts can match nothing.
  std::vector<std::pair<int64_t, Reading>> measured;
  for (const auto& probes : window_probes) {
    if (cancelled && cancelled()) return {};
    std::optional<std::pair<int64_t, Reading>> found;
    size_t mismatches = 0;
    size_t tried = 0;
    for (int64_t k : probes) {
      const Reading r = reading_at(k);
      if (r.kind == Reading::Kind::kUnusable) continue;
      ++tried;
      if (r.kind == Reading::Kind::kOffset) {
        found = {k, r};
        break;
      }
      ++mismatches;
      if (!found) found = {k, r};
    }
    if (!found) continue;
    if (found->second.kind == Reading::Kind::kMismatch &&
        (tried < 2 || mismatches < tried)) {
      continue;
    }
    measured.push_back(*found);
  }
  if (measured.empty()) return {};

  // Pieces, each change bisected to the frame where a frame with motion
  // allows it.
  std::vector<Piece> pieces;
  pieces.push_back({lo, lo, measured.front().second});
  for (size_t m = 1; m < measured.size(); ++m) {
    const Reading before = measured[m - 1].second;
    const Reading after = measured[m].second;
    if (before == after) continue;
    int64_t low = measured[m - 1].first;  // known to read |before|
    int64_t high = measured[m].first;     // known to read |after|
    while (high - low > 1) {
      if (cancelled && cancelled()) break;
      const int64_t mid = low + (high - low) / 2;
      // Only a reading of either side moves the bounds: where a capture's
      // numbering slips, the picture it skips matches nothing.
      std::optional<std::pair<int64_t, Reading>> probe;
      for (int64_t step = 0; step <= kBisectSearch && !probe; ++step) {
        for (int64_t k : {mid + step, mid - step}) {
          if (k <= low || k >= high) continue;
          const Reading r = reading_at(k);
          if (r == before || r == after) {
            probe = {k, r};
            break;
          }
        }
      }
      if (!probe) break;
      (probe->second == before ? low : high) = probe->first;
    }
    pieces.push_back({high, low + 1, after});
  }
  return pieces;
}

// The value most sources read, the reference's 0 among them; on a tie, 0
// when it is among the most read, else the smallest offset.
int32_t consensus(const std::vector<int32_t>& values) {
  std::map<int32_t, size_t> counts;
  for (int32_t v : values) ++counts[v];
  size_t most = 0;
  for (const auto& [v, c] : counts) most = std::max(most, c);
  if (counts[0] == most) return 0;
  int32_t chosen = 0;
  bool any = false;
  for (const auto& [v, c] : counts) {
    if (c != most) continue;
    if (!any || std::abs(v) < std::abs(chosen)) chosen = v;
    any = true;
  }
  return chosen;
}

}  // namespace

ContentOffsets check_content_offsets(
    const std::vector<const VideoFrameRepresentation*>& sources,
    const std::vector<std::optional<int64_t>>& starts,
    const std::function<bool()>& cancelled) {
  const size_t n = sources.size();
  ContentOffsets result;
  result.spans.resize(n);
  result.checked.resize(n);

  // Each source's output coverage, and the reference: the source covering
  // the most.
  std::vector<std::optional<std::pair<int64_t, int64_t>>> coverage(n);
  for (size_t i = 0; i < n && i < starts.size(); ++i) {
    const std::optional<int64_t> start = starts[i];
    if (!sources[i] || !start || sources[i]->frame_count() == 0) continue;
    const FrameIDRange range = sources[i]->frame_range();
    const int64_t first =
        std::max<int64_t>(static_cast<int64_t>(range.first) - *start, 0);
    const int64_t last = static_cast<int64_t>(range.last) - *start;
    if (last >= first) coverage[i] = {first, last};
  }
  std::optional<size_t> reference;
  for (size_t i = 0; i < n; ++i) {
    if (!coverage[i]) continue;
    const auto span = [&](size_t j) {
      return coverage[j]->second - coverage[j]->first;
    };
    if (!reference || span(i) > span(*reference)) reference = i;
  }
  if (!reference) return result;
  result.reference = reference;
  const auto [ref_lo, ref_hi] = *coverage[*reference];
  const std::optional<int64_t> ref_start = starts[*reference];
  SharedReference shared(
      MapReader(sources[*reference], ref_start.value_or(0), {ref_lo, ref_hi}));

  // Frames with motion in each window, found on the reference alone.
  const int64_t span = ref_hi - ref_lo + 1;
  const auto windows =
      static_cast<int64_t>(std::min<int64_t>(kContentWindows, span));
  std::vector<std::vector<int64_t>> window_probes;
  for (int64_t w = 0; w < windows; ++w) {
    if (cancelled && cancelled()) {
      result.cancelled = true;
      return result;
    }
    const int64_t from = ref_lo + w * span / windows;
    const int64_t to = ref_lo + (w + 1) * span / windows;
    std::vector<int64_t> probes;
    const auto width = to - from;
    for (size_t c = 0; c < kProbeCandidates && probes.size() < kProbesPerWindow;
         ++c) {
      const int64_t k = from + static_cast<int64_t>(2 * c + 1) * width /
                                   static_cast<int64_t>(2 * kProbeCandidates);
      if (shared.motion(k)) probes.push_back(k);
    }
    ++result.windows;
    if (!probes.empty()) ++result.windows_with_motion;
    window_probes.push_back(std::move(probes));
  }

  // Every other source on a thread of its own; a source taken twice is
  // checked once.
  std::vector<std::vector<Piece>> pieces(n);
  std::vector<size_t> first_of(n);
  std::vector<std::thread> threads;
  for (size_t i = 0; i < n; ++i) {
    first_of[i] = i;
    for (size_t j = 0; j < i; ++j) {
      if (sources[j] == sources[i] && starts[j] == starts[i]) {
        first_of[i] = j;
        break;
      }
    }
    if (first_of[i] != i || i == *reference || !coverage[i]) continue;
    const std::optional<int64_t> start = starts[i];
    const int64_t lo = std::max(coverage[i]->first, ref_lo);
    const int64_t hi = std::min(coverage[i]->second, ref_hi);
    if (hi - lo < int64_t{2} * kMaxContentOffset || !start) continue;
    result.checked[i] = {lo, hi};
    threads.emplace_back([&, i, lo, hi, start = *start] {
      MapReader reader(sources[i], start, {lo, hi});
      pieces[i] = check_source(reader, shared, window_probes, cancelled);
    });
  }
  for (auto& thread : threads) thread.join();
  for (size_t i = 0; i < n; ++i) {
    if (first_of[i] != i) {
      pieces[i] = pieces[first_of[i]];
      result.checked[i] = result.checked[first_of[i]];
    }
  }
  if (cancelled && cancelled()) {
    result.cancelled = true;
    return result;
  }
  result.checked[*reference] = {ref_lo, ref_hi};

  // Each source against the consensus, interval by interval between every
  // source's changes.
  std::set<int64_t> boundaries = {ref_lo, ref_hi + 1};
  std::map<int64_t, int64_t> earliest;  // a boundary's earliest place
  for (size_t i = 0; i < n; ++i) {
    for (const Piece& piece : pieces[i]) {
      boundaries.insert(piece.from);
      auto [it, inserted] = earliest.emplace(piece.from, piece.earliest_from);
      if (!inserted) it->second = std::min(it->second, piece.earliest_from);
    }
    if (result.checked[i]) {
      boundaries.insert(result.checked[i]->first);
      boundaries.insert(result.checked[i]->second + 1);
    }
  }
  auto reading_of = [&](size_t i, int64_t k) -> std::optional<Reading> {
    if (!result.checked[i] || k < result.checked[i]->first ||
        k > result.checked[i]->second || pieces[i].empty()) {
      return std::nullopt;
    }
    std::optional<Reading> found;
    for (const Piece& piece : pieces[i]) {
      if (piece.from <= k) found = piece.reading;
    }
    // Before a source's first reading it holds that reading.
    return found ? found : pieces[i].front().reading;
  };
  const std::vector<int64_t> edges(boundaries.begin(), boundaries.end());
  for (size_t e = 0; e + 1 < edges.size(); ++e) {
    const int64_t a = edges[e];
    const int64_t b = edges[e + 1] - 1;
    if (a < ref_lo || a > ref_hi) continue;
    std::vector<int32_t> values = {0};
    for (size_t i = 0; i < n; ++i) {
      if (i == *reference) continue;
      const std::optional<Reading> r = reading_of(i, a);
      if (r && r->kind == Reading::Kind::kOffset) values.push_back(r->offset);
    }
    const int32_t agreed = consensus(values);
    for (size_t i = 0; i < n; ++i) {
      std::optional<std::optional<int32_t>> relative;  // outer: differs
      if (i == *reference) {
        if (agreed != 0) relative = std::optional<int32_t>(-agreed);
      } else if (const std::optional<Reading> r = reading_of(i, a)) {
        if (r->kind == Reading::Kind::kMismatch) {
          relative = std::optional<int32_t>();
        } else if (r->kind == Reading::Kind::kOffset && r->offset != agreed) {
          relative = std::optional<int32_t>(r->offset - agreed);
        }
      }
      if (!relative) continue;
      auto& spans = result.spans[i];
      if (!spans.empty() && spans.back().last + 1 == a &&
          spans.back().offset == *relative) {
        spans.back().last = b;
        continue;
      }
      const auto it = earliest.find(a);
      spans.push_back({a, b, it != earliest.end() ? it->second : a, *relative});
    }
  }
  for (size_t i = 0; i < n; ++i) {
    for (const ContentOffsetSpan& s : result.spans[i]) {
      ORC_LOG_DEBUG(
          "Content offsets: source {} frames {}-{} (change from {}): {}", i + 1,
          s.first + 1, s.last + 1, s.earliest_first + 1,
          s.offset ? std::to_string(*s.offset) : std::string("no match"));
    }
  }
  return result;
}

std::optional<int32_t> whole_content_offset(const ContentOffsets& offsets,
                                            size_t source) {
  if (source >= offsets.spans.size() || source >= offsets.checked.size()) {
    return std::nullopt;
  }
  const auto& spans = offsets.spans[source];
  const auto& checked = offsets.checked[source];
  if (spans.size() != 1 || !checked || !spans.front().offset ||
      spans.front().first != checked->first ||
      spans.front().last != checked->second) {
    return std::nullopt;
  }
  return spans.front().offset;
}

ContentOffsetReport describe_content_offsets(const ContentOffsets& offsets) {
  ContentOffsetReport report;
  const size_t n = offsets.spans.size();
  report.sources.resize(n);
  std::ostringstream summary;
  if (offsets.cancelled) {
    summary << "Picture numbers against content: not checked (cancelled).\n";
    report.summary = summary.str();
    return report;
  }
  if (!offsets.reference || offsets.windows_with_motion == 0) {
    summary << "Picture numbers against content: not checked — "
            << (offsets.reference ? "no probed frame's picture differs from "
                                    "its neighbours'"
                                  : "no source covers a frame")
            << ".\n";
    report.summary = summary.str();
    return report;
  }

  auto frames = [](int64_t first, int64_t last) {
    return std::to_string(first + 1) + "–" + std::to_string(last + 1);
  };
  bool any = false;
  for (size_t i = 0; i < n; ++i) {
    const auto& spans = offsets.spans[i];
    auto& data = report.sources[i];
    if (!offsets.checked[i]) continue;
    const std::optional<int32_t> whole = whole_content_offset(offsets, i);
    data["content_offset"] = whole ? *whole : 0;
    std::string listed;
    for (const ContentOffsetSpan& span : spans) {
      listed +=
          (listed.empty() ? "" : ";") + frames(span.first, span.last) + ":" +
          (span.offset
               ? (*span.offset > 0 ? "+" : "") + std::to_string(*span.offset)
               : std::string("none"));
    }
    data["content_offset_spans"] = listed;
    if (spans.empty()) continue;
    any = true;

    std::ostringstream warning;
    warning << "Source " << (i + 1) << ": ";
    for (size_t s = 0; s < spans.size(); ++s) {
      const ContentOffsetSpan& span = spans[s];
      warning << (s == 0 ? "" : "; ") << "over output frames "
              << frames(span.first, span.last);
      if (span.earliest_first < span.first) {
        warning << " (from somewhere after frame " << span.earliest_first
                << ")";
      }
      if (span.offset) {
        const int32_t d = *span.offset;
        warning << " it labels its pictures " << std::abs(d)
                << (d > 0 ? " higher" : " lower") << " than the others ("
                << (d > 0 ? "+" : "") << d << ")";
      } else {
        warning << " its frames hold none of the others' pictures within "
                << kMaxContentOffset << " frames";
      }
    }
    if (whole) {
      warning << ". Moving its alignment by " << (*whole > 0 ? "+" : "")
              << *whole << (std::abs(*whole) == 1 ? " frame" : " frames")
              << " would correct it.";
    } else {
      warning << ". The frames would need their picture numbers corrected "
                 "there; stacked as they are, they blend the neighbouring "
                 "picture in.";
    }
    report.warnings.push_back(warning.str());
  }

  summary << "Picture numbers against content (" << offsets.windows_with_motion
          << " of " << offsets.windows
          << " windows held a frame whose picture differs from its "
             "neighbours', against source "
          << (*offsets.reference + 1) << "): ";
  if (any) {
    summary << "some sources' content sits off their picture numbers; see "
               "the warnings. Nothing has been changed.\n";
  } else {
    summary << "every source's content matches its picture numbers.\n";
  }
  report.summary = summary.str();
  return report;
}

}  // namespace orc
