/*
 * File:        capture_comparison.cpp
 * Module:      orc-core/analysis
 * Purpose:     Compare aligned captures of one disc: pressing groups, noise,
 *              and high-frequency response
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "capture_comparison.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>

namespace orc {

namespace {

// The band the response is measured in.  It sits above the PAL subcarrier and
// its chroma, where a soft capture chain loses most.
constexpr double kHighBandLow = 5.0e6;
constexpr double kHighBandHigh = 6.0e6;

// DFT window for the response, and the line decimation.
constexpr size_t kBandWindow = 128;
constexpr size_t kBandLineStep = 8;

// Pressings need at least this many sources to be told apart.
constexpr size_t kMinSourcesForPressings = 4;

double sample_rate(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
      return kPalSampleRate;
    case VideoSystem::PAL_M:
      return kPalMSampleRate;
    case VideoSystem::NTSC:
    default:
      return kNtscSampleRate;
  }
}

double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t mid = v.size() / 2;
  return v.size() % 2 ? v[mid] : 0.5 * (v[mid - 1] + v[mid]);
}

// Accumulates the cross-spectrum of every pair of sources in the 5–6 MHz
// band.  The picture there is common to every source, scaled by each one's
// response r, while noise of different sources does not correlate, so the
// pair cross-power |C_ij| = r_i·r_j·P and log|C_ij| = a_i + a_j gives each
// source's response up to a common factor.
//
// Noise of different discs of one pressing does correlate: their stamper's.
// The band runs past the top of the picture, where on Community South the
// eight discs of one pressing cohered at 0.95 on their shared noise alone
// and the three discs of other pressings at 0.05, so pairs within the large
// pressing read twice the cross-power of the rest and the three, whose
// detail was as strong as anyone's, read a response of 0.2.  Where the
// pressings allow it, only pairs of different pressings are fitted.
class HighBandMeter {
 public:
  HighBandMeter(size_t sources, VideoSystem system)
      : sources_(sources), cross_(sources * sources) {
    const double rate = sample_rate(system);
    const auto window = static_cast<double>(kBandWindow);
    const auto first =
        static_cast<size_t>(std::ceil(kHighBandLow * window / rate));
    const auto last =
        static_cast<size_t>(std::floor(kHighBandHigh * window / rate));
    constexpr double kTwoPi = 6.283185307179586;
    for (size_t k = first; k <= last && k < kBandWindow / 2; ++k) {
      std::vector<std::complex<double>> basis(kBandWindow);
      for (size_t n = 0; n < kBandWindow; ++n) {
        // Hann window, so the strong luma and chroma below the band do not
        // leak into it.
        const double hann =
            0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(n) / window);
        basis[n] =
            std::polar(hann, -kTwoPi * static_cast<double>(k * n) / window);
      }
      basis_.push_back(std::move(basis));
    }
  }

  void add_frame(const std::vector<const std::vector<StackSample>*>& planes,
                 const std::vector<const std::vector<uint8_t>*>& masks,
                 VideoSystem system, size_t nominal_width, size_t height) {
    std::vector<size_t> valid;
    for (size_t i = 0; i < sources_; ++i) {
      if (planes[i] && masks[i]) valid.push_back(i);
    }
    if (valid.size() < 2 || basis_.empty()) return;

    const size_t field1_lines = std::min(field1_line_count(system), height);
    std::vector<std::vector<std::complex<double>>> spectra(
        sources_, std::vector<std::complex<double>>(basis_.size()));
    for (size_t line = 0; line < height; line += kBandLineStep) {
      const size_t in_field = line < field1_lines ? line : line - field1_lines;
      if (in_field < kFieldBlankingLines) continue;
      const size_t start =
          frame_line_sample_offset(system, nominal_width, line);
      const size_t length =
          frame_line_sample_count(system, nominal_width, line);
      const size_t first = length / 5;
      const size_t last = length > 10 ? length - 10 : length;
      for (size_t x = first; x + kBandWindow <= last; x += kBandWindow) {
        const size_t off = start + x;
        const bool flagged =
            std::any_of(valid.begin(), valid.end(), [&](size_t i) {
              const auto& mask = *masks[i];
              return std::any_of(
                  mask.begin() + static_cast<std::ptrdiff_t>(off),
                  mask.begin() + static_cast<std::ptrdiff_t>(off + kBandWindow),
                  [](uint8_t m) { return m != 0; });
            });
        if (flagged) continue;

        for (size_t i : valid) {
          const auto& plane = *planes[i];
          for (size_t k = 0; k < basis_.size(); ++k) {
            std::complex<double> sum;
            for (size_t n = 0; n < kBandWindow; ++n) {
              sum += static_cast<double>(plane[off + n]) * basis_[k][n];
            }
            spectra[i][k] = sum;
          }
        }
        for (size_t a = 0; a < valid.size(); ++a) {
          for (size_t b = a + 1; b < valid.size(); ++b) {
            const size_t i = valid[a];
            const size_t j = valid[b];
            for (size_t k = 0; k < basis_.size(); ++k) {
              cross_[i * sources_ + j] +=
                  spectra[i][k] * std::conj(spectra[j][k]);
            }
          }
        }
      }
    }
  }

  // Each source's response against the median source's, from the least
  // squares solution of log|C_ij| = a_i + a_j.  Pairs of sources of
  // different families (by |group|) are used when they alone settle every
  // a_i — three families or more, connected — and every pair otherwise.
  // Sources of one family share its mother's noise and their pressings'
  // as well as the picture, so their correlation overstates the response.
  std::vector<std::optional<double>> relative_response(
      const std::vector<std::optional<size_t>>& group) const {
    std::vector<std::optional<double>> response(sources_);
    std::vector<size_t> members;
    for (size_t i = 0; i < sources_; ++i) {
      for (size_t j = 0; j < sources_; ++j) {
        if (i != j && std::abs(pair(i, j)) > 0.0) {
          members.push_back(i);
          break;
        }
      }
    }
    const size_t m = members.size();
    if (m < 3) return response;
    for (size_t p = 0; p < m; ++p) {
      for (size_t q = p + 1; q < m; ++q) {
        if (std::abs(pair(members[p], members[q])) <= 0.0) return response;
      }
    }

    auto across = [&](size_t p, size_t q) {
      const size_t i = members[p];
      const size_t j = members[q];
      return i < group.size() && j < group.size() && group[i] && group[j] &&
             *group[i] != *group[j];
    };
    std::optional<std::vector<double>> a = solve_response(members, across);
    if (!a) a = solve_response(members, [](size_t, size_t) { return true; });
    if (!a) return response;

    const double typical = median_of(*a);
    for (size_t p = 0; p < m; ++p) {
      response[members[p]] = std::exp((*a)[p] - typical);
    }
    return response;
  }

 private:
  std::complex<double> pair(size_t i, size_t j) const {
    return cross_[std::min(i, j) * sources_ + std::max(i, j)];
  }

  // a from the pairs of |members| that |use| accepts, by the normal
  // equations; nullopt when those pairs leave some a undetermined (a pair
  // graph that is disconnected or splits into two sides).
  template <typename Use>
  std::optional<std::vector<double>> solve_response(
      const std::vector<size_t>& members, Use use) const {
    const size_t m = members.size();
    std::vector<std::vector<double>> a(m, std::vector<double>(m + 1, 0.0));
    for (size_t p = 0; p < m; ++p) {
      for (size_t q = p + 1; q < m; ++q) {
        if (!use(p, q)) continue;
        const double y = std::log(std::abs(pair(members[p], members[q])));
        a[p][p] += 1.0;
        a[q][q] += 1.0;
        a[p][q] += 1.0;
        a[q][p] += 1.0;
        a[p][m] += y;
        a[q][m] += y;
      }
    }
    for (size_t col = 0; col < m; ++col) {
      size_t pivot = col;
      for (size_t row = col + 1; row < m; ++row) {
        if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
      }
      if (std::abs(a[pivot][col]) < 1e-9) return std::nullopt;
      std::swap(a[col], a[pivot]);
      for (size_t row = 0; row < m; ++row) {
        if (row == col) continue;
        const double factor = a[row][col] / a[col][col];
        for (size_t k = col; k <= m; ++k) a[row][k] -= factor * a[col][k];
      }
    }
    std::vector<double> x(m);
    for (size_t p = 0; p < m; ++p) x[p] = a[p][m] / a[p][p];
    return x;
  }

  size_t sources_;
  std::vector<std::vector<std::complex<double>>> basis_;
  std::vector<std::complex<double>> cross_;
};

// Each source's padding frames within its coverage, into |result|.  Reading
// every frame's descriptor costs about 50 µs a frame through a frame map
// over a CVBS source, 30 s for eleven captures of a side, so each distinct
// source is counted on a thread of its own; a source taken twice is counted
// once, so no source sees two requests at a time.
void count_padding(const std::vector<const VideoFrameRepresentation*>& sources,
                   const std::vector<std::optional<int64_t>>& starts,
                   CaptureComparison& result) {
  const size_t n = sources.size();
  std::vector<std::thread> threads;
  std::vector<size_t> first_of(n);
  for (size_t i = 0; i < n; ++i) {
    first_of[i] = i;
    for (size_t j = 0; j < i; ++j) {
      if (sources[j] == sources[i] && starts[j] == starts[i]) {
        first_of[i] = j;
        break;
      }
    }
    const std::optional<std::pair<int64_t, int64_t>> coverage =
        result.coverage[i];
    const std::optional<int64_t> start_of = starts[i];
    if (first_of[i] != i || !coverage || !start_of) continue;
    const VideoFrameRepresentation* source = sources[i];
    const int64_t start = *start_of;
    const auto [first, last] = *coverage;
    size_t& padding = result.padding[i];
    threads.emplace_back([source, start, first, last, &padding] {
      for (int64_t k = first; k <= last; ++k) {
        const auto desc =
            source->get_frame_descriptor(static_cast<FrameID>(start + k));
        if (desc && desc->is_padding_frame) ++padding;
      }
    });
  }
  for (auto& thread : threads) thread.join();
  for (size_t i = 0; i < n; ++i) {
    result.padding[i] = result.padding[first_of[i]];
  }
}

}  // namespace

CaptureComparison compare_captures(
    const std::vector<const VideoFrameRepresentation*>& sources,
    const std::vector<std::optional<int64_t>>& starts) {
  const size_t n = sources.size();
  CaptureComparison result;
  result.grid_offsets.resize(n);
  result.noise.resize(n);
  result.response.resize(n);
  result.coverage.resize(n);
  result.padding.resize(n, 0);

  // The stretch of output frames every source taking part covers.
  std::vector<size_t> members;
  int64_t lowest = 0;
  int64_t highest = 0;
  for (size_t i = 0; i < n && i < starts.size(); ++i) {
    if (!sources[i] || !starts[i] || sources[i]->frame_count() == 0) continue;
    const FrameIDRange range = sources[i]->frame_range();
    const int64_t first = static_cast<int64_t>(range.first) - *starts[i];
    const int64_t last = static_cast<int64_t>(range.last) - *starts[i];
    if (last >= 0) result.coverage[i] = {std::max<int64_t>(first, 0), last};
    lowest = members.empty() ? first : std::max(lowest, first);
    highest = members.empty() ? last : std::min(highest, last);
    members.push_back(i);
  }
  count_padding(sources, starts, result);
  if (members.size() < 2 || lowest > highest) return result;

  auto frame_of = [&](size_t i, int64_t k) {
    return static_cast<FrameID>(*starts[i] + k);
  };

  // Positions where every source holds a real frame of the full size, picked
  // as the Stacker picks the frames it groups its inputs from, so the two
  // group the same sources alike.  A position where any source has no frame,
  // or only padding, is passed over.
  const size_t reference = members.front();
  std::optional<FrameDescriptor> geometry;
  std::vector<int64_t> probes;
  const auto span = static_cast<size_t>(highest - lowest + 1);
  for (size_t c = 0;
       c < kPressingProbeCandidates && probes.size() < kPressingProbeFrames;
       ++c) {
    const int64_t k =
        lowest + static_cast<int64_t>(pressing_probe_candidate(c, span));
    if (std::find(probes.begin(), probes.end(), k) != probes.end()) continue;
    bool usable = true;
    for (size_t i : members) {
      const FrameID id = frame_of(i, k);
      const auto desc = sources[i]->has_frame(id)
                            ? sources[i]->get_frame_descriptor(id)
                            : std::nullopt;
      if (!desc || desc->is_padding_frame || desc->height == 0 ||
          desc->samples_per_line_nominal == 0) {
        usable = false;
        break;
      }
      if (!geometry) geometry = desc;
      if (desc->system != geometry->system ||
          desc->height != geometry->height ||
          desc->samples_per_line_nominal !=
              geometry->samples_per_line_nominal) {
        usable = false;
        break;
      }
    }
    if (usable) probes.push_back(k);
  }
  if (probes.empty() || !geometry) return result;

  const VideoSystem system = geometry->system;
  const size_t width = geometry->samples_per_line_nominal;
  const size_t height = geometry->height;
  const size_t frame_samples = frame_line_sample_offset(system, width, height);

  // Luma for a YC source: it carries the picture detail and is the plane both
  // channels' grids share.
  auto read = [&](size_t i, FrameID id) {
    std::vector<StackSample> out;
    if (sources[i]->has_separate_channels()) {
      const StackSample* plane = sources[i]->get_frame_luma(id);
      if (plane) out.assign(plane, plane + frame_samples);
    } else {
      out = sources[i]->get_frame_copy(id);
    }
    if (out.size() < frame_samples) out.clear();
    return out;
  };

  // Grids first: the difference of two sources is noise only once they agree.
  std::vector<SampleGridMeter> grids(n);
  for (int64_t k : probes) {
    const auto reference_frame = read(reference, frame_of(reference, k));
    if (reference_frame.empty()) continue;
    for (size_t i : members) {
      if (i == reference) continue;
      const auto other = read(i, frame_of(i, k));
      if (other.empty()) continue;
      grids[i].add_frame(reference_frame, other, system, width, height);
    }
  }
  result.grid_offsets[reference] = 0;
  for (size_t i : members) {
    if (i == reference) continue;
    if (sources[i] == sources[reference]) {
      result.grid_offsets[i] = 0;
      continue;
    }
    result.grid_offsets[i] = grids[i].measure().offset;
  }

  PairNoiseMeter noise(n);
  HighBandMeter band(n, system);
  std::vector<std::vector<StackSample>> planes(n);
  std::vector<std::vector<uint8_t>> masks(n);
  for (int64_t k : probes) {
    std::vector<const std::vector<StackSample>*> frame_planes(n, nullptr);
    std::vector<const std::vector<uint8_t>*> frame_masks(n, nullptr);
    size_t valid = 0;
    for (size_t i : members) {
      const std::optional<int32_t> offset = result.grid_offsets[i];
      if (!offset) continue;
      const FrameID id = frame_of(i, k);
      planes[i] = read(i, id);
      if (planes[i].empty()) continue;
      auto runs = sources[i]->get_dropout_hints(id);
      shift_frame_samples(planes[i], system, width, height, *offset);
      shift_dropout_runs(runs, system, width, height, *offset);
      masks[i] = dropout_mask(runs, frame_samples);
      frame_planes[i] = &planes[i];
      frame_masks[i] = &masks[i];
      ++valid;
    }
    if (valid < 2) continue;
    noise.add_frame(frame_planes, frame_masks, system, width, height);
    band.add_frame(frame_planes, frame_masks, system, width, height);
    ++result.frames;
  }

  const std::vector<std::optional<double>> pair_variance =
      noise.pair_variances();
  // The pair variances the grouping is decided from, one row per source.
  ORC_LOG_DEBUG(
      "Capture comparison: {} frames, {} pairs from the whole picture",
      result.frames, noise.whole_picture_pairs());
  for (size_t i = 0; i < n; ++i) {
    std::ostringstream row;
    for (size_t j = 0; j < n; ++j) {
      const std::optional<double> v =
          i == j ? std::nullopt
                 : pair_variance[std::min(i, j) * n + std::max(i, j)];
      row << (v ? std::to_string(std::lround(*v)) : std::string("-")) << " ";
    }
    ORC_LOG_DEBUG("Capture comparison: pair variances, source {}: {}", i + 1,
                  row.str());
  }
  result.noise = fit_pressing_noise(n, pair_variance);
  result.fit = check_pressing_fit(n, pair_variance, result.noise);
  result.levels = noise.levels();
  std::vector<std::optional<size_t>> families(n);
  for (size_t i = 0; i < n; ++i) {
    const std::optional<PressingNoise>& source_noise = result.noise[i];
    if (!source_noise) continue;
    result.pressings = std::max(result.pressings, source_noise->group + 1);
    result.families = std::max(result.families, source_noise->family + 1);
    families[i] = source_noise->family;
  }
  // The response needs pairs whose noise does not correlate, which only a
  // trusted grouping can pick out.
  if (result.fit.trusted) {
    result.response = band.relative_response(families);
  } else {
    result.noise = equal_weight_noise(n, pair_variance, result.noise);
  }
  return result;
}

namespace {

// Stack noise σ, under the comparison's model, of every compared source but
// each one; nullopt for a source not compared.
std::vector<std::optional<double>> stack_sigma_without_each(
    const CaptureComparison& comparison) {
  const size_t n = comparison.noise.size();
  std::vector<size_t> compared;
  std::vector<PressingNoise> all;
  for (size_t i = 0; i < n; ++i) {
    const std::optional<PressingNoise>& noise = comparison.noise[i];
    if (!noise) continue;
    compared.push_back(i);
    all.push_back(*noise);
  }
  std::vector<std::optional<double>> without(n);
  if (all.size() < 2) return without;
  for (size_t p = 0; p < compared.size(); ++p) {
    std::vector<PressingNoise> others = all;
    others.erase(others.begin() + static_cast<std::ptrdiff_t>(p));
    without[compared[p]] = combined_sigma(others);
  }
  return without;
}

}  // namespace

std::vector<size_t> rank_references(const CaptureComparison& comparison) {
  std::vector<size_t> candidates;
  std::vector<int64_t> frames_covered(comparison.coverage.size(), 0);
  for (size_t i = 0; i < comparison.coverage.size(); ++i) {
    if (const auto& coverage = comparison.coverage[i]) {
      candidates.push_back(i);
      frames_covered[i] = coverage->second - coverage->first + 1;
    }
  }

  auto padding = [&](size_t i) {
    return i < comparison.padding.size() ? comparison.padding[i] : size_t{0};
  };
  auto grid_of = [&](size_t i) -> std::optional<int32_t> {
    return i < comparison.grid_offsets.size() ? comparison.grid_offsets[i]
                                              : std::nullopt;
  };
  std::vector<double> grids;
  for (size_t i : candidates) {
    if (const std::optional<int32_t> grid = grid_of(i)) grids.push_back(*grid);
  }
  std::sort(grids.begin(), grids.end());
  const double median_grid =
      grids.empty() ? 0.0
                    : (grids.size() % 2 ? grids[grids.size() / 2]
                                        : 0.5 * (grids[grids.size() / 2 - 1] +
                                                 grids[grids.size() / 2]));
  // A source whose grid was not measured ranks after every one that was.
  auto grid_distance = [&](size_t i) {
    const std::optional<int32_t> grid = grid_of(i);
    return grid ? std::abs(*grid - median_grid)
                : std::numeric_limits<double>::infinity();
  };
  // A source not compared adds nothing anyone measured.
  const std::vector<std::optional<double>> without =
      stack_sigma_without_each(comparison);
  auto adds = [&](size_t i) {
    return i < without.size() && without[i]
               ? *without[i]
               : -std::numeric_limits<double>::infinity();
  };

  std::vector<size_t> ranked;
  while (!candidates.empty()) {
    int64_t most = 0;
    for (size_t i : candidates) most = std::max(most, frames_covered[i]);
    size_t fewest = std::numeric_limits<size_t>::max();
    for (size_t i : candidates) {
      if (frames_covered[i] == most) fewest = std::min(fewest, padding(i));
    }
    auto eligible = [&](size_t i) {
      return frames_covered[i] == most &&
             padding(i) <= fewest + kReferencePaddingTolerance;
    };
    // Of the eligible, those adding as much as the most, within what the
    // summary can tell apart.
    double most_added = -std::numeric_limits<double>::infinity();
    for (size_t i : candidates) {
      if (eligible(i)) most_added = std::max(most_added, adds(i));
    }
    auto best = candidates.end();
    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
      if (!eligible(*it) || adds(*it) < most_added - kReferenceNoiseTolerance) {
        continue;
      }
      if (best == candidates.end()) {
        best = it;
        continue;
      }
      const size_t a = *it;
      const size_t b = *best;
      if (grid_distance(a) != grid_distance(b)) {
        if (grid_distance(a) < grid_distance(b)) best = it;
      } else if (padding(a) != padding(b)) {
        if (padding(a) < padding(b)) best = it;
      } else if (adds(a) > adds(b)) {
        best = it;
      }
    }
    ranked.push_back(*best);
    candidates.erase(best);
  }
  return ranked;
}

std::optional<size_t> suggest_reference(const CaptureComparison& comparison) {
  const std::vector<size_t> ranked = rank_references(comparison);
  if (ranked.empty()) return std::nullopt;
  return ranked.front();
}

CaptureComparisonReport describe_capture_comparison(
    const CaptureComparison& comparison) {
  CaptureComparisonReport report;
  std::ostringstream summary;
  const size_t n = comparison.noise.size();

  std::vector<size_t> compared;
  for (size_t i = 0; i < n; ++i) {
    if (comparison.noise[i]) compared.push_back(i);
  }
  report.sources.resize(n);
  for (size_t i = 0; i < n; ++i) {
    report.sources[i].line = "Source " + std::to_string(i + 1) + ": ";
    report.sources[i].data["source"] = static_cast<int>(i + 1);
    report.sources[i].data["compared"] = false;
  }
  if (comparison.frames == 0 || compared.size() < 2) {
    summary << "Capture comparison: the sources could not be compared — "
               "they share no stretch of real frames with a measurable "
               "sample grid.\n";
    for (auto& source : report.sources) source.line += "not compared";
    report.summary = summary.str();
    return report;
  }

  // The stack the Stacker's Weighted mode makes of every compared source,
  // and the same stack without each one.  When the model is not trusted the
  // noise is equal_weight_noise()'s, so this is the equal stack made then.
  std::vector<PressingNoise> all;
  double total_weight = 0.0;
  for (size_t i : compared) {
    all.push_back(*comparison.noise[i]);
    total_weight += comparison.noise[i]->weight;
  }
  const double stack_sigma = combined_sigma(all);
  std::vector<double> sigma_without(n, 0.0);
  const std::vector<std::optional<double>> measured_without =
      stack_sigma_without_each(comparison);
  for (size_t i : compared) sigma_without[i] = measured_without[i].value_or(0);

  summary << "Capture comparison (" << comparison.frames
          << " frames of the same picture):\n";
  for (size_t i = 0; i < n; ++i) {
    CaptureSourceReport& source = report.sources[i];
    std::ostringstream line;
    line << std::fixed;
    const std::optional<PressingNoise>& noise = comparison.noise[i];
    if (!noise) {
      source.line += "not compared";
      summary << "  " << source.line << "\n";
      continue;
    }
    auto& data = source.data;
    data["compared"] = true;
    data["noise_sigma"] = noise->sigma;
    data["own_sigma"] = std::sqrt(noise->own_variance);
    data["pressing_sigma"] = std::sqrt(noise->shared_variance);
    data["family_sigma"] = std::sqrt(noise->family_variance);
    data["stamper_group"] = pressing_name(noise->group);
    data["family"] = pressing_name(noise->family);
    data["weight_share"] =
        total_weight > 0.0 ? 100.0 * noise->weight / total_weight : 0.0;
    data["stack_sigma_without"] = sigma_without[i];
    if (i < comparison.levels.size() && comparison.levels[i]) {
      data["gain_percent"] = 100.0 * (comparison.levels[i]->gain - 1.0);
      data["level_codes"] = comparison.levels[i]->level;
    }
    if (i < comparison.coverage.size() && comparison.coverage[i]) {
      data["first_frame"] = static_cast<int>(comparison.coverage[i]->first + 1);
      data["last_frame"] = static_cast<int>(comparison.coverage[i]->second + 1);
      if (i < comparison.padding.size()) {
        data["padding_frames"] = static_cast<int>(comparison.padding[i]);
      }
    }

    line << "noise σ " << std::setprecision(1) << noise->sigma << " codes";
    const std::optional<double> response = comparison.response[i];
    if (response) {
      line << ", 5–6 MHz response " << std::setprecision(2) << *response;
      data["response_5_6mhz"] = *response;
    } else {
      data["response_note"] =
          std::string(compared.size() < 3       ? "fewer than 3 sources"
                      : !comparison.fit.trusted ? "fit not trusted"
                                                : "not measured");
    }
    data["soft"] = response.has_value() && *response < kSoftResponse;
    if (comparison.pressings > 1) {
      line << ", pressing " << pressing_name(noise->group);
    }
    const std::optional<int32_t> offset = comparison.grid_offsets[i];
    if (offset) data["sample_grid"] = static_cast<int>(*offset);
    if (offset && *offset != 0) {
      line << ", sample grid " << std::showpos << *offset << std::noshowpos;
    }
    source.line += line.str();
    summary << "  " << source.line << "\n";
  }
  summary << std::fixed;

  // Levels, for sources whose gain or level is off the common picture's.
  std::string levels;
  for (size_t i : compared) {
    if (i >= comparison.levels.size()) continue;
    const std::optional<LevelMatch>& level = comparison.levels[i];
    if (!level || (std::abs(level->gain - 1.0) < kReportedGainDifference &&
                   std::abs(level->level) < kReportedLevelDifference)) {
      continue;
    }
    std::ostringstream entry;
    entry << std::fixed << std::showpos << "source " << std::noshowpos
          << (i + 1) << " gain " << std::showpos << std::setprecision(1)
          << 100.0 * (level->gain - 1.0) << " %, level " << level->level
          << std::noshowpos << " codes";
    levels += (levels.empty() ? "" : "; ") + entry.str();
  }
  if (!levels.empty()) {
    summary << "Levels against the picture common to every source: " << levels
            << ". Players and decodes differ like this; the noise figures "
               "above are measured where it cannot count.\n";
  }

  if (compared.size() < kMinSourcesForPressings) {
    summary << "Pressings: cannot be told apart with fewer than "
            << kMinSourcesForPressings << " sources.\n";
  } else if (comparison.pressings <= 1) {
    summary << "Pressings: all " << compared.size()
            << " sources appear to come from one pressing. Stacking them "
               "cannot remove the noise recorded into its stamper; discs of "
               "another pressing would.\n";
  } else {
    summary << "Pressings: " << comparison.pressings << " groups —";
    std::vector<std::vector<size_t>> groups(comparison.pressings);
    for (size_t i : compared) {
      const std::optional<PressingNoise>& noise = comparison.noise[i];
      if (noise) groups[noise->group].push_back(i);
    }
    for (size_t g = 0; g < groups.size(); ++g) {
      summary << (g == 0 ? " " : ", ") << pressing_name(g) << " {";
      for (size_t m = 0; m < groups[g].size(); ++m) {
        summary << (m == 0 ? "" : ", ") << (groups[g][m] + 1);
      }
      summary << "}";
    }
    summary << ". The discs of a pressing share the noise recorded into its "
               "stamper, which only averages out across pressings, so the "
               "Stacker's Weighted mode counts each pressing about once.";
    // The share of each disc's noise its pressing's stamper accounts for.
    std::string shares;
    for (size_t g = 0; g < groups.size() && comparison.fit.trusted; ++g) {
      if (groups[g].size() < 2) continue;
      double total = 0.0;
      double shared = 0.0;
      for (size_t i : groups[g]) {
        const std::optional<PressingNoise>& noise = comparison.noise[i];
        if (!noise) continue;
        total += noise->sigma * noise->sigma;
        shared += noise->shared_variance;
      }
      if (total <= 0.0) continue;
      shares += (shares.empty() ? "" : ", ") + pressing_name(g) + " " +
                std::to_string(std::lround(100.0 * shared / total)) + " %";
    }
    if (!shares.empty()) {
      summary << " Share of each disc's noise that is its pressing's: "
              << shares << ".";
    }
    summary << "\n";

    // Pressings of one stamper family, and the share of their discs' noise
    // that is the family's.
    if (comparison.families < comparison.pressings) {
      std::vector<std::vector<size_t>> family_pressings(comparison.families);
      std::vector<std::pair<double, double>> family_noise(
          comparison.families, {0.0, 0.0});  // Σσ², ΣF
      for (size_t g = 0; g < groups.size(); ++g) {
        for (size_t i : groups[g]) {
          const std::optional<PressingNoise>& noise = comparison.noise[i];
          if (!noise || noise->family >= comparison.families) continue;
          auto& pressings = family_pressings[noise->family];
          if (std::find(pressings.begin(), pressings.end(), g) ==
              pressings.end()) {
            pressings.push_back(g);
          }
          family_noise[noise->family].first += noise->sigma * noise->sigma;
          family_noise[noise->family].second += noise->family_variance;
        }
      }
      std::string families;
      for (size_t f = 0; f < family_pressings.size(); ++f) {
        if (family_pressings[f].size() < 2) continue;
        std::string names;
        for (size_t k = 0; k < family_pressings[f].size(); ++k) {
          names += (k == 0                                ? ""
                    : k + 1 == family_pressings[f].size() ? " and "
                                                          : ", ") +
                   pressing_name(family_pressings[f][k]);
        }
        families += (families.empty() ? "" : "; ") + names;
        if (comparison.fit.trusted && family_noise[f].first > 0.0) {
          families +=
              " (" +
              std::to_string(std::lround(100.0 * family_noise[f].second /
                                         family_noise[f].first)) +
              " % of their discs' noise)";
        }
      }
      if (!families.empty()) {
        summary << "Families: pressings " << families
                << " come from one mother, whose noise the discs of all of "
                   "them share, so the Stacker's Weighted mode counts each "
                   "family about once.\n";
      }
    }
    for (const auto& group : groups) {
      if (group.size() != 1) continue;
      summary << "  Source " << (group.front() + 1)
              << " stands alone: it is either the only disc of its pressing "
                 "or much noisier than the rest, which look the same.\n";
    }
  }

  if (compared.size() >= 3 && !comparison.fit.trusted) {
    summary << "Noise model: no grouping into pressings and families "
               "explains the "
               "sources' differences (sources "
            << (comparison.fit.worst_i + 1) << " and "
            << (comparison.fit.worst_j + 1) << " differ "
            << static_cast<int>(std::lround(
                   100.0 * (std::exp(comparison.fit.worst_misfit) - 1.0)))
            << " % away from what the best grouping predicts), so each σ "
               "above is the source's spread against the others, the 5–6 MHz "
               "response cannot be told from noise the sources share, and the "
               "Stacker's Weighted mode stacks such sources equally.\n";
  }

  // What each source adds: how much noisier the stack would be without it,
  // most first.  This, not each source's own σ, is what to read when
  // choosing which to leave out.
  std::vector<size_t> by_contribution = compared;
  std::stable_sort(
      by_contribution.begin(), by_contribution.end(),
      [&](size_t a, size_t b) { return sigma_without[a] > sigma_without[b]; });
  summary << std::setprecision(2)
          << "What each source adds: the stack's noise σ is predicted at "
          << stack_sigma << " codes";
  if (compared.size() >= 3 && !comparison.fit.trusted) {
    summary << " with every source weighted equally";
  }
  summary << ", and without each source it would be, most first:\n";
  for (size_t i : by_contribution) {
    summary << "  without source " << (i + 1) << ": σ " << std::setprecision(2)
            << sigma_without[i] << " codes, "
            << std::lround(total_weight > 0.0
                               ? 100.0 * comparison.noise[i]->weight /
                                     total_weight
                               : 0.0)
            << " % of the weight\n";
  }
  if (comparison.pressings > 1) {
    summary << "A disc alone in its stamper group (the discs that share the "
               "noise recorded into one stamper) reads noisier than the rest "
               "yet often adds the most, as no other disc carries its noise. "
               "Stamper groups are noise, not versions of the content.\n";
  }

  // The source to put first among a Stacker's inputs, and the next choice,
  // so that a close call can be seen as one.
  const std::vector<size_t> references = rank_references(comparison);
  auto describe_candidate = [&](size_t i) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(2);
    const auto& coverage = comparison.coverage[i];
    text << "covers " << (coverage->second - coverage->first + 1)
         << " output frames, "
         << (i < comparison.padding.size() ? comparison.padding[i] : size_t{0})
         << " of them padding";
    if (comparison.noise[i]) {
      text << "; stack σ without it " << sigma_without[i];
    }
    return text.str();
  };
  if (!references.empty()) {
    const size_t reference = references.front();
    summary << "Suggested reference: source " << (reference + 1) << " ("
            << describe_candidate(reference)
            << "). Put it first among a Stacker's inputs: input 1 sets the "
               "sample grid the stack is made on.";
    if (references.size() > 1) {
      summary << " Next choice: source " << (references[1] + 1) << " ("
              << describe_candidate(references[1]) << ").";
      report.statistics["referenceRunnerUp"] =
          static_cast<int>(references[1] + 1);
    }
    summary << "\n";
    report.statistics["suggestedReference"] = static_cast<int>(reference + 1);
  }

  report.statistics["families"] = static_cast<int>(comparison.families);
  report.statistics["fitTrusted"] = comparison.fit.trusted;
  report.statistics["worstMisfit"] =
      100.0 * (std::exp(comparison.fit.worst_misfit) - 1.0);
  report.statistics["stackSigma"] = stack_sigma;
  // An untrusted model's equal-weight prediction is as wrong as the model;
  // the equal stack it falls back to is the stack itself.
  report.statistics["equalSigma"] =
      comparison.fit.trusted ? comparison.fit.equal_sigma : stack_sigma;
  double best_single = all.front().sigma;
  for (const PressingNoise& noise : all) {
    best_single = std::min(best_single, noise.sigma);
  }
  report.statistics["bestSingleSigma"] = best_single;

  for (size_t i = 0; i < n; ++i) {
    const std::optional<double> response = comparison.response[i];
    if (!response || *response >= kSoftResponse) continue;
    std::ostringstream warning;
    warning << "Source " << (i + 1) << " is soft: its response at 5–6 MHz is "
            << static_cast<int>(std::lround(*response * 100.0))
            << "% of the typical source's. Its capture chain loses fine "
               "detail, which stacking it with sharper sources blurs.";
    report.warnings.push_back(warning.str());
  }

  report.summary = summary.str();
  return report;
}

}  // namespace orc
