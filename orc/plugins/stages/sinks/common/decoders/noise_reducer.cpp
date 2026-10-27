/*
 * File:        noise_reducer.cpp
 * Module:      orc-core
 * Purpose:     Noise reduction for decoded component frames: a
 *              motion-adaptive recursive temporal filter, with a spatial
 *              filter where the picture does not continue
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "noise_reducer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

// Motion thresholds in units of each plane's estimated noise scale. Below the
// still threshold the full blend applies; above the moving threshold the
// sample is filtered spatially; between them the motion measure rises along
// a smoothstep.
constexpr double kStillThresholdScale = 2.5;
constexpr double kMovingThresholdScale = 5.0;

// The noise scale is read off the box-mean difference's distribution at this
// percentile. On still noisy picture the box-mean difference is close to a
// zero-mean normal, so its magnitude is half-normal and the percentile
// relates to the normal's scale by a constant: for the 30th percentile,
// q = sigma * Phi^-1(0.65).
constexpr double kNoisePercentile = 0.30;
constexpr double kHalfNormalPercentileToScale = 0.38532;

// The spatial filter's range follows the scale of the differences between a
// sample and its window neighbours, measured as pair differences at the
// window's own lags (kSpatialSampleReach samples along the line, one
// same-field line down) so that whatever correlation the decoder has left
// in the noise is accounted for. They are read at a lower percentile,
// because picture detail inflates them wherever the picture is not flat: a
// tenth of the measured samples in flat areas is enough. For the 10th
// percentile, q = sigma * Phi^-1(0.55).
constexpr double kPairNoisePercentile = 0.10;
constexpr double kPairHalfNormalPercentileToScale = 0.12566;

// The within-frame anchor for the temporal percentile is the same box
// operator applied between the frame and itself shifted by kAnchorLag
// samples: two subcarrier cycles, beyond the horizontal correlation of the
// decoded luma noise, so on noise the anchor and the frame difference have
// the same distribution whatever the decoder's filtering, while a change
// of picture inflates only the frame difference.
constexpr int32_t kAnchorLag = 8;

// Histograms used for the percentiles: 256 bins of a quarter of a sample
// code, covering 0 to 64 codes. Larger values land in the top bin.
constexpr int32_t kHistogramBins = 256;
constexpr double kHistogramBinWidth = 0.25;
constexpr float kHistogramBinsPerCode = 1.0F / kHistogramBinWidth;

// The pre-pass measures every kMeasureSampleStep-th sample of every
// kMeasureRowStep-th active row: tens of thousands of samples on a
// standard-definition frame, reading three eighths of its rows.
constexpr int32_t kMeasureRowStep = 8;
constexpr int32_t kMeasureSampleStep = 2;

// Clamps on the estimated noise scales, in sample codes. A temporal scale
// at kSigmaMax is beyond any noise the decoders produce and marks a frame
// that does not continue the previous one.
constexpr double kSigmaMin = 0.25;
constexpr double kSigmaMax = 8.0;
constexpr double kDifferenceSigmaMin = 0.35;
constexpr double kDifferenceSigmaMax = 45.0;

// Discontinuity decision. The frame does not continue the previous one when
// its luma temporal percentile exceeds the within-frame anchor by more than
// kDiscontinuityScaleRatio (the picture changed by more than noise over most
// of its area), or when more than kDiscontinuityMovingFraction of the
// measured samples are moving. The anchor has a floor so that a frame-wide
// change of under two box-mean codes on noise-free picture is never a
// discontinuity by itself.
constexpr double kDiscontinuityScaleRatio = 2.0;
constexpr double kDiscontinuityMovingFraction = 0.5;
constexpr double kDiscontinuityAnchorFloor = 1.0;

// Spatial window: a neighbour's weight falls quadratically to zero at
// kSpatialRangeScale pair-difference noise scales from the centre sample, so
// edges and detail above the noise are left alone. Because the weights
// depend on the centre sample's own noise, the window mean is pulled
// towards it: on flat white noise its power is kSpatialMeanNoisePower of the
// input's and its covariance with the centre sample is
// kSpatialMeanCentreCovariance of the input power (both measured on this
// window, 15 taps over 768 x 483 samples of Gaussian noise). Mixing a share
// g of the mean into the centre sample then leaves noise power
//   (1 - g)^2 + v g^2 + 2 k g (1 - g)
// of the input, which is what spatial_gain_for_strength_db() inverts; the
// window saturates at 1 / v, just under 6 dB.
constexpr double kSpatialRangeScale = 1.8;
constexpr double kSpatialMeanNoisePower = 0.254;
constexpr double kSpatialMeanCentreCovariance = 0.458;

double smoothstep(double edge0, double edge1, double x) {
  if (edge1 <= edge0) return x < edge0 ? 0.0 : 1.0;
  const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

int32_t histogram_bin(float value) {
  return std::min(kHistogramBins - 1,
                  static_cast<int32_t>(value * kHistogramBinsPerCode));
}

// Value at fraction p of a histogram's mass, interpolated within its bin.
// The top of the range when the histogram is empty.
double histogram_percentile(const std::vector<uint32_t>& histogram, double p) {
  uint64_t count = 0;
  for (const uint32_t in_bin : histogram) count += in_bin;
  const double target = p * static_cast<double>(count);
  double cumulative = 0.0;
  for (int32_t bin = 0; bin < kHistogramBins; ++bin) {
    const double in_bin = histogram[static_cast<size_t>(bin)];
    if (cumulative + in_bin >= target) {
      const double fraction =
          in_bin > 0.0 ? (target - cumulative) / in_bin : 0.0;
      return (bin + fraction) * kHistogramBinWidth;
    }
    cumulative += in_bin;
  }
  return kHistogramBins * kHistogramBinWidth;
}

// Fraction of a histogram's mass in bins starting at or above threshold.
double histogram_fraction_above(const std::vector<uint32_t>& histogram,
                                double threshold) {
  uint64_t count = 0;
  uint64_t above = 0;
  const int32_t first_bin = std::max(
      0, static_cast<int32_t>(std::ceil(threshold / kHistogramBinWidth)));
  for (int32_t bin = 0; bin < kHistogramBins; ++bin) {
    const uint32_t in_bin = histogram[static_cast<size_t>(bin)];
    count += in_bin;
    if (bin >= first_bin) above += in_bin;
  }
  return count > 0 ? static_cast<double>(above) / static_cast<double>(count)
                   : 0.0;
}

// Running sum of four consecutive values centred on x - 1 .. x + 2. The
// window slides inward at the edges rather than shrinking, so every sample
// is measured over one full subcarrier cycle and a 4fsc residual cancels
// everywhere; a row narrower than the window sums as a whole.
// Adds one tap offset of the spatial window to a row's accumulators: for
// each sample, the neighbour's weight (falling quadratically to zero at the
// range from the centre sample) and its weighted value. The pointers do not
// overlap, which lets the loop vectorise.
void accumulate_taps(const float* __restrict neighbours,
                     const float* __restrict centre,
                     float* __restrict sum_weights,
                     float* __restrict sum_values, int32_t count,
                     float inverse_range2) {
  for (int32_t x = 0; x < count; ++x) {
    const float v = neighbours[x];
    const float d = v - centre[x];
    const float w = std::max(0.0F, 1.0F - d * d * inverse_range2);
    sum_weights[x] += w;
    sum_values[x] += w * v;
  }
}

void running_sum_of_four(const float* in, float* out, int32_t width) {
  if (width < 4) {
    float sum = 0.0F;
    for (int32_t x = 0; x < width; ++x) sum += in[x];
    for (int32_t x = 0; x < width; ++x) out[x] = sum;
    return;
  }
  for (int32_t x = 1; x + 2 < width; ++x) {
    out[x] = in[x - 1] + in[x] + in[x + 1] + in[x + 2];
  }
  out[0] = out[1];
  out[width - 2] = out[width - 3];
  out[width - 1] = out[width - 3];
}

}  // namespace

// Persistent threads for the line bands. run() hands bands 1.. to the
// workers, runs band 0 on the caller, and returns once every band is done,
// so the caller's scratch state is never touched concurrently between
// calls.
class NoiseReducer::BandPool {
 public:
  explicit BandPool(int32_t worker_count) {
    workers_.reserve(static_cast<size_t>(worker_count));
    for (int32_t i = 0; i < worker_count; ++i) {
      workers_.emplace_back([this, i] { worker_loop(i); });
    }
  }

  ~BandPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    start_.notify_all();
    for (auto& worker : workers_) worker.join();
  }

  BandPool(const BandPool&) = delete;
  BandPool& operator=(const BandPool&) = delete;

  int32_t worker_count() const { return static_cast<int32_t>(workers_.size()); }

  // bands must not exceed worker_count() + 1.
  void run(int32_t bands, const std::function<void(int32_t)>& task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_ = &task;
      bands_ = bands;
      pending_ = bands - 1;
      ++generation_;
    }
    start_.notify_all();
    task(0);
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this] { return pending_ <= 0; });
  }

 private:
  void worker_loop(int32_t index) {
    const int32_t band = index + 1;
    uint64_t seen = 0;
    for (;;) {
      const std::function<void(int32_t)>* task = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        start_.wait(lock,
                    [this, seen] { return stop_ || generation_ != seen; });
        if (stop_) return;
        seen = generation_;
        if (band >= bands_) continue;
        task = task_;
      }
      (*task)(band);
      std::lock_guard<std::mutex> lock(mutex_);
      if (--pending_ <= 0) done_.notify_one();
    }
  }

  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable start_;
  std::condition_variable done_;
  const std::function<void(int32_t)>* task_ = nullptr;
  int32_t bands_ = 0;
  int32_t pending_ = 0;
  uint64_t generation_ = 0;
  bool stop_ = false;
};

NoiseReducer::NoiseReducer(const Settings& settings) : settings_(settings) {
  if (settings_.strength_db > 0.0 && settings_.threads > 1) {
    pool_ = std::make_unique<BandPool>(settings_.threads - 1);
  }
}

NoiseReducer::~NoiseReducer() = default;

double NoiseReducer::blend_weight_for_strength_db(double strength_db) {
  if (strength_db <= 0.0) return 0.0;
  const double r = std::pow(10.0, -strength_db / 10.0);
  return (1.0 - r) / (1.0 + r);
}

double NoiseReducer::spatial_gain_for_strength_db(double strength_db) {
  if (strength_db <= 0.0) return 0.0;
  // Solve (1 - g)^2 + v g^2 + 2 k g (1 - g) = r for the share g in [0, 1]:
  // g^2 (1 + v - 2k) - 2 g (1 - k) + (1 - r) = 0, smaller root. Beyond the
  // window's reach the share saturates at 1.
  const double r = std::pow(10.0, -strength_db / 10.0);
  const double a =
      1.0 + kSpatialMeanNoisePower - 2.0 * kSpatialMeanCentreCovariance;
  const double b = 1.0 - kSpatialMeanCentreCovariance;
  const double discriminant = b * b - a * (1.0 - r);
  if (discriminant <= 0.0) return 1.0;
  return std::clamp((b - std::sqrt(discriminant)) / a, 0.0, 1.0);
}

void NoiseReducer::reset() { has_previous_ = false; }

NoiseReducer::ActiveRect NoiseReducer::clamped_active_rect(
    const ComponentFrame& frame) const {
  ActiveRect rect;
  rect.x_start = std::clamp(settings_.active_x_start, 0, frame.getWidth());
  rect.x_end = std::clamp(settings_.active_x_end, 0, frame.getWidth());
  rect.line_start =
      std::clamp(settings_.active_line_start, 0, frame.getHeight());
  rect.line_end = std::clamp(settings_.active_line_end, 0, frame.getHeight());
  return rect;
}

bool NoiseReducer::geometry_matches(const ComponentFrame& frame) const {
  if (frame.getWidth() != previous_.width) return false;
  if (frame.getHeight() != previous_.height) return false;
  if (frame.getY()->size() != previous_.planes[kPlaneY].size()) return false;
  const bool frame_has_chroma = !frame.getU()->empty();
  return frame_has_chroma == previous_.has_chroma;
}

void NoiseReducer::prepare_previous(const ComponentFrame& frame) {
  previous_.width = frame.getWidth();
  previous_.height = frame.getHeight();
  previous_.has_chroma = !frame.getU()->empty();
  previous_.planes[kPlaneY].resize(frame.getY()->size());
  if (previous_.has_chroma) {
    previous_.planes[kPlaneU].resize(frame.getU()->size());
    previous_.planes[kPlaneV].resize(frame.getV()->size());
  } else {
    previous_.planes[kPlaneU].clear();
    previous_.planes[kPlaneV].clear();
  }
}

int32_t NoiseReducer::band_count(int32_t lines) const {
  const int32_t available = pool_ ? pool_->worker_count() + 1 : 1;
  return std::clamp(lines / kMinBandRows, 1, available);
}

void NoiseReducer::run_bands(int32_t lines, int32_t bands,
                             const BandTask& task) {
  const auto band_rows = [lines, bands](int32_t band) {
    return static_cast<int32_t>(static_cast<int64_t>(lines) * band / bands);
  };
  if (bands <= 1 || !pool_) {
    task(0, 0, lines);
    return;
  }
  pool_->run(bands, [&](int32_t band) {
    task(band, band_rows(band), band_rows(band + 1));
  });
}

void NoiseReducer::measure(const ComponentFrame& frame, const ActiveRect& rect,
                           bool has_chroma, bool continuing) {
  const int32_t frame_width = frame.getWidth();
  const int32_t width = rect.width();
  const int32_t lines = rect.lines();
  const size_t plane_count = has_chroma ? kPlaneCount : 1;

  const std::array<const double*, kPlaneCount> current = {
      frame.getY()->data(), has_chroma ? frame.getU()->data() : nullptr,
      has_chroma ? frame.getV()->data() : nullptr};
  std::array<const float*, kPlaneCount> previous = {nullptr, nullptr, nullptr};
  if (continuing) {
    for (size_t p = 0; p < plane_count; ++p) {
      previous[p] = previous_.planes[p].data();
    }
  }

  const int32_t box_count = std::min(kBoxWidth, width) * kBoxLines;
  const float inverse_area = 1.0F / static_cast<float>(box_count);
  const int32_t reach = kSameFieldRowStep * (kBoxLines / 2);
  const bool can_slide = lines > 2 * reach;
  const int32_t anchor_lag = std::min(kAnchorLag, width - 1);
  const int32_t pair_lag = std::min(kSpatialSampleReach, width - 1);

  // Scratch rows: the three difference rows of the box and their running
  // sums.
  const size_t row_floats = static_cast<size_t>(width);
  measure_scratch_.resize(row_floats * 6);
  float* diff = measure_scratch_.data();
  float* sums = diff + 3 * row_floats;

  for (size_t p = 0; p < plane_count; ++p) {
    temporal_histogram_[p].assign(kHistogramBins, 0);
    horizontal_histogram_[p].assign(kHistogramBins, 0);
    vertical_histogram_[p].assign(kHistogramBins, 0);
  }
  anchor_histogram_.assign(kHistogramBins, 0);

  const auto row_offset = [&](int32_t r) {
    return static_cast<size_t>(rect.line_start + r) * frame_width +
           rect.x_start;
  };

  // Box-mean magnitude of the three difference rows in diff, binned into
  // histogram at the measured samples.
  const auto bin_box = [&](uint32_t* histogram) {
    for (size_t k = 0; k < kBoxLines; ++k) {
      running_sum_of_four(diff + k * row_floats, sums + k * row_floats, width);
    }
    for (int32_t x = 0; x < width; x += kMeasureSampleStep) {
      const float t = inverse_area * std::fabs(sums[x] + sums[row_floats + x] +
                                               sums[2 * row_floats + x]);
      ++histogram[histogram_bin(t)];
    }
  };

  for (int32_t r = 0; r < lines; r += kMeasureRowStep) {
    int32_t centre = r;
    if (can_slide) centre = std::clamp(r, reach, lines - 1 - reach);
    const int32_t above = std::max(0, centre - kSameFieldRowStep);
    const int32_t below = std::min(lines - 1, centre + kSameFieldRowStep);
    const std::array<int32_t, kBoxLines> rows = {above, centre, below};
    // The vertical pair partner of the centre row: the same-field line
    // below, or above at the bottom edge.
    const int32_t partner = below != centre ? below : above;

    for (size_t p = 0; p < plane_count; ++p) {
      const double* cur_centre = current[p] + row_offset(centre);
      const double* cur_partner = current[p] + row_offset(partner);
      uint32_t* horizontal = horizontal_histogram_[p].data();
      uint32_t* vertical = vertical_histogram_[p].data();
      for (int32_t x = 0; x < width; x += kMeasureSampleStep) {
        const int32_t xx = x + pair_lag < width ? x + pair_lag : x - pair_lag;
        ++horizontal[histogram_bin(
            static_cast<float>(std::fabs(cur_centre[x] - cur_centre[xx])))];
        ++vertical[histogram_bin(
            static_cast<float>(std::fabs(cur_centre[x] - cur_partner[x])))];
      }

      if (p == kPlaneY && anchor_lag > 0) {
        for (size_t k = 0; k < kBoxLines; ++k) {
          const double* cur = current[p] + row_offset(rows[k]);
          float* d = diff + k * row_floats;
          for (int32_t x = 0; x < width; ++x) {
            const int32_t xx =
                x + anchor_lag < width ? x + anchor_lag : x - anchor_lag;
            d[x] = static_cast<float>(cur[x] - cur[xx]);
          }
        }
        bin_box(anchor_histogram_.data());
      }

      if (continuing) {
        for (size_t k = 0; k < kBoxLines; ++k) {
          const double* cur = current[p] + row_offset(rows[k]);
          const float* prev = previous[p] + row_offset(rows[k]);
          float* d = diff + k * row_floats;
          for (int32_t x = 0; x < width; ++x) {
            d[x] = static_cast<float>(cur[x] - static_cast<double>(prev[x]));
          }
        }
        bin_box(temporal_histogram_[p].data());
      }
    }
  }

  measurement_ = Measurement{};
  for (size_t p = 0; p < plane_count; ++p) {
    const double pair_percentile = std::max(
        histogram_percentile(horizontal_histogram_[p], kPairNoisePercentile),
        histogram_percentile(vertical_histogram_[p], kPairNoisePercentile));
    measurement_.difference_sigma[p] =
        std::clamp(pair_percentile / kPairHalfNormalPercentileToScale,
                   kDifferenceSigmaMin, kDifferenceSigmaMax);
    if (continuing) {
      const double temporal_percentile =
          histogram_percentile(temporal_histogram_[p], kNoisePercentile);
      measurement_.sigma[p] =
          std::clamp(temporal_percentile / kHalfNormalPercentileToScale,
                     kSigmaMin, kSigmaMax);
      if (p == kPlaneY) {
        measurement_.temporal_percentile_y = temporal_percentile;
      }
    }
  }
  measurement_.anchor_percentile_y =
      histogram_percentile(anchor_histogram_, kNoisePercentile);

  if (continuing) {
    // Samples moving in any plane, approximated from the per-plane
    // histograms by the plane with the most of them.
    double moving = 0.0;
    for (size_t p = 0; p < plane_count; ++p) {
      const double threshold = kMovingThresholdScale * settings_.motion_scale *
                               measurement_.sigma[p];
      moving = std::max(
          moving, histogram_fraction_above(temporal_histogram_[p], threshold));
    }
    measurement_.moving_fraction = moving;
  }
}
void NoiseReducer::frame_pass(ComponentFrame& frame, const ActiveRect& rect,
                              int32_t bands, bool has_chroma, bool temporal) {
  const int32_t frame_width = frame.getWidth();
  const int32_t width = rect.width();
  const int32_t lines = rect.lines();
  const size_t plane_count = has_chroma ? kPlaneCount : 1;

  std::array<double*, kPlaneCount> current = {frame.getY()->data(), nullptr,
                                              nullptr};
  std::array<float*, kPlaneCount> previous = {previous_.planes[kPlaneY].data(),
                                              nullptr, nullptr};
  if (has_chroma) {
    current[kPlaneU] = frame.getU()->data();
    current[kPlaneV] = frame.getV()->data();
    previous[kPlaneU] = previous_.planes[kPlaneU].data();
    previous[kPlaneV] = previous_.planes[kPlaneV].data();
  }

  // Vertical window: kBoxLines same-field lines (kSameFieldRowStep rows
  // apart), sliding inward at the top and bottom of the active area.
  const int32_t box_count = std::min(kBoxWidth, width) * kBoxLines;
  const float inverse_area = 1.0F / static_cast<float>(box_count);
  const int32_t reach = kSameFieldRowStep * (kBoxLines / 2);
  const bool can_slide = lines > 2 * reach;

  const double alpha = blend_weight_for_strength_db(settings_.strength_db);
  const double still_threshold = kStillThresholdScale * settings_.motion_scale;
  const double moving_threshold =
      kMovingThresholdScale * settings_.motion_scale;
  std::array<float, kPlaneCount> inverse_sigma = {};
  std::array<double, kPlaneCount> inverse_range_squared = {};
  for (size_t p = 0; p < plane_count; ++p) {
    if (temporal) {
      inverse_sigma[p] = static_cast<float>(1.0 / measurement_.sigma[p]);
    }
    const double range = kSpatialRangeScale * measurement_.difference_sigma[p];
    inverse_range_squared[p] = 1.0 / (range * range);
  }
  const double spatial_gain =
      spatial_gain_for_strength_db(settings_.strength_db);
  const bool spatial_enabled = settings_.spatial;

  const auto row_offset = [&](int32_t r) {
    return static_cast<size_t>(rect.line_start + r) * frame_width +
           rect.x_start;
  };

  // Signed difference of row r (within the active rectangle) of plane p and
  // its running sum over kBoxWidth consecutive samples.
  const auto sum_row = [&](size_t p, int32_t r, float* diff, float* sums) {
    const double* cur = current[p] + row_offset(r);
    const float* prev = previous[p] + row_offset(r);
    for (int32_t x = 0; x < width; ++x) {
      diff[x] = static_cast<float>(cur[x] - static_cast<double>(prev[x]));
    }
    running_sum_of_four(diff, sums, width);
  };

  // A band keeps row sums for its own rows and kChunkHalo rows on each side
  // of them, clamped to the active area.
  const auto sums_begin_of = [&](int32_t row_begin) {
    return std::max(0, row_begin - kChunkHalo);
  };
  const auto sums_end_of = [&](int32_t row_end) {
    return std::min(lines, row_end + kChunkHalo);
  };
  const size_t raw_rows_size =
      static_cast<size_t>(kSpatialRowReach) * static_cast<size_t>(width);
  const auto prepare_band = [&](BandScratch& scratch, int32_t row_begin,
                                int32_t row_end) {
    const size_t sum_rows =
        static_cast<size_t>(sums_end_of(row_end) - sums_begin_of(row_begin));
    const size_t chunk_size = static_cast<size_t>(kChunkRows) * width;
    for (size_t p = 0; p < plane_count; ++p) {
      PlaneScratch& plane = scratch.planes[p];
      plane.diff_row.resize(width);
      plane.row_sums.resize(temporal ? sum_rows * width : 0);
      plane.motion.resize(temporal ? chunk_size : 0);
      plane.spatial.resize(chunk_size);
      plane.raw_above.resize(raw_rows_size);
      plane.raw_below.resize(raw_rows_size);
      plane.raw_swap.resize(raw_rows_size);
    }
    scratch.mix.resize(chunk_size);
    scratch.sum_weights.resize(width);
    scratch.sum_values.resize(width);
    scratch.window_rows.resize(static_cast<size_t>(3) * width);
  };
  const auto sums_row = [&](PlaneScratch& plane, int32_t sums_begin,
                            int32_t r) {
    return plane.row_sums.data() + static_cast<size_t>(r - sums_begin) * width;
  };

  // Unfiltered row r of plane p as seen from a chunk starting at chunk_begin
  // in a band ending at row_end: rows above the chunk and below the band
  // come from the copies taken before they were blended.
  const auto raw_row = [&](PlaneScratch& plane, size_t p, int32_t r,
                           int32_t chunk_begin,
                           int32_t row_end) -> const double* {
    if (r < chunk_begin) {
      return plane.raw_above.data() +
             static_cast<size_t>(r - (chunk_begin - kSpatialRowReach)) * width;
    }
    if (r >= row_end) {
      return plane.raw_below.data() + static_cast<size_t>(r - row_end) * width;
    }
    return current[p] + row_offset(r);
  };
  const auto copy_raw_rows =
      [&](std::vector<double>& into, int32_t first_row,
          const std::function<const double*(int32_t)>& source) {
        for (int32_t k = 0; k < kSpatialRowReach; ++k) {
          const int32_t r = first_row + k;
          double* out = into.data() + static_cast<size_t>(k) * width;
          if (r < 0 || r >= lines) continue;
          const double* in = source(r);
          std::copy(in, in + width, out);
        }
      };

  // Spatially filtered value of sample x of a row, from the row's same-field
  // neighbours above and below (mirrored at the active area's edges) and
  // kSpatialSampleReach samples either side: a range-weighted mean whose
  // weights fall to zero at the plane's range from the centre sample, mixed
  // in by the gain.
  // Spatially filtered value of sample x of a row, from the row's same-field
  // neighbours above and below (mirrored at the active area's edges) and
  // kSpatialSampleReach samples either side: a range-weighted mean whose
  // weights fall to zero at the plane's range from the centre sample, mixed
  // in by the gain. Taps beyond the row's ends are left out.
  const auto spatial_sample = [&](const float* above, const float* centre,
                                  const float* below, int32_t x,
                                  float inverse_range2) {
    const float c = centre[x];
    float sum_weights = 1.0F;
    float sum_values = c;
    const int32_t x0 = std::max(0, x - kSpatialSampleReach);
    const int32_t x1 = std::min(width - 1, x + kSpatialSampleReach);
    const auto tap = [&](float v) {
      const float d = v - c;
      const float w = std::max(0.0F, 1.0F - d * d * inverse_range2);
      sum_weights += w;
      sum_values += w * v;
    };
    for (int32_t xx = x0; xx <= x1; ++xx) tap(above[xx]);
    for (int32_t xx = x0; xx < x; ++xx) tap(centre[xx]);
    for (int32_t xx = x + 1; xx <= x1; ++xx) tap(centre[xx]);
    for (int32_t xx = x0; xx <= x1; ++xx) tap(below[xx]);
    return c +
           static_cast<float>(spatial_gain) * (sum_values / sum_weights - c);
  };
  // The same for a whole row, one pass per tap offset so the inner loop is
  // branch-free and vectorises: used when the row has more than a few
  // moving samples.
  const auto spatial_row = [&](BandScratch& scratch, const float* above,
                               const float* centre, const float* below,
                               float* out, float inverse_range2) {
    float* sum_weights = scratch.sum_weights.data();
    float* sum_values = scratch.sum_values.data();
    for (int32_t x = 0; x < width; ++x) {
      sum_weights[x] = 1.0F;
      sum_values[x] = centre[x];
    }
    const std::array<const float*, 3> rows = {above, centre, below};
    for (size_t k = 0; k < rows.size(); ++k) {
      for (int32_t dx = -kSpatialSampleReach; dx <= kSpatialSampleReach; ++dx) {
        if (k == 1 && dx == 0) continue;
        const int32_t x_begin = std::max(0, -dx);
        const int32_t x_end = std::min(width, width - dx);
        accumulate_taps(rows[k] + dx + x_begin, centre + x_begin,
                        sum_weights + x_begin, sum_values + x_begin,
                        x_end - x_begin, inverse_range2);
      }
    }
    const float gain = static_cast<float>(spatial_gain);
    for (int32_t x = 0; x < width; ++x) {
      const float c = centre[x];
      out[x] = c + gain * (sum_values[x] / sum_weights[x] - c);
    }
  };
  // The three unfiltered rows of a spatial window in single precision.
  const auto load_rows = [&](BandScratch& scratch, const double* above,
                             const double* centre, const double* below) {
    float* rows = scratch.window_rows.data();
    for (int32_t x = 0; x < width; ++x) {
      rows[x] = static_cast<float>(above[x]);
      rows[width + x] = static_cast<float>(centre[x]);
      rows[2 * width + x] = static_cast<float>(below[x]);
    }
  };
  const auto mirrored_above = [&](int32_t r) {
    return r - kSpatialRowReach >= 0
               ? r - kSpatialRowReach
               : std::min(lines - 1, r + kSpatialRowReach);
  };
  const auto mirrored_below = [&](int32_t r) {
    return r + kSpatialRowReach < lines ? r + kSpatialRowReach
                                        : std::max(0, r - kSpatialRowReach);
  };

  // Before any band blends: copy the unfiltered rows each band's spatial
  // windows reach outside it (they belong to the neighbouring bands, which
  // blend them concurrently) and, with the temporal blend on, sum the halo
  // rows its boxes reach for the same reason.
  if (bands > 1) {
    run_bands(
        lines, bands, [&](int32_t band, int32_t row_begin, int32_t row_end) {
          BandScratch& scratch = bands_[band];
          prepare_band(scratch, row_begin, row_end);
          const int32_t sums_begin = sums_begin_of(row_begin);
          const int32_t sums_end = sums_end_of(row_end);
          for (size_t p = 0; p < plane_count; ++p) {
            PlaneScratch& plane = scratch.planes[p];
            const auto frame_row = [&](int32_t r) {
              return static_cast<const double*>(current[p] + row_offset(r));
            };
            copy_raw_rows(plane.raw_above, row_begin - kSpatialRowReach,
                          frame_row);
            copy_raw_rows(plane.raw_below, row_end, frame_row);
            if (!temporal) continue;
            float* diff = plane.diff_row.data();
            for (int32_t r = sums_begin; r < row_begin; ++r) {
              sum_row(p, r, diff, sums_row(plane, sums_begin, r));
            }
            for (int32_t r = row_end; r < sums_end; ++r) {
              sum_row(p, r, diff, sums_row(plane, sums_begin, r));
            }
          }
        });
  }

  run_bands(
      lines, bands, [&](int32_t band, int32_t row_begin, int32_t row_end) {
        BandScratch& scratch = bands_[band];
        prepare_band(scratch, row_begin, row_end);
        const int32_t sums_begin = sums_begin_of(row_begin);
        std::array<int32_t, kPlaneCount> summed = {};
        for (size_t p = 0; p < plane_count; ++p) summed[p] = row_begin;

        for (int32_t chunk_begin = row_begin; chunk_begin < row_end;
             chunk_begin += kChunkRows) {
          const int32_t chunk_end = std::min(row_end, chunk_begin + kChunkRows);
          const int32_t chunk_rows = chunk_end - chunk_begin;

          if (temporal) {
            // Rows this chunk's vertical boxes can reach, summed before the
            // blend of this chunk (and so before the blend of any earlier chunk
            // could have changed them).
            const int32_t need_end = std::min(row_end, chunk_end + kChunkHalo);
            for (size_t p = 0; p < plane_count; ++p) {
              PlaneScratch& plane = scratch.planes[p];
              float* diff = plane.diff_row.data();
              for (int32_t r = summed[p]; r < need_end; ++r) {
                sum_row(p, r, diff, sums_row(plane, sums_begin, r));
              }
              summed[p] = need_end;

              // Magnitude of the box mean for the chunk's own rows.
              for (int32_t r = chunk_begin; r < chunk_end; ++r) {
                int32_t centre = r;
                if (can_slide) centre = std::clamp(r, reach, lines - 1 - reach);
                const int32_t above = std::max(0, centre - kSameFieldRowStep);
                const int32_t below =
                    std::min(lines - 1, centre + kSameFieldRowStep);
                const float* s_above = sums_row(plane, sums_begin, above);
                const float* s_centre = sums_row(plane, sums_begin, centre);
                const float* s_below = sums_row(plane, sums_begin, below);
                float* out = plane.motion.data() +
                             static_cast<size_t>(r - chunk_begin) * width;
                for (int32_t x = 0; x < width; ++x) {
                  out[x] = inverse_area *
                           std::fabs(s_above[x] + s_centre[x] + s_below[x]);
                }
              }
            }
          }

          // Motion measure of every sample in the chunk: in units of each
          // plane's own noise scale, the largest plane winning, so a change in
          // any plane counts and a noisier plane does not dominate; shared by
          // all three planes. Without the temporal blend everything is moving.
          float* mix = scratch.mix.data();
          const float* motion_y = scratch.planes[kPlaneY].motion.data();
          const float* motion_u = scratch.planes[kPlaneU].motion.data();
          const float* motion_v = scratch.planes[kPlaneV].motion.data();
          const size_t chunk_samples = static_cast<size_t>(chunk_rows) * width;
          if (temporal) {
            for (size_t i = 0; i < chunk_samples; ++i) {
              float motion = motion_y[i] * inverse_sigma[kPlaneY];
              if (has_chroma) {
                motion = std::max(motion, motion_u[i] * inverse_sigma[kPlaneU]);
                motion = std::max(motion, motion_v[i] * inverse_sigma[kPlaneV]);
              }
              mix[i] = static_cast<float>(
                  smoothstep(still_threshold, moving_threshold, motion));
            }
          } else {
            std::fill(mix, mix + chunk_samples, 1.0F);
          }

          // Spatial filter of every row with a moving sample, when the filter
          // is on, computed for the whole chunk from unfiltered rows before
          // any row of the chunk is blended.
          for (size_t p = 0; p < plane_count; ++p) {
            PlaneScratch& plane = scratch.planes[p];
            for (int32_t r = chunk_begin; r < chunk_end; ++r) {
              const size_t chunk_offset =
                  static_cast<size_t>(r - chunk_begin) * width;
              const float* m = mix + chunk_offset;
              float* out = plane.spatial.data() + chunk_offset;
              const double* above =
                  raw_row(plane, p, mirrored_above(r), chunk_begin, row_end);
              const double* centre = current[p] + row_offset(r);
              const double* below =
                  raw_row(plane, p, mirrored_below(r), chunk_begin, row_end);
              const int32_t moving = static_cast<int32_t>(std::count_if(
                  m, m + width, [](float motion) { return motion > 0.0F; }));
              if (!spatial_enabled || moving == 0) continue;
              load_rows(scratch, above, centre, below);
              const float* row_above = scratch.window_rows.data();
              const float* row_centre = row_above + static_cast<size_t>(width);
              const float* row_below = row_centre + static_cast<size_t>(width);
              const float inverse_range2 =
                  static_cast<float>(inverse_range_squared[p]);
              if (moving * kSpatialRowShare < width) {
                for (int32_t x = 0; x < width; ++x) {
                  if (m[x] <= 0.0F) continue;
                  out[x] = spatial_sample(row_above, row_centre, row_below, x,
                                          inverse_range2);
                }
              } else {
                spatial_row(scratch, row_above, row_centre, row_below, out,
                            inverse_range2);
              }
            }
            // The next chunk's windows reach back into this chunk's last rows,
            // which are about to be blended: keep their unfiltered values.
            const auto chunk_raw = [&](int32_t r) {
              return raw_row(plane, p, r, chunk_begin, row_end);
            };
            copy_raw_rows(plane.raw_swap, chunk_end - kSpatialRowReach,
                          chunk_raw);
            std::swap(plane.raw_above, plane.raw_swap);
          }

          // Blend the chunk's rows, and keep the result as the next previous
          // frame.
          for (int32_t r = chunk_begin; r < chunk_end; ++r) {
            const size_t chunk_offset =
                static_cast<size_t>(r - chunk_begin) * width;
            const float* m = mix + chunk_offset;
            for (size_t p = 0; p < plane_count; ++p) {
              double* out = current[p] + row_offset(r);
              float* prev = previous[p] + row_offset(r);
              const float* spatial =
                  scratch.planes[p].spatial.data() + chunk_offset;
              for (int32_t x = 0; x < width; ++x) {
                const double motion = m[x];
                const double in = out[x];
                double value = in;
                if (temporal) {
                  value += alpha * (1.0 - motion) *
                           (static_cast<double>(prev[x]) - in);
                }
                if (spatial_enabled && motion > 0.0) {
                  value += motion * (static_cast<double>(spatial[x]) - in);
                }
                out[x] = value;
                prev[x] = static_cast<float>(value);
              }
            }
          }
        }
      });
}

void NoiseReducer::filter(ComponentFrame& frame, bool may_continue) {
  if (settings_.strength_db <= 0.0) return;

  stats_ = Stats{};
  const ActiveRect rect = clamped_active_rect(frame);
  if (rect.empty()) return;

  const bool has_chroma = !frame.getU()->empty() && !frame.getV()->empty();
  const bool continuing =
      may_continue && has_previous_ && geometry_matches(frame);
  if (!continuing) prepare_previous(frame);

  measure(frame, rect, has_chroma, continuing);

  bool discontinuity = !continuing;
  if (continuing) {
    const double anchor =
        std::max(measurement_.anchor_percentile_y, kDiscontinuityAnchorFloor);
    stats_.scale_ratio = measurement_.temporal_percentile_y / anchor;
    stats_.moving_fraction = measurement_.moving_fraction;
    if (measurement_.temporal_percentile_y / kHalfNormalPercentileToScale >=
        kSigmaMax) {
      discontinuity = true;
    }
    if (stats_.scale_ratio > kDiscontinuityScaleRatio) discontinuity = true;
    if (stats_.moving_fraction > kDiscontinuityMovingFraction) {
      discontinuity = true;
    }
  }

  const int32_t bands = band_count(rect.lines());
  bands_.resize(static_cast<size_t>(bands));
  frame_pass(frame, rect, bands, has_chroma, /*temporal=*/!discontinuity);
  has_previous_ = true;

  stats_.filtered = true;
  stats_.temporal = !discontinuity;
  stats_.discontinuity = discontinuity;
  if (!discontinuity) {
    stats_.sigma_y = measurement_.sigma[kPlaneY];
    if (has_chroma) {
      stats_.sigma_u = measurement_.sigma[kPlaneU];
      stats_.sigma_v = measurement_.sigma[kPlaneV];
    }
  }
  const double pair_to_sample = 1.0 / std::sqrt(2.0);
  stats_.sample_sigma_y =
      pair_to_sample * measurement_.difference_sigma[kPlaneY];
  if (has_chroma) {
    stats_.sample_sigma_u =
        pair_to_sample * measurement_.difference_sigma[kPlaneU];
    stats_.sample_sigma_v =
        pair_to_sample * measurement_.difference_sigma[kPlaneV];
  }
}
