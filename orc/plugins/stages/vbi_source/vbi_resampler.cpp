/*
 * File:        vbi_resampler.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Band-limited resampling of VBI records onto the output lattice
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace orc {

namespace {

// Modified Bessel function of the first kind, order zero, by its power series.
// The series converges quickly for the arguments a Kaiser window uses, and the
// loop stops as soon as a term stops contributing.
double bessel_i0(double x) {
  double sum = 1.0;
  double term = 1.0;
  for (int order = 1; order < 64; ++order) {
    const double factor = x / (2.0 * static_cast<double>(order));
    term *= factor * factor;
    sum += term;
    if (term < sum * 1e-18) {
      break;
    }
  }
  return sum;
}

// Normalised sinc, sin(pi x) / (pi x), with its removable singularity filled
// in.
double normalised_sinc(double x) {
  if (std::abs(x) < 1e-12) {
    return 1.0;
  }
  constexpr double kPi = 3.14159265358979323846;
  const double argument = kPi * x;
  return std::sin(argument) / argument;
}

// Records worked side by side (see resample_records()), where the
// compiler offers vectors: GCC and Clang, on any target.
#if defined(__GNUC__)
#define ORC_VBI_RESAMPLER_LANES 1
#if defined(__x86_64__)
#define ORC_VBI_RESAMPLER_AVX2 1
#endif
#endif

#if defined(ORC_VBI_RESAMPLER_LANES)

// Four records side by side, one to a lane: a single AVX2 register, or two
// SSE2 ones.  Lane by lane the arithmetic is the scalar's — the product, then
// its addition to the running sum — so each lane holds the bits the
// single-record resample() gives its record.
using RecordLanes = double __attribute__((vector_size(4 * sizeof(double))));
constexpr size_t kRecordLanes = 4;

// Fours of records a pass carries at once.
constexpr size_t kMaxRecordFours = 4;

// Independent sums a pass keeps going at once: each is a chain of additions,
// every one waiting on the last, and it takes about four chains to keep the
// adder busy.  Fewer fours of records than that are made up with output
// samples worked side by side, each its own sum.
constexpr size_t kChains = 4;

// What one pass reads and writes.  The pass's records are interleaved index by
// index — at each record index, the first four's samples, then the next
// four's — so that a four's samples at one index are one load, and the
// interleaving already holds each record's constant extension.
struct RecordPass {
  const double* bank = nullptr;
  const int32_t* first_tap = nullptr;
  const uint32_t* row = nullptr;
  uint32_t output_count = 0;
  uint32_t taps = 0;
  // The record index the interleaving starts at.
  int32_t first_index = 0;
  const double* interleaved = nullptr;
  // Where each lane's output samples go; null for a lane with no record.
  double* outputs[kMaxRecordFours * kRecordLanes] = {};
};

// Output samples |begin| to |end| of a pass, |kOutputs| at a time (which must
// divide the count), tap by tap in the kernel's order, |kFours| fours of
// records at a time: kOutputs x kFours sums side by side, each added in the
// order the single-record resample() adds it.
template <size_t kFours, size_t kOutputs>
[[gnu::always_inline]] inline void sum_record_outputs(const RecordPass& pass,
                                                      uint32_t begin,
                                                      uint32_t end) {
  constexpr size_t kStride = kFours * kRecordLanes;
  for (uint32_t sample = begin; sample < end; sample += kOutputs) {
    const double* weights[kOutputs];
    const double* values[kOutputs];
    for (size_t k = 0; k < kOutputs; ++k) {
      weights[k] =
          pass.bank + static_cast<size_t>(pass.row[sample + k]) * pass.taps;
      values[k] =
          pass.interleaved +
          static_cast<size_t>(pass.first_tap[sample + k] - pass.first_index) *
              kStride;
    }

    RecordLanes sums[kOutputs][kFours] = {};
    for (uint32_t tap = 0; tap < pass.taps; ++tap) {
      for (size_t k = 0; k < kOutputs; ++k) {
        const double weight = weights[k][tap];
        const RecordLanes weight_lanes = {weight, weight, weight, weight};
        for (size_t four = 0; four < kFours; ++four) {
          RecordLanes lanes;
          std::memcpy(&lanes, values[k] + four * kRecordLanes, sizeof(lanes));
          sums[k][four] += weight_lanes * lanes;
        }
        values[k] += kStride;
      }
    }

    for (size_t k = 0; k < kOutputs; ++k) {
      for (size_t four = 0; four < kFours; ++four) {
        for (size_t lane = 0; lane < kRecordLanes; ++lane) {
          double* output = pass.outputs[four * kRecordLanes + lane];
          if (output != nullptr) {
            output[sample + k] = sums[k][four][lane];
          }
        }
      }
    }
  }
}

// A whole pass: as many output samples side by side as its fours of records
// leave chains for, and the few left over one at a time.
template <size_t kFours>
[[gnu::always_inline]] inline void sum_records(const RecordPass& pass) {
  constexpr size_t kOutputs = kFours < kChains ? kChains / kFours : 1;
  const uint32_t whole = pass.output_count - pass.output_count % kOutputs;
  sum_record_outputs<kFours, kOutputs>(pass, 0, whole);
  sum_record_outputs<kFours, 1>(pass, whole, pass.output_count);
}

template <size_t kFours>
void sum_records_portable(const RecordPass& pass) {
  sum_records<kFours>(pass);
}

#if defined(ORC_VBI_RESAMPLER_AVX2)
template <size_t kFours>
[[gnu::target("avx2")]] void sum_records_avx2(const RecordPass& pass) {
  sum_records<kFours>(pass);
}

bool avx2_available() {
  static const bool available = __builtin_cpu_supports("avx2") != 0;
  return available;
}
#endif

void run_record_pass(size_t fours, const RecordPass& pass, bool use_avx2) {
#if defined(ORC_VBI_RESAMPLER_AVX2)
  if (use_avx2 && avx2_available()) {
    switch (fours) {
      case 1:
        sum_records_avx2<1>(pass);
        return;
      case 2:
        sum_records_avx2<2>(pass);
        return;
      case 3:
        sum_records_avx2<3>(pass);
        return;
      default:
        sum_records_avx2<kMaxRecordFours>(pass);
        return;
    }
  }
#else
  (void)use_avx2;
#endif
  switch (fours) {
    case 1:
      sum_records_portable<1>(pass);
      return;
    case 2:
      sum_records_portable<2>(pass);
      return;
    case 3:
      sum_records_portable<3>(pass);
      return;
    default:
      sum_records_portable<kMaxRecordFours>(pass);
      return;
  }
}

#endif  // ORC_VBI_RESAMPLER_LANES

}  // namespace

VBIRecordResampler::VBIRecordResampler(const VBIDataPlacement& placement,
                                       uint32_t record_samples,
                                       VBIResamplerConfig config)
    : config_(config) {
  config_.half_width_samples = std::max(1u, config_.half_width_samples);
  config_.phase_steps = std::max(1u, config_.phase_steps);
  config_.cutoff_fraction = std::clamp(config_.cutoff_fraction, 0.01, 1.0);
  config_.kaiser_beta = std::max(0.0, config_.kaiser_beta);

  const double ratio = placement.source_samples_per_output_sample;
  if (!(ratio > 0.0) || !std::isfinite(ratio) || record_samples == 0 ||
      placement.output_count() == 0) {
    return;
  }

  output_count_ = placement.output_count();

  // A capture already on the output's own lattice: the same rate, and a whole
  // number of samples between the two origins.  There is nothing to
  // interpolate and nothing that could alias, so the record is copied.
  // Filtering it would attenuate the top of the data band to no purpose, and
  // cost a hundred and twenty-eight taps a sample doing it.  This is every
  // TBC-derived capture, whose 4 x fsc rate is the output's by construction
  // (design §5.5).
  const double origin = placement.source_position_at_output_zero;
  if (ratio == 1.0 && origin == std::floor(origin)) {
    cutoff_normalised_ = 0.5;
    taps_ = 1;
    bank_.assign(1, 1.0);
    first_tap_.resize(output_count_);
    row_.assign(output_count_, 0);
    for (uint32_t sample = 0; sample < output_count_; ++sample) {
      first_tap_[sample] = static_cast<int32_t>(
          placement.source_position(placement.output_begin + sample));
    }
    return;
  }

  // The passband that must survive is the narrower of the two Nyquist limits.
  // Decimation is bounded by the output rate; anything at or below 1:1 is
  // bounded by the source rate, since there is no more bandwidth to keep.
  const double limiting_ratio = std::max(1.0, ratio);
  cutoff_normalised_ = config_.cutoff_fraction * 0.5 / limiting_ratio;

  taps_ = config_.half_width_samples * 2u;

  first_tap_.resize(output_count_);
  row_.resize(output_count_);

  const double window_denominator = bessel_i0(config_.kaiser_beta);
  const auto half_width = static_cast<int64_t>(config_.half_width_samples);
  const double steps = static_cast<double>(config_.phase_steps);

  // Bank row per distinct quantised fractional delay.  Every output sample of
  // a whole-number ratio quantises to the same delay, so the bank is one row.
  std::map<uint32_t, uint32_t> rows;

  for (uint32_t sample = 0; sample < output_count_; ++sample) {
    const double position = placement.source_position(
        static_cast<double>(placement.output_begin + sample));
    double floor_position = std::floor(position);
    auto phase = static_cast<uint32_t>(
        std::llround((position - floor_position) * steps));
    if (phase >= config_.phase_steps) {
      // The delay rounded up to a whole sample, which is the next centre.
      phase = 0;
      floor_position += 1.0;
    }
    const auto base = static_cast<int64_t>(floor_position);

    const auto existing = rows.find(phase);
    uint32_t row = 0;
    if (existing != rows.end()) {
      row = existing->second;
    } else {
      row = static_cast<uint32_t>(bank_.size() / taps_);
      rows.emplace(phase, row);

      const double fraction = static_cast<double>(phase) / steps;
      bank_.resize(bank_.size() + taps_);
      double* weights = bank_.data() + static_cast<size_t>(row) * taps_;

      double weight_sum = 0.0;
      for (int64_t tap = -half_width + 1; tap <= half_width; ++tap) {
        // Distance from the kernel centre to the stored sample this tap reads.
        const double offset = fraction - static_cast<double>(tap);
        const double window_position = offset / static_cast<double>(half_width);
        const double window_argument = 1.0 - window_position * window_position;
        const double window =
            (window_argument <= 0.0)
                ? 0.0
                : bessel_i0(config_.kaiser_beta * std::sqrt(window_argument)) /
                      window_denominator;
        const double weight =
            normalised_sinc(2.0 * cutoff_normalised_ * offset) * window;

        weights[tap + half_width - 1] = weight;
        weight_sum += weight;
      }

      // A row that sums to one has a response of exactly one at DC, at every
      // fractional delay.
      if (weight_sum != 0.0) {
        const double scale = 1.0 / weight_sum;
        for (uint32_t tap = 0; tap < taps_; ++tap) {
          weights[tap] *= scale;
        }
      }
    }

    row_[sample] = row;
    first_tap_[sample] = static_cast<int32_t>(base - half_width + 1);
  }
}

void VBIRecordResampler::resample(const std::vector<double>& record,
                                  std::vector<double>& out_samples) const {
  out_samples.assign(output_count_, 0.0);
  if (output_count_ == 0 || record.empty()) {
    return;
  }
  resample_into(record, out_samples.data());
}

void VBIRecordResampler::resample_into(const std::vector<double>& record,
                                       double* out) const {
  const double* samples = record.data();
  const auto last = static_cast<int32_t>(record.size()) - 1;
  const auto taps = static_cast<int32_t>(taps_);

  for (uint32_t sample = 0; sample < output_count_; ++sample) {
    const double* weights =
        bank_.data() + static_cast<size_t>(row_[sample]) * taps_;
    const int32_t first = first_tap_[sample];

    double accumulator = 0.0;
    if (first >= 0 && first + taps - 1 <= last) {
      // The whole kernel falls inside the record, which is every sample of the
      // data region on a real capture.
      const double* window = samples + first;
      for (int32_t tap = 0; tap < taps; ++tap) {
        accumulator += weights[tap] * window[tap];
      }
    } else {
      // Constant extension at whichever end the kernel ran off.
      for (int32_t tap = 0; tap < taps; ++tap) {
        const int32_t index = std::clamp(first + tap, 0, last);
        accumulator += weights[tap] * samples[index];
      }
    }
    out[sample] = accumulator;
  }
}

void VBIRecordResampler::resample_records(
    const std::vector<const std::vector<double>*>& records,
    std::vector<double>& out_samples) const {
  out_samples.resize(records.size() * output_count_);
  if (output_count_ == 0) {
    return;
  }

  // The records that have samples to resample; an empty one is all zeros, as
  // it is alone.
  std::vector<size_t> present;
  present.reserve(records.size());
  for (size_t index = 0; index < records.size(); ++index) {
    double* out = out_samples.data() + index * output_count_;
    if (records[index]->empty()) {
      std::fill(out, out + output_count_, 0.0);
    } else {
      present.push_back(index);
    }
  }

#if defined(ORC_VBI_RESAMPLER_LANES)
  // A copied record has one tap: there are no sums to overlap.
  if (taps_ > 1) {
    // Every record index a kernel reads, its first tap to its last.
    const auto [lowest, highest] =
        std::minmax_element(first_tap_.begin(), first_tap_.end());
    const int32_t first_index = *lowest;
    const auto span =
        static_cast<size_t>(*highest - *lowest) + static_cast<size_t>(taps_);

    // The interleaving, kept from frame to frame by the thread that places
    // them, so a frame neither allocates nor clears it.
    thread_local std::vector<double> interleaved;

    for (size_t start = 0; start < present.size();
         start += kMaxRecordFours * kRecordLanes) {
      const size_t count =
          std::min(present.size() - start, kMaxRecordFours * kRecordLanes);
      const size_t fours = (count + kRecordLanes - 1) / kRecordLanes;
      const size_t stride = fours * kRecordLanes;
      interleaved.resize(span * stride);

      RecordPass pass;
      pass.bank = bank_.data();
      pass.first_tap = first_tap_.data();
      pass.row = row_.data();
      pass.output_count = output_count_;
      pass.taps = taps_;
      pass.first_index = first_index;
      pass.interleaved = interleaved.data();

      // Each lane's record, and the last sample it has.
      const double* lane_samples[kMaxRecordFours * kRecordLanes] = {};
      int32_t lane_last[kMaxRecordFours * kRecordLanes] = {};
      int32_t shortest_last = std::numeric_limits<int32_t>::max();
      for (size_t lane = 0; lane < count; ++lane) {
        const size_t record = present[start + lane];
        lane_samples[lane] = records[record]->data();
        lane_last[lane] = static_cast<int32_t>(records[record]->size()) - 1;
        shortest_last = std::min(shortest_last, lane_last[lane]);
        pass.outputs[lane] = out_samples.data() + record * output_count_;
      }

      // Index by index, every lane's sample there: its nearest stored one off
      // either end of its record, and zero in a lane with no record, which is
      // summed with the rest and thrown away.
      double* to = interleaved.data();
      for (size_t index = 0; index < span; ++index, to += stride) {
        const int32_t at = first_index + static_cast<int32_t>(index);
        if (at >= 0 && at <= shortest_last) {
          for (size_t lane = 0; lane < count; ++lane) {
            to[lane] = lane_samples[lane][at];
          }
        } else {
          for (size_t lane = 0; lane < count; ++lane) {
            to[lane] = lane_samples[lane][std::clamp(at, 0, lane_last[lane])];
          }
        }
        std::fill(to + count, to + stride, 0.0);
      }

      run_record_pass(fours, pass, config_.use_avx2);
    }
    return;
  }
#endif

  for (const size_t index : present) {
    resample_into(*records[index], out_samples.data() + index * output_count_);
  }
}

}  // namespace orc
