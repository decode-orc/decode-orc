/*
 * File:        vbi_level_mapper.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Maps card-capture sample levels into the CVBS amplitude domain
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_level_mapper.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace orc {

namespace {

// Linear-interpolated quantile of an already sorted, non-empty sample set.
double sorted_quantile(const std::vector<double>& sorted, double quantile) {
  if (sorted.empty()) {
    return 0.0;
  }
  if (sorted.size() == 1) {
    return sorted.front();
  }

  const double position = quantile * static_cast<double>(sorted.size() - 1);
  const double floor_position = std::floor(position);
  const size_t lower = static_cast<size_t>(floor_position);
  const size_t upper = std::min(lower + 1, sorted.size() - 1);
  const double fraction = position - floor_position;
  return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

// Linear-interpolated quantile of an unsorted sample set, by selection rather
// than a whole sort.  |values| is reordered.
double select_quantile(std::vector<double>& values, double quantile) {
  if (values.empty()) {
    return 0.0;
  }
  const double position =
      std::clamp(quantile, 0.0, 1.0) * static_cast<double>(values.size() - 1);
  const double floor_position = std::floor(position);
  const size_t lower = static_cast<size_t>(floor_position);
  const auto lower_it = values.begin() + static_cast<ptrdiff_t>(lower);
  std::nth_element(values.begin(), lower_it, values.end());
  const double lower_value = *lower_it;
  if (lower + 1 >= values.size()) {
    return lower_value;
  }
  // The next order statistic is the smallest of everything above the lower
  // one, which nth_element has left to its right.
  const double upper_value = *std::min_element(lower_it + 1, values.end());
  return lower_value +
         (upper_value - lower_value) * (position - floor_position);
}

double median_of(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  return sorted_quantile(values, 0.5);
}

// Copy a half-open window of a record, clamped to the samples present.
std::vector<double> window_samples(const std::vector<double>& samples,
                                   uint32_t begin, uint32_t end) {
  const size_t count = samples.size();
  const size_t first = std::min(static_cast<size_t>(begin), count);
  const size_t last = std::min(static_cast<size_t>(end), count);
  if (last <= first) {
    return {};
  }
  return std::vector<double>(samples.begin() + static_cast<ptrdiff_t>(first),
                             samples.begin() + static_cast<ptrdiff_t>(last));
}

// Quantiles of a half-open window of a record, clamped to the samples present,
// each the value select_quantile() gives for it.
//
// An 8-bit card's samples are whole numbers from 0 to 255, decode_vbi_samples()
// writing each byte as its own value, and a window of those is counted rather
// than selected from: the two order statistics a quantile lies between are read
// off the running count, which finds the same two values nth_element() does,
// so the quantile is the same to the bit at a seventh of the cost — and the
// count serves both of a window's quantiles.  A window holding anything else (a
// 16-bit sample, a fraction, a negative zero, a NaN) is copied and selected
// from as before.
class WindowQuantiles {
 public:
  // |bytes| vouches that every sample is a whole number from 0 to 255, so
  // that none needs checking.
  WindowQuantiles(const std::vector<double>& samples, uint32_t begin,
                  uint32_t end, bool bytes) {
    const size_t first = std::min(static_cast<size_t>(begin), samples.size());
    const size_t last = std::min(static_cast<size_t>(end), samples.size());
    if (last <= first) {
      return;
    }
    size_ = last - first;

    counted_ = true;
    if (bytes) {
      for (size_t index = first; index < last; ++index) {
        ++counts_[static_cast<uint32_t>(samples[index])];
      }
      return;
    }
    for (size_t index = first; index < last; ++index) {
      const double value = samples[index];
      // Every double from +0 to 255 has bits no greater than 255's; a negative
      // one (negative zero included), a NaN, an infinity and anything above
      // 255 all compare greater.
      uint64_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      if (bits > kBitsOf255) {
        counted_ = false;
        break;
      }
      const auto byte = static_cast<uint32_t>(value);
      if (static_cast<double>(byte) != value) {
        counted_ = false;
        break;
      }
      ++counts_[byte];
    }
    if (!counted_) {
      values_ = window_samples(samples, begin, end);
    }
  }

  bool empty() const { return size_ == 0; }

  double quantile(double quantile) {
    if (!counted_) {
      return select_quantile(values_, quantile);
    }
    const double position =
        std::clamp(quantile, 0.0, 1.0) * static_cast<double>(size_ - 1);
    const double floor_position = std::floor(position);
    const auto lower = static_cast<size_t>(floor_position);

    // The value at rank |lower|, counting from zero, and how many lie below
    // it.
    size_t below = 0;
    uint32_t value = 0;
    while (below + counts_[value] <= lower) {
      below += counts_[value];
      ++value;
    }
    const auto lower_value = static_cast<double>(value);
    if (lower + 1 >= size_) {
      return lower_value;
    }

    // The next rank up: the same value while it has copies left, otherwise
    // the next value present.
    uint32_t upper = value;
    if (below + counts_[value] <= lower + 1) {
      do {
        ++upper;
      } while (counts_[upper] == 0);
    }
    const auto upper_value = static_cast<double>(upper);
    return lower_value +
           (upper_value - lower_value) * (position - floor_position);
  }

 private:
  // 255.0 as IEEE 754 binary64.
  static constexpr uint64_t kBitsOf255 = 0x406FE00000000000ULL;

  size_t size_ = 0;
  bool counted_ = false;
  std::array<uint32_t, 256> counts_{};
  std::vector<double> values_;
};

}  // namespace

VBISampleMap make_vbi_sample_map(const VBILineLevels& levels,
                                 const VBIOutputLevels& output_levels) {
  VBISampleMap map;
  map.output_logic0 = static_cast<double>(output_levels.logic0);

  const double source_amplitude = levels.amplitude();
  if (!levels.usable || !(source_amplitude > 0.0)) {
    // Nothing gave a level reference, so there is no honest scale to apply.
    // A zero gain holds the whole record at blanking rather than amplifying
    // noise by a fabricated one (design §5.3.4).
    map.output_logic0 = static_cast<double>(output_levels.blanking);
    map.gain = 0.0;
    return map;
  }

  map.source_logic0 = levels.logic0;
  map.gain =
      static_cast<double>(output_levels.data_amplitude()) / source_amplitude;
  return map;
}

VBILineLevels vbi_levels_from_caption_levels(const VBILineLevels& caption,
                                             const VBIOutputLevels& output) {
  VBILineLevels levels;
  if (!caption.usable || !(caption.amplitude() > 0.0)) {
    return levels;
  }

  // SMPTE 170M: blanking at 0 IRE and white at 100 IRE, which the output
  // domain puts at kNtscBlanking and kNtscWhite.
  const double output_per_ire =
      static_cast<double>(kNtscWhite - kNtscBlanking) / 100.0;
  const double source_per_ire = caption.amplitude() / kCaptionDataOneIre;
  const auto source_at = [&](uint16_t output_level) {
    const double ire = (static_cast<double>(output_level) -
                        static_cast<double>(kNtscBlanking)) /
                       output_per_ire;
    return caption.logic0 + ire * source_per_ire;
  };

  levels.logic0 = source_at(output.logic0);
  levels.logic1 = source_at(output.logic1);
  levels.usable = levels.amplitude() > 0.0;
  return levels;
}

VBILevelMapperConfig vbi_absolute_level_config(
    const VBIOutputLevels& output_levels) {
  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kFixed;
  config.fixed_logic0 =
      static_cast<double>(output_levels.logic0) * kTBCSampleScale;
  config.fixed_logic1 =
      static_cast<double>(output_levels.logic1) * kTBCSampleScale;
  return config;
}

VBIDataWindows vbi_data_windows(const VBITeletextService& service,
                                double sample_rate_hz, double cri_start_samples,
                                uint32_t valid_samples) {
  VBIDataWindows windows;

  const double samples_per_bit = service.samples_per_bit(sample_rate_hz);
  if (!(samples_per_bit > 0.0) || !std::isfinite(cri_start_samples)) {
    return windows;
  }

  const double valid = static_cast<double>(valid_samples);
  const auto clamp_to_record = [valid](double position) {
    return static_cast<uint32_t>(std::max(0.0, std::min(position, valid)));
  };
  const auto at_bit = [&](double bit) {
    return cri_start_samples + samples_per_bit * bit;
  };

  // Whole samples strictly inside the bits each window spans: the run-in's
  // alternating bits, and the payload from the end of the framing code to the
  // end of the packet.
  const double run_in_bits = static_cast<double>(service.cri_bits);
  const double prefix_bits =
      static_cast<double>(service.cri_bits + service.frc_bits);
  const double packet_bits =
      prefix_bits + 8.0 * static_cast<double>(service.payload_bytes);
  windows.run_in_begin = clamp_to_record(std::ceil(at_bit(0.0)));
  windows.run_in_end = std::max(
      windows.run_in_begin, clamp_to_record(std::floor(at_bit(run_in_bits))));
  windows.payload_begin = clamp_to_record(std::ceil(at_bit(prefix_bits)));
  windows.payload_end = std::max(
      windows.payload_begin, clamp_to_record(std::floor(at_bit(packet_bits))));
  return windows;
}

VBIRecordWindows vbi_record_windows(const VBISourceFormat& format,
                                    const VBITeletextService& service) {
  VBIRecordWindows windows;

  const double cri_start = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  if (!(service.samples_per_bit(format.sample_rate_hz) > 0.0) ||
      !std::isfinite(cri_start)) {
    return windows;
  }

  // The fit put the run-in at cri_start; a line carrying the service has it
  // there to within the scatter the fit itself is allowed.
  const double tolerance =
      std::max(0.0, format.calibration.maximum_spread_samples);
  windows.presence.begin_samples = std::max(0.0, cri_start - tolerance);
  windows.presence.end_samples = std::max(0.0, cri_start + tolerance);

  windows.data = vbi_data_windows(service, format.sample_rate_hz, cri_start,
                                  format.valid_samples);
  return windows;
}

VBILineLevels estimate_vbi_data_levels(const std::vector<double>& samples,
                                       const VBIDataWindows& windows,
                                       const VBILevelMapperConfig& config,
                                       bool byte_samples) {
  VBILineLevels levels;

  WindowQuantiles payload(samples, windows.payload_begin, windows.payload_end,
                          byte_samples);
  if (payload.empty()) {
    return levels;
  }
  levels.logic0 = payload.quantile(config.data_low_quantile);
  levels.logic1 = payload.quantile(config.data_high_quantile);

  WindowQuantiles run_in(samples, windows.run_in_begin, windows.run_in_end,
                         byte_samples);
  if (!run_in.empty()) {
    levels.logic1 =
        std::max(levels.logic1, run_in.quantile(config.data_high_quantile));
  }

  levels.usable = levels.amplitude() >= config.minimum_amplitude_counts;
  return levels;
}

VBILevelMapper::VBILevelMapper(const VBISourceFormat& format,
                               const VBITeletextService& service,
                               VBILevelMapperConfig config)
    : config_(std::move(config)),
      windows_(vbi_record_windows(format, service)),
      presence_correlation_(format.calibration.acceptance_correlation) {
  // A template that cannot be made leaves the presence test failing every
  // line, so frames are mapped at the capture's measured levels.
  std::string error_message;
  if (!make_vbi_cri_frc_template(service, format.sample_rate_hz,
                                 VBICRITemplateConfig{}, cri_template_,
                                 error_message)) {
    cri_template_ = VBICRITemplate{};
  }
}

bool VBILevelMapper::carries_service(const std::vector<double>& samples) const {
  if (cri_template_.samples.empty() || windows_.presence.empty()) {
    return false;
  }
  return detect_vbi_cri_position(samples, cri_template_, windows_.presence,
                                 presence_correlation_)
      .accepted;
}

VBILineLevels VBILevelMapper::frame_levels(
    const std::vector<VBILineLevels>& measured,
    const std::vector<uint8_t>& carries) const {
  VBILineLevels levels;

  std::vector<double> logic0_values;
  std::vector<double> amplitude_values;
  logic0_values.reserve(measured.size());
  amplitude_values.reserve(measured.size());
  for (size_t index = 0; index < measured.size(); ++index) {
    const VBILineLevels& line = measured[index];
    if (!line.usable || carries[index] == 0) {
      continue;
    }
    logic0_values.push_back(line.logic0);
    amplitude_values.push_back(line.amplitude());
  }

  if (logic0_values.empty()) {
    // No line of this frame carries the service, so the frame says nothing
    // about the card's gain.  The capture's own data lines, measured when it
    // was opened, are the best reference there is.
    if (config_.capture_levels.has_value()) {
      levels = *config_.capture_levels;
      levels.usable = levels.usable &&
                      levels.amplitude() >= config_.minimum_amplitude_counts;
    }
    return levels;
  }

  levels.logic0 = median_of(std::move(logic0_values));
  levels.logic1 = levels.logic0 + median_of(std::move(amplitude_values));
  levels.usable = levels.amplitude() >= config_.minimum_amplitude_counts;
  return levels;
}

void VBILevelMapper::map_frame(const std::vector<VBILineRecord>& records,
                               std::vector<VBILineLevels>& out_levels) const {
  const bool fixed = config_.mode == VBILevelMode::kFixed;

  out_levels.clear();
  out_levels.reserve(records.size());
  std::vector<uint8_t> carries(records.size(), 0);
  for (size_t index = 0; index < records.size(); ++index) {
    const VBILineRecord& record = records[index];
    if (fixed) {
      out_levels.push_back(VBILineLevels{});
      continue;
    }
    out_levels.push_back(estimate_vbi_data_levels(
        record.samples, windows_.data, config_, record.byte_samples));
    carries[index] = carries_service(record.samples) ? 1 : 0;
  }

  VBILineLevels common;
  if (fixed) {
    common.logic0 = config_.fixed_logic0;
    common.logic1 = config_.fixed_logic1;
    common.usable = common.amplitude() >= config_.minimum_amplitude_counts;
  } else {
    common = frame_levels(out_levels, carries);
  }

  const double amplitude_floor =
      common.amplitude() * config_.minimum_amplitude_fraction;

  for (VBILineLevels& levels : out_levels) {
    if (!common.usable) {
      // Nothing in the frame gave a level reference, so every line of it is
      // emitted as blanking.
      levels = VBILineLevels{};
      continue;
    }

    if (fixed) {
      levels = common;
      continue;
    }

    // A line far below the frame's data amplitude carries no data service, so
    // its own estimate is noise and the frame's levels are used instead.
    bool apply_own = levels.usable && levels.amplitude() >= amplitude_floor;
    if (apply_own && config_.mode == VBILevelMode::kRolling) {
      // Hold the line at the frame's median unless it departs from it by more
      // than estimation noise, so that per-line gain noise is not propagated
      // into the output while a real gain change still is.
      const double deviation =
          std::fabs(levels.amplitude() - common.amplitude());
      apply_own =
          deviation > common.amplitude() * config_.rolling_deviation_fraction;
    }

    if (!apply_own) {
      levels = common;
    }
  }
}

}  // namespace orc
