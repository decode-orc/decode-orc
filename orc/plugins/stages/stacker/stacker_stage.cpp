/*
 * File:        stacker_stage.cpp
 * Module:      orc-core
 * Purpose:     Multi-source CVBS_U10_4FSC frame stacking stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <fmt/format.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_content.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>
#include <orc/support/stack_measurement.h>
#include <stacker_stage.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#include "efm_confidence_stack.h"

namespace orc {

namespace {

// ── Sample-grid alignment ───────────────────────────────────────────────────
//
// Independent decodes of the same disc need not start a line on the same 4FSC
// sample, and one sample is 90° of subcarrier, so combining composite samples
// across sources whose grids differ erodes chroma rather than reinforcing it.
// SampleGridMeter (orc/support/stack_measurement.h) measures each source's
// offset from the reference.  Real captures measure up to 12 samples apart,
// and the offset drifts by a sample or more across a side, so it is measured
// per segment of frames rather than once per source.

// Probe frames per segment, and the candidate positions they are taken from,
// when measuring an offset.
constexpr size_t kAlignProbeFrames = 4;
constexpr size_t kAlignProbeCandidates = 8;

// How many segments either side a segment that cannot be measured looks for
// a measured one to hold the offset from.
constexpr size_t kMaxSegmentSearch = 4;

// ── Field agreement ─────────────────────────────────────────────────────────
//
// Frame alignment matches pictures by number, so a frame whose field 2 came
// from a different picture (a player skip in mid-frame) or any remaining
// mapping error reaches the stacker under the right number with the wrong
// content.  Each input's fields are therefore compared with the per-sample
// median of all the inputs before they are combined.  Correct inputs score
// 0.93 or more on real captures, a foreign field under 0.1, so a fixed floor
// separates them.

// Correlation with the inputs' median an input's field must reach to be used.
constexpr double kMinFieldCorrelation = 0.8;

// Fields whose median varies less than this (10-bit codes, standard
// deviation of the low-passed luma) carry too little picture to judge: a
// black or flat field correlates as noise.
constexpr double kMinFieldDetail = 20.0;

// Decimation of the comparison grid.
constexpr size_t kFieldCheckLineStep = 8;
constexpr size_t kFieldCheckSampleStep = 4;

// ── Chroma gain ─────────────────────────────────────────────────────────────
//
// See StackerStage::burst_amplitude().  On five Volcanoes captures the
// mismatched capture's burst measured 1.13-1.23 times the inputs' median, and
// the gain that burst gave brought its pair variances back to those of the
// other inputs.  Elsewhere the per-frame measurement scattered within 2 %.

// Samples left off each end of the burst window (cvbs_signal_constants.h),
// clear of the burst's rise and fall.
constexpr size_t kBurstEdgeSamples = 4;

// Share of a line's burst-window power that must lie at fsc for the line to
// be measured: a clean burst gives nearly all of it, a window of picture or
// noise a small part.
constexpr double kMinBurstPurity = 0.7;

// Lines that must be measured for a frame's burst to be taken.
constexpr size_t kMinBurstLines = 64;

// Share of the lines' amplitudes left off each end before they are averaged.
constexpr double kBurstTrim = 0.1;

// Inputs whose bursts lie within this fraction of the reference are left as
// they are: the measurement's own scatter.
constexpr double kChromaGainTolerance = 0.05;

// The largest gain, either way, taken for a correction; beyond it the burst
// says more about the capture than the decode's correction.
constexpr double kMaxChromaGain = 1.5;

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
  const auto count = static_cast<double>(a.size());
  double mean_a = 0.0;
  double mean_b = 0.0;
  for (size_t k = 0; k < a.size(); ++k) {
    mean_a += a[k];
    mean_b += b[k];
  }
  mean_a /= count;
  mean_b /= count;
  double xy = 0.0;
  double xx = 0.0;
  double yy = 0.0;
  for (size_t k = 0; k < a.size(); ++k) {
    xy += (a[k] - mean_a) * (b[k] - mean_b);
    xx += (a[k] - mean_a) * (a[k] - mean_a);
    yy += (b[k] - mean_b) * (b[k] - mean_b);
  }
  const double denominator = std::sqrt(xx * yy);
  return denominator > 0.0 ? xy / denominator : 0.0;
}

double standard_deviation(const std::vector<double>& v) {
  double mean = 0.0;
  for (double x : v) mean += x;
  mean /= static_cast<double>(v.size());
  double sum = 0.0;
  for (double x : v) sum += (x - mean) * (x - mean);
  return std::sqrt(sum / static_cast<double>(v.size()));
}

// ── Report records ──────────────────────────────────────────────────────────
//
// What the stacker measures, for a script reading its log: one line per
// record, "StackerStage report: " followed by a single-line JSON object.  The
// prefix and the object's "v" key are the contract (the stage's
// instructions.md lists the records and their keys); the human-readable
// lines beside them may change.  Frames are numbered from 1.
constexpr int kReportVersion = 1;

// Logs a record of |kind| for frames |first|-|last|; |fields| is the rest of
// the object, each key with a leading comma.
void log_report(const char* kind, FrameID first, FrameID last,
                const std::string& fields) {
  ORC_LOG_INFO(
      "StackerStage report: {{\"v\":{},\"kind\":\"{}\",\"frames\":[{},{}]{}}}",
      kReportVersion, kind, first + 1, last + 1, fields);
}

// The output frame's id, for log messages: the id every contributing source
// is read at.
FrameID frame_id_of(const std::vector<FrameID>& source_ids) {
  for (FrameID id : source_ids) {
    if (id != UINT64_MAX) return id;
  }
  return 0;
}

// ── Input noise ─────────────────────────────────────────────────────────────
//
// The Weighted mode weights each input by its noise and rejects values far
// from the per-sample median, so it needs each input's noise.  Once their
// grids are aligned the inputs carry the same picture, so the difference of
// two inputs is noise alone.  PairNoiseMeter measures each pair's variance and
// fit_pressing_noise() solves each input's own noise and each pressing's
// shared noise from them (orc/support/stack_measurement.h).  Solving
// v_ij = σᵢ² + σⱼ² instead would charge each input with the pressing noise it
// does not share with the rest of the stack, so the inputs of an
// under-represented pressing would measure noisier, be weighted down, and pull
// the stack towards the larger group — while shared noise only averages out
// across groups.

// σ given to every input when nothing could be measured.  With kRejectScale
// and the default reject_sigma of 5 that rejects at 60 codes, ld-disc-stacker's
// smart threshold on the 10-bit scale.
constexpr double kFallbackNoiseSigma = 10.0;

// The rejection scale σ̂ against the median input σ.  The study's best policy
// rejected at 5σ̂ with this scale.
constexpr double kRejectScale = 1.2;

// A segment's noise is logged again only when an input's σ moves by more
// than this fraction.
constexpr double kNoiseLogChange = 0.25;

// Each measured pair's variance, inputs numbered from 1, for the debug log.
std::string describe_pair_variances(
    size_t n, const std::vector<std::optional<double>>& pair_variance) {
  std::string text;
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      const std::optional<double> v = pair_variance[i * n + j];
      if (!v) continue;
      if (!text.empty()) text += ", ";
      text += fmt::format("{}-{} {:.1f}", i + 1, j + 1, *v);
    }
  }
  return text.empty() ? std::string("none") : text;
}

// " (N pair(s) measured on the whole picture)" when some pairs had too few
// flat blocks, for the debug log.
std::string describe_whole_picture_pairs(size_t pairs) {
  return pairs == 0 ? std::string()
                    : fmt::format(" ({} pair(s) measured on the whole picture)",
                                  pairs);
}

// Each input's luma gain and level against the common picture, numbered from
// 1, for the debug log.
std::string describe_levels(
    const std::vector<std::optional<LevelMatch>>& levels) {
  std::string text;
  for (size_t i = 0; i < levels.size(); ++i) {
    if (!text.empty()) text += ", ";
    const std::optional<LevelMatch>& level = levels[i];
    text += level ? fmt::format("{} {:+.1f}% {:+.1f}", i + 1,
                                100.0 * (level->gain - 1.0), level->level)
                  : fmt::format("{} -", i + 1);
  }
  return text;
}

// What the fitted noise model predicts for the stack, and how well it fits.
std::string describe_fit_check(const PressingFitCheck& check) {
  return fmt::format(
      "predicted stack noise σ {:.1f} codes weighted, {:.1f} equal, best "
      "single input {:.1f}; model misfit {:.0f}% worst, {:.0f}% rms over {} "
      "pair(s)",
      check.weighted_sigma, check.equal_sigma, check.best_single_sigma,
      100.0 * (std::exp(check.worst_misfit) - 1.0),
      100.0 * (std::exp(check.rms_misfit) - 1.0), check.pairs);
}

// The keys of a weights report record after its frames: each input's σ,
// share of the weight, stamper group and family (σ null for an input not
// measured), whether the fit was trusted, whether the inputs are therefore
// stacked with equal weights, the model's predicted stack σ, and how well it
// fitted: the worst pair and its misfit, the rms misfit and the pairs
// compared, which say why a block was not trusted.
std::string describe_weights_record(
    const std::vector<std::optional<PressingNoise>>& measured,
    const PressingFitCheck& check) {
  double weight_total = 0.0;
  for (const auto& noise : measured) {
    if (noise) weight_total += noise->weight;
  }
  std::string inputs;
  for (size_t i = 0; i < measured.size(); ++i) {
    const std::optional<PressingNoise>& noise = measured[i];
    if (!inputs.empty()) inputs += ",";
    inputs +=
        noise ? fmt::format(
                    "{{\"input\":{},\"sigma\":{:.2f},\"share\":{:.1f},"
                    "\"stamperGroup\":\"{}\",\"family\":\"{}\"}}",
                    i + 1, noise->sigma, 100.0 * noise->weight / weight_total,
                    pressing_name(noise->group), pressing_name(noise->family))
              : fmt::format("{{\"input\":{},\"sigma\":null}}", i + 1);
  }
  return fmt::format(
      ",\"inputs\":[{}],\"fitTrusted\":{},\"fellBackToMean\":{},"
      "\"weightedSigma\":{:.2f},\"equalSigma\":{:.2f},"
      "\"bestSingleSigma\":{:.2f},\"worstPair\":[{},{}],"
      "\"worstMisfitPercent\":{:.1f},\"rmsMisfitPercent\":{:.1f},"
      "\"pairs\":{}",
      inputs, check.trusted, !check.trusted, check.weighted_sigma,
      check.equal_sigma, check.best_single_sigma, check.worst_i + 1,
      check.worst_j + 1, 100.0 * (std::exp(check.worst_misfit) - 1.0),
      100.0 * (std::exp(check.rms_misfit) - 1.0), check.pairs);
}

// The inputs of each pressing, numbered from 1, as " {1,2} {3}".
std::string describe_pressings(
    const std::vector<std::optional<size_t>>& pressing) {
  size_t groups = 0;
  for (const auto& g : pressing) {
    if (g) groups = std::max(groups, *g + 1);
  }
  std::string text;
  for (size_t g = 0; g < groups; ++g) {
    std::string members;
    for (size_t i = 0; i < pressing.size(); ++i) {
      if (pressing[i] != g) continue;
      members += (members.empty() ? "" : ",") + std::to_string(i + 1);
    }
    text += " {" + members + "}";
  }
  return text;
}

double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t mid = v.size() / 2;
  return v.size() % 2 ? v[mid] : 0.5 * (v[mid - 1] + v[mid]);
}

// Round a combined value to the nearest sample.  Ties go to even (the default
// floating-point rounding mode), so the mean of two inputs, half of which
// fall on a tie, carries no bias either way.
int16_t round_sample(double value) {
  return static_cast<int16_t>(
      std::clamp<int64_t>(std::llrint(value), INT16_MIN, INT16_MAX));
}

// ── Still-frame copies ──────────────────────────────────────────────────────
//
// The repeats of a held still within one source are found by the frame
// content measurements (orc/support/frame_content.h), against a reference
// noise measured per source and segment: BlockNoiseMeter over the pairs
// among a spread of the segment's frames that show the same picture.  The
// first pairs are found against the within-frame anchor, which reads high,
// and the noise they give is used to find the pairs again, a few times over.

// Frames per segment, each paired with the frame a colour cadence later,
// that the reference noise is measured on.
constexpr size_t kStillNoiseProbes = 24;

// Frames whose signatures are built ahead of the search window when frames
// are read in order, so that they are built in parallel.
constexpr FrameID kSignatureLookahead = 8;

// Frames beyond the search window that other threads may be stacking at the
// same time: an export's decode workers, or a FrameReadAhead (whose window is
// at most 32) running ahead of a serial reader.
constexpr FrameID kConcurrentFrames = 32;

// Field pairs that must match before their noise is taken as the reference.
constexpr size_t kMinStillNoisePairs = 4;

// Rounds of matching the probe pairs against the noise the last round gave.
constexpr size_t kStillNoisePasses = 3;

// Pressing-defect repair (StackerStage::repair_pressing_defects()): the
// shortest run of defect samples replaced, as the streak path of
// dp111's domesday-aiv-restore takes it (impulses and dips are not repaired),
// and the samples either side of a run replaced with it, where the 4-sample
// luma the run was found on blurs its ends.  On Community North picture 3001,
// field 2 line 208, every disc of pressing A carries a dashed streak at code 4
// for about 40 samples, where the other pressings carry picture.
constexpr size_t kMinRepairRun = 6;
// Sources a pressing needs before a departure of theirs is taken as its
// defect: a lone capture's departure is its own (an unflagged dropout),
// which rejection already leaves out where the inputs outnumber it.
constexpr size_t kMinDefectSources = 2;
constexpr size_t kRepairMargin = 2;

// Video geometry of a source, from its parameters.
struct FrameGeometry {
  VideoSystem system = VideoSystem::PAL;
  size_t width = 0;
  size_t height = 0;
  size_t samples = 0;
};

std::optional<FrameGeometry> geometry_of(const VideoFrameRepresentation& src) {
  const auto params = src.get_video_parameters();
  if (!params) return std::nullopt;
  FrameGeometry g;
  g.system = params->system;
  g.width = static_cast<size_t>(params->frame_width_nominal);
  g.height = static_cast<size_t>(params->frame_height);
  if (g.width == 0 || g.height == 0) return std::nullopt;
  g.samples = frame_line_sample_offset(g.system, g.width, g.height);
  return g;
}

// Dropout severities: a sample no input could supply, and a sample the
// inputs could not agree on (Weighted mode only).
constexpr uint8_t kHoleSeverity = 50;
constexpr uint8_t kLowConfidenceSeverity = 25;

// Builds one line's output dropout runs from per-sample marks, starting a new
// run wherever the severity changes.
class RunBuilder {
 public:
  explicit RunBuilder(std::vector<DropoutRun>& out) : out_(out) {}

  // Mark the sample at |offset|: severity 0 for a clean sample.
  void mark(uint64_t offset, uint8_t severity) {
    if (severity == severity_) return;
    finish(offset);
    start_ = offset;
    severity_ = severity;
  }

  // Close any open run at |end|.
  void finish(uint64_t end) {
    if (severity_ != 0 && end > start_) {
      DropoutRun r;
      r.frame_id = 0;
      r.sample_start = start_;
      r.sample_count = static_cast<uint32_t>(end - start_);
      r.severity = severity_;
      out_.push_back(r);
    }
    severity_ = 0;
  }

 private:
  std::vector<DropoutRun>& out_;
  uint64_t start_ = 0;
  uint8_t severity_ = 0;
};

// The record of what went into one output frame, logged at DEBUG as there
// is one per frame: inputs and copies stacked per field, and the samples
// left marked, low-confidence ones apart from those no input could supply.
void log_frame_report(FrameID id, const FrameStackUse& use,
                      const std::vector<DropoutRun>& dropouts) {
  size_t low_confidence = 0;
  size_t unrecoverable = 0;
  for (const DropoutRun& run : dropouts) {
    (run.severity == kLowConfidenceSeverity ? low_confidence : unrecoverable) +=
        run.sample_count;
  }
  ORC_LOG_DEBUG(
      "StackerStage report: {{\"v\":{},\"kind\":\"frame\",\"frames\":[{},{}],"
      "\"inputs\":[{},{}],\"copies\":[{},{}],\"lowConfidenceSamples\":{},"
      "\"unrecoverableSamples\":{},\"repairedSamples\":{}}}",
      kReportVersion, id + 1, id + 1, use.inputs[0], use.inputs[1],
      use.copies[0], use.copies[1], low_confidence, unrecoverable,
      use.repaired_samples);
}

// Fills |use|, when given, from the rows' per-field validity: the first
// |num_sources| rows are the inputs, the rest still-frame copies.
void count_frame_use(const std::vector<bool>& valid,
                     const std::vector<std::array<bool, 2>>& field_valid,
                     size_t num_sources, FrameStackUse* use) {
  if (!use) return;
  *use = {};
  for (size_t i = 0; i < field_valid.size() && i < valid.size(); ++i) {
    if (!valid[i]) continue;
    auto& count = i < num_sources ? use->inputs : use->copies;
    for (size_t field = 0; field < 2; ++field) {
      if (field_valid[i][field]) ++count[field];
    }
  }
}

// Runs work(i) for every i below |count| on up to |threads| threads, the
// calling thread among them.
void run_parallel(size_t count, size_t threads,
                  const std::function<void(size_t)>& work) {
  std::atomic<size_t> next{0};
  auto worker = [&]() {
    for (size_t i = next++; i < count; i = next++) work(i);
  };
  std::vector<std::thread> pool;
  for (size_t t = 1; t < std::min(threads, count); ++t) {
    pool.emplace_back(worker);
  }
  worker();
  for (auto& thread : pool) thread.join();
}

}  // namespace

StackerStage::FieldAgreement StackerStage::check_field_agreement(
    const std::vector<std::vector<sample_type>>& planes,
    const std::vector<bool>& frame_valid,
    const std::vector<std::vector<DropoutRun>>& dropouts, VideoSystem system,
    size_t nominal_width, size_t height, FrameID frame_id) {
  const size_t num_sources = planes.size();
  FieldAgreement result;
  result.field_valid.assign(num_sources, {true, true});

  std::vector<size_t> valid;
  for (size_t i = 0; i < num_sources; ++i) {
    if (frame_valid[i]) valid.push_back(i);
  }
  if (valid.size() < 2) return result;

  const size_t field1_lines = std::min(field1_line_count(system), height);
  const std::array<std::pair<size_t, size_t>, 2> fields = {
      std::pair<size_t, size_t>{0, field1_lines},
      std::pair<size_t, size_t>{field1_lines, height}};

  for (size_t field = 0; field < 2; ++field) {
    const auto [field_first, field_end] = fields[field];

    // values[k] holds valid input k's low-passed luma on the decimated grid.
    std::vector<std::vector<double>> values(valid.size());
    std::vector<bool> masked;
    for (size_t line = field_first + kFieldBlankingLines; line < field_end;
         line += kFieldCheckLineStep) {
      const size_t start =
          frame_line_sample_offset(system, nominal_width, line);
      const size_t length =
          frame_line_sample_count(system, nominal_width, line);
      const size_t first = length / 5;
      const size_t last = length > 10 ? length - 10 : length;
      if (last <= first + kGridLumaBox) continue;

      // Samples any input flags as dropout are left out for every input, so
      // the inputs stay compared position for position.
      masked.assign(length, false);
      for (size_t i : valid) {
        for (const auto& run : dropouts[i]) {
          const uint64_t run_end = run.sample_start + run.sample_count;
          if (run_end <= start || run.sample_start >= start + length) continue;
          const size_t from =
              run.sample_start > start ? run.sample_start - start : 0;
          const size_t to = std::min<uint64_t>(run_end - start,
                                               static_cast<uint64_t>(length));
          std::fill(masked.begin() + static_cast<std::ptrdiff_t>(from),
                    masked.begin() + static_cast<std::ptrdiff_t>(to), true);
        }
      }

      for (size_t n = first; n + kGridLumaBox <= last;
           n += kFieldCheckSampleStep) {
        bool skip = false;
        for (size_t k = 0; k < kGridLumaBox; ++k) skip = skip || masked[n + k];
        if (skip) continue;
        for (size_t k = 0; k < valid.size(); ++k) {
          const auto& plane = planes[valid[k]];
          if (start + n + kGridLumaBox > plane.size()) {
            skip = true;
            break;
          }
        }
        if (skip) continue;
        for (size_t k = 0; k < valid.size(); ++k) {
          const auto* p = planes[valid[k]].data() + start + n;
          double sum = 0.0;
          for (size_t j = 0; j < kGridLumaBox; ++j) sum += p[j];
          values[k].push_back(sum / static_cast<double>(kGridLumaBox));
        }
      }
    }

    const size_t positions = values[0].size();
    if (positions < 16) continue;

    std::vector<double> median(positions);
    std::vector<double> column(valid.size());
    for (size_t p = 0; p < positions; ++p) {
      for (size_t k = 0; k < valid.size(); ++k) column[k] = values[k][p];
      std::sort(column.begin(), column.end());
      const size_t mid = column.size() / 2;
      median[p] = column.size() % 2 ? column[mid]
                                    : 0.5 * (column[mid - 1] + column[mid]);
    }
    if (standard_deviation(median) < kMinFieldDetail) continue;

    // With two inputs the median is their mean and says nothing about which
    // one is wrong, so the pair is judged against each other.
    std::vector<double> scores(valid.size());
    if (valid.size() == 2) {
      scores[0] = scores[1] = correlation(values[0], values[1]);
    } else {
      for (size_t k = 0; k < valid.size(); ++k) {
        scores[k] = correlation(values[k], median);
      }
    }

    const auto agreeing = static_cast<size_t>(
        std::count_if(scores.begin(), scores.end(),
                      [](double s) { return s >= kMinFieldCorrelation; }));

    if (agreeing >= 2) {
      for (size_t k = 0; k < valid.size(); ++k) {
        if (scores[k] >= kMinFieldCorrelation) continue;
        result.field_valid[valid[k]][field] = false;
        ORC_LOG_WARN(
            "StackerStage: frame {} field {}: input {} correlates with the "
            "other inputs at only {:.2f}, so it carries a different picture; "
            "leaving it out of this field",
            frame_id, field + 1, valid[k] + 1, scores[k]);
      }
    } else {
      result.disagreed[field] = true;
      ORC_LOG_WARN(
          "StackerStage: frame {} field {}: the inputs do not agree on the "
          "picture, so none can be preferred; stacking all of them and "
          "marking the field as dropout",
          frame_id, field + 1);
    }
  }

  return result;
}

void StackerStage::mark_disagreed_fields(
    const FieldAgreement& agreement, VideoSystem system, size_t nominal_width,
    size_t height, std::vector<DropoutRun>& output_dropouts) {
  const size_t field1_lines = std::min(field1_line_count(system), height);
  for (size_t field = 0; field < 2; ++field) {
    if (!agreement.disagreed[field]) continue;
    const size_t first_line = field == 0 ? 0 : field1_lines;
    const size_t end_line = field == 0 ? field1_lines : height;
    const uint64_t field_start =
        frame_line_sample_offset(system, nominal_width, first_line);
    const uint64_t field_end =
        frame_line_sample_offset(system, nominal_width, end_line);

    // The whole field replaces any residual runs inside it.
    output_dropouts.erase(
        std::remove_if(output_dropouts.begin(), output_dropouts.end(),
                       [&](const DropoutRun& r) {
                         return r.sample_start >= field_start &&
                                r.sample_start < field_end;
                       }),
        output_dropouts.end());
    for (size_t line = first_line; line < end_line; ++line) {
      DropoutRun r;
      r.frame_id = 0;
      r.sample_start = frame_line_sample_offset(system, nominal_width, line);
      r.sample_count = static_cast<uint32_t>(
          frame_line_sample_count(system, nominal_width, line));
      r.severity = 50;
      output_dropouts.push_back(r);
    }
  }
}

std::optional<double> StackerStage::burst_amplitude(
    const std::vector<sample_type>& plane,
    const std::vector<DropoutRun>& dropouts, VideoSystem system,
    size_t nominal_width, size_t height) {
  // Whole subcarrier cycles (4 samples each at 4FSC) inside the burst, so
  // the line's level cancels out of the demodulation.
  const auto [burst_start, burst_end] = colour_burst_range(system);
  const size_t first = static_cast<size_t>(burst_start) + kBurstEdgeSamples;
  const size_t last = static_cast<size_t>(burst_end) - kBurstEdgeSamples;
  if (last <= first + 8) return std::nullopt;
  const size_t length = (last - first) & ~size_t{3};

  const size_t field1_lines = std::min(field1_line_count(system), height);
  const std::array<std::pair<size_t, size_t>, 2> fields = {
      std::pair<size_t, size_t>{0, field1_lines},
      std::pair<size_t, size_t>{field1_lines, height}};

  std::vector<double> amplitudes;
  for (const auto& [field_first, field_end] : fields) {
    for (size_t line = field_first + kFieldBlankingLines; line < field_end;
         ++line) {
      const size_t start =
          frame_line_sample_offset(system, nominal_width, line) + first;
      if (start + length > plane.size()) continue;
      const bool flagged =
          std::any_of(dropouts.begin(), dropouts.end(), [&](const auto& run) {
            return run.sample_start < start + length &&
                   run.sample_start + run.sample_count > start;
          });
      if (flagged) continue;

      // The window's component at fsc, z = Σ s[k]·e^(-jπk/2).  A sinusoid
      // of amplitude A over whole cycles gives |z| = A·length/2, and its
      // power is 2|z|²/length.
      const sample_type* p = plane.data() + start;
      double re = 0.0;
      double im = 0.0;
      double power = 0.0;
      double mean = 0.0;
      for (size_t k = 0; k < length; ++k) mean += p[k];
      mean /= static_cast<double>(length);
      for (size_t k = 0; k < length; k += 4) {
        const double s0 = p[k] - mean;
        const double s1 = p[k + 1] - mean;
        const double s2 = p[k + 2] - mean;
        const double s3 = p[k + 3] - mean;
        re += s0 - s2;
        im += s3 - s1;
        power += s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3;
      }
      const double magnitude_squared = re * re + im * im;
      const double fsc_power =
          2.0 * magnitude_squared / static_cast<double>(length);
      if (power <= 0.0 || fsc_power < kMinBurstPurity * power) continue;
      amplitudes.push_back(2.0 * std::sqrt(magnitude_squared) /
                           static_cast<double>(length));
    }
  }
  if (amplitudes.size() < kMinBurstLines) return std::nullopt;

  std::sort(amplitudes.begin(), amplitudes.end());
  const auto trim =
      static_cast<size_t>(kBurstTrim * static_cast<double>(amplitudes.size()));
  double sum = 0.0;
  for (size_t k = trim; k < amplitudes.size() - trim; ++k) sum += amplitudes[k];
  return sum / static_cast<double>(amplitudes.size() - 2 * trim);
}

std::optional<double> StackerStage::burst_reference(
    const std::vector<std::optional<double>>& amplitudes) {
  std::vector<double> measured;
  for (const auto& a : amplitudes) {
    if (a) measured.push_back(*a);
  }
  if (measured.empty()) return std::nullopt;
  return median_of(std::move(measured));
}

double StackerStage::chroma_gain(std::optional<double> amplitude,
                                 std::optional<double> reference) {
  if (!amplitude || !reference || *amplitude <= 0.0) return 1.0;
  const double gain = *reference / *amplitude;
  if (std::abs(gain - 1.0) <= kChromaGainTolerance) return 1.0;
  if (gain > kMaxChromaGain || gain < 1.0 / kMaxChromaGain) return 1.0;
  return gain;
}

void StackerStage::apply_chroma_gain(std::vector<sample_type>& plane,
                                     double gain) {
  if (gain == 1.0 || plane.size() < 5) return;
  // hp = [1, -4, 6, -4, 1] / 4: (6 - 8 cos ωT + 2 cos 2ωT) / 4 = (1 - cos ωT)².
  const double scale = (gain - 1.0) / 4.0;
  const std::vector<sample_type> in = plane;
  for (size_t n = 2; n + 2 < in.size(); ++n) {
    const int32_t hp =
        in[n - 2] - 4 * in[n - 1] + 6 * in[n] - 4 * in[n + 1] + in[n + 2];
    plane[n] = round_sample(in[n] + scale * hp);
  }
}

std::vector<std::optional<double>> StackerStage::row_burst_amplitudes(
    const StackRows& rows, bool yc, VideoSystem system, size_t nominal_width,
    size_t height) const {
  std::vector<std::optional<double>> amplitudes(rows.luma.size());
  if (!m_chroma_gain_match) return amplitudes;
  for (size_t i = 0; i < rows.luma.size(); ++i) {
    if (!rows.valid[i]) continue;
    amplitudes[i] =
        burst_amplitude(yc ? rows.chroma[i] : rows.luma[i], rows.dropouts[i],
                        system, nominal_width, height);
  }
  return amplitudes;
}

void StackerStage::match_chroma_gain(std::vector<sample_type>& plane,
                                     std::optional<double> amplitude,
                                     std::optional<double> reference,
                                     FrameID frame_id, size_t input) {
  const double gain = chroma_gain(amplitude, reference);
  if (gain == 1.0 || !amplitude || !reference) return;
  ORC_LOG_DEBUG(
      "StackerStage: frame {}: input {} burst is {:.3f} times the inputs' "
      "median; scaling its subcarrier band by {:.3f}",
      frame_id, input + 1, *amplitude / *reference, gain);
  apply_chroma_gain(plane, gain);
}

// Luma level matching.  A gain within this of 1 and a level within this
// many codes of 0 are left alone, so sources that already agree are
// stacked exactly as they were; a gain beyond the cap, or a level beyond its
// cap, is a different picture rather than a different player, and is left
// alone too.  Real captures differ by up to 7 % and 11 codes.
constexpr double kLumaGainTolerance = 0.002;
constexpr double kLumaLevelTolerance = 0.5;
constexpr double kMaxLumaGain = 1.15;
constexpr double kMaxLumaLevel = 40.0;

void StackerStage::match_luma_levels(StackRows& rows, VideoSystem system,
                                     size_t nominal_width, size_t height,
                                     FrameID frame_id) const {
  const size_t n = rows.luma.size();
  rows.levels.assign(n, std::nullopt);
  if (!m_luma_level_match) return;

  const size_t total = frame_line_sample_offset(system, nominal_width, height);
  std::vector<std::vector<uint8_t>> masks(n);
  std::vector<const std::vector<sample_type>*> planes(n, nullptr);
  std::vector<const std::vector<uint8_t>*> plane_masks(n, nullptr);
  for (size_t i = 0; i < n; ++i) {
    if (!rows.valid[i] || rows.luma[i].size() < total) continue;
    masks[i] = dropout_mask(rows.dropouts[i], total);
    planes[i] = &rows.luma[i];
    plane_masks[i] = &masks[i];
  }
  LumaLevelMeter meter(n);
  meter.add_frame(planes, plane_masks, system, nominal_width, height);
  const std::vector<std::optional<LevelMatch>> levels = meter.levels();

  for (size_t i = 0; i < n; ++i) {
    const std::optional<LevelMatch>& level = levels[i];
    if (!level || !planes[i]) continue;
    if (std::abs(level->gain - 1.0) < kLumaGainTolerance &&
        std::abs(level->level) < kLumaLevelTolerance) {
      continue;
    }
    if (level->gain > kMaxLumaGain || level->gain < 1.0 / kMaxLumaGain ||
        std::abs(level->level) > kMaxLumaLevel) {
      continue;
    }
    ORC_LOG_DEBUG(
        "StackerStage: frame {}: input {} luma gain {:+.1f}% and level "
        "{:+.1f} codes against the typical input's; brought to it",
        frame_id, i + 1, 100.0 * (level->gain - 1.0), level->level);
    apply_luma_level(rows.luma[i], *level);
    rows.levels[i] = level;
  }
}

// ============================================================================
// StackedVideoFrameRepresentation
// ============================================================================

StackedVideoFrameRepresentation::StackedVideoFrameRepresentation(
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    StackerStage* stage)
    : VideoFrameRepresentationWrapper(sources.empty() ? nullptr : sources[0]),
      Artifact(ArtifactID("stacked_frame"), Provenance{}),
      sources_(sources),
      stage_(stage),
      stacked_frames_(kMaxCachedFrames),
      stacked_luma_(kMaxCachedFrames),
      stacked_chroma_(kMaxCachedFrames),
      stacked_dropouts_(kMaxCachedFrames),
      stacked_audio_(kMaxCachedFrames),
      stacked_efm_(kMaxCachedFrames),
      best_source_cache_(kMaxCachedFrames),
      // Every frame within the search either side of the frames being
      // stacked at once and the lookahead, for every source.  A segment's probe
      // pairs are held only while it is measured.
      signatures_(
          std::max<size_t>(sources.size(), 1) *
          (2 * static_cast<size_t>(
                   stage ? std::max(stage->m_still_frame_search, 0) : 0) +
           2 + kSignatureLookahead + kConcurrentFrames)),
      // Every pair of frames within a search of each other, for the frames
      // of the search window either side, for every source.
      sample_checks_(
          std::max<size_t>(sources.size(), 1) *
          (2 * static_cast<size_t>(
                   stage ? std::max(stage->m_still_frame_search, 0) : 0) +
           2) *
          static_cast<size_t>(stage ? std::max(stage->m_still_frame_search, 1)
                                    : 1)),
      still_copies_(kMaxCachedFrames) {
  if (sources_.size() > kMaxSources) {
    throw std::runtime_error("StackerStage: at most 16 sources");
  }
  if (!sources_.empty()) {
    bool first_yc = sources_[0]->has_separate_channels();
    for (size_t i = 1; i < sources_.size(); ++i) {
      if (sources_[i]->has_separate_channels() != first_yc) {
        throw std::runtime_error(
            "StackerStage: all sources must have the same channel mode "
            "(all composite or all YC)");
      }
    }
  }
}

// Sources are aligned to the same frame ids but need not end together, so
// the stack covers the union of their ranges and every picture any input
// carries is kept.
FrameIDRange StackedVideoFrameRepresentation::frame_range() const {
  bool found = false;
  FrameIDRange range{};
  for (const auto& src : sources_) {
    if (!src || src->frame_count() == 0) continue;
    const FrameIDRange r = src->frame_range();
    if (!found) {
      range = r;
      found = true;
    } else {
      range.first = std::min(range.first, r.first);
      range.last = std::max(range.last, r.last);
    }
  }
  return range;
}

size_t StackedVideoFrameRepresentation::frame_count() const {
  for (const auto& src : sources_) {
    if (src && src->frame_count() > 0) {
      return static_cast<size_t>(frame_range().count());
    }
  }
  return 0;
}

bool StackedVideoFrameRepresentation::has_frame(FrameID id) const {
  for (const auto& src : sources_) {
    if (src && src->has_frame(id)) return true;
  }
  return false;
}

// Described by the first input with a real frame here, so a stacked frame is
// marked as padding only when every input is padding.
std::optional<FrameDescriptor>
StackedVideoFrameRepresentation::get_frame_descriptor(FrameID id) const {
  std::optional<FrameDescriptor> padding;
  for (const auto& src : sources_) {
    if (!src || !src->has_frame(id)) continue;
    auto desc = src->get_frame_descriptor(id);
    if (!desc) continue;
    if (!desc->is_padding_frame) {
      desc->frame_id = id;
      desc->held_frames_before = 0;
      desc->held_frames_after = 0;
      desc->held_checked = stage_ && stage_->m_still_frame_stacking;
      for (const StillCopy& copy : still_copies(id)) {
        if (!copy.field[0] || !copy.field[1]) continue;
        if (copy.id < id) {
          desc->held_frames_before = std::max(
              desc->held_frames_before, static_cast<uint32_t>(id - copy.id));
        } else {
          desc->held_frames_after = std::max(
              desc->held_frames_after, static_cast<uint32_t>(copy.id - id));
        }
      }
      return desc;
    }
    if (!padding) padding = desc;
  }
  if (padding) padding->frame_id = id;
  return padding;
}

std::vector<VideoFrameRepresentation::sample_type>
StackedVideoFrameRepresentation::padding_frame_copy(FrameID id) const {
  for (const auto& src : sources_) {
    if (src && src->has_frame(id)) return src->get_frame_copy(id);
  }
  return {};
}

bool StackedVideoFrameRepresentation::has_separate_channels() const {
  return sources_.empty() ? false : sources_[0]->has_separate_channels();
}

std::vector<FrameID> StackedVideoFrameRepresentation::collect_source_frame_ids(
    FrameID ref_id) const {
  if (sources_.empty()) return {};

  // Inputs arrive frame-aligned — frame_map normalises each source onto the
  // disc's frame numbering and source_align applies any residual offset, both
  // driven by VBI frame numbers — so the same frame id is read from every
  // source.
  //
  // This deliberately does NOT re-align on colour_frame_index.  That index is
  // derived from the burst phase measured against the source's own 4FSC
  // sample grid (colour_frame_phase_observer.cpp), and PAL's TBC layout does
  // not pin subcarrier phase to sample index the way NTSC's 910 samples/line
  // = 227.5 subcarrier cycles does.  Two decodes of the same disc whose line
  // starts land one sample apart — 90° of subcarrier — therefore report
  // colour frame indices two apart while carrying the very same picture.
  // Searching neighbouring frames for a matching index then substitutes a
  // frame 1-4 away, displacing that source's video and, through
  // stack_audio(), its audio with it.  Measured on three captures of one
  // Domesday side, that fired on 54% of frames for the one capture whose grid
  // was a sample out.  Grid differences are corrected by shifting samples
  // (see sample_offsets()), not by moving to a different frame.

  std::vector<FrameID> ids;
  ids.reserve(sources_.size());

  static constexpr FrameID kInvalid = UINT64_MAX;

  for (const auto& src : sources_) {
    if (!src || !src->has_frame(ref_id)) {
      ids.push_back(kInvalid);
      continue;
    }
    auto desc = src->get_frame_descriptor(ref_id);
    if (desc && desc->is_padding_frame) {
      ids.push_back(kInvalid);
      continue;
    }
    ids.push_back(ref_id);
  }

  return ids;
}

std::vector<int32_t> StackedVideoFrameRepresentation::sample_offsets(
    FrameID id) const {
  std::vector<int32_t> offsets(sources_.size(), 0);
  if (sources_.size() < 2) return offsets;
  if (stage_ && !stage_->m_sample_align) return offsets;

  const auto [segment, segments] = segment_of(id);
  if (segments == 0) return offsets;

  std::lock_guard<std::mutex> lk(align_mutex_);
  const auto& own = segment_offsets(segment);
  for (size_t i = 1; i < sources_.size(); ++i) {
    const std::optional<int32_t> mine = own[i];
    if (mine) {
      offsets[i] = *mine;
      continue;
    }
    // Hold the offset from the nearest segment that could be measured,
    // earlier first on a tie.
    for (size_t distance = 1; distance <= kMaxSegmentSearch; ++distance) {
      std::optional<int32_t> held;
      if (segment >= distance) held = segment_offsets(segment - distance)[i];
      if (!held && segment + distance < segments) {
        held = segment_offsets(segment + distance)[i];
      }
      if (held) {
        offsets[i] = *held;
        break;
      }
    }
  }
  return offsets;
}

const std::vector<std::optional<int32_t>>&
StackedVideoFrameRepresentation::segment_offsets(size_t segment) const {
  auto it = segment_offsets_.find(segment);
  if (it == segment_offsets_.end()) {
    it = segment_offsets_.emplace(segment, measure_segment(segment)).first;
  }
  return it->second;
}

std::vector<FrameID> StackedVideoFrameRepresentation::grid_probes(
    size_t segment, size_t source) const {
  std::vector<FrameID> probes;
  const auto& src = sources_[source];
  if (!src) return probes;

  const FrameIDRange range = frame_range();
  const FrameID segment_first =
      range.first + static_cast<FrameID>(segment * kSampleAlignSegmentFrames);
  const size_t segment_length = std::min<size_t>(
      kSampleAlignSegmentFrames, range.count() - (segment_first - range.first));

  // A spread of the segment rather than its head, which is often lead-in
  // carrying too little picture detail to correlate.
  for (size_t k = 1; k <= kAlignProbeCandidates; ++k) {
    const FrameID id =
        segment_first + static_cast<FrameID>((segment_length * k) /
                                             (kAlignProbeCandidates + 1));
    if (!src->has_frame(id)) continue;
    const auto desc = src->get_frame_descriptor(id);
    if (desc && !desc->is_padding_frame) probes.push_back(id);
  }
  return probes;
}

std::optional<int32_t> StackedVideoFrameRepresentation::linked_offset(
    size_t segment, size_t source) const {
  const size_t segments = segment_of(frame_range().first).second;
  for (size_t distance = 1; distance < segments; ++distance) {
    for (const bool earlier : {true, false}) {
      if (earlier ? segment < distance : segment + distance >= segments) {
        continue;
      }
      const size_t other = earlier ? segment - distance : segment + distance;
      // Only a segment measured against sources_[0] itself links the two,
      // and measuring it never looks for a stand-in in turn.
      if (grid_probes(other, 0).empty()) continue;
      // Measuring it here is not a return to sources_[0] for the log.
      const size_t logged = last_logged_stand_in_;
      const std::optional<int32_t> offset = segment_offsets(other)[source];
      last_logged_stand_in_ = logged;
      if (offset) return offset;
    }
  }
  return std::nullopt;
}

std::vector<std::optional<int32_t>>
StackedVideoFrameRepresentation::measure_segment(size_t segment) const {
  std::vector<std::optional<int32_t>> measured(sources_.size());
  measured[0] = 0;
  if (!source_) return measured;
  if (!grid_probes(segment, 0).empty()) {
    last_logged_stand_in_ = 0;
    return measure_segment_against(segment, 0, 0);
  }

  const FrameIDRange range = frame_range();
  const FrameID segment_first =
      range.first + static_cast<FrameID>(segment * kSampleAlignSegmentFrames);
  const size_t segment_length = std::min<size_t>(
      kSampleAlignSegmentFrames, range.count() - (segment_first - range.first));
  const FrameID segment_last =
      segment_first + static_cast<FrameID>(segment_length) - 1;

  // Input 1 has no frame here, past the end of a short capture or across a
  // stretch it is missing: measure against the first source that has one,
  // and carry that source's offset from input 1 from the nearest segment
  // where both were measured.  Offsets drift by a sample or so across a
  // side, so the nearest segment's is the one to take.
  bool others = false;
  for (size_t s = 1; s < sources_.size(); ++s) {
    if (!sources_[s] || sources_[s] == source_) continue;
    if (grid_probes(segment, s).empty()) continue;
    others = true;
    const std::optional<int32_t> link = linked_offset(segment, s);
    if (!link) continue;
    if (last_logged_stand_in_ != s || last_logged_stand_in_offset_ != *link) {
      last_logged_stand_in_ = s;
      last_logged_stand_in_offset_ = *link;
      ORC_LOG_INFO(
          "StackerStage: frames {}-{}: source 0 has no frame to measure the "
          "sample grids against, so source {} stands in for it, {:+d} "
          "sample(s) from source 0 in the nearest segment that measured both",
          segment_first, segment_last, s, *link);
      log_report("reference", segment_first, segment_last,
                 fmt::format(",\"input\":{},\"offset\":{}", s + 1, *link));
    }
    return measure_segment_against(segment, s, *link);
  }

  if (others && last_logged_stand_in_ != SIZE_MAX) {
    last_logged_stand_in_ = SIZE_MAX;
    ORC_LOG_WARN(
        "StackerStage: frames {}-{}: source 0 has no frame here and no other "
        "source has been measured against it nearby, so these frames are "
        "stacked without correcting the sources' sample grids",
        segment_first, segment_last);
    log_report("reference", segment_first, segment_last, ",\"input\":null");
  }
  return measured;
}

std::vector<std::optional<int32_t>>
StackedVideoFrameRepresentation::measure_segment_against(size_t segment,
                                                         size_t reference,
                                                         int32_t shift) const {
  std::vector<std::optional<int32_t>> measured(sources_.size());
  measured[0] = 0;
  measured[reference] = shift;
  const auto& reference_source = sources_[reference];

  const auto params = reference_source->get_video_parameters();
  if (!params) return measured;

  const VideoSystem system = params->system;
  const auto nominal_width = static_cast<size_t>(params->frame_width_nominal);
  const auto height = static_cast<size_t>(params->frame_height);
  if (nominal_width == 0 || height == 0) return measured;

  const size_t frame_samples =
      frame_line_sample_offset(system, nominal_width, height);

  const FrameIDRange range = frame_range();
  const FrameID segment_first =
      range.first + static_cast<FrameID>(segment * kSampleAlignSegmentFrames);
  const size_t segment_length = std::min<size_t>(
      kSampleAlignSegmentFrames, range.count() - (segment_first - range.first));

  const bool yc = reference_source->has_separate_channels();

  auto usable = [](const VideoFrameRepresentation& src, FrameID id) {
    if (!src.has_frame(id)) return false;
    const auto desc = src.get_frame_descriptor(id);
    return desc.has_value() && !desc->is_padding_frame;
  };

  // Luma carries the picture detail the correlation needs, and for a YC
  // source it is the only plane that both channels' grids share.
  auto read = [&](const VideoFrameRepresentation& src, FrameID id) {
    std::vector<sample_type> out;
    if (yc) {
      const sample_type* plane = src.get_frame_luma(id);
      if (plane) out.assign(plane, plane + frame_samples);
    } else {
      out = src.get_frame_copy(id);
    }
    return out;
  };

  // A source that IS the reference shares its grid by construction.
  auto is_reference = [&](size_t i) {
    return i == reference || sources_[i] == reference_source;
  };

  std::vector<SampleGridMeter> meters(sources_.size());

  for (const FrameID id : grid_probes(segment, reference)) {
    std::vector<sample_type> reference_frame;
    for (size_t i = 1; i < sources_.size(); ++i) {
      const auto& src = sources_[i];
      if (!src || is_reference(i) || meters[i].frames() >= kAlignProbeFrames) {
        continue;
      }
      if (!usable(*src, id)) continue;

      if (reference_frame.empty()) {
        reference_frame = read(*reference_source, id);
      }
      const auto other_frame = read(*src, id);
      if (reference_frame.size() < frame_samples ||
          other_frame.size() < frame_samples) {
        continue;
      }

      meters[i].add_frame(reference_frame, other_frame, system, nominal_width,
                          height);
    }
  }

  const FrameID segment_last =
      segment_first + static_cast<FrameID>(segment_length) - 1;

  for (size_t i = 1; i < sources_.size(); ++i) {
    if (!sources_[i]) continue;
    if (is_reference(i)) {
      measured[i] = shift;
      continue;
    }
    if (meters[i].frames() == 0) {
      ORC_LOG_DEBUG(
          "StackerStage: frames {}-{}: source {} has no frame in common with "
          "source {} to measure its sample grid against",
          segment_first, segment_last, i, reference);
      continue;
    }

    const SampleGridMeter::Result grid = meters[i].measure();
    if (grid.flat) {
      ORC_LOG_DEBUG(
          "StackerStage: frames {}-{}: source {} carries no picture detail to "
          "measure its sample grid against source {}",
          segment_first, segment_last, i, reference);
      continue;
    }
    if (!grid.offset) {
      ORC_LOG_DEBUG(
          "StackerStage: frames {}-{}: source {} correlates with source {} at "
          "only {:.2f} over {} frame(s); cannot measure its sample grid",
          segment_first, segment_last, i, reference, grid.correlation,
          meters[i].frames());
      continue;
    }

    const int32_t offset = *grid.offset + shift;
    measured[i] = offset;
    if (last_logged_offsets_.size() != sources_.size()) {
      last_logged_offsets_.assign(sources_.size(), 0);
    }
    if (offset != last_logged_offsets_[i]) {
      last_logged_offsets_[i] = offset;
      const double fractional =
          grid.fractional_offset.value_or(*grid.offset) + shift;
      ORC_LOG_DEBUG(
          "StackerStage: frames {}-{}: source {} sample grid is {:+d} "
          "sample(s) from the reference ({:+.2f} to a fraction; correlation "
          "{:.3f}, measured against source {}); shifting it onto the "
          "reference grid before stacking",
          segment_first, segment_last, i, offset, fractional, grid.correlation,
          reference);
      log_report("grid", segment_first, segment_last,
                 fmt::format(",\"input\":{},\"offset\":{},\"fractional_"
                             "offset\":{:.3f},\"correlation\":{:.3f},"
                             "\"reference\":{}",
                             i + 1, offset, fractional, grid.correlation,
                             reference + 1));
    }
  }

  return measured;
}

std::vector<StackedVideoFrameRepresentation::SourceNoise>
StackedVideoFrameRepresentation::weighting_noise(FrameID id) const {
  if (!stage_ || stage_->m_mode != StackerStage::StackingMode::WEIGHTED) {
    return {};
  }
  return noise_model(id);
}

std::pair<size_t, size_t> StackedVideoFrameRepresentation::segment_of(
    FrameID id) const {
  const FrameIDRange range = frame_range();
  const size_t total = range.count();
  if (total == 0) return {0, 0};
  const size_t segments =
      (total + kSampleAlignSegmentFrames - 1) / kSampleAlignSegmentFrames;
  const size_t position = id > range.first ? id - range.first : 0;
  return {std::min(position / kSampleAlignSegmentFrames, segments - 1),
          segments};
}

std::vector<StackedVideoFrameRepresentation::SourceNoise>
StackedVideoFrameRepresentation::noise_model(FrameID id) const {
  const size_t n = sources_.size();
  std::vector<SourceNoise> model(n);
  for (size_t i = 0; i < n; ++i) {
    model[i].sigma = kFallbackNoiseSigma;
    model[i].weight = 1.0;
    model[i].group = i;
    model[i].family = i;
    model[i].own_variance = kFallbackNoiseSigma * kFallbackNoiseSigma;
  }
  if (n < 2) return model;

  const auto [segment, segments] = segment_of(id);
  if (segments == 0) return model;

  std::lock_guard<std::mutex> lk(noise_mutex_);
  std::vector<std::optional<SourceNoise>> resolved = segment_noise(segment);
  for (size_t i = 0; i < n; ++i) {
    // Hold the noise from the nearest segment that could be measured,
    // earlier first on a tie.
    for (size_t distance = 1; !resolved[i] && distance <= kMaxSegmentSearch;
         ++distance) {
      if (segment >= distance) {
        resolved[i] = segment_noise(segment - distance)[i];
      }
      if (!resolved[i] && segment + distance < segments) {
        resolved[i] = segment_noise(segment + distance)[i];
      }
    }
  }

  std::vector<double> sigmas;
  std::vector<double> weights;
  for (const auto& noise : resolved) {
    if (!noise) continue;
    sigmas.push_back(noise->sigma);
    weights.push_back(noise->weight);
  }
  if (sigmas.empty()) return model;

  // A source never measured nearby is given the typical figures, in a group
  // and family of its own, with the own noise that gives it the typical
  // weight.
  const double typical_sigma = median_of(sigmas);
  const double typical_weight = median_of(weights);
  for (size_t i = 0; i < n; ++i) {
    const std::optional<SourceNoise> noise = resolved[i];
    if (noise) {
      model[i] = *noise;
    } else {
      model[i].sigma = typical_sigma;
      model[i].weight = typical_weight;
      model[i].group = n + i;
      model[i].family = n + i;
      model[i].own_variance = 1.0 / typical_weight;
    }
  }
  return model;
}

const std::vector<std::optional<StackedVideoFrameRepresentation::SourceNoise>>&
StackedVideoFrameRepresentation::segment_noise(size_t segment) const {
  auto it = segment_noise_.find(segment);
  if (it == segment_noise_.end()) {
    it = segment_noise_.emplace(segment, measure_segment_noise(segment)).first;
  }
  return it->second;
}

bool StackedVideoFrameRepresentation::add_noise_probe(PairNoiseMeter& meter,
                                                      FrameID id) const {
  const size_t n = sources_.size();
  if (!source_) return false;

  const auto params = source_->get_video_parameters();
  if (!params) return false;

  const VideoSystem system = params->system;
  const auto nominal_width = static_cast<size_t>(params->frame_width_nominal);
  const auto height = static_cast<size_t>(params->frame_height);
  if (nominal_width == 0 || height == 0) return false;

  const size_t frame_samples =
      frame_line_sample_offset(system, nominal_width, height);

  // The difference of two inputs is noise only once their grids agree.
  const std::vector<int32_t> offsets = sample_offsets(id);
  const bool yc = source_->has_separate_channels();
  std::vector<std::vector<sample_type>> planes(n);
  std::vector<std::vector<uint8_t>> masks(n);
  std::vector<const std::vector<sample_type>*> frame_planes(n, nullptr);
  std::vector<const std::vector<uint8_t>*> frame_masks(n, nullptr);
  std::vector<std::optional<double>> amplitudes(n);
  const bool match_chroma = !yc && stage_ && stage_->m_chroma_gain_match;
  size_t valid = 0;
  for (size_t i = 0; i < n; ++i) {
    const auto& src = sources_[i];
    if (!src || !src->has_frame(id)) continue;
    const auto desc = src->get_frame_descriptor(id);
    if (!desc || desc->is_padding_frame) continue;

    // Luma for a YC source: the plane the grid was measured on.
    if (yc) {
      const sample_type* plane = src->get_frame_luma(id);
      if (plane) planes[i].assign(plane, plane + frame_samples);
    } else {
      planes[i] = src->get_frame_copy(id);
    }
    if (planes[i].size() < frame_samples) continue;

    auto runs = src->get_dropout_hints(id);
    shift_frame_samples(planes[i], system, nominal_width, height, offsets[i]);
    shift_dropout_runs(runs, system, nominal_width, height, offsets[i]);
    if (match_chroma) {
      amplitudes[i] = StackerStage::burst_amplitude(planes[i], runs, system,
                                                    nominal_width, height);
    }
    masks[i] = dropout_mask(runs, frame_samples);
    frame_planes[i] = &planes[i];
    frame_masks[i] = &masks[i];
    ++valid;
  }
  if (valid < 2) return false;

  // Chroma that the stack will match to the inputs' burst is matched here
  // too, or the mismatch is measured as the input's noise.
  const std::optional<double> burst = StackerStage::burst_reference(amplitudes);
  for (size_t i = 0; i < n; ++i) {
    if (!frame_planes[i]) continue;
    StackerStage::match_chroma_gain(planes[i], amplitudes[i], burst, id, i);
  }
  meter.add_frame(frame_planes, frame_masks, system, nominal_width, height);
  return true;
}

const StackedVideoFrameRepresentation::Pressings&
StackedVideoFrameRepresentation::pressings() const {
  if (!pressings_) pressings_ = measure_pressings();
  return *pressings_;
}

StackedVideoFrameRepresentation::Pressings
StackedVideoFrameRepresentation::measure_pressings() const {
  const size_t n = sources_.size();
  if (stage_ && !stage_->m_group_pressings) {
    ORC_LOG_INFO(
        "StackerStage: pressing grouping is off; every input is taken as one "
        "pressing");
    const std::vector<std::optional<size_t>> one(n, size_t{0});
    return {one, one};
  }

  const FrameIDRange range = frame_range();
  const size_t count = range.count();
  PairNoiseMeter meter(n);
  size_t frames_used = 0;
  // Grouped once, from frames spread across the whole stack, and each
  // segment's noise is solved with these groups: grouped per segment, the
  // groups changed from segment to segment, and a split pressing gave one
  // input nearly all the weight.
  for (size_t c = 0;
       c < kPressingProbeCandidates && frames_used < kPressingProbeFrames;
       ++c) {
    const FrameID id =
        range.first + static_cast<FrameID>(pressing_probe_candidate(c, count));
    if (add_noise_probe(meter, id)) ++frames_used;
  }

  const std::vector<std::optional<double>> pair_variance =
      meter.pair_variances();
  Pressings found;
  found.groups = group_by_pressing(n, pair_variance);
  found.families = group_into_families(n, pair_variance, found.groups);
  ORC_LOG_DEBUG(
      "StackerStage: pair variances over {} frame(s) across the stack: {}",
      frames_used, describe_pair_variances(n, pair_variance));

  size_t groups = 0;
  size_t families = 0;
  for (size_t i = 0; i < n; ++i) {
    if (const std::optional<size_t>& g = found.groups[i]) {
      groups = std::max(groups, *g + 1);
    }
    if (const std::optional<size_t>& f = found.families[i]) {
      families = std::max(families, *f + 1);
    }
  }
  if (groups > 1) {
    ORC_LOG_INFO(
        "StackerStage: inputs grouped by pressing over {} frame(s) across the "
        "stack:{}",
        frames_used, describe_pressings(found.groups));
  } else if (groups == 1) {
    ORC_LOG_INFO(
        "StackerStage: inputs are of one pressing over {} frame(s) across the "
        "stack",
        frames_used);
  }
  if (families < groups) {
    ORC_LOG_INFO(
        "StackerStage: pressings grouped into stamper families, whose "
        "pressings share their mother's noise:{}",
        describe_pressings(found.families));
  }
  return found;
}

std::vector<std::optional<StackedVideoFrameRepresentation::SourceNoise>>
StackedVideoFrameRepresentation::measure_segment_noise(size_t segment) const {
  const size_t n = sources_.size();
  std::vector<std::optional<SourceNoise>> measured(n);
  if (!source_) return measured;

  const FrameIDRange range = frame_range();
  const FrameID segment_first =
      range.first + static_cast<FrameID>(segment * kSampleAlignSegmentFrames);
  const size_t segment_length = std::min<size_t>(
      kSampleAlignSegmentFrames, range.count() - (segment_first - range.first));
  const FrameID segment_last =
      segment_first + static_cast<FrameID>(segment_length) - 1;

  PairNoiseMeter meter(n);
  size_t frames_used = 0;

  // Probe the same spread of the segment as the grid measurement.
  for (size_t k = 1;
       k <= kAlignProbeCandidates && frames_used < kAlignProbeFrames; ++k) {
    const FrameID id =
        segment_first + static_cast<FrameID>((segment_length * k) /
                                             (kAlignProbeCandidates + 1));
    if (add_noise_probe(meter, id)) ++frames_used;
  }

  const std::vector<std::optional<double>> pair_variance =
      meter.pair_variances();
  ORC_LOG_DEBUG("StackerStage: frames {}-{}: pair variances {}{}",
                segment_first, segment_last,
                describe_pair_variances(n, pair_variance),
                describe_whole_picture_pairs(meter.whole_picture_pairs()));
  ORC_LOG_DEBUG("StackerStage: frames {}-{}: input luma gain and level {}",
                segment_first, segment_last, describe_levels(meter.levels()));
  const Pressings& found = pressings();
  measured = fit_pressing_noise(n, pair_variance, found.groups, found.families);
  if (std::none_of(measured.begin(), measured.end(),
                   [](const auto& noise) { return noise.has_value(); })) {
    ORC_LOG_DEBUG(
        "StackerStage: frames {}-{}: too little in common between the inputs "
        "to measure their noise",
        segment_first, segment_last);
    return measured;
  }

  // A model that does not explain the pair variances it was fitted to cannot
  // be trusted to weight the inputs: stack them equally instead.
  const PressingFitCheck check = check_pressing_fit(n, pair_variance, measured);
  if (!check.trusted) measured = equal_weight_noise(n, pair_variance, measured);

  bool changed = last_logged_noise_.size() != n;
  for (size_t i = 0; i < n && !changed; ++i) {
    const std::optional<SourceNoise> noise = measured[i];
    if (noise && std::abs(noise->sigma - last_logged_noise_[i]) >
                     kNoiseLogChange * last_logged_noise_[i]) {
      changed = true;
    }
  }

  // σ and share of the weight per input, numbered from 1 as in the other
  // messages.  The pressings are logged once, when they are found.
  double weight_total = 0.0;
  for (const auto& noise : measured) {
    if (noise) weight_total += noise->weight;
  }
  std::string summary;
  for (size_t i = 0; i < n; ++i) {
    const std::optional<SourceNoise> noise = measured[i];
    if (!summary.empty()) summary += ", ";
    summary += noise ? fmt::format("{:.1f} ({:.0f}%)", noise->sigma,
                                   100.0 * noise->weight / weight_total)
                     : std::string("-");
  }
  const std::string prediction = describe_fit_check(check);
  ORC_LOG_DEBUG(
      "StackerStage: frames {}-{}: input noise σ in codes (share of the "
      "weight) {}; {}",
      segment_first, segment_last, summary, prediction);
  if (changed || check.trusted != last_fit_trusted_) {
    last_logged_noise_.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
      const std::optional<SourceNoise> noise = measured[i];
      last_logged_noise_[i] = noise ? noise->sigma : 0.0;
    }
    last_fit_trusted_ = check.trusted;
    log_report("weights", segment_first, segment_last,
               describe_weights_record(measured, check));
    if (!check.trusted) {
      ORC_LOG_WARN(
          "StackerStage: frames {}-{}: the noise model does not explain the "
          "inputs (pair {}-{} misfits by {:.0f}%, {:.0f}% rms over {} "
          "pair(s)), so they are stacked with equal weights",
          segment_first, segment_last, check.worst_i + 1, check.worst_j + 1,
          100.0 * (std::exp(check.worst_misfit) - 1.0),
          100.0 * (std::exp(check.rms_misfit) - 1.0), check.pairs);
    }
  }

  return measured;
}

// ── Still-frame copies ───────────────────────────────────────────────────────

std::vector<VideoFrameRepresentation::sample_type>
StackedVideoFrameRepresentation::picture_plane(size_t source,
                                               FrameID id) const {
  const auto& src = sources_[source];
  if (!src || !src->has_frame(id)) return {};
  const auto desc = src->get_frame_descriptor(id);
  if (!desc || desc->is_padding_frame) return {};
  const auto geometry = geometry_of(*src);
  if (!geometry) return {};

  std::vector<sample_type> plane;
  if (src->has_separate_channels()) {
    const sample_type* luma = src->get_frame_luma(id);
    if (luma) plane.assign(luma, luma + geometry->samples);
  } else {
    plane = src->get_frame_copy(id);
  }
  if (plane.size() < geometry->samples) return {};
  return plane;
}

std::optional<StackedVideoFrameRepresentation::SignatureInput>
StackedVideoFrameRepresentation::signature_input(size_t source,
                                                 FrameID id) const {
  SignatureInput input;
  input.plane = picture_plane(source, id);
  if (input.plane.empty()) return std::nullopt;
  const auto& src = sources_[source];
  const auto geometry = geometry_of(*src);
  if (!geometry) return std::nullopt;
  input.system = geometry->system;
  input.width = geometry->width;
  input.height = geometry->height;
  input.dropouts = src->get_dropout_hints(id);
  input.yc = src->has_separate_channels();
  if (input.yc) {
    const sample_type* p = src->get_frame_chroma(id);
    if (p) input.chroma.assign(p, p + geometry->samples);
  }
  return input;
}

std::shared_ptr<const StackedVideoFrameRepresentation::FrameSignature>
StackedVideoFrameRepresentation::build_signature(const SignatureInput& input) {
  auto signature = std::make_shared<FrameSignature>();
  signature->fields = build_field_signatures(
      input.plane, input.system, input.width, input.height, input.dropouts,
      standard_picture_levels(input.system));
  // A YC source's chroma plane, or the composite itself.  A YC frame whose
  // chroma could not be read has no chroma signature, so its phase reads as
  // unknown.
  if (!input.yc) {
    signature->chroma = build_chroma_signature(
        input.plane, input.system, input.width, input.height, input.dropouts);
  } else if (input.chroma.size() == input.plane.size()) {
    signature->chroma = build_chroma_signature(
        input.chroma, input.system, input.width, input.height, input.dropouts);
  }
  signature->hash = frame_sample_hash(input.plane);
  return signature;
}

size_t StackedVideoFrameRepresentation::still_worker_threads() const {
  const auto configured =
      static_cast<size_t>(std::max(stage_->m_thread_count, 0));
  if (configured > 0) return configured;
  return std::max(std::thread::hardware_concurrency(), 1U);
}

StackedVideoFrameRepresentation::SignatureMap
StackedVideoFrameRepresentation::prefetch_signatures(
    const std::vector<std::pair<size_t, FrameID>>& wanted, bool keep) const {
  SignatureMap found;
  std::vector<std::pair<size_t, FrameID>> missing;
  // Kept signatures another thread is building already; read from the cache
  // once it has finished.
  std::vector<FrameID> awaited;
  for (const auto& [source, id] : wanted) {
    const FrameID key = signature_key(source, id);
    if (found.count(key)) continue;
    if (auto cached = signatures_.get(key)) {
      found.emplace(key, *cached);
    } else if (keep && !signatures_building_.try_claim(key)) {
      found.emplace(key, nullptr);
      awaited.push_back(key);
    } else if (auto built = keep ? signatures_.get(key) : std::nullopt) {
      // Built and released by another thread between the miss and the claim.
      signatures_building_.release(key);
      found.emplace(key, *built);
    } else {
      found.emplace(key, nullptr);
      missing.emplace_back(source, id);
    }
  }

  // Release every claim however this returns, so no waiter is left waiting.
  size_t released = 0;
  auto release_claims = [&](size_t upto) {
    if (keep) {
      for (; released < upto; ++released) {
        signatures_building_.release(
            signature_key(missing[released].first, missing[released].second));
      }
    }
  };
  struct ReleaseRest {
    std::function<void()> release;
    ~ReleaseRest() { release(); }
  } release_rest{[&] { release_claims(missing.size()); }};

  const size_t n_threads = still_worker_threads();

  // A chunk of one frame a thread at a time, so that only that many frames'
  // samples are held at once.
  for (size_t start = 0; start < missing.size(); start += n_threads) {
    const size_t end = std::min(start + n_threads, missing.size());
    std::vector<std::optional<SignatureInput>> inputs;
    for (size_t i = start; i < end; ++i) {
      inputs.push_back(signature_input(missing[i].first, missing[i].second));
    }
    std::vector<std::shared_ptr<const FrameSignature>> built(inputs.size());
    run_parallel(inputs.size(), n_threads, [&](size_t i) {
      if (inputs[i]) built[i] = build_signature(*inputs[i]);
    });

    for (size_t i = start; i < end; ++i) {
      const FrameID key = signature_key(missing[i].first, missing[i].second);
      if (keep) signatures_.put(key, built[i - start]);
      found[key] = built[i - start];
    }
    release_claims(end);
  }

  // Every claim this call made is released before it waits on another's, so
  // two calls can never wait on each other.
  for (const FrameID key : awaited) {
    signatures_building_.wait(key);
    if (auto cached = signatures_.get(key)) {
      found[key] = *cached;
    } else {
      // Evicted already, or its build failed: build it here.
      const size_t source = static_cast<size_t>(key % kMaxSources);
      const FrameID id = key / kMaxSources;
      const auto input = signature_input(source, id);
      found[key] = input ? build_signature(*input) : nullptr;
    }
  }
  return found;
}

std::optional<double> StackedVideoFrameRepresentation::still_reference_noise(
    size_t source, FrameID id) const {
  if (source >= sources_.size()) return std::nullopt;
  const auto [segment, segments] = segment_of(id);
  if (segments == 0) return std::nullopt;

  std::lock_guard<std::mutex> lk(still_noise_mutex_);
  std::optional<double> sigma = segment_still_noise(segment)[source];
  // Hold the noise from the nearest segment that could be measured, earlier
  // first on a tie.
  for (size_t distance = 1; !sigma && distance <= kMaxSegmentSearch;
       ++distance) {
    if (segment >= distance) {
      sigma = segment_still_noise(segment - distance)[source];
    }
    if (!sigma && segment + distance < segments) {
      sigma = segment_still_noise(segment + distance)[source];
    }
  }
  return sigma;
}

const std::vector<std::optional<double>>&
StackedVideoFrameRepresentation::segment_still_noise(size_t segment) const {
  auto it = segment_still_noise_.find(segment);
  if (it == segment_still_noise_.end()) {
    it = segment_still_noise_
             .emplace(segment, measure_segment_still_noise(segment))
             .first;
  }
  return it->second;
}

std::vector<std::optional<double>>
StackedVideoFrameRepresentation::measure_segment_still_noise(
    size_t segment) const {
  const size_t n = sources_.size();
  std::vector<std::optional<double>> measured(n);
  if (!source_) return measured;
  const auto geometry = geometry_of(*source_);
  if (!geometry) return measured;
  const auto cadence =
      static_cast<FrameID>(colour_frame_cadence(geometry->system));

  const FrameIDRange range = frame_range();
  const FrameID segment_first =
      range.first + static_cast<FrameID>(segment * kSampleAlignSegmentFrames);
  const size_t segment_length = std::min<size_t>(
      kSampleAlignSegmentFrames, range.count() - (segment_first - range.first));
  const FrameID segment_last =
      segment_first + static_cast<FrameID>(segment_length) - 1;

  for (size_t s = 0; s < n; ++s) {
    if (!sources_[s]) continue;
    // The probes are used once, so they are not cached, where they would
    // push out the search window.
    std::vector<std::pair<size_t, FrameID>> probes;
    for (size_t k = 1; k <= kStillNoiseProbes; ++k) {
      const FrameID first =
          segment_first +
          static_cast<FrameID>((segment_length * k) / (kStillNoiseProbes + 1));
      if (first + cadence > range.last) continue;
      probes.emplace_back(s, first);
      probes.emplace_back(s, first + cadence);
    }
    const SignatureMap probe_signatures =
        prefetch_signatures(probes, /*keep=*/false);

    std::vector<double> anchors;
    std::vector<std::pair<std::shared_ptr<const FrameSignature>,
                          std::shared_ptr<const FrameSignature>>>
        pairs;
    for (size_t k = 1; k <= kStillNoiseProbes; ++k) {
      const FrameID first =
          segment_first +
          static_cast<FrameID>((segment_length * k) / (kStillNoiseProbes + 1));
      const FrameID second = first + cadence;
      if (second > range.last) continue;
      auto a = probe_signatures.at(signature_key(s, first));
      auto b = probe_signatures.at(signature_key(s, second));
      if (!a || !b) continue;
      pairs.emplace_back(std::move(a), std::move(b));

      const std::vector<sample_type> plane = picture_plane(s, first);
      if (plane.empty()) continue;
      const std::optional<double> anchor = within_frame_noise(
          plane, geometry->system, geometry->width, geometry->height,
          sources_[s]->get_dropout_hints(first));
      if (anchor) anchors.push_back(*anchor);
    }
    if (anchors.empty()) continue;

    double sigma = median_of(anchors);
    for (size_t pass = 0; pass < kStillNoisePasses; ++pass) {
      BlockNoiseMeter meter;
      size_t matched = 0;
      for (const auto& [a, b] : pairs) {
        for (size_t field = 0; field < 2; ++field) {
          if (same_picture(a->fields[field], b->fields[field], sigma).match !=
              PictureMatch::Same) {
            continue;
          }
          meter.add(a->fields[field], b->fields[field]);
          ++matched;
        }
      }
      const std::optional<double> found = meter.sigma();
      if (matched < kMinStillNoisePairs || !found) break;
      sigma = *found;
      measured[s] = sigma;
    }
  }

  bool changed = last_logged_still_noise_.size() != n;
  for (size_t s = 0; s < n && !changed; ++s) {
    const std::optional<double> sigma = measured[s];
    if (sigma && std::abs(*sigma - last_logged_still_noise_[s]) >
                     kNoiseLogChange * last_logged_still_noise_[s]) {
      changed = true;
    }
  }
  std::string summary;
  for (size_t s = 0; s < n; ++s) {
    const std::optional<double> sigma = measured[s];
    if (!summary.empty()) summary += ", ";
    summary += sigma ? fmt::format("{:.1f}", *sigma) : std::string("-");
  }
  if (changed &&
      std::any_of(measured.begin(), measured.end(),
                  [](const auto& sigma) { return sigma.has_value(); })) {
    last_logged_still_noise_.assign(n, 0.0);
    for (size_t s = 0; s < n; ++s) {
      last_logged_still_noise_[s] = measured[s].value_or(0.0);
    }
    ORC_LOG_INFO(
        "StackerStage: frames {}-{}: still-frame reference noise σ in codes "
        "per input {}",
        segment_first, segment_last, summary);
  } else {
    ORC_LOG_DEBUG(
        "StackerStage: frames {}-{}: still-frame reference noise σ in codes "
        "per input {}",
        segment_first, segment_last, summary);
  }
  return measured;
}

std::vector<StackedVideoFrameRepresentation::StillCopy>
StackedVideoFrameRepresentation::still_copies(FrameID id) const {
  if (!stage_ || !stage_->m_still_frame_stacking) return {};
  if (auto cached = still_copies_.get(id)) return *cached;

  std::vector<StillCopy> copies = find_still_copies(id);
  if (!copies.empty()) {
    std::string list;
    for (const StillCopy& copy : copies) {
      if (!list.empty()) list += ", ";
      list += fmt::format(
          "input {} frame {}{}", copy.source + 1, copy.id,
          copy.field[0] && copy.field[1]
              ? ""
              : fmt::format(" (field {} only)", copy.field[0] ? 1 : 2));
    }
    ORC_LOG_DEBUG("StackerStage: frame {}: {} still-frame copies: {}", id,
                  copies.size(), list);
  }
  still_copies_.put(id, copies);
  return copies;
}

std::vector<StackedVideoFrameRepresentation::StillCopy>
StackedVideoFrameRepresentation::find_still_copies(FrameID id) const {
  std::vector<StillCopy> copies;
  if (!source_) return copies;
  const auto geometry = geometry_of(*source_);
  if (!geometry) return copies;

  // Only frames at the colour cadence carry the same subcarrier phase, and
  // only those can be added into the composite.  It also keeps slow motion
  // out: it moves further over a cadence than over one frame.
  const auto cadence =
      static_cast<FrameID>(colour_frame_cadence(geometry->system));
  const auto search = static_cast<FrameID>(stage_->m_still_frame_search);
  const FrameIDRange range = frame_range();

  // Build the window's signatures together.  Every frame of the window is
  // asked for, not only those at the cadence, so the LRU keeps the frames
  // later frames will need ahead of those behind the window.  When the frame
  // furthest ahead is missing, frames are being read in order, so the next
  // kSignatureLookahead frames are built with it.
  std::vector<std::pair<size_t, FrameID>> wanted;
  const FrameID ahead = std::min(id + search, range.last);
  bool reading_on = false;
  for (size_t s = 0; s < sources_.size(); ++s) {
    if (!sources_[s]) continue;
    for (FrameID other = id >= range.first + search ? id - search : range.first;
         other <= ahead; ++other) {
      wanted.emplace_back(s, other);
    }
    reading_on = reading_on || !signatures_.contains(signature_key(s, ahead));
  }
  if (reading_on) {
    for (size_t s = 0; s < sources_.size(); ++s) {
      if (!sources_[s]) continue;
      for (FrameID other = ahead + 1;
           other <= std::min(ahead + kSignatureLookahead, range.last);
           ++other) {
        wanted.emplace_back(s, other);
      }
    }
  }
  const SignatureMap window = prefetch_signatures(wanted);
  auto signature_of = [&window](size_t s, FrameID frame) {
    const auto it = window.find(signature_key(s, frame));
    return it == window.end() ? nullptr : it->second;
  };

  for (size_t s = 0; s < sources_.size(); ++s) {
    const auto own = signature_of(s, id);
    if (!own) continue;
    const std::optional<double> sigma = still_reference_noise(s, id);
    if (!sigma) continue;

    std::vector<StillCopy> found;
    for (FrameID step = cadence; step <= search; step += cadence) {
      for (const bool before : {true, false}) {
        if (before ? id < range.first + step : id + step > range.last) {
          continue;
        }
        const FrameID other = before ? id - step : id + step;
        const auto signature = signature_of(s, other);
        if (!signature) continue;

        // The same physical read, repeated by Frame Map, carries the same
        // noise and dropouts and is not an independent copy.
        if (signature->hash == own->hash &&
            is_duplicate_read(picture_plane(s, id), picture_plane(s, other))) {
          continue;
        }

        // Unknown is a frame without chroma, where the cadence alone keeps
        // the phase.
        const ColourPhaseRelation phase =
            colour_phase_relation(own->chroma, signature->chroma).relation;
        if (phase != ColourPhaseRelation::Same &&
            phase != ColourPhaseRelation::Unknown) {
          continue;
        }

        StillCopy copy;
        copy.source = s;
        copy.id = other;
        for (size_t field = 0; field < 2; ++field) {
          copy.field[field] =
              same_picture(own->fields[field], signature->fields[field], *sigma)
                  .match == PictureMatch::Same;
        }
        if (copy.field[0] || copy.field[1]) found.push_back(copy);
      }
    }
    check_still_copy_samples(s, id, found);

    for (size_t field = 0; field < 2; ++field) {
      const auto count = static_cast<size_t>(
          std::count_if(found.begin(), found.end(),
                        [field](const auto& c) { return c.field[field]; }));
      if (count >= kMinStillCopies) continue;
      for (auto& copy : found) copy.field[field] = false;
    }
    for (const StillCopy& copy : found) {
      if (copy.field[0] || copy.field[1]) copies.push_back(copy);
    }
  }
  return copies;
}

void StackedVideoFrameRepresentation::check_still_copy_samples(
    size_t source, FrameID id, std::vector<StillCopy>& copies) const {
  if (copies.empty()) return;
  const auto geometry = geometry_of(*sources_[source]);
  if (!geometry) return;

  // The check gives the same answer either way round, so a pair is checked
  // once, when the first of its frames is stacked.
  auto pair_key = [source](FrameID a, FrameID b) {
    const FrameID first = std::min(a, b);
    return signature_key(source, first) * kMaxPairOffsets +
           (std::max(a, b) - first);
  };
  std::vector<std::array<int8_t, 2>> known(copies.size(), {-1, -1});
  std::vector<std::pair<size_t, size_t>> checks;  // (copy, field)
  for (size_t c = 0; c < copies.size(); ++c) {
    if (auto cached = sample_checks_.get(pair_key(id, copies[c].id))) {
      known[c] = *cached;
    }
    for (size_t field = 0; field < 2; ++field) {
      if (copies[c].field[field] && known[c][field] < 0) {
        checks.emplace_back(c, field);
      }
    }
  }

  if (!checks.empty()) {
    struct Plane {
      std::vector<sample_type> samples;
      std::vector<DropoutRun> dropouts;
    };
    auto fetch = [&](FrameID frame) {
      return Plane{picture_plane(source, frame),
                   sources_[source]->get_dropout_hints(frame)};
    };
    const Plane own = fetch(id);
    std::map<size_t, Plane> planes;
    for (const auto& [c, field] : checks) {
      if (!planes.count(c)) planes.emplace(c, fetch(copies[c].id));
    }

    std::vector<int8_t> same(checks.size(), 0);
    run_parallel(checks.size(), still_worker_threads(), [&](size_t i) {
      const auto [c, field] = checks[i];
      const Plane& other = planes.at(c);
      if (own.samples.empty() || other.samples.size() != own.samples.size()) {
        return;
      }
      const SampleComparison result = compare_field_samples(
          own.samples, own.dropouts, other.samples, other.dropouts, field,
          geometry->system, geometry->width, geometry->height);
      same[i] = result.match == PictureMatch::Same ? 1 : 0;
    });
    for (size_t i = 0; i < checks.size(); ++i) {
      known[checks[i].first][checks[i].second] = same[i];
    }
    for (const auto& [c, plane] : planes) {
      sample_checks_.put(pair_key(id, copies[c].id), known[c]);
    }
  }

  for (size_t c = 0; c < copies.size(); ++c) {
    for (size_t field = 0; field < 2; ++field) {
      copies[c].field[field] = copies[c].field[field] && known[c][field] == 1;
    }
  }
}

FrameID StackedVideoFrameRepresentation::resolve_source_frame(
    size_t src_idx, FrameID ref_id) const {
  auto ids = collect_source_frame_ids(ref_id);
  if (src_idx >= ids.size()) {
    return UINT64_MAX;
  }
  return ids[src_idx];
}

size_t StackedVideoFrameRepresentation::get_source_count(FrameID ref_id) const {
  size_t count = 0;
  for (const auto& src : sources_) {
    if (!src || !src->has_frame(ref_id)) {
      continue;
    }
    auto desc = src->get_frame_descriptor(ref_id);
    if (desc && !desc->is_padding_frame) {
      ++count;
    }
  }
  return count;
}

size_t StackedVideoFrameRepresentation::get_best_source_index(
    FrameID id) const {
  std::lock_guard<std::mutex> lk(cache_mutex_);
  if (const auto* p = best_source_cache_.get_ptr(id)) {
    return *p;
  }

  auto src_ids = collect_source_frame_ids(id);
  size_t best = 0;
  size_t min_dropouts = SIZE_MAX;

  for (size_t i = 0; i < sources_.size() && i < src_ids.size(); ++i) {
    if (src_ids[i] == UINT64_MAX) {
      continue;
    }
    if (!sources_[i]) {
      continue;
    }
    auto runs = sources_[i]->get_dropout_hints(src_ids[i]);
    size_t total = 0;
    for (const auto& r : runs) {
      total += r.sample_count;
    }
    if (total < min_dropouts) {
      min_dropouts = total;
      best = i;
    }
  }

  best_source_cache_.put(id, best);
  return best;
}

void StackedVideoFrameRepresentation::stack_composite(
    FrameID id, std::vector<sample_type>& samples,
    std::vector<DropoutRun>& dropouts) const {
  ORC_LOG_DEBUG("StackedVideoFrameRepresentation: stacking frame {}", id);

  auto src_ids = collect_source_frame_ids(id);
  FrameStackUse use;
  stage_->stack_frame(src_ids, sources_, sample_offsets(id),
                      weighting_noise(id), still_copies(id), samples, dropouts,
                      &use);
  if (samples.empty()) {
    // Every input is padding here.
    samples = padding_frame_copy(id);
  } else {
    log_frame_report(id, use, dropouts);
  }

  // stack_frame builds runs without frame context; stamp this frame's ID so
  // downstream consumers (e.g. dropout_map) see consistent hints.
  for (auto& run : dropouts) {
    run.frame_id = id;
  }
}

void StackedVideoFrameRepresentation::stack_yc(
    FrameID id, std::vector<sample_type>& luma,
    std::vector<sample_type>& chroma, std::vector<DropoutRun>& dropouts) const {
  ORC_LOG_DEBUG("StackedVideoFrameRepresentation: stacking YC frame {}", id);

  auto src_ids = collect_source_frame_ids(id);
  FrameStackUse use;
  stage_->stack_frame_yc(src_ids, sources_, sample_offsets(id),
                         weighting_noise(id), still_copies(id), luma, chroma,
                         dropouts, &use);
  if (!luma.empty()) log_frame_report(id, use, dropouts);

  // stack_frame_yc builds runs without frame context; stamp this frame's ID
  // so downstream consumers (e.g. dropout_map) see consistent hints.
  for (auto& run : dropouts) {
    run.frame_id = id;
  }
}

namespace {

// Releases a claimed build however the scope is left.
class BuildClaim {
 public:
  BuildClaim(BuildsInProgress& builds, FrameID key)
      : builds_(builds), key_(key) {}
  ~BuildClaim() { builds_.release(key_); }
  BuildClaim(const BuildClaim&) = delete;
  BuildClaim& operator=(const BuildClaim&) = delete;

 private:
  BuildsInProgress& builds_;
  FrameID key_;
};

}  // namespace

const std::vector<VideoFrameRepresentation::sample_type>*
StackedVideoFrameRepresentation::stacked_frame(FrameID id) const {
  // Stacked outside the cache lock, so readers on other threads stack other
  // frames meanwhile. A reader wanting a frame another thread is stacking
  // waits for it and reads it from the cache.
  auto cached = [&]() -> const std::vector<sample_type>* {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    return stacked_dropouts_.contains(id) ? stacked_frames_.get_ptr(id)
                                          : nullptr;
  };
  for (;;) {
    if (const auto* p = cached()) return p;
    if (frames_building_.try_claim(id)) break;
    frames_building_.wait(id);
  }
  const BuildClaim claim(frames_building_, id);
  // Another thread may have stacked the frame and released its claim
  // between the cache miss and the claim.
  if (const auto* p = cached()) return p;

  std::vector<sample_type> samples;
  std::vector<DropoutRun> dropouts;
  stack_composite(id, samples, dropouts);

  // put_if_absent: replacing an entry would free a buffer a caller may
  // already hold a pointer into.
  std::lock_guard<std::mutex> lk(cache_mutex_);
  stacked_frames_.put_if_absent(id, std::move(samples));
  stacked_dropouts_.put_if_absent(id, std::move(dropouts));
  return stacked_frames_.get_ptr(id);
}

std::pair<const std::vector<VideoFrameRepresentation::sample_type>*,
          const std::vector<VideoFrameRepresentation::sample_type>*>
StackedVideoFrameRepresentation::stacked_frame_yc(FrameID id) const {
  // As stacked_frame().
  using Planes = std::pair<const std::vector<sample_type>*,
                           const std::vector<sample_type>*>;
  auto cached = [&]() -> Planes {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    if (!stacked_dropouts_.contains(id)) return {nullptr, nullptr};
    const auto* luma = stacked_luma_.get_ptr(id);
    const auto* chroma = stacked_chroma_.get_ptr(id);
    if (!luma || !chroma) return {nullptr, nullptr};
    return {luma, chroma};
  };
  for (;;) {
    if (const auto planes = cached(); planes.first) return planes;
    if (yc_frames_building_.try_claim(id)) break;
    yc_frames_building_.wait(id);
  }
  const BuildClaim claim(yc_frames_building_, id);
  if (const auto planes = cached(); planes.first) return planes;

  std::vector<sample_type> luma, chroma;
  std::vector<DropoutRun> dropouts;
  stack_yc(id, luma, chroma, dropouts);

  std::lock_guard<std::mutex> lk(cache_mutex_);
  stacked_luma_.put_if_absent(id, std::move(luma));
  stacked_chroma_.put_if_absent(id, std::move(chroma));
  stacked_dropouts_.put_if_absent(id, std::move(dropouts));
  return {stacked_luma_.get_ptr(id), stacked_chroma_.get_ptr(id)};
}

// ── Flat access ──────────────────────────────────────────────────────────────

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_frame(FrameID id) const {
  const auto* p = stacked_frame(id);
  return (p && !p->empty()) ? p->data() : nullptr;
}

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_line(FrameID id, size_t line) const {
  const sample_type* frame = get_frame(id);
  if (!frame) return nullptr;
  auto params = get_video_parameters();
  if (!params) return nullptr;
  if (line >= static_cast<size_t>(params->frame_height)) return nullptr;
  return frame + frame_line_sample_offset(
                     params->system,
                     static_cast<size_t>(params->frame_width_nominal), line);
}

std::vector<VideoFrameRepresentation::sample_type>
StackedVideoFrameRepresentation::get_frame_copy(FrameID id) const {
  const auto* p = stacked_frame(id);
  return p ? *p : std::vector<sample_type>{};
}

// ── YC access ────────────────────────────────────────────────────────────────

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_frame_luma(FrameID id) const {
  if (!has_separate_channels()) {
    return nullptr;
  }
  const auto* p = stacked_frame_yc(id).first;
  return (p && !p->empty()) ? p->data() : nullptr;
}

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_frame_chroma(FrameID id) const {
  if (!has_separate_channels()) {
    return nullptr;
  }
  const auto* p = stacked_frame_yc(id).second;
  return (p && !p->empty()) ? p->data() : nullptr;
}

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_line_luma(FrameID id, size_t line) const {
  const sample_type* frame = get_frame_luma(id);
  if (!frame) {
    return nullptr;
  }
  auto desc = get_frame_descriptor(id);
  if (!desc || line >= desc->height) {
    return nullptr;
  }
  return frame + frame_line_sample_offset(desc->system,
                                          desc->samples_per_line_nominal, line);
}

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_line_chroma(FrameID id,
                                                 size_t line) const {
  const sample_type* frame = get_frame_chroma(id);
  if (!frame) {
    return nullptr;
  }
  auto desc = get_frame_descriptor(id);
  if (!desc || line >= desc->height) {
    return nullptr;
  }
  return frame + frame_line_sample_offset(desc->system,
                                          desc->samples_per_line_nominal, line);
}

// ── Dropout hints
// ─────────────────────────────────────────────────────────────

std::vector<DropoutRun> StackedVideoFrameRepresentation::get_dropout_hints(
    FrameID id) const {
  {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    if (stacked_dropouts_.contains(id)) {
      const auto* p = stacked_dropouts_.get_ptr(id);
      if (p) {
        return *p;
      }
    }
  }

  (void)stacked_frame(id);  // populates the dropout cache as a side-effect

  std::lock_guard<std::mutex> lk(cache_mutex_);
  const auto* p = stacked_dropouts_.get_ptr(id);
  return p ? *p : std::vector<DropoutRun>{};
}

// ── Audio
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<const VideoFrameRepresentation>
StackedVideoFrameRepresentation::reference_audio_source() const {
  for (const auto& src : sources_) {
    if (src && src->has_audio()) {
      return src;
    }
  }
  return nullptr;
}

bool StackedVideoFrameRepresentation::pair_in_all_sources(size_t pair) const {
  for (const auto& src : sources_) {
    if (!src || pair >= src->audio_channel_pair_count()) return false;
  }
  return true;
}

size_t StackedVideoFrameRepresentation::audio_channel_pair_count() const {
  const auto ref = reference_audio_source();
  return ref ? ref->audio_channel_pair_count() : 0;
}

std::optional<AudioChannelPairDescriptor>
StackedVideoFrameRepresentation::get_audio_channel_pair_descriptor(
    size_t pair) const {
  const auto ref = reference_audio_source();
  return ref ? ref->get_audio_channel_pair_descriptor(pair) : std::nullopt;
}

std::vector<int32_t> StackedVideoFrameRepresentation::get_audio_samples(
    size_t pair, FrameID id) const {
  // Still-frame copies never add to audio, so one source passes through.
  if (sources_.size() == 1) {
    return sources_[0] ? sources_[0]->get_audio_samples(pair, id)
                       : std::vector<int32_t>{};
  }

  const auto ref = reference_audio_source();
  if (!ref || pair >= ref->audio_channel_pair_count()) {
    return {};
  }

  if (!pair_in_all_sources(pair)) {
    // Pairs not common to all inputs pass through from the per-frame best
    // source (falling back to the first source that carries the frame).
    const auto src_ids = collect_source_frame_ids(id);
    const size_t best = get_best_source_index(id);
    if (best < sources_.size() && src_ids[best] != UINT64_MAX &&
        sources_[best] && pair < sources_[best]->audio_channel_pair_count()) {
      return sources_[best]->get_audio_samples(pair, src_ids[best]);
    }
    for (size_t i = 0; i < sources_.size(); ++i) {
      if (sources_[i] && src_ids[i] != UINT64_MAX &&
          pair < sources_[i]->audio_channel_pair_count()) {
        return sources_[i]->get_audio_samples(pair, src_ids[i]);
      }
    }
    return {};
  }

  const FrameID key = audio_cache_key(id, pair);
  {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    if (const auto* p = stacked_audio_.get_ptr(key)) {
      return *p;
    }
  }

  size_t best = get_best_source_index(id);
  auto src_ids = collect_source_frame_ids(id);
  auto result = stage_->stack_audio(pair, src_ids, sources_, best);

  std::lock_guard<std::mutex> lk(cache_mutex_);
  if (!stacked_audio_.contains(key)) {
    stacked_audio_.put(key, result);
  }
  return result;
}

// ── EFM ──────────────────────────────────────────────────────────────────────

bool StackedVideoFrameRepresentation::has_efm() const {
  for (const auto& src : sources_) {
    if (src && src->has_efm()) {
      return true;
    }
  }
  return false;
}

// An estimate, not a promise. Stacking preserves the combined stream's length
// in channel bits, not its t-value count: a transition the sources disagree
// about is one t-value in the capture that saw it and none in the capture that
// did not. Consumers that need the exact count must read the samples. This is
// used only to size progress reporting, so the first contributing source's
// count is close enough - and unlike asking which source is best, it costs
// nothing per frame.
uint32_t StackedVideoFrameRepresentation::get_efm_sample_count(
    FrameID id) const {
  for (const auto& src : sources_) {
    if (src && src->has_efm() && src->has_frame(id)) {
      return src->get_efm_sample_count(id);
    }
  }
  return 0;
}

std::vector<uint8_t> StackedVideoFrameRepresentation::get_efm_samples(
    FrameID id) const {
  if (!has_efm()) {
    return {};
  }

  // Still-frame copies never add to EFM, so one source passes through.
  if (sources_.size() == 1) return sources_[0]->get_efm_samples(id);

  {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    if (const auto* p = stacked_efm_.get_ptr(id)) {
      return *p;
    }
  }

  size_t best = get_best_source_index(id);
  auto src_ids = collect_source_frame_ids(id);
  auto result = stage_->stack_efm(src_ids, sources_, best);

  std::lock_guard<std::mutex> lk(cache_mutex_);
  if (!stacked_efm_.contains(id)) {
    stacked_efm_.put(id, result);
  }
  return result;
}

// ── AC3 RF ───────────────────────────────────────────────────────────────────

bool StackedVideoFrameRepresentation::has_ac3_rf() const {
  return std::any_of(sources_.begin(), sources_.end(),
                     [](const auto& src) { return src && src->has_ac3_rf(); });
}

size_t StackedVideoFrameRepresentation::ac3_source_index(FrameID id) const {
  const auto src_ids = collect_source_frame_ids(id);
  auto carries = [&](size_t i) {
    return i < sources_.size() && sources_[i] && src_ids[i] != UINT64_MAX &&
           sources_[i]->has_ac3_rf();
  };
  const size_t best = get_best_source_index(id);
  if (carries(best)) return best;
  for (size_t i = 0; i < sources_.size(); ++i) {
    if (carries(i)) return i;
  }
  return SIZE_MAX;
}

uint32_t StackedVideoFrameRepresentation::get_ac3_symbol_count(
    FrameID id) const {
  const size_t i = ac3_source_index(id);
  return i == SIZE_MAX ? 0 : sources_[i]->get_ac3_symbol_count(id);
}

std::vector<uint8_t> StackedVideoFrameRepresentation::get_ac3_symbols(
    FrameID id) const {
  const size_t i = ac3_source_index(id);
  return i == SIZE_MAX ? std::vector<uint8_t>{}
                       : sources_[i]->get_ac3_symbols(id);
}

// ============================================================================
// StackerStage
// ============================================================================

StackerStage::StackerStage() = default;

std::vector<ArtifactPtr> StackerStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& /*observation_context*/) {
  if (inputs.empty()) {
    throw DAGExecutionError("StackerStage requires at least 1 input");
  }
  if (inputs.size() > 16) {
    throw DAGExecutionError("StackerStage supports maximum 16 inputs");
  }

  std::vector<std::shared_ptr<const VideoFrameRepresentation>> sources;
  for (const auto& inp : inputs) {
    auto vfr = std::dynamic_pointer_cast<const VideoFrameRepresentation>(inp);
    if (!vfr) {
      throw DAGExecutionError(
          "StackerStage: input is not a VideoFrameRepresentation");
    }
    sources.push_back(vfr);
  }

  // Serialize access to cached_output_ and cached_sources_: two concurrent
  // DAG render threads share the same stage instance, so the read-check-update
  // of the cache must be atomic to avoid heap corruption via shared_ptr races.
  std::shared_ptr<const VideoFrameRepresentation> result;
  {
    std::lock_guard<std::mutex> lk(execute_mutex_);

    // Only invalidate the cached stacked representation when the parameters
    // actually change. The DAG passes the node's stored parameters on every
    // execute(); resetting unconditionally discards the wrapper — and its warm
    // per-frame stack cache — on each call, forcing every analysis-sink sweep
    // to re-stack the whole source from scratch.
    if (!parameters.empty() && parameters != parameters_) {
      set_parameters(parameters);
      cached_output_.reset();
    }

    bool reuse = false;
    if (cached_output_ && cached_sources_.size() == sources.size()) {
      reuse = true;
      for (size_t i = 0; i < sources.size(); ++i) {
        if (cached_sources_[i] != sources[i]) {
          reuse = false;
          break;
        }
      }
    }

    if (reuse) {
      result = cached_output_;
    } else {
      result = process(sources);
      cached_output_ = result;
      cached_sources_ = sources;
    }
  }

  // The stacked path returns a StackedVideoFrameRepresentation, which
  // inherits from both VideoFrameRepresentationWrapper and Artifact.  When
  // process() returns the single input unchanged (passthrough), the input is
  // already an Artifact.
  auto stacked = std::const_pointer_cast<StackedVideoFrameRepresentation>(
      std::dynamic_pointer_cast<const StackedVideoFrameRepresentation>(result));
  if (!stacked) {
    return {inputs[0]};
  }
  return {stacked};
}

std::shared_ptr<const VideoFrameRepresentation> StackerStage::process(
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources)
    const {
  if (sources.empty()) {
    return nullptr;
  }
  // A single source has nothing to stack with unless its own repeats are.
  if (sources.size() == 1 && !m_still_frame_stacking) {
    return sources[0];
  }

  return std::make_shared<StackedVideoFrameRepresentation>(
      sources, const_cast<StackerStage*>(this));
}

// ── Core stacking
// ─────────────────────────────────────────────────────────────

StackerStage::NoiseWeights StackerStage::noise_weights(
    const std::vector<SourceNoise>& model, const std::vector<bool>& valid,
    const std::vector<std::array<bool, 2>>& field_valid) const {
  const size_t num_sources = valid.size();
  NoiseWeights noise;
  noise.weight[0].assign(num_sources, 1.0);
  noise.weight[1].assign(num_sources, 1.0);
  double typical = kFallbackNoiseSigma;
  if (model.size() == num_sources && num_sources > 0) {
    noise.model = model;
    std::vector<double> sigmas(num_sources);
    for (size_t i = 0; i < num_sources; ++i) sigmas[i] = model[i].sigma;
    typical = median_of(sigmas);

    std::array<std::vector<double>, 2> weights;
    double largest = 0.0;
    for (size_t field = 0; field < 2; ++field) {
      std::vector<SourceNoise> used;
      for (size_t i = 0; i < num_sources; ++i) {
        if (valid[i] && field_valid[i][field]) used.push_back(model[i]);
      }
      weights[field] = pressing_weights(used);
      for (double w : weights[field]) largest = std::max(largest, w);
    }
    if (largest > 0.0) noise.scale = 1.0 / largest;
    for (size_t field = 0; field < 2; ++field) {
      for (size_t i = 0, k = 0; i < num_sources; ++i) {
        if (valid[i] && field_valid[i][field]) {
          noise.weight[field][i] = noise.scale * weights[field][k++];
        }
      }
    }
  }
  noise.reject_distance = m_reject_sigma * kRejectScale * typical;
  return noise;
}

void StackerStage::stack_frame(
    const std::vector<FrameID>& source_ids,
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    const std::vector<int32_t>& sample_offsets,
    const std::vector<SourceNoise>& noise_model,
    const std::vector<StillCopy>& copies,
    std::vector<sample_type>& output_samples,
    std::vector<DropoutRun>& output_dropouts, FrameStackUse* use) const {
  if (use) *use = {};
  if (source_ids.size() != sources.size()) {
    return;
  }

  std::optional<FrameDescriptor> ref_desc;
  size_t ref_src = 0;
  for (size_t i = 0; i < source_ids.size(); ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    ref_desc = sources[i]->get_frame_descriptor(source_ids[i]);
    if (ref_desc) {
      ref_src = i;
      break;
    }
  }
  if (!ref_desc) {
    return;
  }

  size_t height = ref_desc->height;
  size_t nominal_width = ref_desc->samples_per_line_nominal;

  auto params = sources[ref_src]->get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::PAL;
  int32_t black_level = params ? params->black_level : 282;

  // Total sample count respects non-orthogonal PAL layout.
  const size_t total = frame_line_sample_offset(system, nominal_width, height);
  output_samples.resize(total, static_cast<sample_type>(black_level));
  output_dropouts.clear();

  const size_t num_sources = sources.size();
  StackRows rows;
  rows.luma.resize(num_sources);
  rows.valid.assign(num_sources, false);
  rows.dropouts.resize(num_sources);

  for (size_t i = 0; i < num_sources; ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    if (!sources[i]->has_frame(source_ids[i])) {
      continue;
    }
    auto d = sources[i]->get_frame_descriptor(source_ids[i]);
    if (!d || d->is_padding_frame) {
      continue;
    }
    rows.luma[i] = sources[i]->get_frame_copy(source_ids[i]);
    if (!rows.luma[i].empty()) {
      rows.valid[i] = true;
      rows.dropouts[i] = sources[i]->get_dropout_hints(source_ids[i]);

      // Bring this source onto the reference's sample grid before its samples
      // are combined with the others'.
      const int32_t offset = i < sample_offsets.size() ? sample_offsets[i] : 0;
      shift_frame_samples(rows.luma[i], system, nominal_width, height, offset);
      shift_dropout_runs(rows.dropouts[i], system, nominal_width, height,
                         offset);
    }
  }

  // A single source with nothing to stack it with passes through.
  if (num_sources == 1 && copies.empty() && rows.valid[0]) {
    output_samples = rows.luma[0];
    output_dropouts = rows.dropouts[0];
    if (use) use->inputs = {1, 1};
    return;
  }

  // Bring every input's luma to the typical input's gain and level, and then
  // its subcarrier band, which that scaled too, to the inputs' median burst.
  match_luma_levels(rows, system, nominal_width, height,
                    frame_id_of(source_ids));
  const std::vector<std::optional<double>> amplitudes =
      row_burst_amplitudes(rows, false, system, nominal_width, height);
  const std::optional<double> burst = burst_reference(amplitudes);
  for (size_t i = 0; i < num_sources; ++i) {
    if (!rows.valid[i]) continue;
    match_chroma_gain(rows.luma[i], amplitudes[i], burst,
                      frame_id_of(source_ids), i);
  }

  // Copies are vetted per field by the matcher, so only the sources are
  // compared with each other.
  const FieldAgreement agreement =
      check_field_agreement(rows.luma, rows.valid, rows.dropouts, system,
                            nominal_width, height, frame_id_of(source_ids));
  rows.field_valid = agreement.field_valid;

  NoiseWeights noise = noise_weights(noise_model, rows.valid, rows.field_valid);
  append_copy_rows(copies, sources, sample_offsets, system, nominal_width,
                   height, false, burst, rows, noise);
  const std::vector<uint8_t> takes_copies =
      copy_lines(rows.field_valid, num_sources, system, height);
  count_frame_use(rows.valid, rows.field_valid, num_sources, use);

  std::vector<std::vector<uint8_t>> masks(rows.luma.size());
  for (size_t i = 0; i < rows.luma.size(); ++i) {
    if (rows.valid[i]) masks[i] = dropout_mask(rows.dropouts[i], total);
  }

  size_t n_threads = static_cast<size_t>(m_thread_count);
  if (n_threads == 0) {
    n_threads = std::thread::hardware_concurrency();
    if (n_threads == 0) {
      n_threads = 4;
    }
  }
  if (n_threads == 1 || height < n_threads * 4) {
    n_threads = 1;
  }

  size_t total_do = 0;
  size_t total_stacked = 0;

  if (n_threads == 1) {
    process_lines_range(0, height, nominal_width, system, rows.luma, rows.valid,
                        rows.field_valid, masks, takes_copies, noise,
                        num_sources, black_level, output_samples,
                        output_dropouts, total_do, total_stacked);
  } else {
    std::vector<std::thread> threads;
    std::vector<std::vector<DropoutRun>> thread_dos(n_threads);
    std::vector<size_t> thread_do(n_threads, 0);
    std::vector<size_t> thread_st(n_threads, 0);
    size_t lpt = (height + n_threads - 1) / n_threads;

    for (size_t t = 0; t < n_threads; ++t) {
      size_t s = t * lpt;
      size_t e = std::min(s + lpt, height);
      if (s >= height) {
        break;
      }
      threads.emplace_back([&, t, s, e]() {
        process_lines_range(s, e, nominal_width, system, rows.luma, rows.valid,
                            rows.field_valid, masks, takes_copies, noise,
                            num_sources, black_level, output_samples,
                            thread_dos[t], thread_do[t], thread_st[t]);
      });
    }
    for (auto& th : threads) {
      th.join();
    }
    for (size_t t = 0; t < n_threads; ++t) {
      output_dropouts.insert(output_dropouts.end(), thread_dos[t].begin(),
                             thread_dos[t].end());
      total_do += thread_do[t];
      total_stacked += thread_st[t];
    }
  }

  if (m_pressing_repair && m_mode == StackingMode::WEIGHTED &&
      noise.model.size() == num_sources) {
    std::atomic<size_t> repaired{0};
    const FrameID frame_id = frame_id_of(source_ids);
    run_parallel(height, n_threads, [&](size_t y) {
      repaired += repair_pressing_defects(
          y, nominal_width, system, rows.luma, rows.valid, rows.field_valid,
          masks, noise, num_sources, frame_id, output_samples);
    });
    if (use) use->repaired_samples = repaired;
  }

  mark_disagreed_fields(agreement, system, nominal_width, height,
                        output_dropouts);

  if (num_sources == 1 && rows.valid[0]) {
    restore_uncopied_lines(takes_copies, rows.luma[0], rows.dropouts[0], system,
                           nominal_width, height, output_samples,
                           output_dropouts);
  }

  ORC_LOG_DEBUG("StackerStage::stack_frame: {} dropouts, {} stacked pixels",
                output_dropouts.size(), total_stacked);
  (void)total_do;
}

void StackerStage::stack_frame_yc(
    const std::vector<FrameID>& source_ids,
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    const std::vector<int32_t>& sample_offsets,
    const std::vector<SourceNoise>& noise_model,
    const std::vector<StillCopy>& copies, std::vector<sample_type>& output_luma,
    std::vector<sample_type>& output_chroma,
    std::vector<DropoutRun>& output_dropouts, FrameStackUse* use) const {
  if (use) *use = {};
  if (source_ids.size() != sources.size()) {
    return;
  }

  std::optional<FrameDescriptor> ref_desc;
  size_t ref_src = 0;
  for (size_t i = 0; i < source_ids.size(); ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    ref_desc = sources[i]->get_frame_descriptor(source_ids[i]);
    if (ref_desc) {
      ref_src = i;
      break;
    }
  }
  if (!ref_desc) {
    return;
  }

  size_t height = ref_desc->height;
  size_t nominal_width = ref_desc->samples_per_line_nominal;

  auto params = sources[ref_src]->get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::PAL;
  int32_t black_level = params ? params->black_level : 282;

  const size_t total = frame_line_sample_offset(system, nominal_width, height);
  output_luma.resize(total, static_cast<sample_type>(black_level));
  output_chroma.resize(total, static_cast<sample_type>(black_level));
  output_dropouts.clear();

  const size_t num_sources = sources.size();
  StackRows rows;
  rows.luma.resize(num_sources);
  rows.chroma.resize(num_sources);
  rows.valid.assign(num_sources, false);
  rows.dropouts.resize(num_sources);

  for (size_t i = 0; i < num_sources; ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    if (!sources[i]->has_frame(source_ids[i])) {
      continue;
    }
    auto d = sources[i]->get_frame_descriptor(source_ids[i]);
    if (!d || d->is_padding_frame) {
      continue;
    }
    const sample_type* lp = sources[i]->get_frame_luma(source_ids[i]);
    const sample_type* cp = sources[i]->get_frame_chroma(source_ids[i]);
    if (!lp || !cp) {
      continue;
    }
    rows.luma[i].assign(lp, lp + total);
    rows.chroma[i].assign(cp, cp + total);
    rows.valid[i] = true;
    rows.dropouts[i] = sources[i]->get_dropout_hints(source_ids[i]);

    // Bring this source onto the reference's sample grid before its samples
    // are combined with the others'.
    const int32_t offset = i < sample_offsets.size() ? sample_offsets[i] : 0;
    shift_frame_samples(rows.luma[i], system, nominal_width, height, offset);
    shift_frame_samples(rows.chroma[i], system, nominal_width, height, offset);
    shift_dropout_runs(rows.dropouts[i], system, nominal_width, height, offset);
  }

  // A single source with nothing to stack it with passes through.
  if (num_sources == 1 && copies.empty() && rows.valid[0]) {
    output_luma = rows.luma[0];
    output_chroma = rows.chroma[0];
    output_dropouts = rows.dropouts[0];
    if (use) use->inputs = {1, 1};
    return;
  }

  // Bring every input's luma to the typical input's gain and level, and its
  // chroma to the inputs' median burst, which a YC source carries on its
  // chroma plane.
  match_luma_levels(rows, system, nominal_width, height,
                    frame_id_of(source_ids));
  const std::vector<std::optional<double>> amplitudes =
      row_burst_amplitudes(rows, true, system, nominal_width, height);
  const std::optional<double> burst = burst_reference(amplitudes);
  for (size_t i = 0; i < num_sources; ++i) {
    if (!rows.valid[i]) continue;
    match_chroma_gain(rows.chroma[i], amplitudes[i], burst,
                      frame_id_of(source_ids), i);
  }

  // Luma carries the picture the fields are compared on.  Copies are vetted
  // per field by the matcher, so only the sources are compared.
  const FieldAgreement agreement =
      check_field_agreement(rows.luma, rows.valid, rows.dropouts, system,
                            nominal_width, height, frame_id_of(source_ids));
  rows.field_valid = agreement.field_valid;

  NoiseWeights noise = noise_weights(noise_model, rows.valid, rows.field_valid);
  append_copy_rows(copies, sources, sample_offsets, system, nominal_width,
                   height, true, burst, rows, noise);
  const std::vector<uint8_t> takes_copies =
      copy_lines(rows.field_valid, num_sources, system, height);
  count_frame_use(rows.valid, rows.field_valid, num_sources, use);

  std::vector<std::vector<uint8_t>> masks(rows.luma.size());
  for (size_t i = 0; i < rows.luma.size(); ++i) {
    if (rows.valid[i]) masks[i] = dropout_mask(rows.dropouts[i], total);
  }

  size_t n_threads = static_cast<size_t>(m_thread_count);
  if (n_threads == 0) {
    n_threads = std::thread::hardware_concurrency();
    if (n_threads == 0) {
      n_threads = 4;
    }
  }
  if (n_threads == 1 || height < n_threads * 4) {
    n_threads = 1;
  }

  size_t total_do = 0;
  size_t total_stacked = 0;

  if (n_threads == 1) {
    process_lines_range_yc(
        0, height, nominal_width, system, rows.luma, rows.chroma, rows.valid,
        rows.field_valid, masks, takes_copies, noise, num_sources, black_level,
        output_luma, output_chroma, output_dropouts, total_do, total_stacked);
  } else {
    std::vector<std::thread> threads;
    std::vector<std::vector<DropoutRun>> thread_dos(n_threads);
    std::vector<size_t> thread_do(n_threads, 0);
    std::vector<size_t> thread_st(n_threads, 0);
    size_t lpt = (height + n_threads - 1) / n_threads;

    for (size_t t = 0; t < n_threads; ++t) {
      size_t s = t * lpt;
      size_t e = std::min(s + lpt, height);
      if (s >= height) {
        break;
      }
      threads.emplace_back([&, t, s, e]() {
        process_lines_range_yc(s, e, nominal_width, system, rows.luma,
                               rows.chroma, rows.valid, rows.field_valid, masks,
                               takes_copies, noise, num_sources, black_level,
                               output_luma, output_chroma, thread_dos[t],
                               thread_do[t], thread_st[t]);
      });
    }
    for (auto& th : threads) {
      th.join();
    }
    for (size_t t = 0; t < n_threads; ++t) {
      output_dropouts.insert(output_dropouts.end(), thread_dos[t].begin(),
                             thread_dos[t].end());
    }
  }
  mark_disagreed_fields(agreement, system, nominal_width, height,
                        output_dropouts);

  if (num_sources == 1 && rows.valid[0]) {
    restore_uncopied_lines(takes_copies, rows.luma[0], rows.dropouts[0], system,
                           nominal_width, height, output_luma, output_dropouts);
    std::vector<DropoutRun> unused;
    restore_uncopied_lines(takes_copies, rows.chroma[0], {}, system,
                           nominal_width, height, output_chroma, unused);
  }
  (void)total_do;
  (void)total_stacked;
}

void StackerStage::append_copy_rows(
    const std::vector<StillCopy>& copies,
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    const std::vector<int32_t>& sample_offsets, VideoSystem system,
    size_t nominal_width, size_t height, bool yc,
    std::optional<double> burst_reference, StackRows& rows,
    NoiseWeights& noise) const {
  const size_t total = frame_line_sample_offset(system, nominal_width, height);
  const size_t first_copy_row = rows.luma.size();
  std::vector<const StillCopy*> appended;
  for (const StillCopy& copy : copies) {
    const size_t s = copy.source;
    if (s >= sources.size() || !sources[s] || !rows.valid[s]) continue;
    // A field of the source's frame that carries a different picture from
    // the other sources' carries it in its copies too.
    const std::array<bool, 2> fields = {
        copy.field[0] && rows.field_valid[s][0],
        copy.field[1] && rows.field_valid[s][1]};
    if (!fields[0] && !fields[1]) continue;

    std::vector<sample_type> luma;
    std::vector<sample_type> chroma;
    if (yc) {
      const sample_type* lp = sources[s]->get_frame_luma(copy.id);
      const sample_type* cp = sources[s]->get_frame_chroma(copy.id);
      if (!lp || !cp) continue;
      luma.assign(lp, lp + total);
      chroma.assign(cp, cp + total);
    } else {
      luma = sources[s]->get_frame_copy(copy.id);
      if (luma.size() < total) continue;
    }
    auto runs = sources[s]->get_dropout_hints(copy.id);

    // On the grid of the source's own frame, which the matcher compared it
    // with.
    const int32_t offset = s < sample_offsets.size() ? sample_offsets[s] : 0;
    shift_frame_samples(luma, system, nominal_width, height, offset);
    if (yc) shift_frame_samples(chroma, system, nominal_width, height, offset);
    shift_dropout_runs(runs, system, nominal_width, height, offset);
    if (s < rows.levels.size()) {
      if (const std::optional<LevelMatch>& level = rows.levels[s]) {
        apply_luma_level(luma, *level);
      }
    }

    // A copy is another frame of its source, which the decode may have
    // corrected differently, so it is matched to the burst on its own.
    if (burst_reference) {
      auto& plane = yc ? chroma : luma;
      match_chroma_gain(
          plane, burst_amplitude(plane, runs, system, nominal_width, height),
          burst_reference, copy.id, s);
    }

    rows.luma.push_back(std::move(luma));
    if (yc) rows.chroma.push_back(std::move(chroma));
    rows.valid.push_back(true);
    rows.field_valid.push_back(fields);
    rows.dropouts.push_back(std::move(runs));
    for (auto& weight : noise.weight) weight.push_back(weight[s]);
    appended.push_back(&copy);
  }
  if (noise.model.empty()) return;

  // In each field, weight the copies of each frame as that frame's copies of
  // the field combined.
  for (size_t field = 0; field < 2; ++field) {
    auto in_field = [&](size_t a) {
      return rows.field_valid[first_copy_row + a][field];
    };
    std::vector<bool> weighted(appended.size(), false);
    for (size_t a = 0; a < appended.size(); ++a) {
      if (weighted[a] || !in_field(a)) continue;
      std::vector<size_t> frame_rows;
      std::vector<SourceNoise> frame_noise;
      for (size_t b = a; b < appended.size(); ++b) {
        if (appended[b]->id != appended[a]->id || !in_field(b)) continue;
        frame_rows.push_back(b);
        frame_noise.push_back(noise.model[appended[b]->source]);
        weighted[b] = true;
      }
      const std::vector<double> weights = pressing_weights(frame_noise);
      for (size_t k = 0; k < frame_rows.size(); ++k) {
        noise.weight[field][first_copy_row + frame_rows[k]] =
            noise.scale * weights[k];
      }
    }
  }
}

std::vector<uint8_t> StackerStage::copy_lines(
    const std::vector<std::array<bool, 2>>& field_valid, size_t num_sources,
    VideoSystem system, size_t height) {
  std::array<bool, 2> copied{false, false};
  for (size_t r = num_sources; r < field_valid.size(); ++r) {
    copied[0] = copied[0] || field_valid[r][0];
    copied[1] = copied[1] || field_valid[r][1];
  }
  if (!copied[0] && !copied[1]) return {};

  // Active picture lines of a field (cvbs_signal_constants.h).  In 525-line
  // systems the first of them, line 21 (284 in field 2), carries closed
  // captions (CTA-608-E), which differ on every frame.
  const bool pal = system == VideoSystem::PAL;
  size_t first = static_cast<size_t>(pal ? kPalFirstActiveFrameLine
                                         : kNtscFirstActiveFrameLine) /
                 2;
  const size_t end = static_cast<size_t>(pal ? kPalLastActiveFrameLine
                                             : kNtscLastActiveFrameLine) /
                     2;
  if (!pal) ++first;

  std::vector<uint8_t> lines(height, 0);
  const size_t field1_lines = std::min(field1_line_count(system), height);
  for (size_t field = 0; field < 2; ++field) {
    if (!copied[field]) continue;
    const size_t base = field == 0 ? 0 : field1_lines;
    const size_t field_end = field == 0 ? field1_lines : height;
    for (size_t line = first; line < end && base + line < field_end; ++line) {
      lines[base + line] = 1;
    }
  }
  return lines;
}

std::pair<size_t, size_t> StackerStage::copy_samples(VideoSystem system) {
  // Active video window (cvbs_signal_constants.h).
  if (system == VideoSystem::PAL) {
    return {static_cast<size_t>(kPalActiveVideoStart),
            static_cast<size_t>(kPalActiveVideoEnd)};
  }
  return {static_cast<size_t>(kNtscActiveVideoStart),
          static_cast<size_t>(kNtscActiveVideoEnd)};
}

void StackerStage::restore_uncopied_lines(
    const std::vector<uint8_t>& takes_copies,
    const std::vector<sample_type>& source_plane,
    const std::vector<DropoutRun>& source_dropouts, VideoSystem system,
    size_t nominal_width, size_t height, std::vector<sample_type>& output,
    std::vector<DropoutRun>& output_dropouts) {
  std::vector<uint64_t> starts(height + 1);
  for (size_t y = 0; y <= height; ++y) {
    starts[y] = frame_line_sample_offset(system, nominal_width, y);
  }
  const auto [copy_first, copy_end] = copy_samples(system);
  auto uncopied = [&](uint64_t sample) {
    const auto it = std::upper_bound(starts.begin(), starts.end(), sample);
    if (it == starts.begin() || it == starts.end()) return false;
    const auto y = static_cast<size_t>(it - starts.begin()) - 1;
    if (takes_copies.empty() || takes_copies[y] == 0) return true;
    const uint64_t x = sample - starts[y];
    return x < copy_first || x >= copy_end;
  };

  auto restore = [&](uint64_t from, uint64_t to) {
    to = std::min<uint64_t>(to, source_plane.size());
    if (from >= to || to > output.size()) return;
    std::copy(source_plane.begin() + static_cast<std::ptrdiff_t>(from),
              source_plane.begin() + static_cast<std::ptrdiff_t>(to),
              output.begin() + static_cast<std::ptrdiff_t>(from));
  };
  for (size_t y = 0; y < height; ++y) {
    if (takes_copies.empty() || !takes_copies[y]) {
      restore(starts[y], starts[y + 1]);
      continue;
    }
    restore(starts[y], std::min(starts[y] + copy_first, starts[y + 1]));
    restore(std::min(starts[y] + copy_end, starts[y + 1]), starts[y + 1]);
  }

  output_dropouts.erase(
      std::remove_if(
          output_dropouts.begin(), output_dropouts.end(),
          [&](const DropoutRun& r) { return uncopied(r.sample_start); }),
      output_dropouts.end());
  for (const DropoutRun& run : source_dropouts) {
    if (uncopied(run.sample_start)) output_dropouts.push_back(run);
  }
  std::sort(output_dropouts.begin(), output_dropouts.end(),
            [](const DropoutRun& a, const DropoutRun& b) {
              return a.sample_start < b.sample_start;
            });
}

// ── Per-line processing
// ───────────────────────────────────────────────────────

void StackerStage::process_lines_range(
    size_t start_line, size_t end_line, size_t width, VideoSystem system,
    const std::vector<std::vector<sample_type>>& all_frames,
    const std::vector<bool>& frame_valid,
    const std::vector<std::array<bool, 2>>& field_valid,
    const std::vector<std::vector<uint8_t>>& dropout_masks,
    const std::vector<uint8_t>& takes_copies, const NoiseWeights& noise,
    size_t num_sources, int32_t black_level,
    std::vector<sample_type>& output_samples,
    std::vector<DropoutRun>& output_dropouts, size_t& total_dropouts,
    size_t& total_stacked) const {
  const size_t field1_lines = field1_line_count(system);
  const auto [copy_first, copy_end] = copy_samples(system);
  std::vector<int16_t> good_values;
  std::vector<double> good_weights;
  std::vector<int16_t> flagged_values;
  good_values.reserve(all_frames.size());
  good_weights.reserve(all_frames.size());
  flagged_values.reserve(all_frames.size());

  for (size_t y = start_line; y < end_line; ++y) {
    RunBuilder runs(output_dropouts);
    const size_t line_base = frame_line_sample_offset(system, width, y);
    const size_t line_len = frame_line_sample_count(system, width, y);
    const size_t field = y < field1_lines ? 0 : 1;
    const bool copied_line = !takes_copies.empty() && takes_copies[y];

    for (size_t x = 0; x < line_len; ++x) {
      const size_t line_rows = copied_line && x >= copy_first && x < copy_end
                                   ? all_frames.size()
                                   : num_sources;
      good_values.clear();
      good_weights.clear();
      flagged_values.clear();

      const size_t off = line_base + x;
      for (size_t si = 0; si < line_rows; ++si) {
        if (!frame_valid[si] || !field_valid[si][field] ||
            off >= all_frames[si].size()) {
          continue;
        }
        const int16_t val = all_frames[si][off];
        if (!dropout_masks[si][off]) {
          good_values.push_back(val);
          good_weights.push_back(noise.weight[field][si]);
        } else {
          flagged_values.push_back(val);
        }
      }

      int16_t stacked;
      uint8_t severity = 0;
      if (good_values.empty()) {
        // Every input is flagged here.  diff_dod may recover values when the
        // inputs agree, and passthrough keeps the flagged values rather than
        // black, but either way the values come from flagged samples, so the
        // output stays marked.
        severity = kHoleSeverity;
        std::vector<int16_t> recovered;
        if (line_rows >= 3 && !m_no_diff_dod && !flagged_values.empty()) {
          recovered = diff_dod(flagged_values);
        }
        if (!recovered.empty()) {
          stacked = stack_mode(recovered);
        } else if (m_passthrough && !flagged_values.empty()) {
          stacked = stack_mode(flagged_values);
        } else {
          stacked = static_cast<int16_t>(black_level);
        }
        ++total_dropouts;
      } else if (m_mode == StackingMode::WEIGHTED) {
        bool low_confidence = false;
        stacked = stack_weighted(good_values, good_weights,
                                 noise.reject_distance, low_confidence);
        if (low_confidence) severity = kLowConfidenceSeverity;
        ++total_stacked;
      } else {
        stacked = stack_mode(good_values);
        ++total_stacked;
      }
      runs.mark(off, severity);
      output_samples[off] = stacked;
    }
    runs.finish(line_base + line_len);
  }
}

size_t StackerStage::repair_pressing_defects(
    size_t y, size_t width, VideoSystem system,
    const std::vector<std::vector<sample_type>>& all_frames,
    const std::vector<bool>& frame_valid,
    const std::vector<std::array<bool, 2>>& field_valid,
    const std::vector<std::vector<uint8_t>>& dropout_masks,
    const NoiseWeights& noise, size_t num_sources, FrameID frame_id,
    std::vector<sample_type>& output_samples) const {
  // Active picture lines with a neighbour of their own field either side
  // (cvbs_signal_constants.h; fields are stored one after the other).
  const bool pal = system == VideoSystem::PAL;
  const auto first = static_cast<size_t>(pal ? kPalFirstActiveFrameLine
                                             : kNtscFirstActiveFrameLine) /
                     2;
  const auto end = static_cast<size_t>(pal ? kPalLastActiveFrameLine
                                           : kNtscLastActiveFrameLine) /
                   2;
  const size_t field1_lines = field1_line_count(system);
  const size_t field = y < field1_lines ? 0 : 1;
  const size_t field_line = y - (field * field1_lines);
  if (field_line <= first || field_line + 1 >= end) return 0;

  // The sources in this field, and their pressings.
  std::vector<size_t> rows;
  std::vector<size_t> groups;
  for (size_t r = 0; r < num_sources && r < noise.model.size(); ++r) {
    if (!frame_valid[r] || !field_valid[r][field]) continue;
    rows.push_back(r);
    const size_t g = noise.model[r].group;
    if (std::find(groups.begin(), groups.end(), g) == groups.end()) {
      groups.push_back(g);
    }
  }
  if (groups.size() < 2) return 0;

  const auto [copy_first, copy_end] = copy_samples(system);
  std::array<size_t, 3> base{};
  size_t length = SIZE_MAX;
  for (size_t k = 0; k < 3; ++k) {
    base[k] = frame_line_sample_offset(system, width, y + k - 1);
    length =
        std::min(length, frame_line_sample_count(system, width, y + k - 1));
  }
  const size_t x_end = std::min(copy_end, length);
  if (copy_first + kGridLumaBox >= x_end) return 0;

  // The median of |members|' unflagged samples on each of the three lines,
  // and its luma: a centred 4-sample box, NaN where a sample is missing.
  const double missing = std::numeric_limits<double>::quiet_NaN();
  std::vector<int16_t> values;
  auto consensus_luma = [&](const std::vector<size_t>& members) {
    std::array<std::vector<double>, 3> luma;
    std::vector<double> median(length);
    for (size_t k = 0; k < 3; ++k) {
      for (size_t x = 0; x < length; ++x) {
        values.clear();
        for (size_t r : members) {
          const size_t off = base[k] + x;
          if (off >= all_frames[r].size() || dropout_masks[r][off]) continue;
          values.push_back(all_frames[r][off]);
        }
        median[x] = values.empty() ? missing : compute_median(values);
      }
      luma[k].assign(length, missing);
      for (size_t x = 1; x + 2 < length; ++x) {
        luma[k][x] =
            (median[x - 1] + median[x] + median[x + 1] + median[x + 2]) /
            static_cast<double>(kGridLumaBox);
      }
    }
    return luma;
  };
  auto departure = [](const std::array<std::vector<double>, 3>& luma,
                      size_t x) {
    return std::abs(luma[1][x] - (0.5 * (luma[0][x] + luma[2][x])));
  };

  const double gross = noise.reject_distance;
  sample_type* out = output_samples.data() + base[1];
  size_t replaced = 0;
  for (size_t g : groups) {
    std::vector<size_t> own;
    std::vector<size_t> rest;
    for (size_t r : rows) {
      (noise.model[r].group == g ? own : rest).push_back(r);
    }
    if (own.size() < kMinDefectSources || rest.empty()) continue;
    const auto own_luma = consensus_luma(own);
    const auto rest_luma = consensus_luma(rest);

    // The samples where this pressing is the defect.  Comparisons with a
    // missing value are false, so a sample missing on either side is not.
    std::vector<uint8_t> defect(length, 0);
    for (size_t x = copy_first; x < x_end; ++x) {
      const double difference = std::abs(own_luma[1][x] - rest_luma[1][x]);
      defect[x] = difference > gross &&
                  departure(own_luma, x) - departure(rest_luma, x) > gross;
    }

    for (size_t x = copy_first; x < x_end;) {
      if (!defect[x]) {
        ++x;
        continue;
      }
      size_t run_end = x;
      while (run_end < x_end && defect[run_end]) ++run_end;
      const size_t found_first = x;
      x = run_end;
      if (run_end - found_first < kMinRepairRun) continue;
      const size_t from = std::max(
          found_first - std::min(found_first, kRepairMargin), copy_first);
      const size_t to = std::min(run_end + kRepairMargin, x_end);

      // The other pressings' stack over the run.
      std::vector<sample_type> repair(out + from, out + to);
      std::vector<int16_t> rest_values;
      std::vector<double> rest_weights;
      bool complete = true;
      for (size_t n = from; n < to && complete; ++n) {
        rest_values.clear();
        rest_weights.clear();
        for (size_t r : rest) {
          const size_t off = base[1] + n;
          if (off >= all_frames[r].size() || dropout_masks[r][off]) continue;
          rest_values.push_back(all_frames[r][off]);
          rest_weights.push_back(noise.weight[field][r]);
        }
        complete = !rest_values.empty();
        if (!complete) break;
        bool low_confidence = false;
        repair[n - from] = stack_weighted(
            rest_values, rest_weights, noise.reject_distance, low_confidence);
      }
      if (!complete) continue;

      // The luma guard: the run's luma, before and after, against the
      // other pressings' neighbours.
      auto guard_error = [&](auto sample_at) {
        double error = 0.0;
        for (size_t n = from + 1; n + 2 < to; ++n) {
          const double luma = (sample_at(n - 1) + sample_at(n) +
                               sample_at(n + 1) + sample_at(n + 2)) /
                              static_cast<double>(kGridLumaBox);
          const double expected = 0.5 * (rest_luma[0][n] + rest_luma[2][n]);
          if (!std::isnan(expected)) error += std::abs(luma - expected);
        }
        return error;
      };
      const double before =
          guard_error([&](size_t n) { return static_cast<double>(out[n]); });
      const double after = guard_error([&](size_t n) {
        return static_cast<double>(n >= from && n < to ? repair[n - from]
                                                       : out[n]);
      });
      if (!(after < before)) {
        ORC_LOG_DEBUG(
            "StackerStage: frame {}: line {} samples {}-{}: pressing {} "
            "differs from the others, but their samples do not bring the "
            "line nearer its neighbours; left as it was",
            frame_id, y + 1, from, to - 1, pressing_name(g));
        continue;
      }
      double own_mean = 0.0;
      double rest_mean = 0.0;
      for (size_t n = found_first; n < run_end; ++n) {
        own_mean += own_luma[1][n];
        rest_mean += rest_luma[1][n];
      }
      const auto count = static_cast<double>(run_end - found_first);
      ORC_LOG_DEBUG(
          "StackerStage: frame {}: line {} samples {}-{}: pressing {} "
          "({} source(s)) reads {:.0f} where the other pressings read "
          "{:.0f}; replaced from them",
          frame_id, y + 1, from, to - 1, pressing_name(g), own.size(),
          own_mean / count, rest_mean / count);
      std::copy(repair.begin(), repair.end(), out + from);
      replaced += to - from;
    }
  }
  return replaced;
}

// The YC path has no diff_dod: the recovery is a composite-signal heuristic.
// Weighted mode judges confidence on luma, which the noise was measured on.
void StackerStage::process_lines_range_yc(
    size_t start_line, size_t end_line, size_t width, VideoSystem system,
    const std::vector<std::vector<sample_type>>& all_luma,
    const std::vector<std::vector<sample_type>>& all_chroma,
    const std::vector<bool>& frame_valid,
    const std::vector<std::array<bool, 2>>& field_valid,
    const std::vector<std::vector<uint8_t>>& dropout_masks,
    const std::vector<uint8_t>& takes_copies, const NoiseWeights& noise,
    size_t num_sources, int32_t black_level,
    std::vector<sample_type>& output_luma,
    std::vector<sample_type>& output_chroma,
    std::vector<DropoutRun>& output_dropouts, size_t& total_dropouts,
    size_t& total_stacked) const {
  const size_t field1_lines = field1_line_count(system);
  const auto [copy_first, copy_end] = copy_samples(system);
  std::vector<int16_t> good_luma;
  std::vector<int16_t> good_chroma;
  std::vector<double> luma_weights;
  std::vector<double> chroma_weights;
  std::vector<int16_t> flagged_luma;
  std::vector<int16_t> flagged_chroma;

  for (size_t y = start_line; y < end_line; ++y) {
    RunBuilder runs(output_dropouts);
    const size_t line_base = frame_line_sample_offset(system, width, y);
    const size_t line_len = frame_line_sample_count(system, width, y);
    const size_t field = y < field1_lines ? 0 : 1;
    const bool copied_line = !takes_copies.empty() && takes_copies[y];

    for (size_t x = 0; x < line_len; ++x) {
      const size_t line_rows = copied_line && x >= copy_first && x < copy_end
                                   ? all_luma.size()
                                   : num_sources;
      good_luma.clear();
      good_chroma.clear();
      luma_weights.clear();
      chroma_weights.clear();
      flagged_luma.clear();
      flagged_chroma.clear();

      const size_t off = line_base + x;
      for (size_t si = 0; si < line_rows; ++si) {
        if (!frame_valid[si] || !field_valid[si][field] ||
            off >= all_luma[si].size()) {
          continue;
        }
        const bool flagged = dropout_masks[si][off] != 0;
        auto& luma = flagged ? flagged_luma : good_luma;
        auto& chroma = flagged ? flagged_chroma : good_chroma;
        luma.push_back(all_luma[si][off]);
        if (!flagged) luma_weights.push_back(noise.weight[field][si]);
        if (off < all_chroma[si].size()) {
          chroma.push_back(all_chroma[si][off]);
          if (!flagged) chroma_weights.push_back(noise.weight[field][si]);
        }
      }

      int16_t out_luma;
      int16_t out_chroma;
      uint8_t severity = 0;
      if (good_luma.empty()) {
        severity = kHoleSeverity;
        if (m_passthrough && !flagged_luma.empty()) {
          out_luma = stack_mode(flagged_luma);
          out_chroma = flagged_chroma.empty()
                           ? static_cast<int16_t>(black_level)
                           : stack_mode(flagged_chroma);
        } else {
          out_luma = static_cast<int16_t>(black_level);
          out_chroma = static_cast<int16_t>(black_level);
        }
        ++total_dropouts;
      } else if (m_mode == StackingMode::WEIGHTED) {
        bool low_confidence = false;
        out_luma = stack_weighted(good_luma, luma_weights,
                                  noise.reject_distance, low_confidence);
        if (low_confidence) severity = kLowConfidenceSeverity;
        bool chroma_low_confidence = false;
        out_chroma =
            good_chroma.empty()
                ? static_cast<int16_t>(black_level)
                : stack_weighted(good_chroma, chroma_weights,
                                 noise.reject_distance, chroma_low_confidence);
        ++total_stacked;
      } else {
        out_luma = stack_mode(good_luma);
        out_chroma = good_chroma.empty() ? static_cast<int16_t>(black_level)
                                         : stack_mode(good_chroma);
        ++total_stacked;
      }

      runs.mark(off, severity);
      output_luma[off] = out_luma;
      output_chroma[off] = out_chroma;
    }
    runs.finish(line_base + line_len);
  }
}

// ── Stacking mode
// ─────────────────────────────────────────────────────────────

int16_t StackerStage::stack_mode(const std::vector<int16_t>& values) const {
  if (values.empty()) {
    return 0;
  }

  switch (m_mode) {
    case StackingMode::MEAN:
      return static_cast<int16_t>(compute_mean(values));
    case StackingMode::MEDIAN:
      return compute_median(values);
    case StackingMode::WEIGHTED: {
      double sum = 0.0;
      for (int16_t v : values) sum += v;
      return round_sample(sum / static_cast<double>(values.size()));
    }
    default:
      return compute_median(values);
  }
}

// Two values cannot outvote each other, so they are always combined and only
// flagged when they disagree.  With three or more, values beyond the
// rejection distance from the median are left out; the median itself stands
// in when none is close to it.
int16_t StackerStage::stack_weighted(const std::vector<int16_t>& values,
                                     const std::vector<double>& weights,
                                     double reject_distance,
                                     bool& low_confidence) {
  low_confidence = false;
  const size_t n = values.size();
  if (n == 0) return 0;
  if (n == 1) return values[0];

  if (n == 2) {
    low_confidence =
        std::abs(static_cast<double>(values[0]) - values[1]) > reject_distance;
    return round_sample((weights[0] * values[0] + weights[1] * values[1]) /
                        (weights[0] + weights[1]));
  }

  // Called per sample, with a value per input and per still-frame copy, so
  // the median is selected in a reused buffer rather than sorted in a fresh
  // one.  O(n).
  thread_local std::vector<int16_t> scratch;
  scratch.assign(values.begin(), values.end());
  const auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(n / 2);
  std::nth_element(scratch.begin(), mid, scratch.end());
  double median = *mid;
  if (n % 2 == 0) {
    median = 0.5 * (median + *std::max_element(scratch.begin(), mid));
  }

  double weight_sum = 0.0;
  double weighted = 0.0;
  for (size_t k = 0; k < n; ++k) {
    if (std::abs(values[k] - median) > reject_distance) continue;
    weight_sum += weights[k];
    weighted += weights[k] * values[k];
  }
  const double result = weight_sum > 0.0 ? weighted / weight_sum : median;

  size_t agreeing = 0;
  for (int16_t v : values) {
    if (std::abs(v - result) <= reject_distance) ++agreeing;
  }
  low_confidence = 2 * agreeing <= n;
  return round_sample(result);
}

int16_t StackerStage::compute_median(std::vector<int16_t> v) const {
  if (v.empty()) {
    return 0;
  }
  size_t n = v.size();
  std::sort(v.begin(), v.end());
  if (n % 2 == 0) {
    return static_cast<int16_t>(
        (static_cast<int32_t>(v[n / 2 - 1]) + v[n / 2]) / 2);
  }
  return v[n / 2];
}

int32_t StackerStage::compute_mean(const std::vector<int16_t>& v) const {
  if (v.empty()) {
    return 0;
  }
  int64_t sum = 0;
  for (int16_t s : v) {
    sum += s;
  }
  return static_cast<int32_t>(sum / static_cast<int64_t>(v.size()));
}

std::vector<int16_t> StackerStage::diff_dod(
    const std::vector<int16_t>& input) const {
  if (input.size() < 3) {
    return {};
  }
  const double median = compute_median(input);
  const double window = std::abs(median) / 10.0;
  std::vector<int16_t> result;
  for (int16_t v : input) {
    if (v > median - window && v < median + window) {
      result.push_back(v);
    }
  }
  return result;
}

// ── Audio / EFM ──────────────────────────────────────────────────────────────

std::vector<int32_t> StackerStage::stack_audio(
    size_t pair, const std::vector<FrameID>& source_ids,
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    size_t best_src) const {
  if (m_audio_stacking_mode == AudioStackingMode::DISABLED) {
    if (best_src < sources.size() && source_ids[best_src] != UINT64_MAX &&
        sources[best_src] && sources[best_src]->has_audio()) {
      return sources[best_src]->get_audio_samples(pair, source_ids[best_src]);
    }
    return {};
  }

  // Inputs are stacked at the same frame ID across sources, so per-frame
  // pair counts always agree (audio_pairs_in_frame is a model invariant).
  std::vector<std::vector<int32_t>> all_audio;
  for (size_t i = 0; i < sources.size(); ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    if (pair >= sources[i]->audio_channel_pair_count() ||
        !sources[i]->has_frame(source_ids[i])) {
      continue;
    }
    auto s = sources[i]->get_audio_samples(pair, source_ids[i]);
    if (!s.empty()) {
      all_audio.push_back(std::move(s));
    }
  }

  if (all_audio.empty()) {
    return {};
  }
  if (all_audio.size() == 1) {
    return all_audio[0];
  }

  size_t n = all_audio[0].size();
  std::vector<int32_t> result(n);
  for (size_t si = 0; si < n; ++si) {
    std::vector<int32_t> vals;
    for (const auto& a : all_audio) {
      if (si < a.size()) {
        vals.push_back(a[si]);
      }
    }
    if (m_audio_stacking_mode == AudioStackingMode::MEDIAN) {
      result[si] = audio_median(vals);
    } else {
      result[si] = audio_mean(vals);
    }
  }
  return result;
}

std::vector<uint8_t> StackerStage::stack_efm(
    const std::vector<FrameID>& source_ids,
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    size_t best_src) const {
  if (m_efm_stacking_mode == EFMStackingMode::DISABLED) {
    if (best_src < sources.size() && source_ids[best_src] != UINT64_MAX &&
        sources[best_src] && sources[best_src]->has_efm()) {
      return sources[best_src]->get_efm_samples(source_ids[best_src]);
    }
    return {};
  }

  std::vector<std::vector<uint8_t>> all_efm;
  size_t reference = 0;
  for (size_t i = 0; i < sources.size(); ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    if (!sources[i]->has_efm() || !sources[i]->has_frame(source_ids[i])) {
      continue;
    }
    auto s = sources[i]->get_efm_samples(source_ids[i]);
    if (!s.empty()) {
      if (i == best_src) {
        reference = all_efm.size();
      }
      all_efm.push_back(std::move(s));
    }
  }

  if (all_efm.empty()) {
    return {};
  }

  // Confidence stacking works on the packed bytes: the doubt nibble is what it
  // weighs the sources by, and it emits a doubt of its own derived from how
  // far the sources agreed. See efm_confidence_stack.h.
  if (m_efm_stacking_mode == EFMStackingMode::CONFIDENCE) {
    return stack_efm_confidence(all_efm, reference);
  }

  // Mean and median are defined on t-values, not on the packed byte: taking
  // the mean or median of whole bytes would fold each producer's doubt nibble
  // into the t-value field and corrupt it. Strip the doubt first, so a stacked
  // t-value comes out with zero doubt - which packs to the bare t-value. That
  // is honest for these two modes: the stacked value is an average that no
  // producer vouched for, and the doubt of the inputs says nothing about it.
  for (auto& stream : all_efm) {
    for (auto& value : stream) {
      value = efm_tvalue(value);
    }
  }

  if (all_efm.size() == 1) {
    return all_efm[0];
  }

  size_t n = all_efm[0].size();
  std::vector<uint8_t> result(n);
  for (size_t si = 0; si < n; ++si) {
    std::vector<uint8_t> vals;
    for (const auto& e : all_efm) {
      if (si < e.size()) {
        vals.push_back(e[si]);
      }
    }
    if (m_efm_stacking_mode == EFMStackingMode::MEDIAN) {
      result[si] = efm_median(vals);
    } else {
      result[si] = efm_mean(vals);
    }
  }
  return result;
}

namespace {

// Saturate a combined audio value to the 24-bit two's-complement range
// carried in int32_t (see audio_channel_pair.h).
int32_t saturate_audio_24bit(int64_t value) {
  constexpr int64_t kMin = -8388608;
  constexpr int64_t kMax = 8388607;
  return static_cast<int32_t>(std::clamp(value, kMin, kMax));
}

}  // namespace

int32_t StackerStage::audio_mean(const std::vector<int32_t>& v) const {
  if (v.empty()) {
    return 0;
  }
  int64_t s = 0;
  for (int32_t x : v) {
    s += x;
  }
  return saturate_audio_24bit(s / static_cast<int64_t>(v.size()));
}

int32_t StackerStage::audio_median(std::vector<int32_t> v) const {
  if (v.empty()) {
    return 0;
  }
  size_t n = v.size();
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2),
                   v.end());
  if (n % 2 == 0) {
    std::nth_element(v.begin(),
                     v.begin() + static_cast<std::ptrdiff_t>((n - 1) / 2),
                     v.end());
    return saturate_audio_24bit(
        (static_cast<int64_t>(v[(n - 1) / 2]) + v[n / 2]) / 2);
  }
  return saturate_audio_24bit(v[n / 2]);
}

uint8_t StackerStage::efm_mean(const std::vector<uint8_t>& v) const {
  if (v.empty()) {
    return 0;
  }
  uint32_t s = 0;
  for (uint8_t x : v) {
    s += x;
  }
  return static_cast<uint8_t>(s / static_cast<uint32_t>(v.size()));
}

uint8_t StackerStage::efm_median(std::vector<uint8_t> v) const {
  if (v.empty()) {
    return 0;
  }
  size_t n = v.size();
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2),
                   v.end());
  if (n % 2 == 0) {
    std::nth_element(v.begin(),
                     v.begin() + static_cast<std::ptrdiff_t>((n - 1) / 2),
                     v.end());
    return static_cast<uint8_t>(
        (static_cast<uint16_t>(v[(n - 1) / 2]) + v[n / 2]) / 2);
  }
  return v[n / 2];
}

// ── Parameters
// ────────────────────────────────────────────────────────────────

std::vector<ParameterDescriptor> StackerStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType /*source_type*/) const {
  std::vector<ParameterDescriptor> d;

  d.push_back(
      {"mode", "Stacking Mode",
       "Per-pixel combining algorithm. Weighted = each source weighted by its "
       "measured noise, rejecting values far from the median and marking "
       "samples the sources disagree on as dropout. Mean and Median are "
       "kept to reproduce earlier results",
       ParameterType::STRING,
       ParameterConstraints{std::nullopt,
                            std::nullopt,
                            ParameterValue{std::string("Weighted")},
                            {"Weighted", "Mean", "Median"},
                            false,
                            std::nullopt}});
  d.push_back(
      {"reject_sigma", "Rejection Distance",
       "Weighted mode: how far from the per-sample median, in multiples of "
       "the measured noise, a value may lie and still be used (2-10, default "
       "5)",
       ParameterType::DOUBLE,
       ParameterConstraints{ParameterValue{2.0},
                            ParameterValue{10.0},
                            ParameterValue{5.0},
                            {},
                            false,
                            ParameterDependency{"mode", {"Weighted"}}}});
  d.push_back(
      {"group_pressings", "Group Sources by Pressing",
       "Weighted mode: find which sources share a pressing and count each "
       "pressing about once. Turn off for sources known to be one pressing: "
       "every source is then weighted by its own noise alone",
       ParameterType::BOOL,
       ParameterConstraints{std::nullopt,
                            std::nullopt,
                            ParameterValue{true},
                            {},
                            false,
                            ParameterDependency{"mode", {"Weighted"}}}});
  d.push_back({"no_diff_dod", "Disable Differential Dropout Detection",
               "Strictly trust dropout markings; disable diff_dod recovery",
               ParameterType::BOOL,
               ParameterConstraints{std::nullopt,
                                    std::nullopt,
                                    ParameterValue{false},
                                    {},
                                    false,
                                    std::nullopt}});
  d.push_back({"passthrough", "Passthrough Universal Dropouts",
               "Where every source is flagged as dropout, output the sources' "
               "combined values (still marked) instead of black",
               ParameterType::BOOL,
               ParameterConstraints{std::nullopt,
                                    std::nullopt,
                                    ParameterValue{false},
                                    {},
                                    false,
                                    std::nullopt}});
  d.push_back(
      {"sample_align", "Align Source Sample Grids",
       "Measure each source's 4FSC sample grid against the first source and "
       "shift it into line before stacking. One sample is 90° of subcarrier, "
       "so a source a sample out erodes chroma when combined unshifted",
       ParameterType::BOOL,
       ParameterConstraints{std::nullopt,
                            std::nullopt,
                            ParameterValue{true},
                            {},
                            false,
                            std::nullopt}});
  d.push_back(
      {"chroma_gain_match", "Match Source Chroma Levels",
       "Measure each source's colour burst on every frame and bring its "
       "chroma to the sources' median burst before stacking. A decode can "
       "lift one capture's chroma by up to a fifth over stretches of a side "
       "while its luma still matches the others'",
       ParameterType::BOOL,
       ParameterConstraints{std::nullopt,
                            std::nullopt,
                            ParameterValue{true},
                            {},
                            false,
                            std::nullopt}});
  d.push_back(
      {"luma_level_match", "Match Source Luma Levels",
       "Measure each source's luma gain and level against the picture common "
       "to every source on every frame and bring it to the typical source's "
       "before stacking. Captures through different players and decodes "
       "differ by a few per cent in contrast and a few codes in level, which "
       "the stack would otherwise average, and which its per-sample "
       "rejection reads as disagreement in bright areas",
       ParameterType::BOOL,
       ParameterConstraints{std::nullopt,
                            std::nullopt,
                            ParameterValue{true},
                            {},
                            false,
                            std::nullopt}});
  d.push_back(
      {"still_frame_stacking", "Stack Still-Frame Repeats",
       "Also stack the repeats of a held still picture within each source: "
       "frames at the colour cadence (4 frames apart in PAL, 2 in NTSC) "
       "within the search range that show the same picture at the same "
       "subcarrier phase. Active picture lines only; VBI, audio and EFM come "
       "from the frame itself. Works with a single input",
       ParameterType::BOOL,
       ParameterConstraints{std::nullopt,
                            std::nullopt,
                            ParameterValue{false},
                            {},
                            false,
                            std::nullopt}});
  d.push_back(
      {"still_frame_search", "Still-Frame Search Range",
       "How many frames either side of each frame to look for repeats of its "
       "picture (2-50, default 24). Only frames at the colour cadence are "
       "tried, so PAL needs at least 4. Every frame of a hold up to this plus "
       "one frames long (25 at the default, one second in PAL) reaches all "
       "of its repeats",
       ParameterType::INT32,
       ParameterConstraints{
           ParameterValue{int32_t{2}},
           ParameterValue{int32_t{50}},
           ParameterValue{int32_t{24}},
           {},
           false,
           ParameterDependency{"still_frame_stacking", {"true"}}}});
  d.push_back(
      {"pressing_repair", "Repair Pressing Defects",
       "Weighted mode, two or more pressings: where every source of one "
       "pressing carries a streak (6 samples or more) that the other "
       "pressings do not, and the lines either side show it to be a defect "
       "rather than picture, replace it from the other pressings. A stamper "
       "defect is on every disc of its pressing, so they agree on it and the "
       "stack keeps it. Replaced samples are not marked as dropouts; the "
       "per-frame report counts them. Composite sources only",
       ParameterType::BOOL,
       ParameterConstraints{std::nullopt,
                            std::nullopt,
                            ParameterValue{false},
                            {},
                            false,
                            ParameterDependency{"mode", {"Weighted"}}}});
  d.push_back({"audio_stacking", "Audio Stacking Mode",
               "How to combine audio: Disabled | Mean | Median",
               ParameterType::STRING,
               ParameterConstraints{std::nullopt,
                                    std::nullopt,
                                    ParameterValue{std::string("Mean")},
                                    {"Disabled", "Mean", "Median"},
                                    false,
                                    std::nullopt}});
  d.push_back(
      {"efm_stacking", "EFM Stacking Mode",
       "How to combine EFM t-values: Disabled | Confidence | Mean | Median",
       ParameterType::STRING,
       ParameterConstraints{std::nullopt,
                            std::nullopt,
                            ParameterValue{std::string("Disabled")},
                            {"Disabled", "Confidence", "Mean", "Median"},
                            false,
                            std::nullopt}});
  return d;
}

std::map<std::string, ParameterValue> StackerStage::get_parameters() const {
  const char* mode_names[] = {"Mean", "Median", "Weighted"};
  const auto mi = static_cast<size_t>(m_mode);

  const char* audio_names[] = {"Disabled", "Mean", "Median"};
  const char* efm_names[] = {"Disabled", "Mean", "Median", "Confidence"};

  return {
      {"mode", ParameterValue{std::string(mode_names[mi])}},
      {"reject_sigma", ParameterValue{m_reject_sigma}},
      {"group_pressings", ParameterValue{m_group_pressings}},
      {"no_diff_dod", ParameterValue{m_no_diff_dod}},
      {"passthrough", ParameterValue{m_passthrough}},
      {"sample_align", ParameterValue{m_sample_align}},
      {"chroma_gain_match", ParameterValue{m_chroma_gain_match}},
      {"luma_level_match", ParameterValue{m_luma_level_match}},
      {"still_frame_stacking", ParameterValue{m_still_frame_stacking}},
      {"still_frame_search", ParameterValue{m_still_frame_search}},
      {"pressing_repair", ParameterValue{m_pressing_repair}},
      {"audio_stacking",
       ParameterValue{
           std::string(audio_names[static_cast<int>(m_audio_stacking_mode)])}},
      {"efm_stacking", ParameterValue{std::string(
                           efm_names[static_cast<int>(m_efm_stacking_mode)])}}};
}

bool StackerStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  parameters_ = params;

  for (const auto& [key, value] : params) {
    if (key == "mode") {
      if (const auto* v = std::get_if<std::string>(&value)) {
        // Smart Neighbor and Neighbor were never implemented and always
        // stacked as Median, which they are still read as.  Smart Mean, and
        // Auto (Smart Mean with 3+ usable values), rejected values at a fixed
        // threshold; both are deprecated and read as Weighted.
        if (*v == "Weighted" || *v == "Auto" || *v == "Smart Mean") {
          m_mode = StackingMode::WEIGHTED;
        } else if (*v == "Mean") {
          m_mode = StackingMode::MEAN;
        } else if (*v == "Median" || *v == "Smart Neighbor" ||
                   *v == "Neighbor") {
          m_mode = StackingMode::MEDIAN;
        } else {
          ORC_LOG_WARN("StackerStage: unknown mode '{}'", *v);
          return false;
        }
      } else if (const auto* iv = std::get_if<int32_t>(&value)) {
        // Legacy integer modes: -1 Auto and 2 Smart Mean (both now
        // Weighted), 0 Mean, 1 Median, 3 and 4 the neighbour modes.
        static constexpr StackingMode kLegacyModes[] = {
            StackingMode::WEIGHTED, StackingMode::MEAN,   StackingMode::MEDIAN,
            StackingMode::WEIGHTED, StackingMode::MEDIAN, StackingMode::MEDIAN};
        if (*iv < -1 || *iv > 4) {
          return false;
        }
        m_mode = kLegacyModes[*iv + 1];
      } else {
        return false;
      }
    } else if (key == "reject_sigma") {
      double k = 0.0;
      if (const auto* v = std::get_if<double>(&value)) {
        k = *v;
      } else if (const auto* iv = std::get_if<int32_t>(&value)) {
        k = *iv;
      } else {
        return false;
      }
      if (!(k >= 2.0 && k <= 10.0)) {
        return false;
      }
      m_reject_sigma = k;
    } else if (key == "group_pressings") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_group_pressings = *v;
      } else {
        return false;
      }
    } else if (key == "no_diff_dod") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_no_diff_dod = *v;
      } else {
        return false;
      }
    } else if (key == "passthrough") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_passthrough = *v;
      } else {
        return false;
      }
    } else if (key == "sample_align") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_sample_align = *v;
      } else {
        return false;
      }
    } else if (key == "chroma_gain_match") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_chroma_gain_match = *v;
      } else {
        return false;
      }
    } else if (key == "luma_level_match") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_luma_level_match = *v;
      } else {
        return false;
      }
    } else if (key == "still_frame_stacking") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_still_frame_stacking = *v;
      } else {
        return false;
      }
    } else if (key == "pressing_repair") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_pressing_repair = *v;
      } else {
        return false;
      }
    } else if (key == "still_frame_search") {
      if (const auto* v = std::get_if<int32_t>(&value)) {
        if (*v < 2 || *v > 50) return false;
        m_still_frame_search = *v;
      } else {
        return false;
      }
    } else if (key == "audio_stacking") {
      if (const auto* v = std::get_if<std::string>(&value)) {
        if (*v == "Disabled") {
          m_audio_stacking_mode = AudioStackingMode::DISABLED;
        } else if (*v == "Mean") {
          m_audio_stacking_mode = AudioStackingMode::MEAN;
        } else if (*v == "Median") {
          m_audio_stacking_mode = AudioStackingMode::MEDIAN;
        } else {
          ORC_LOG_WARN("StackerStage: unknown audio_stacking '{}'", *v);
          return false;
        }
      } else {
        return false;
      }
    } else if (key == "efm_stacking") {
      if (const auto* v = std::get_if<std::string>(&value)) {
        if (*v == "Disabled") {
          m_efm_stacking_mode = EFMStackingMode::DISABLED;
        } else if (*v == "Mean") {
          m_efm_stacking_mode = EFMStackingMode::MEAN;
        } else if (*v == "Median") {
          m_efm_stacking_mode = EFMStackingMode::MEDIAN;
        } else if (*v == "Confidence") {
          m_efm_stacking_mode = EFMStackingMode::CONFIDENCE;
        } else {
          ORC_LOG_WARN("StackerStage: unknown efm_stacking '{}'", *v);
          return false;
        }
      } else {
        return false;
      }
    }
  }
  return true;
}

// ── Report / Preview
// ──────────────────────────────────────────────────────────

StagePreviewCapability StackerStage::get_preview_capability() const {
  std::lock_guard<std::mutex> lk(execute_mutex_);
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
