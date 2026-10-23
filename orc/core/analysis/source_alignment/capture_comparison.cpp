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

#include <algorithm>
#include <cmath>
#include <complex>
#include <iomanip>
#include <sstream>

namespace orc {

namespace {

// Frames compared, and the candidate positions they are taken from.  A
// position where any source has no frame, or only padding, is passed over.
constexpr size_t kComparisonFrames = 8;
constexpr size_t kComparisonCandidates = 16;

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

  // Each source's response against the median source's.  With every pair
  // measured, the least-squares solution of log|C_ij| = a_i + a_j is
  // a_i = (R_i − S) / (m − 2), where R_i sums source i's pairs and
  // S = ΣR / (2(m − 1)).
  std::vector<std::optional<double>> relative_response() const {
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

    std::vector<double> row_sum(m, 0.0);
    double total = 0.0;
    for (size_t p = 0; p < m; ++p) {
      for (size_t q = 0; q < m; ++q) {
        if (p == q) continue;
        row_sum[p] += std::log(std::abs(pair(members[p], members[q])));
      }
      total += row_sum[p];
    }
    const double s = total / (2.0 * static_cast<double>(m - 1));
    std::vector<double> a(m);
    for (size_t p = 0; p < m; ++p) {
      a[p] = (row_sum[p] - s) / static_cast<double>(m - 2);
    }
    const double typical = median_of(a);
    for (size_t p = 0; p < m; ++p) {
      response[members[p]] = std::exp(a[p] - typical);
    }
    return response;
  }

 private:
  std::complex<double> pair(size_t i, size_t j) const {
    return cross_[std::min(i, j) * sources_ + std::max(i, j)];
  }

  size_t sources_;
  std::vector<std::vector<std::complex<double>>> basis_;
  std::vector<std::complex<double>> cross_;
};

// Pressing groups as letters: A, B, ... then A2, B2 past Z.
std::string pressing_name(size_t group) {
  std::string name(1, static_cast<char>('A' + group % 26));
  if (group >= 26) name += std::to_string(group / 26 + 1);
  return name;
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

  // The stretch of output frames every source taking part covers.
  std::vector<size_t> members;
  int64_t lowest = 0;
  int64_t highest = 0;
  for (size_t i = 0; i < n && i < starts.size(); ++i) {
    if (!sources[i] || !starts[i] || sources[i]->frame_count() == 0) continue;
    const FrameIDRange range = sources[i]->frame_range();
    const int64_t first = static_cast<int64_t>(range.first) - *starts[i];
    const int64_t last = static_cast<int64_t>(range.last) - *starts[i];
    lowest = members.empty() ? first : std::max(lowest, first);
    highest = members.empty() ? last : std::min(highest, last);
    members.push_back(i);
  }
  if (members.size() < 2 || lowest > highest) return result;

  auto frame_of = [&](size_t i, int64_t k) {
    return static_cast<FrameID>(*starts[i] + k);
  };

  // Positions where every source holds a real frame of the full size.
  const size_t reference = members.front();
  std::optional<FrameDescriptor> geometry;
  std::vector<int64_t> probes;
  const int64_t span = highest - lowest + 1;
  for (size_t c = 1;
       c <= kComparisonCandidates && probes.size() < kComparisonFrames; ++c) {
    const int64_t k =
        lowest + (span * static_cast<int64_t>(c)) /
                     static_cast<int64_t>(kComparisonCandidates + 1);
    if (!probes.empty() && probes.back() == k) continue;
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

  result.noise = fit_pressing_noise(n, noise.pair_variances());
  result.response = band.relative_response();
  for (const auto& source_noise : result.noise) {
    if (source_noise) {
      result.pressings = std::max(result.pressings, source_noise->group + 1);
    }
  }
  return result;
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
  if (comparison.frames == 0 || compared.size() < 2) {
    summary << "Capture comparison: the sources could not be compared — "
               "they share no stretch of real frames with a measurable "
               "sample grid.\n";
    report.summary = summary.str();
    return report;
  }

  summary << "Capture comparison (" << comparison.frames
          << " frames of the same picture):\n";
  summary << std::fixed;
  for (size_t i = 0; i < n; ++i) {
    summary << "  Source " << (i + 1) << ": ";
    const std::optional<PressingNoise>& noise = comparison.noise[i];
    if (!noise) {
      summary << "not compared\n";
      continue;
    }
    summary << "noise σ " << std::setprecision(1) << noise->sigma << " codes";
    const std::optional<double> response = comparison.response[i];
    if (response) {
      summary << ", 5–6 MHz response " << std::setprecision(2) << *response;
    }
    if (comparison.pressings > 1) {
      summary << ", pressing " << pressing_name(noise->group);
    }
    const std::optional<int32_t> offset = comparison.grid_offsets[i];
    if (offset && *offset != 0) {
      summary << ", sample grid " << std::showpos << *offset << std::noshowpos;
    }
    summary << "\n";
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
               "Stacker's Weighted mode counts each pressing about once.\n";
    for (const auto& group : groups) {
      if (group.size() != 1) continue;
      summary << "  Source " << (group.front() + 1)
              << " stands alone: it is either the only disc of its pressing "
                 "or much noisier than the rest, which look the same.\n";
    }
  }

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
