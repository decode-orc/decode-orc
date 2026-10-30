/*
 * File:        truth_comparison.cpp
 * Module:      orc-videosynth-support
 * Purpose:     Error measurements of a stage's output against a videosynth
 *              truth case: per band, and inside and outside sample runs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "truth_comparison.h"

#include <fftw3.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/stack_measurement.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orc_videosynth {

namespace {

// The PAL band edges, in Hz.
constexpr double kPalBandEdges[] = {0.0, 2.0e6, 3.5e6, 5.5e6};
constexpr const char* kBandNames[] = {"luma_low", "luma_high", "chroma"};

// The flat-buffer lines of the active picture. first/last_active_frame_line
// are interlaced frame numbers, the last exclusive; each field holds every
// other one, field 1 the even lines (as dropout_correct maps them).
std::vector<size_t> active_lines(const orc::SourceParameters& params) {
  std::vector<size_t> lines;
  const size_t field1 = orc::field1_line_count(params.system);
  const auto first = static_cast<size_t>(params.first_active_frame_line / 2);
  const auto field1_end =
      static_cast<size_t>((params.last_active_frame_line + 1) / 2);
  const auto field2_end =
      static_cast<size_t>(params.last_active_frame_line / 2);
  for (size_t line = first; line < field1_end; ++line) lines.push_back(line);
  for (size_t line = first; line < field2_end; ++line) {
    lines.push_back(field1 + line);
  }
  return lines;
}

void check_active_area(const orc::SourceParameters& params) {
  if (params.active_video_start < 0 ||
      params.active_video_end <= params.active_video_start ||
      params.first_active_frame_line < 0 ||
      params.last_active_frame_line <= params.first_active_frame_line) {
    throw std::invalid_argument("source parameters carry no active area");
  }
}

}  // namespace

std::vector<Band> composite_bands(orc::VideoSystem system) {
  const double scale = orc::fsc_from_system(system) /
                       orc::fsc_from_system(orc::VideoSystem::PAL);
  std::vector<Band> bands;
  for (size_t b = 0; b < 3; ++b) {
    bands.push_back({kBandNames[b], kPalBandEdges[b] * scale,
                     kPalBandEdges[b + 1] * scale});
  }
  return bands;
}

double ire_per_code(const orc::SourceParameters& params) {
  if (params.white_level <= params.blanking_level) {
    throw std::invalid_argument("source parameters carry no signal levels");
  }
  return 100.0 /
         static_cast<double>(params.white_level - params.blanking_level);
}

std::optional<double> gain_db(double capture_rms, double output_rms) {
  if (capture_rms <= 0.0 || output_rms <= 0.0) return std::nullopt;
  return 20.0 * std::log10(capture_rms / output_rms);
}

// ── BandErrorMeter ──────────────────────────────────────────────────────────

// One real-to-complex FFTW plan for the line length in use. Every line of an
// active area has the same length, so the plan is rebuilt only if it changes.
struct BandErrorMeter::Transform {
  size_t length = 0;
  double* input = nullptr;
  fftw_complex* output = nullptr;
  fftw_plan plan = nullptr;

  ~Transform() { release(); }

  void release() {
    if (plan != nullptr) fftw_destroy_plan(plan);
    fftw_free(input);
    fftw_free(output);
    plan = nullptr;
    input = nullptr;
    output = nullptr;
    length = 0;
  }

  void prepare(size_t n) {
    if (n == length) return;
    release();
    input = fftw_alloc_real(n);
    output = fftw_alloc_complex(n / 2 + 1);
    plan =
        fftw_plan_dft_r2c_1d(static_cast<int>(n), input, output, FFTW_ESTIMATE);
    length = n;
  }
};

BandErrorMeter::BandErrorMeter(orc::VideoSystem system, double ire_per_code)
    : system_(system),
      ire_per_code_(ire_per_code),
      bands_(composite_bands(system)),
      band_energy_(bands_.size(), 0.0),
      transform_(std::make_unique<Transform>()) {}

BandErrorMeter::~BandErrorMeter() = default;

void BandErrorMeter::add_line(const Sample* output, const Sample* truth,
                              size_t count) {
  if (count == 0) return;
  transform_->prepare(count);
  for (size_t n = 0; n < count; ++n) {
    transform_->input[n] =
        static_cast<double>(output[n]) - static_cast<double>(truth[n]);
  }
  fftw_execute(transform_->plan);

  // Parseval for a real transform: sum(e^2) = (1/N) sum over all N bins of
  // |X|^2, and the bins above N/2 mirror those below, so every bin but DC
  // (and Nyquist, for even N) counts twice.
  const double sample_rate = orc::sample_rate_from_system(system_);
  const size_t bins = count / 2 + 1;
  for (size_t k = 0; k < bins; ++k) {
    const double re = transform_->output[k][0];
    const double im = transform_->output[k][1];
    const bool single = k == 0 || (count % 2 == 0 && k == count / 2);
    const double energy =
        (single ? 1.0 : 2.0) * (re * re + im * im) / static_cast<double>(count);
    total_energy_ += energy;
    const double frequency =
        static_cast<double>(k) * sample_rate / static_cast<double>(count);
    for (size_t b = 0; b < bands_.size(); ++b) {
      if (frequency >= bands_[b].low_hz && frequency < bands_[b].high_hz) {
        band_energy_[b] += energy;
        break;
      }
    }
  }
  samples_ += count;
}

void BandErrorMeter::add_frame(const Sample* output, const Sample* truth,
                               const orc::SourceParameters& params) {
  check_active_area(params);
  const auto width = static_cast<size_t>(params.frame_width_nominal);
  const auto start = static_cast<size_t>(params.active_video_start);
  const auto count =
      static_cast<size_t>(params.active_video_end - params.active_video_start);
  for (const size_t line : active_lines(params)) {
    const size_t offset =
        orc::frame_line_sample_offset(params.system, width, line) + start;
    add_line(output + offset, truth + offset, count);
  }
}

std::vector<double> BandErrorMeter::rms_ire() const {
  std::vector<double> rms(bands_.size(), 0.0);
  if (samples_ == 0) return rms;
  for (size_t b = 0; b < bands_.size(); ++b) {
    rms[b] = std::sqrt(band_energy_[b] / static_cast<double>(samples_)) *
             ire_per_code_;
  }
  return rms;
}

double BandErrorMeter::total_rms_ire() const {
  if (samples_ == 0) return 0.0;
  return std::sqrt(total_energy_ / static_cast<double>(samples_)) *
         ire_per_code_;
}

// ── RunErrorMeter ───────────────────────────────────────────────────────────

RunErrorMeter::RunErrorMeter(double ire_per_code)
    : ire_per_code_(ire_per_code) {}

void RunErrorMeter::add_line(const Sample* output, const Sample* truth,
                             const uint8_t* inside, size_t count) {
  for (size_t n = 0; n < count; ++n) {
    const double e =
        static_cast<double>(output[n]) - static_cast<double>(truth[n]);
    if (inside[n] != 0) {
      inside_energy_ += e * e;
      ++inside_samples_;
    } else {
      outside_energy_ += e * e;
      ++outside_samples_;
    }
  }
}

void RunErrorMeter::add_frame(const Sample* output, const Sample* truth,
                              size_t total,
                              const std::vector<orc::DropoutRun>& runs,
                              const orc::SourceParameters& params) {
  check_active_area(params);
  const auto mask = orc::dropout_mask(runs, total);
  const auto width = static_cast<size_t>(params.frame_width_nominal);
  const auto start = static_cast<size_t>(params.active_video_start);
  const auto count =
      static_cast<size_t>(params.active_video_end - params.active_video_start);
  for (const size_t line : active_lines(params)) {
    const size_t offset =
        orc::frame_line_sample_offset(params.system, width, line) + start;
    if (offset + count > total) break;
    add_line(output + offset, truth + offset, mask.data() + offset, count);
  }
}

SplitError RunErrorMeter::result() const {
  SplitError split;
  split.inside_samples = inside_samples_;
  split.outside_samples = outside_samples_;
  if (inside_samples_ > 0) {
    split.inside_rms_ire =
        std::sqrt(inside_energy_ / static_cast<double>(inside_samples_)) *
        ire_per_code_;
  }
  if (outside_samples_ > 0) {
    split.outside_rms_ire =
        std::sqrt(outside_energy_ / static_cast<double>(outside_samples_)) *
        ire_per_code_;
  }
  return split;
}

// ── Damage the runs do not flag ────────────────────────────────────────────

std::vector<orc::DropoutRun> damage_runs(const Sample* capture,
                                         const Sample* truth,
                                         const orc::SourceParameters& params,
                                         size_t total, orc::FrameID frame_id) {
  std::vector<orc::DropoutRun> runs;
  const auto width = static_cast<size_t>(params.frame_width_nominal);
  const auto height = static_cast<size_t>(params.frame_height);
  for (size_t line = 0; line < height; ++line) {
    const size_t start =
        orc::frame_line_sample_offset(params.system, width, line);
    const size_t end = std::min(total, start + orc::frame_line_sample_count(
                                                   params.system, width, line));
    size_t n = start;
    while (n < end) {
      if (capture[n] == truth[n]) {
        ++n;
        continue;
      }
      const size_t first = n;
      while (n < end && capture[n] != truth[n]) ++n;
      orc::DropoutRun run;
      run.frame_id = frame_id;
      run.sample_start = first;
      run.sample_count = static_cast<uint32_t>(n - first);
      runs.push_back(run);
    }
  }
  return runs;
}

double Coverage::flagged_fraction() const {
  if (damaged_samples == 0) return 1.0;
  return static_cast<double>(flagged_samples) /
         static_cast<double>(damaged_samples);
}

void CoverageMeter::add_frame(const std::vector<orc::DropoutRun>& damage,
                              const std::vector<orc::DropoutRun>& flagged,
                              size_t total,
                              const orc::SourceParameters& params) {
  check_active_area(params);
  const auto damaged = orc::dropout_mask(damage, total);
  const auto covered = orc::dropout_mask(flagged, total);
  const auto width = static_cast<size_t>(params.frame_width_nominal);
  const auto start = static_cast<size_t>(params.active_video_start);
  const auto count =
      static_cast<size_t>(params.active_video_end - params.active_video_start);
  for (const size_t line : active_lines(params)) {
    const size_t offset =
        orc::frame_line_sample_offset(params.system, width, line) + start;
    if (offset + count > total) break;
    for (size_t n = offset; n < offset + count; ++n) {
      if (damaged[n] == 0) continue;
      ++coverage_.damaged_samples;
      if (covered[n] != 0) ++coverage_.flagged_samples;
    }
  }
}

}  // namespace orc_videosynth
