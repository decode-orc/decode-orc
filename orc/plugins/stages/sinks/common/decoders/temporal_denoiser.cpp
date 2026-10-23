/*
 * File:        temporal_denoiser.cpp
 * Module:      orc-core
 * Purpose:     Motion-adaptive recursive temporal noise reduction for
 *              decoded component frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "temporal_denoiser.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

// Motion thresholds in units of each plane's estimated noise scale. Below the
// still threshold the full blend applies; above the moving threshold the
// sample passes through; between them the weight falls along a smoothstep.
constexpr double kStillThresholdScale = 2.5;
constexpr double kMovingThresholdScale = 5.0;

// The noise scale is read off the box-mean difference's distribution at this
// percentile. On still noisy picture the box-mean difference is close to a
// zero-mean normal, so its magnitude is half-normal and the percentile
// relates to the normal's scale by a constant: for the 30th percentile,
// q = sigma * Phi^-1(0.65).
constexpr double kNoisePercentile = 0.30;
constexpr double kHalfNormalPercentileToScale = 0.38532;

// Histogram of the box-mean difference used for the percentile: 256 bins of a
// quarter of a sample code, covering 0 to 64 codes. Larger values land in
// the top bin.
constexpr int32_t kHistogramBins = 256;
constexpr double kHistogramBinWidth = 0.25;
constexpr float kHistogramBinsPerCode = 1.0F / kHistogramBinWidth;

// The noise estimate bins every kHistogramSampleStep-th sample of every
// kHistogramRowStep-th active row: an eighth of the picture, still tens of
// thousands of samples on a standard-definition frame.
constexpr int32_t kHistogramRowStep = 4;
constexpr int32_t kHistogramSampleStep = 2;

// Clamp on the estimated noise scale, in sample codes.
constexpr double kSigmaMin = 0.25;
constexpr double kSigmaMax = 8.0;

// When more than this fraction of the active samples are moving the frame is
// treated as a scene cut and its noise measurement is discarded.
constexpr double kCutGuardFraction = 0.5;

double smoothstep(double edge0, double edge1, double x) {
  if (edge1 <= edge0) return x < edge0 ? 0.0 : 1.0;
  const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

int32_t histogram_bin(float value) {
  return std::min(kHistogramBins - 1,
                  static_cast<int32_t>(value * kHistogramBinsPerCode));
}

}  // namespace

// Persistent threads for the line bands. run() hands bands 1.. to the
// workers, runs band 0 on the caller, and returns once every band is done,
// so the caller's scratch state is never touched concurrently between
// calls.
class TemporalDenoiser::BandPool {
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

TemporalDenoiser::TemporalDenoiser(const Settings& settings)
    : settings_(settings) {
  if (settings_.strength_db > 0.0 && settings_.threads > 1) {
    pool_ = std::make_unique<BandPool>(settings_.threads - 1);
  }
}

TemporalDenoiser::~TemporalDenoiser() = default;

double TemporalDenoiser::blend_weight_for_strength_db(double strength_db) {
  if (strength_db <= 0.0) return 0.0;
  const double r = std::pow(10.0, -strength_db / 10.0);
  return (1.0 - r) / (1.0 + r);
}

void TemporalDenoiser::reset() {
  has_previous_ = false;
  has_sigma_ = false;
}

TemporalDenoiser::ActiveRect TemporalDenoiser::clamped_active_rect(
    const ComponentFrame& frame) const {
  ActiveRect rect;
  rect.x_start = std::clamp(settings_.active_x_start, 0, frame.getWidth());
  rect.x_end = std::clamp(settings_.active_x_end, 0, frame.getWidth());
  rect.line_start =
      std::clamp(settings_.active_line_start, 0, frame.getHeight());
  rect.line_end = std::clamp(settings_.active_line_end, 0, frame.getHeight());
  return rect;
}

bool TemporalDenoiser::geometry_matches(const ComponentFrame& frame) const {
  if (frame.getWidth() != previous_.width) return false;
  if (frame.getHeight() != previous_.height) return false;
  if (frame.getY()->size() != previous_.planes[kPlaneY].size()) return false;
  const bool frame_has_chroma = !frame.getU()->empty();
  return frame_has_chroma == previous_.has_chroma;
}

void TemporalDenoiser::pass_through(const ComponentFrame& frame) {
  previous_.width = frame.getWidth();
  previous_.height = frame.getHeight();
  previous_.has_chroma = !frame.getU()->empty();
  const std::array<const std::vector<double>*, kPlaneCount> source = {
      frame.getY(), frame.getU(), frame.getV()};
  for (size_t p = 0; p < kPlaneCount; ++p) {
    previous_.planes[p].assign(source[p]->begin(), source[p]->end());
  }
  has_previous_ = true;
  has_sigma_ = false;
  stats_.filtered = false;
}

int32_t TemporalDenoiser::band_count(int32_t lines) const {
  const int32_t available = pool_ ? pool_->worker_count() + 1 : 1;
  return std::clamp(lines / kMinBandRows, 1, available);
}

void TemporalDenoiser::run_bands(int32_t lines, int32_t bands,
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

void TemporalDenoiser::frame_pass(ComponentFrame& frame, const ActiveRect& rect,
                                  int32_t bands, bool has_chroma, bool blend) {
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
  for (size_t p = 0; p < plane_count; ++p) {
    inverse_sigma[p] = static_cast<float>(1.0 / sigma_[p]);
  }

  // Signed difference of row r (within the active rectangle) of plane p and
  // its running sum over a window of kBoxWidth consecutive samples. The
  // window is centred on x - 1 .. x + 2 and slides inward at the edges rather
  // than shrinking, so every sample is measured over one full subcarrier
  // cycle and a 4fsc residual cancels everywhere.
  const auto sum_row = [&](size_t p, int32_t r, float* diff, float* sums) {
    const size_t row_offset =
        static_cast<size_t>(rect.line_start + r) * frame_width + rect.x_start;
    const double* cur = current[p] + row_offset;
    const float* prev = previous[p] + row_offset;
    for (int32_t x = 0; x < width; ++x) {
      diff[x] = static_cast<float>(cur[x] - static_cast<double>(prev[x]));
    }
    if (width < kBoxWidth) {
      float sum = 0.0F;
      for (int32_t x = 0; x < width; ++x) sum += diff[x];
      for (int32_t x = 0; x < width; ++x) sums[x] = sum;
      return;
    }
    for (int32_t x = 1; x + 2 < width; ++x) {
      sums[x] = diff[x - 1] + diff[x] + diff[x + 1] + diff[x + 2];
    }
    // The first sample shares the first window, the last two the last one.
    sums[0] = sums[1];
    sums[width - 2] = sums[width - 3];
    sums[width - 1] = sums[width - 3];
  };

  // Blend one plane's row towards the previous output with the per-sample
  // weights, and keep the result as the next previous output.
  const auto blend_row = [width](double* out, float* prev,
                                 const double* weights) {
    for (int32_t x = 0; x < width; ++x) {
      out[x] += weights[x] * (static_cast<double>(prev[x]) - out[x]);
      prev[x] = static_cast<float>(out[x]);
    }
  };

  // A band keeps row sums for its own rows and kChunkHalo rows on each side
  // of them, clamped to the active area.
  const auto sums_begin_of = [&](int32_t row_begin) {
    return std::max(0, row_begin - kChunkHalo);
  };
  const auto sums_end_of = [&](int32_t row_end) {
    return std::min(lines, row_end + kChunkHalo);
  };
  const auto prepare_band = [&](BandScratch& scratch, int32_t row_begin,
                                int32_t row_end) {
    const size_t sum_rows =
        static_cast<size_t>(sums_end_of(row_end) - sums_begin_of(row_begin));
    for (size_t p = 0; p < plane_count; ++p) {
      PlaneScratch& plane = scratch.planes[p];
      plane.diff_row.resize(width);
      plane.row_sums.resize(sum_rows * width);
      plane.motion.resize(static_cast<size_t>(kChunkRows) * width);
    }
    scratch.weights.resize(width);
  };
  const auto sums_row = [&](PlaneScratch& plane, int32_t sums_begin,
                            int32_t r) {
    return plane.row_sums.data() + static_cast<size_t>(r - sums_begin) * width;
  };

  // Every row must be summed before any blend touches it. Within a band the
  // chunks sum ahead of the blend; the halo rows beyond a band's ends belong
  // to the neighbouring bands, which blend them concurrently, so they are
  // summed first, before any band starts blending.
  if (bands > 1) {
    run_bands(lines, bands,
              [&](int32_t band, int32_t row_begin, int32_t row_end) {
                BandScratch& scratch = bands_[band];
                prepare_band(scratch, row_begin, row_end);
                const int32_t sums_begin = sums_begin_of(row_begin);
                const int32_t sums_end = sums_end_of(row_end);
                for (size_t p = 0; p < plane_count; ++p) {
                  PlaneScratch& plane = scratch.planes[p];
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
        scratch.moving = 0;
        const int32_t sums_begin = sums_begin_of(row_begin);
        std::array<int32_t, kPlaneCount> summed = {};
        for (size_t p = 0; p < plane_count; ++p) {
          scratch.planes[p].histogram.assign(kHistogramBins, 0);
          summed[p] = row_begin;
        }

        for (int32_t chunk_begin = row_begin; chunk_begin < row_end;
             chunk_begin += kChunkRows) {
          const int32_t chunk_end = std::min(row_end, chunk_begin + kChunkRows);
          // Rows this chunk's vertical boxes can reach, summed before the blend
          // of this chunk (and so before the blend of any earlier chunk could
          // have changed them).
          const int32_t need_end = std::min(row_end, chunk_end + kChunkHalo);

          for (size_t p = 0; p < plane_count; ++p) {
            PlaneScratch& plane = scratch.planes[p];
            float* diff = plane.diff_row.data();
            for (int32_t r = summed[p]; r < need_end; ++r) {
              sum_row(p, r, diff, sums_row(plane, sums_begin, r));
            }
            summed[p] = need_end;

            // Magnitude of the box mean for the chunk's own rows, binned for
            // the noise estimate as it goes.
            uint32_t* histogram = plane.histogram.data();
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
              // The noise estimate bins a fixed subsample of the rows and
              // samples, chosen in active-area coordinates so the band split
              // does not change it.
              if (r % kHistogramRowStep == 0) {
                for (int32_t x = 0; x < width; x += kHistogramSampleStep) {
                  ++histogram[histogram_bin(out[x])];
                }
              }
            }
          }

          if (!blend) continue;

          // Blend the chunk's rows. Motion is in units of each plane's own
          // noise scale, the largest plane winning, so a change in any plane
          // counts and a noisier plane does not dominate the measure; the
          // weight is shared by all three planes.
          const float* motion_y = scratch.planes[kPlaneY].motion.data();
          const float* motion_u = scratch.planes[kPlaneU].motion.data();
          const float* motion_v = scratch.planes[kPlaneV].motion.data();
          double* weights = scratch.weights.data();
          for (int32_t r = chunk_begin; r < chunk_end; ++r) {
            const size_t row_offset =
                static_cast<size_t>(rect.line_start + r) * frame_width +
                rect.x_start;
            const size_t motion_offset =
                static_cast<size_t>(r - chunk_begin) * width;
            size_t moving = 0;
            for (int32_t x = 0; x < width; ++x) {
              const size_t i = motion_offset + x;
              float motion = motion_y[i] * inverse_sigma[kPlaneY];
              if (has_chroma) {
                motion = std::max(motion, motion_u[i] * inverse_sigma[kPlaneU]);
                motion = std::max(motion, motion_v[i] * inverse_sigma[kPlaneV]);
              }
              if (motion > moving_threshold) ++moving;
              weights[x] = alpha * (1.0 - smoothstep(still_threshold,
                                                     moving_threshold, motion));
            }
            scratch.moving += moving;

            blend_row(current[kPlaneY] + row_offset,
                      previous[kPlaneY] + row_offset, weights);
            if (has_chroma) {
              blend_row(current[kPlaneU] + row_offset,
                        previous[kPlaneU] + row_offset, weights);
              blend_row(current[kPlaneV] + row_offset,
                        previous[kPlaneV] + row_offset, weights);
            }
          }
        }
      });
}

double TemporalDenoiser::estimate_sigma(Plane plane, int32_t bands) {
  histogram_.assign(kHistogramBins, 0);
  uint64_t count = 0;
  for (int32_t band = 0; band < bands; ++band) {
    const auto& band_histogram = bands_[band].planes[plane].histogram;
    for (int32_t bin = 0; bin < kHistogramBins; ++bin) {
      histogram_[bin] += band_histogram[bin];
      count += band_histogram[bin];
    }
  }

  // Walk to the bin holding the percentile and interpolate within it.
  const double target = kNoisePercentile * static_cast<double>(count);
  double cumulative = 0.0;
  double percentile_value = kHistogramBins * kHistogramBinWidth;
  for (int32_t bin = 0; bin < kHistogramBins; ++bin) {
    const double in_bin = histogram_[bin];
    if (cumulative + in_bin >= target) {
      const double fraction =
          in_bin > 0.0 ? (target - cumulative) / in_bin : 0.0;
      percentile_value = (bin + fraction) * kHistogramBinWidth;
      break;
    }
    cumulative += in_bin;
  }

  const double sigma = percentile_value / kHalfNormalPercentileToScale;
  return std::clamp(sigma, kSigmaMin, kSigmaMax);
}

void TemporalDenoiser::filter(ComponentFrame& frame) {
  if (settings_.strength_db <= 0.0) return;

  stats_ = Stats{};
  const ActiveRect rect = clamped_active_rect(frame);
  if (!has_previous_ || rect.empty() || !geometry_matches(frame)) {
    pass_through(frame);
    return;
  }

  const bool has_chroma = !frame.getU()->empty() && !frame.getV()->empty();
  const size_t plane_count = has_chroma ? kPlaneCount : 1;
  const size_t count = static_cast<size_t>(rect.width()) * rect.lines();
  const int32_t bands = band_count(rect.lines());
  bands_.resize(static_cast<size_t>(bands));

  const auto adopt_measurement = [&]() {
    for (size_t p = 0; p < plane_count; ++p) {
      sigma_[p] = estimate_sigma(static_cast<Plane>(p), bands);
    }
    has_sigma_ = true;
  };

  // The first filtered frame of a sequence has no noise scales to apply yet,
  // so it measures itself before blending.
  if (!has_sigma_) {
    frame_pass(frame, rect, bands, has_chroma, /*blend=*/false);
    adopt_measurement();
  }

  frame_pass(frame, rect, bands, has_chroma, /*blend=*/true);

  size_t moving = 0;
  for (int32_t band = 0; band < bands; ++band) moving += bands_[band].moving;
  stats_.moving_fraction =
      static_cast<double>(moving) / static_cast<double>(count);
  stats_.cut_detected = stats_.moving_fraction > kCutGuardFraction;

  // A frame that is mostly motion says nothing about the noise; keep the
  // scales from before the cut for the frames that follow it.
  if (!stats_.cut_detected) adopt_measurement();

  stats_.filtered = !stats_.cut_detected;
  stats_.sigma_y = sigma_[kPlaneY];
  if (has_chroma) {
    stats_.sigma_u = sigma_[kPlaneU];
    stats_.sigma_v = sigma_[kPlaneV];
  }
}
