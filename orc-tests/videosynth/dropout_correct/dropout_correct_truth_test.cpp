/*
 * File:        dropout_correct_truth_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     dropout_correct on the smoke, dropout and concealment cases,
 *              measured against their truth: the error per band, inside the
 *              dropouts and outside them, the dropouts the sidecar does not
 *              flag, and line_copy against interpolate, against
 *              baselines.json
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/common_types.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <truth_comparison.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dropout_correct_stage.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::Better;
using orc_videosynth::CaseFile;
using orc_videosynth::Metric;
using orc_videosynth::VfrPtr;

struct TruthCase {
  CaseFile capture;
  CaseFile truth;
};

// A case and its truth, derived by the smoke-clean variant.
TruthCase truth_case(const std::string& name, orc::VideoSystem system) {
  return {{name, "videosynth_" + name, system},
          {name + "_clean", "videosynth_" + name + "_clean", system}};
}

// A concealment case and its truth (the conceal-clean variant), or their Y/C
// renderings (conceal-yc and conceal-yc-clean), which keep the composite
// cases' file names.
TruthCase conceal_case(const std::string& system_name, orc::VideoSystem system,
                       bool yc) {
  const std::string name = system_name + "_conceal";
  const std::string suffix = yc ? "_yc" : "";
  return {
      {name + suffix, "videosynth_" + name, system, yc},
      {name + suffix + "_clean", "videosynth_" + name + "_clean", system, yc}};
}

const TruthCase kPal = truth_case("pal_smoke", orc::VideoSystem::PAL);
const TruthCase kNtsc = truth_case("ntsc_smoke", orc::VideoSystem::NTSC);
const TruthCase kPalM = truth_case("pal_m_smoke", orc::VideoSystem::PAL_M);

// The smoke and dropout cases measure line_copy, which their baselines
// record; the concealment cases measure both methods.
const std::map<std::string, orc::ParameterValue> kLineCopy{
    {"method", std::string("line_copy")}};

// A capture, its truth and dropout_correct's output. The output computes
// each frame when it is read and refers to the stage and its context, so
// both are kept with it.
struct Corrected {
  VfrPtr capture;
  VfrPtr truth;
  VfrPtr output;
  orc::SourceParameters params;
  std::unique_ptr<orc::DropoutCorrectStage> stage;
  std::unique_ptr<orc::ObservationContext> context;
};

// dropout_correct, run with |parameters| on the capture of |c|; nullopt
// (with the test failed) when a step fails.
std::optional<Corrected> correct(
    const TruthCase& c,
    const std::map<std::string, orc::ParameterValue>& parameters) {
  const auto capture_artifact = orc_videosynth::open_case(c.capture);
  Corrected r;
  r.capture = orc_videosynth::as_vfr(capture_artifact);
  r.truth = orc_videosynth::as_vfr(orc_videosynth::open_case(c.truth));
  if (!r.capture || !r.truth) return std::nullopt;
  if (r.capture->frame_count() != r.truth->frame_count()) {
    ADD_FAILURE() << "capture and truth differ in length";
    return std::nullopt;
  }
  const auto params = r.capture->get_video_parameters();
  if (!params) {
    ADD_FAILURE() << "the capture has no video parameters";
    return std::nullopt;
  }
  r.params = *params;

  r.stage = std::make_unique<orc::DropoutCorrectStage>();
  r.context = std::make_unique<orc::ObservationContext>();
  if (!r.stage->set_parameters(parameters)) {
    ADD_FAILURE() << "dropout_correct rejected its parameters";
    return std::nullopt;
  }
  const auto outputs =
      r.stage->execute({capture_artifact}, parameters, *r.context);
  if (outputs.size() != 1) {
    ADD_FAILURE() << "dropout_correct gave no output";
    return std::nullopt;
  }
  r.output = orc_videosynth::as_vfr(outputs[0]);
  if (!r.output) {
    ADD_FAILURE() << "dropout_correct's output is not video";
    return std::nullopt;
  }
  return r;
}

// The frames a section of a case occupies, [first, first + count).
struct FrameRange {
  orc::FrameID first;
  size_t count;
};

// The correction's metrics over |range|, each name prefixed with |prefix|:
// the error inside the flagged runs and outside them, and per band, the
// error and its gain over the uncorrected capture. Every sample the
// impairments changed must be flagged; |unflagged| sections are measured by
// unflagged_metrics() instead.
std::vector<Metric> correction_metrics(const Corrected& r, FrameRange range,
                                       const std::string& prefix) {
  const double ire = orc_videosynth::ire_per_code(r.params);
  orc_videosynth::BandErrorMeter capture_bands(r.params.system, ire);
  orc_videosynth::BandErrorMeter output_bands(r.params.system, ire);
  orc_videosynth::RunErrorMeter capture_runs(ire);
  orc_videosynth::RunErrorMeter output_runs(ire);
  orc_videosynth::CoverageMeter coverage;
  for (orc::FrameID f = range.first; f < range.first + range.count; ++f) {
    // Owned copies: a representation's frame pointer lasts only until its
    // next call.
    const auto truth_frame = r.truth->get_frame_copy(f);
    const auto capture_frame = r.capture->get_frame_copy(f);
    const auto output_frame = r.output->get_frame_copy(f);
    const auto runs = r.capture->get_dropout_hints(f);
    if (truth_frame.size() != capture_frame.size() ||
        output_frame.size() != capture_frame.size()) {
      ADD_FAILURE() << "frame " << f << " differs in size";
      return {};
    }
    const size_t total = capture_frame.size();
    capture_bands.add_frame(capture_frame.data(), truth_frame.data(), r.params);
    output_bands.add_frame(output_frame.data(), truth_frame.data(), r.params);
    capture_runs.add_frame(capture_frame.data(), truth_frame.data(), total,
                           runs, r.params);
    output_runs.add_frame(output_frame.data(), truth_frame.data(), total, runs,
                          r.params);
    coverage.add_frame(
        orc_videosynth::damage_runs(capture_frame.data(), truth_frame.data(),
                                    r.params, total, f),
        runs, total, r.params);
  }

  // The capture differs from its truth only inside the dropouts; were it
  // otherwise, the errors below would not be the correction's.
  const auto capture_split = capture_runs.result();
  EXPECT_GT(capture_split.inside_samples, 0u) << prefix;
  EXPECT_EQ(capture_split.outside_rms_ire, 0.0) << prefix;
  EXPECT_EQ(coverage.result().flagged_fraction(), 1.0) << prefix;

  using orc_videosynth::new_tolerance;
  std::vector<Metric> metrics;
  const auto split = output_runs.result();
  metrics.push_back({prefix + "dropout_rms_ire", split.inside_rms_ire,
                     Better::Lower,
                     new_tolerance(split.inside_rms_ire, 0.02, 0.01)});
  metrics.push_back({prefix + "outside_rms_ire", split.outside_rms_ire,
                     Better::Lower,
                     new_tolerance(split.outside_rms_ire, 0.02, 0.01)});
  const auto& bands = output_bands.bands();
  const auto capture_rms = capture_bands.rms_ire();
  const auto output_rms = output_bands.rms_ire();
  for (size_t b = 0; b < bands.size(); ++b) {
    metrics.push_back({prefix + bands[b].name + "_rms_ire", output_rms[b],
                       Better::Lower,
                       new_tolerance(output_rms[b], 0.02, 0.01)});
    if (const auto gain =
            orc_videosynth::gain_db(capture_rms[b], output_rms[b])) {
      metrics.push_back(
          {prefix + bands[b].name + "_gain_db", *gain, Better::Higher, 0.2});
    }
  }
  return metrics;
}

// The metrics of a section whose dropouts the sidecar does not flag: how
// much of the damage the capture's runs flag (none, until something detects
// dropouts from the signal), and the error left in it after correction.
std::vector<Metric> unflagged_metrics(const Corrected& r, FrameRange range,
                                      const std::string& prefix) {
  const double ire = orc_videosynth::ire_per_code(r.params);
  orc_videosynth::RunErrorMeter output_runs(ire);
  orc_videosynth::CoverageMeter coverage;
  for (orc::FrameID f = range.first; f < range.first + range.count; ++f) {
    const auto truth_frame = r.truth->get_frame_copy(f);
    const auto capture_frame = r.capture->get_frame_copy(f);
    const auto output_frame = r.output->get_frame_copy(f);
    const size_t total = capture_frame.size();
    const auto damage = orc_videosynth::damage_runs(
        capture_frame.data(), truth_frame.data(), r.params, total, f);
    coverage.add_frame(damage, r.capture->get_dropout_hints(f), total,
                       r.params);
    output_runs.add_frame(output_frame.data(), truth_frame.data(), total,
                          damage, r.params);
  }
  EXPECT_GT(coverage.result().damaged_samples, 0u) << prefix;
  const auto split = output_runs.result();
  return {
      {prefix + "flagged_fraction", coverage.result().flagged_fraction(),
       Better::Higher, 0.01},
      {prefix + "damage_rms_ire", split.inside_rms_ire, Better::Lower,
       orc_videosynth::new_tolerance(split.inside_rms_ire, 0.02, 0.01)},
  };
}

// dropout_correct's metrics over the whole of |c|.
std::vector<Metric> measure(
    const TruthCase& c,
    const std::map<std::string, orc::ParameterValue>& parameters) {
  const auto r = correct(c, parameters);
  if (!r) return {};
  return correction_metrics(*r, {0, r->capture->frame_count()}, "");
}

// The sections of projects/dropouts/*_dropouts.yaml, four frames each.
struct DropoutSection {
  const char* prefix;
  bool flagged;
};
constexpr DropoutSection kDropoutSections[] = {
    {"still_random_", true},  {"still_scratch_", true},
    {"motion_random_", true}, {"motion_scratch_", true},
    {"unflagged_", false},
};
constexpr size_t kDropoutSectionFrames = 4;

void run_dropout_sections(const TruthCase& c) {
  const auto r = correct(c, kLineCopy);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->capture->frame_count(),
            std::size(kDropoutSections) * kDropoutSectionFrames);
  std::vector<Metric> metrics;
  for (size_t i = 0; i < std::size(kDropoutSections); ++i) {
    const auto& section = kDropoutSections[i];
    const FrameRange range{static_cast<orc::FrameID>(i * kDropoutSectionFrames),
                           kDropoutSectionFrames};
    const auto m = section.flagged
                       ? correction_metrics(*r, range, section.prefix)
                       : unflagged_metrics(*r, range, section.prefix);
    metrics.insert(metrics.end(), m.begin(), m.end());
  }
  orc_videosynth::expect_metrics(metrics);
}

}  // namespace

TEST(DropoutCorrectTruth, PAL_Composite) {
  orc_videosynth::expect_metrics(measure(kPal, kLineCopy));
}

TEST(DropoutCorrectTruth, NTSC_Composite) {
  orc_videosynth::expect_metrics(measure(kNtsc, kLineCopy));
}

TEST(DropoutCorrectTruth, PALM_Composite) {
  orc_videosynth::expect_metrics(measure(kPalM, kLineCopy));
}

// Random dropouts and scratches on a still and on motion, flagged, and random
// dropouts the sidecar does not flag. PAL and PAL-M search four lines apart
// to keep chroma in phase, against NTSC's two, so their motion sections
// (luma-only moving content) come out a little worse than NTSC's.
TEST(DropoutCorrectTruth, PAL_Sections) {
  run_dropout_sections(truth_case("pal_dropouts", orc::VideoSystem::PAL));
}

TEST(DropoutCorrectTruth, NTSC_Sections) {
  run_dropout_sections(truth_case("ntsc_dropouts", orc::VideoSystem::NTSC));
}

TEST(DropoutCorrectTruth, PALM_Sections) {
  run_dropout_sections(truth_case("pal_m_dropouts", orc::VideoSystem::PAL_M));
}

// The baselines catch a real degradation: replacing dropouts from the nearest
// line regardless of chroma phase, rather than from one whose subcarrier
// phase matches, must regress PAL_Composite's recorded metrics.
TEST(DropoutCorrectTruth, PAL_UnmatchedPhaseRegresses) {
  const auto* file = orc_videosynth::baselines();
  ASSERT_NE(file, nullptr);
  if (!file->find("DropoutCorrectTruth.PAL_Composite", "dropout_rms_ire")) {
    GTEST_SKIP() << "DropoutCorrectTruth.PAL_Composite has no baselines yet";
  }
  const auto metrics = measure(kPal, {{"method", std::string("line_copy")},
                                      {"match_chroma_phase", false}});
  ASSERT_FALSE(metrics.empty());
  std::vector<std::string> regressed;
  for (const auto& m : metrics) {
    if (orc_videosynth::judge(
            file->find("DropoutCorrectTruth.PAL_Composite", m.name), m.value) ==
        orc_videosynth::Verdict::Regressed) {
      regressed.push_back(m.name);
    }
  }
  EXPECT_NE(std::find(regressed.begin(), regressed.end(), "dropout_rms_ire"),
            regressed.end())
      << "an unmatched-phase correction did not regress the dropout error";
}

namespace {
// ── line_copy against interpolate ─────────────────────────────────────────

// One byte per sample of a flat frame of |total| samples, 1 inside |runs|.
std::vector<uint8_t> run_mask(const std::vector<orc::DropoutRun>& runs,
                              size_t total) {
  std::vector<uint8_t> mask(total, 0);
  for (const auto& run : runs) {
    const size_t end =
        std::min<size_t>(total, run.sample_start + run.sample_count);
    for (size_t i = run.sample_start; i < end; ++i) mask[i] = 1;
  }
  return mask;
}

// The error inside the dropouts in luma and in chroma. A composite frame's
// error inside the runs (zero outside) is split by band: luma below the
// chroma band, chroma in it (composite_bands()). A Y/C frame's planes are
// measured apart.
struct ChannelError {
  double luma_rms_ire = 0.0;
  double chroma_rms_ire = 0.0;
};

ChannelError conceal_error(const Corrected& r, FrameRange range, bool yc) {
  const double ire = orc_videosynth::ire_per_code(r.params);
  if (yc) {
    orc_videosynth::RunErrorMeter luma(ire);
    orc_videosynth::RunErrorMeter chroma(ire);
    for (orc::FrameID f = range.first; f < range.first + range.count; ++f) {
      const auto desc = r.capture->get_frame_descriptor(f);
      if (!desc) continue;
      const size_t total = desc->samples_total;
      const auto runs = r.capture->get_dropout_hints(f);
      // Owned copies: a representation's frame pointer lasts only until its
      // next call.
      const auto copy =
          [total](const orc::VideoFrameRepresentation::sample_type* p) {
            return p ? std::vector<int16_t>(p, p + total)
                     : std::vector<int16_t>();
          };
      const auto truth_y = copy(r.truth->get_frame_luma(f));
      const auto truth_c = copy(r.truth->get_frame_chroma(f));
      const auto out_y = copy(r.output->get_frame_luma(f));
      const auto out_c = copy(r.output->get_frame_chroma(f));
      if (truth_y.empty() || truth_c.empty() || out_y.empty() ||
          out_c.empty()) {
        ADD_FAILURE() << "frame " << f << " has no luma or chroma plane";
        return {};
      }
      luma.add_frame(out_y.data(), truth_y.data(), total, runs, r.params);
      chroma.add_frame(out_c.data(), truth_c.data(), total, runs, r.params);
    }
    return {luma.result().inside_rms_ire, chroma.result().inside_rms_ire};
  }

  orc_videosynth::BandErrorMeter bands(r.params.system, ire);
  orc_videosynth::RunErrorMeter runs_meter(ire);
  for (orc::FrameID f = range.first; f < range.first + range.count; ++f) {
    const auto truth_frame = r.truth->get_frame_copy(f);
    auto output_frame = r.output->get_frame_copy(f);
    const auto runs = r.capture->get_dropout_hints(f);
    const size_t total = truth_frame.size();
    if (output_frame.size() != total) {
      ADD_FAILURE() << "frame " << f << " differs in size";
      return {};
    }
    runs_meter.add_frame(output_frame.data(), truth_frame.data(), total, runs,
                         r.params);
    const auto mask = run_mask(runs, total);
    for (size_t i = 0; i < total; ++i) {
      if (!mask[i]) output_frame[i] = truth_frame[i];
    }
    bands.add_frame(output_frame.data(), truth_frame.data(), r.params);
  }
  // The bands' RMS is over every active sample; the error lives inside the
  // runs only, so it is rescaled to an RMS over the samples inside them.
  const auto split = runs_meter.result();
  if (split.inside_samples == 0) return {};
  const double scale = std::sqrt(
      static_cast<double>(split.inside_samples + split.outside_samples) /
      static_cast<double>(split.inside_samples));
  const auto rms = bands.rms_ire();
  // Bands: low luma, high luma, chroma.
  const double luma = std::hypot(rms[0], rms[1]) * scale;
  return {luma, rms[2] * scale};
}

// The sections of projects/conceal/*_conceal.yaml, four frames each, and
// what each must show of interpolate against line_copy:
//   Better: lower error in luma and in chroma (pictures with vertical
//     detail: the gate for interpolate).
//   Level: no more than kLevelIre above it (bars, constant down the picture,
//     which a copy rebuilds exactly but for the line where a band ends).
//   Recorded: the figures only. The moving zone plate holds every vertical
//     frequency, up to the field's limit, and favours whichever distance its
//     rings repeat at: copying from four lines away beats copying from the
//     adjacent line there (35 against 57 IRE of luma on PAL Y/C), which no
//     picture does. PAL's and PAL-M's composite copies come from four lines
//     away, and interpolate loses to them on it; NTSC's from two, and it
//     wins.
enum class Expect { Better, Level, Recorded };
struct ConcealSection {
  const char* prefix;
  Expect expect;
};
constexpr double kLevelIre = 1.0;
constexpr size_t kConcealSectionFrames = 4;
const std::vector<ConcealSection> kPalConcealSections{
    {"testcard_", Expect::Better},
    {"tartan_", Expect::Better},
    {"motion_", Expect::Recorded}};
const std::vector<ConcealSection> kNtscConcealSections{
    {"bars_", Expect::Level},
    {"tartan_", Expect::Better},
    {"motion_", Expect::Recorded}};

// One channel's error under interpolate against line_copy's, as |expect|
// says.
void expect_against_copy(Expect expect, double interpolate, double copy,
                         const std::string& what) {
  switch (expect) {
    case Expect::Better:
      // Y/C planes with nothing to conceal read 0 either way.
      if (copy == 0.0) {
        EXPECT_LE(interpolate, kLevelIre) << what;
      } else {
        EXPECT_LT(interpolate, copy) << what;
      }
      break;
    case Expect::Level:
      EXPECT_LE(interpolate, copy + kLevelIre) << what;
      break;
    case Expect::Recorded:
      break;
  }
}

// Each section corrected by both methods: their errors in luma and chroma
// as metrics, held against each other as the section expects.
void run_conceal_sections(const TruthCase& c,
                          const std::vector<ConcealSection>& sections,
                          bool yc) {
  const auto copied = correct(c, {{"method", std::string("line_copy")}});
  const auto interpolated =
      correct(c, {{"method", std::string("interpolate")}});
  ASSERT_TRUE(copied.has_value());
  ASSERT_TRUE(interpolated.has_value());
  ASSERT_EQ(copied->capture->frame_count(),
            sections.size() * kConcealSectionFrames);
  std::vector<Metric> metrics;
  using orc_videosynth::new_tolerance;
  for (size_t i = 0; i < sections.size(); ++i) {
    const FrameRange range{static_cast<orc::FrameID>(i * kConcealSectionFrames),
                           kConcealSectionFrames};
    const auto copy = conceal_error(*copied, range, yc);
    const auto interp = conceal_error(*interpolated, range, yc);
    const std::string prefix = sections[i].prefix;
    expect_against_copy(sections[i].expect, interp.luma_rms_ire,
                        copy.luma_rms_ire, prefix + "luma");
    expect_against_copy(sections[i].expect, interp.chroma_rms_ire,
                        copy.chroma_rms_ire, prefix + "chroma");
    for (const auto& [method, e] :
         {std::pair{"line_copy_", copy}, std::pair{"interpolate_", interp}}) {
      metrics.push_back({prefix + method + "luma_rms_ire", e.luma_rms_ire,
                         Better::Lower,
                         new_tolerance(e.luma_rms_ire, 0.02, 0.01)});
      metrics.push_back({prefix + method + "chroma_rms_ire", e.chroma_rms_ire,
                         Better::Lower,
                         new_tolerance(e.chroma_rms_ire, 0.02, 0.01)});
    }
  }
  orc_videosynth::expect_metrics(metrics);
}

}  // namespace

TEST(DropoutCorrectTruth, PAL_Conceal) {
  run_conceal_sections(conceal_case("pal", orc::VideoSystem::PAL, false),
                       kPalConcealSections, false);
}

TEST(DropoutCorrectTruth, NTSC_Conceal) {
  run_conceal_sections(conceal_case("ntsc", orc::VideoSystem::NTSC, false),
                       kNtscConcealSections, false);
}

TEST(DropoutCorrectTruth, PALM_Conceal) {
  run_conceal_sections(conceal_case("pal_m", orc::VideoSystem::PAL_M, false),
                       kNtscConcealSections, false);
}

TEST(DropoutCorrectTruth, PAL_ConcealYC) {
  run_conceal_sections(conceal_case("pal", orc::VideoSystem::PAL, true),
                       kPalConcealSections, true);
}

TEST(DropoutCorrectTruth, NTSC_ConcealYC) {
  run_conceal_sections(conceal_case("ntsc", orc::VideoSystem::NTSC, true),
                       kNtscConcealSections, true);
}

TEST(DropoutCorrectTruth, PALM_ConcealYC) {
  run_conceal_sections(conceal_case("pal_m", orc::VideoSystem::PAL_M, true),
                       kNtscConcealSections, true);
}

}  // namespace orc_videosynth_test
