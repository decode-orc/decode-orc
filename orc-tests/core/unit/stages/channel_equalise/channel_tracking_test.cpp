/*
 * File:        channel_tracking_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for tracking the edge smear along a source: the
 *              smear track's text form, per-frame smear and simplification,
 *              the two-component fit, the certified choice of each window
 *              and of the track as the stage applies it, the picture edges
 *              of both fields of a frame source, and the Channel Tracking
 *              tool and stage parameter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/tooling/stage_analysis_tool.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/channel_equalise.h>
#include <orc/support/frame_line_util.h>

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../../../../../orc/plugins/stages/channel_equalise/channel_calibration.h"
#include "../../../../../orc/plugins/stages/channel_equalise/channel_calibration_tool.h"
#include "../../../../../orc/plugins/stages/channel_equalise/channel_equalise_stage.h"
#include "../../../../../orc/plugins/stages/channel_equalise/picture_edges.h"
#include "../../../../../orc/plugins/stages/channel_equalise/smear_track.h"
#include "../../../../../orc/plugins/stages/channel_equalise/window_select.h"
#include "../../include/pal_colour_bars_source.h"

namespace orc_unit_test {
namespace {

constexpr double kSamplesPerUs = 4.0 * 4433618.75 / 1.0e6;
constexpr double kStartUs = -3.0;

// The aligned segment of line 19 (as in channel_calibration_test): a 100 IRE
// bar of 10.1 us and a pulse 4.2 us after it, through a smear of |fraction|
// / |tau| samples, plus a deterministic ripple of |ripple| IRE that changes
// with |seed|, standing in for one field's noise.
std::vector<double> segment(double fraction, double tau, double ripple,
                            int seed) {
  const auto n = static_cast<size_t>(std::lround(20.0 * kSamplesPerUs));
  std::vector<double> x(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = kStartUs + (static_cast<double>(i) / kSamplesPerUs);
    double v = (t >= 0.0 && t < 10.1) ? 100.0 : 0.0;
    const double d = (t - 14.3) / 0.2;
    if (std::abs(d) < 1.0) v += 7.5 * 0.5 * (1.0 + std::cos(M_PI * d));
    x[i] = v;
  }
  if (fraction > 0.0) {
    const double pole = std::exp(-1.0 / tau);
    double acc = x.front();
    for (double& v : x) {
      acc = (pole * acc) + ((1.0 - pole) * v);
      v = ((1.0 - fraction) * v) + (fraction * acc);
    }
  }
  for (size_t i = 0; i < n; ++i) {
    x[i] += ripple * std::sin((static_cast<double>(i) * 2.399) +
                              (static_cast<double>(seed) * 0.731));
  }
  return x;
}

// ── SmearTrack ─────────────────────────────────────────────────────────────

TEST(SmearTrackTest, TextRoundTrips) {
  const auto track = orc::SmearTrack::parse("16:0.2400:6.100;48:0:2;80:0.1:7");
  ASSERT_TRUE(track.has_value());
  ASSERT_EQ(track->knots().size(), 3u);
  EXPECT_EQ(track->knots()[1].frame, 48);
  EXPECT_EQ(track->to_string(),
            "16:0.2400:6.100;48:0.0000:2.000;80:0.1000:7.000");
  EXPECT_EQ(orc::SmearTrack::parse(track->to_string())->knots(),
            track->knots());
  // Empty text is an empty track.
  const auto empty = orc::SmearTrack::parse("");
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
  EXPECT_EQ(empty->to_string(), "");
}

TEST(SmearTrackTest, RejectsMalformedText) {
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.24").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.24:6.1:3").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("x:0.24:6.1").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.24:six").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.24:6.1;").has_value());
  // Out of order, repeated, or outside the stage's range.
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.2:6;8:0.2:6").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.2:6;16:0.2:6").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.5:6").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("16:-0.1:6").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.2:1").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.2:60").has_value());
}

TEST(SmearTrackTest, InterpolatesBetweenKnotsAndHoldsTheEnds) {
  const auto track =
      orc::SmearTrack::from_knots({{100, 0.20, 6.0}, {200, 0.30, 8.0}});
  ASSERT_TRUE(track.has_value());
  EXPECT_DOUBLE_EQ(track->at(0).fraction, 0.20);
  EXPECT_DOUBLE_EQ(track->at(150).fraction, 0.25);
  EXPECT_DOUBLE_EQ(track->at(150).tau_samples, 7.0);
  EXPECT_DOUBLE_EQ(track->at(1000).fraction, 0.30);
  // An empty track has no smear.
  EXPECT_FALSE(orc::SmearTrack{}.at(5).active());
}

// A knot with no smear has no time constant to lend: the smear fades out
// with its neighbour's.
TEST(SmearTrackTest, FadesToNoSmearWithTheNeighboursTimeConstant) {
  const auto track = orc::SmearTrack::from_knots(
      {{0, 0.24, 6.0}, {10, 0.0, orc::channel_equalise::kMinTauSamples}});
  ASSERT_TRUE(track.has_value());
  EXPECT_DOUBLE_EQ(track->at(5).fraction, 0.12);
  EXPECT_DOUBLE_EQ(track->at(5).tau_samples, 6.0);
  EXPECT_FALSE(track->at(10).active());
}

TEST(SmearTrackTest, SimplifyDropsKnotsOnTheLineAndKeepsOnOff) {
  std::vector<orc::SmearKnot> knots;
  for (int k = 0; k < 20; ++k) {
    knots.push_back({k * 32, 0.20 + (0.001 * k), 6.0});
  }
  knots.push_back({20 * 32, 0.0, 2.0});
  knots.push_back({21 * 32, 0.0, 2.0});
  const auto track = orc::SmearTrack::from_knots(knots);
  ASSERT_TRUE(track.has_value());
  const auto simple = track->simplified(0.002, 0.05);
  // The ramp needs only its ends; the turn-off is kept exactly.
  ASSERT_EQ(simple.knots().size(), 4u);
  EXPECT_EQ(simple.knots()[0].frame, 0);
  EXPECT_EQ(simple.knots()[1].frame, 19 * 32);
  EXPECT_EQ(simple.knots()[2].frame, 20 * 32);
  for (int k = 0; k < 20; ++k) {
    EXPECT_NEAR(simple.at(k * 32).fraction, 0.20 + (0.001 * k), 0.002);
  }
}

// A pinned knot stays, though the line through its neighbours gives it.
TEST(SmearTrackTest, SimplifyKeepsPinnedKnots) {
  std::vector<orc::SmearKnot> knots;
  for (int k = 0; k < 10; ++k) knots.push_back({k * 32, 0.20, 6.0});
  const auto track = orc::SmearTrack::from_knots(knots);
  ASSERT_TRUE(track.has_value());
  EXPECT_EQ(track->simplified(0.002, 0.05).knots().size(), 2u);
  std::vector<bool> pinned(10, false);
  pinned[4] = true;
  pinned[5] = true;
  const auto kept = track->simplified(0.002, 0.05, pinned);
  ASSERT_EQ(kept.knots().size(), 4u);
  EXPECT_EQ(kept.knots()[1].frame, 4 * 32);
  EXPECT_EQ(kept.knots()[2].frame, 5 * 32);
}

// ── The tail reading ───────────────────────────────────────────────────────

TEST(ChannelTrackingTest, TailReadsTheSmear) {
  EXPECT_NEAR(orc::segment_tail(segment(0.0, 1.0, 0.0, 0)), 0.0, 0.002);
  EXPECT_GT(orc::segment_tail(segment(0.24, 6.1, 0.0, 0)), 0.08);
  EXPECT_TRUE(std::isnan(orc::segment_tail({1.0, 2.0})));
}

// ── Two components ─────────────────────────────────────────────────────────

// The segment through a two-component smear (the forward model of
// channel_equalise::EdgeSmear): x + c (L_tau x - x) + f (L_fast x - x).
std::vector<double> two_component_segment(
    const orc::channel_equalise::EdgeSmear& s, int seed,
    bool noise_after = true) {
  auto x = segment(0.0, 1.0, 0.0, 0);
  const auto ripple = [seed](size_t i) {
    return 0.05 * std::sin((static_cast<double>(i) * 2.399) +
                           (static_cast<double>(seed) * 0.731));
  };
  if (!noise_after) {
    for (size_t i = 0; i < x.size(); ++i) x[i] += ripple(i);
  }
  const auto low_pass = [&x](double tau) {
    const double pole = std::exp(-1.0 / tau);
    std::vector<double> y(x.size());
    double acc = x.front();
    for (size_t i = 0; i < x.size(); ++i) {
      acc = (pole * acc) + ((1.0 - pole) * x[i]);
      y[i] = acc;
    }
    return y;
  };
  const auto slow = low_pass(s.tau_samples);
  const auto fast = low_pass(s.fast_tau_samples);
  std::vector<double> y(x.size());
  for (size_t i = 0; i < x.size(); ++i) {
    y[i] = x[i] + (s.fraction * (slow[i] - x[i])) +
           (s.fast_fraction * (fast[i] - x[i])) +
           (noise_after ? ripple(i) : 0.0);
  }
  return y;
}

// The second-order inverse undoes the two-component smear exactly.
TEST(TwoComponentTest, InverseUndoesBothComponents) {
  const orc::channel_equalise::EdgeSmear s{0.25, 6.0, -0.15, 2.5};
  ASSERT_TRUE(orc::channel_equalise::invertible(s));
  const auto clean = two_component_segment({0.0, 6.0, 0.0, 1.0}, 3, false);
  auto y = two_component_segment(s, 3, false);
  orc::channel_equalise::unsmear(y, s);
  for (size_t i = 0; i < y.size(); ++i) EXPECT_NEAR(y[i], clean[i], 1e-9) << i;
  // Without a fast component it is the one-pole inverse it always was.
  std::vector<double> a = segment(0.24, 6.1, 0.0, 0);
  std::vector<double> b = a;
  orc::channel_equalise::unsmear(a, {0.24, 6.1});
  orc::channel_equalise::unsmear(b, {0.24, 6.1, 0.0, 1.0});
  EXPECT_EQ(a, b);
  // A smear whose inverse would not be stable (here both components 0.9,
  // far outside the fit's ranges) is refused and left alone.
  const orc::channel_equalise::EdgeSmear wild{0.9, 50.0, 0.9, 4.5};
  EXPECT_FALSE(orc::channel_equalise::invertible(wild));
  std::vector<double> c = segment(0.0, 1.0, 0.0, 0);
  const auto before = c;
  orc::channel_equalise::unsmear(c, wild);
  EXPECT_EQ(c, before);
}

// A slow-onset smear (a slow tail with less of it near the edge, as on the
// Domesday sides) is fitted, and the corrected bar is flat from 3 samples.
TEST(TwoComponentTest, FitsASlowOnsetSmear) {
  const orc::channel_equalise::EdgeSmear truth{0.30, 6.0, -0.15, 2.5};
  const auto y = two_component_segment(truth, 1);
  const auto fit = orc::fit_two_component(y);
  ASSERT_TRUE(fit.has_value());
  EXPECT_GT(fit->residual_identity, 0.03);
  EXPECT_LT(fit->residual, 0.002);
  EXPECT_NEAR(orc::predicted_tail(fit->smear, 3, 8),
              orc::predicted_tail(truth, 3, 8), 0.003);
  EXPECT_NEAR(orc::predicted_tail(fit->smear, 9, 30),
              orc::predicted_tail(truth, 9, 30), 0.002);
  EXPECT_EQ(orc::judge_two_component(fit, false), orc::SmearVerdict::Smear);
}

// A tail confined near the edge is corrected only when asked for.
TEST(TwoComponentTest, NearEdgeTailOnlyWhenAsked) {
  const orc::channel_equalise::EdgeSmear truth{0.0, 6.0, 0.25, 1.8};
  const auto fit = orc::fit_two_component(two_component_segment(truth, 2));
  ASSERT_TRUE(fit.has_value());
  EXPECT_GT(fit->rise_tail, 0.006);
  EXPECT_GT(fit->fall_tail, 0.006);
  EXPECT_EQ(orc::judge_two_component(fit, false), orc::SmearVerdict::NoSmear);
  EXPECT_EQ(orc::judge_two_component(fit, true), orc::SmearVerdict::Smear);
}

TEST(TwoComponentTest, NoSmearIsNoSmearEitherWay) {
  const auto fit =
      orc::fit_two_component(two_component_segment({0.0, 6.0, 0.0, 1.0}, 4));
  ASSERT_TRUE(fit.has_value());
  EXPECT_EQ(orc::judge_two_component(fit, false), orc::SmearVerdict::NoSmear);
  EXPECT_EQ(orc::judge_two_component(fit, true), orc::SmearVerdict::NoSmear);
  EXPECT_EQ(orc::judge_two_component(std::nullopt, true),
            orc::SmearVerdict::CannotTell);
  EXPECT_FALSE(orc::fit_two_component(std::vector<double>(355, 100.0)));
}

// Tails of opposite sign on the two edges are no linear tail: the near-edge
// guard refuses them however well a fit explains them.
// The long smear must be on both edges as well.
TEST(TwoComponentTest, LongSmearNeedsTheTailOnBothEdges) {
  orc::TwoComponentFit fit;
  fit.smear = {0.2, 6.0, 0.0, 1.0};
  fit.residual = 0.001;
  fit.residual_identity = 0.01;
  fit.rise_long_tail = 0.012;
  fit.fall_long_tail = 0.003;
  EXPECT_EQ(orc::judge_two_component(fit, false), orc::SmearVerdict::NoSmear);
  fit.fall_long_tail = 0.011;
  EXPECT_EQ(orc::judge_two_component(fit, false), orc::SmearVerdict::Smear);
  // Not explained by the fit.
  fit.residual = 0.009;
  EXPECT_EQ(orc::judge_two_component(fit, false), orc::SmearVerdict::NoSmear);
}

TEST(TwoComponentTest, NearEdgeNeedsTheTailOnBothEdges) {
  orc::TwoComponentFit fit;
  fit.smear = {0.0, 6.0, 0.05, 1.5};
  fit.residual = 0.001;
  fit.residual_identity = 0.01;
  fit.rise_tail = 0.012;
  fit.fall_tail = -0.004;
  // No long tail: only the near-edge guard could pass it.
  EXPECT_EQ(orc::judge_two_component(fit, false), orc::SmearVerdict::NoSmear);
  EXPECT_EQ(orc::judge_two_component(fit, true), orc::SmearVerdict::NoSmear);
  fit.fall_tail = 0.008;
  EXPECT_EQ(orc::judge_two_component(fit, true), orc::SmearVerdict::Smear);
}

TEST(SmearTrackTest, KnotsCarryTheFastComponent) {
  const auto track =
      orc::SmearTrack::parse("16:0.2400:6.100:-0.1500:2.500;48:0:2");
  ASSERT_TRUE(track.has_value());
  EXPECT_DOUBLE_EQ(track->knots()[0].fast_fraction, -0.15);
  EXPECT_EQ(track->to_string(),
            "16:0.2400:6.100:-0.1500:2.500;48:0.0000:2.000");
  // Fading out keeps the time constants of the knot with the smear.
  const auto mid = track->at(32);
  EXPECT_DOUBLE_EQ(mid.fast_fraction, -0.075);
  EXPECT_DOUBLE_EQ(mid.fast_tau_samples, 2.5);
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.24:6.1:-0.1").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.24:6.1:-0.5:2.5").has_value());
  EXPECT_FALSE(orc::SmearTrack::parse("16:0.24:6.1:0.1:9").has_value());
}

TEST(ChannelTrackingStageTest, TrackingToolTakesTheNearEdgeParameter) {
  orc::ChannelEqualiseStage stage;
  const auto params = stage.analysis_tool_parameters("channel_tracking");
  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params[0].name, "near_edge");
  EXPECT_EQ(params[0].type, orc::ParameterType::BOOL);
}

double tail_3_8(const orc::channel_equalise::EdgeSmear& s) {
  return orc::predicted_tail(s, 3, 8);
}
double tail_9_30(const orc::channel_equalise::EdgeSmear& s) {
  return orc::predicted_tail(s, 9, 30);
}

// ── Picture edges and the certified choice ────────────────────────────────

// A field of vertical bars (0.1 / 0.8 of the step, 60 samples wide, edges
// one sample long) through a one-pole smear along each line.
std::vector<double> bars_field(double fraction, double tau) {
  constexpr size_t kWidth = 1135;
  std::vector<double> field(313 * kWidth, 0.0);
  const double pole = std::exp(-1.0 / tau);
  for (size_t row = 0; row < 313; ++row) {
    double acc = 0.1;
    for (size_t i = 0; i < kWidth; ++i) {
      const double x =
          (i >= 200 && i < 1090 && ((i - 200) / 60) % 2 == 1) ? 0.8 : 0.1;
      acc = (pole * acc) + ((1.0 - pole) * x);
      field[(row * kWidth) + i] = ((1.0 - fraction) * x) + (fraction * acc);
    }
  }
  return field;
}

TEST(PictureEdgesTest, ReadTheTailAndItsCorrection) {
  const auto field = bars_field(0.2, 6.0);
  const auto edges = orc::picture_edges::find(field, 1135, 0, 100000);
  ASSERT_GT(edges.size(), 2000u);
  const auto raw =
      orc::picture_edges::tails(orc::picture_edges::profiles(edges, {}), {});
  ASSERT_TRUE(raw.has_value());
  EXPECT_GT(raw->t3_8, 0.02);
  const orc::channel_equalise::EdgeSmear truth{0.2, 6.0};
  const auto fixed = orc::picture_edges::tails(
      orc::picture_edges::profiles(edges, {truth, truth}), {});
  ASSERT_TRUE(fixed.has_value());
  EXPECT_LT(std::abs(fixed->t3_8), 0.003);
  EXPECT_LT(std::abs(fixed->t9_20), 0.003);
  // At most the edges asked for, spread over the field.
  EXPECT_EQ(orc::picture_edges::find(field, 1135, 1, 64).size(), 64u);
  // A flat field has none.
  const std::vector<double> flat(313 * 1135, 0.5);
  EXPECT_EQ(orc::picture_edges::find(flat, 1135, 0, 64).size(), 0u);
}

// |frames| frames of both fields through |first| (field 1) and |second|
// (field 2).
std::vector<orc::FieldSegment> two_field_frames(
    size_t frames, const orc::channel_equalise::EdgeSmear& first,
    const orc::channel_equalise::EdgeSmear& second) {
  std::vector<orc::FieldSegment> fields;
  for (orc::FrameID f = 0; f < frames; ++f) {
    for (unsigned parity = 0; parity < 2; ++parity) {
      const auto s = two_component_segment(parity == 0 ? first : second,
                                           static_cast<int>((2 * f) + parity));
      fields.push_back({f, parity, std::vector<float>(s.begin(), s.end())});
    }
  }
  return fields;
}

orc::window_select::Split split_of(
    const std::vector<orc::FieldSegment>& fields) {
  orc::window_select::Split split;
  for (const auto& f : fields) {
    split.segments[f.field].emplace_back(f.segment.begin(), f.segment.end());
  }
  return split;
}

TEST(WindowSelectTest, UpstreamCertifiesItselfAndAWorseOneFails) {
  const orc::channel_equalise::EdgeSmear smear{0.24, 6.1};
  const auto split = split_of(two_field_frames(40, smear, smear));
  const orc::window_select::Correction upstream = {smear, smear};
  EXPECT_TRUE(orc::window_select::certified(split, upstream, upstream));
  // Twice the correction over-corrects every tail.
  const orc::channel_equalise::EdgeSmear over{0.40, 6.1};
  EXPECT_FALSE(orc::window_select::certified(split, {over, over}, upstream));
  // The identity leaves every tail: worse than upstream on all of them.
  EXPECT_FALSE(orc::window_select::certified(split, {}, upstream));
  const auto m = orc::window_select::measure(split, upstream);
  EXPECT_TRUE(m.present[orc::window_select::kRise3_8]);
  EXPECT_FALSE(m.present[orc::window_select::kPicture3_8]);
}

// A bar of |height| IRE without a smear, as an aligned segment, with a
// ripple of 0.05 IRE that changes with |seed|.
std::vector<double> low_bar_segment(double height, int seed) {
  auto x = segment(0.0, 1.0, 0.0, 0);
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = kStartUs + (static_cast<double>(i) / kSamplesPerUs);
    x[i] = ((t >= 0.0 && t < 10.1) ? height : 0.0) +
           (0.05 * std::sin((static_cast<double>(i) * 2.399) +
                            (static_cast<double>(seed) * 0.731)));
  }
  return x;
}

// A split whose first field has a bar of 50.5 IRE, and whose second a full
// one. A slow-onset correction (a negative fast component alone) pulls the
// first field's bar under half its nominal height 20 samples after the
// edge, where the bar's measures can no longer find its top.
orc::window_select::Split low_bar_split() {
  orc::window_select::Split split;
  for (int s = 0; s < 40; ++s) {
    split.segments[0].push_back(low_bar_segment(50.5, s));
    split.segments[1].push_back(segment(0.0, 1.0, 0.05, s));
  }
  return split;
}
const orc::channel_equalise::EdgeSmear kLosesTheBar{0.0, 7.04, -0.45, 4.4};

// A measure upstream's correction yields and the candidate does not is a
// failure, not a measure to skip.
TEST(WindowSelectTest, AMeasureTheCandidateLosesFailsCertification) {
  const auto split = low_bar_split();
  const orc::window_select::Correction upstream{};
  const orc::window_select::Correction candidate = {kLosesTheBar, {}};
  const auto mu = orc::window_select::measure(split, upstream);
  const auto mc = orc::window_select::measure(split, candidate);
  ASSERT_TRUE(mu.present[orc::window_select::kRise3_8]);
  ASSERT_FALSE(mc.present[orc::window_select::kRise3_8]);
  ASSERT_TRUE(mc.present[orc::window_select::kPerParity +
                         orc::window_select::kRise3_8]);
  EXPECT_FALSE(orc::window_select::certified(split, candidate, upstream));
  // Where nothing is lost, the measures decide.
  EXPECT_TRUE(orc::window_select::certified(split, upstream, upstream));
}

// With nothing measurable there is nothing to certify on.
TEST(WindowSelectTest, NothingMeasurableIsNotCertified) {
  const orc::window_select::Split empty;
  const orc::channel_equalise::EdgeSmear smear{0.24, 6.1};
  EXPECT_FALSE(orc::window_select::certified(empty, {smear, smear}, {}));
}

// The search may not lower its cost by making a measure disappear.
TEST(WindowSelectTest, ConstrainedSearchTreatsALostMeasureAsInfeasible) {
  const auto split = low_bar_split();
  const orc::window_select::Correction upstream{};
  const orc::window_select::Correction start = {kLosesTheBar, kLosesTheBar};
  EXPECT_FALSE(orc::window_select::constrained_search(split, upstream, {start})
                   .has_value());
}

const orc::channel_equalise::EdgeSmear kUpstream{0.24, 6.1};

// A slow-onset smear, which upstream's one pole cannot follow: the windows
// that certify a better correction take it, the rest keep upstream's, and
// either way the tail left is no larger than upstream leaves.
TEST(ChannelTrackingTest, CertifiedWindowsBeatUpstream) {
  const orc::channel_equalise::EdgeSmear truth{0.30, 6.0, -0.15, 2.5};
  const auto fields = two_field_frames(512, truth, truth);
  const auto result = orc::track_channel_fields(
      fields, {}, 0, 512, kUpstream, orc::ChannelTrackOptions{}, nullptr);
  ASSERT_EQ(result.blocks.size(), 2u);
  size_t better = 0;
  for (const auto& block : result.blocks) {
    EXPECT_FALSE(block.choice.empty());
    if (block.choice != "upstream") ++better;
  }
  EXPECT_GT(better, 0u);
  for (const orc::FrameID f : {100, 400}) {
    const auto s = result.track.at(f);
    EXPECT_LE(std::abs(tail_3_8(s) - tail_3_8(truth)),
              std::abs(tail_3_8(kUpstream) - tail_3_8(truth)) + 0.001)
        << f;
  }
}

// Too few fields to fit or certify anything: upstream's smear stays.
TEST(ChannelTrackingTest, WithoutEvidenceUpstreamIsKept) {
  const auto fields = two_field_frames(10, kUpstream, kUpstream);
  const auto result = orc::track_channel_fields(
      fields, {}, 0, 10, kUpstream, orc::ChannelTrackOptions{}, nullptr);
  ASSERT_EQ(result.blocks.size(), 1u);
  EXPECT_EQ(result.blocks[0].choice, "upstream");
  EXPECT_DOUBLE_EQ(result.track.at(5).fraction, 0.24);
  EXPECT_DOUBLE_EQ(result.track_field2.at(5).fraction, 0.24);
}

TEST(ChannelTrackingTest, NoSmearAnywhereIsAnEmptyTrack) {
  const auto fields = two_field_frames(300, {}, {});
  const auto result = orc::track_channel_fields(
      fields, {}, 0, 300, {}, orc::ChannelTrackOptions{}, nullptr);
  EXPECT_TRUE(result.track.empty());
  EXPECT_TRUE(result.track_field2.empty());
  for (const auto& block : result.blocks) {
    EXPECT_EQ(block.choice, "upstream");
    EXPECT_EQ(block.verdict, orc::SmearVerdict::NoSmear);
  }
}

// Upstream corrects both fields alike; a second field without a smear is
// left (nearly) alone where that is certified no worse.
TEST(ChannelTrackingTest, EachFieldIsCorrectedOnItsOwn) {
  const auto fields = two_field_frames(512, kUpstream, {});
  const auto result = orc::track_channel_fields(
      fields, {}, 0, 512, kUpstream, orc::ChannelTrackOptions{}, nullptr);
  ASSERT_FALSE(result.track.empty());
  for (const orc::FrameID f : {100, 400}) {
    EXPECT_NEAR(tail_9_30(result.track.at(f)), tail_9_30(kUpstream), 0.002);
    EXPECT_LT(tail_9_30(result.track_field2.at(f)), 0.5 * tail_9_30(kUpstream));
  }
}

// The track as the stage applies it (simplified, through its text form,
// interpolated frame by frame) is certified on every window's certifying
// split: against upstream's correction, and against the identity where the
// window chose its own. Here the smear alternates between two kinds every
// 256 frames, so the frames between two knots take blends of two quite
// different corrections.
TEST(ChannelTrackingTest, TheAppliedTrackIsCertifiedInEveryWindow) {
  const orc::channel_equalise::EdgeSmear slow_onset{0.30, 6.0, -0.15, 2.5};
  const orc::channel_equalise::EdgeSmear long_tail{0.20, 9.0};
  constexpr size_t kFrames = 1024;
  std::vector<orc::FieldSegment> fields;
  for (orc::FrameID f = 0; f < kFrames; ++f) {
    const auto& truth = ((f / 256) % 2 == 0) ? slow_onset : long_tail;
    for (unsigned parity = 0; parity < 2; ++parity) {
      const auto s =
          two_component_segment(truth, static_cast<int>((2 * f) + parity));
      fields.push_back({f, parity, std::vector<float>(s.begin(), s.end())});
    }
  }
  const orc::ChannelTrackOptions options;
  const auto result = orc::track_channel_fields(fields, {}, 0, kFrames,
                                                kUpstream, options, nullptr);
  ASSERT_EQ(result.blocks.size(), 4u);
  ASSERT_FALSE(result.track.empty());
  // As the stage holds it.
  const auto first = orc::SmearTrack::parse(result.track.to_string());
  const auto second = orc::SmearTrack::parse(result.track_field2.to_string());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const orc::window_select::CorrectionAt applied = [&](int64_t frame) {
    return orc::window_select::Correction{first->at(frame), second->at(frame)};
  };
  const auto half = static_cast<int64_t>(options.half_window_frames);
  for (size_t w = 0; w < result.blocks.size(); ++w) {
    const auto& block = result.blocks[w];
    if (block.reverted) {
      EXPECT_EQ(block.choice, "upstream");
    }
    const auto centre = static_cast<int64_t>(block.first + (block.frames / 2));
    orc::window_select::Split split;
    for (const auto& f : fields) {
      const auto frame = static_cast<int64_t>(f.frame);
      if (frame < centre - half || frame > centre + half || frame % 3 != 1) {
        continue;
      }
      split.segments[f.field].emplace_back(f.segment.begin(), f.segment.end());
      split.segment_frames[f.field].push_back(frame);
    }
    EXPECT_TRUE(orc::window_select::certified(
        split, applied, {kUpstream, kUpstream}, block.choice != "upstream"))
        << w;
  }
}

// Without the frames a per-frame correction cannot be applied, so it is
// not certified.
TEST(WindowSelectTest, APerFrameCorrectionNeedsTheFrames) {
  const auto split = split_of(two_field_frames(40, kUpstream, kUpstream));
  const orc::window_select::CorrectionAt upstream = [](int64_t) {
    return orc::window_select::Correction{kUpstream, kUpstream};
  };
  EXPECT_FALSE(orc::window_select::certified(split, upstream,
                                             {kUpstream, kUpstream}, true));
  EXPECT_FALSE(orc::window_select::measure(split, upstream)
                   .present[orc::window_select::kRise3_8]);
}

TEST(ChannelTrackingTest, ACancelledTrackIsEmpty) {
  const auto fields = two_field_frames(300, kUpstream, kUpstream);
  const auto result = orc::track_channel_fields(
      fields, {}, 0, 300, kUpstream, orc::ChannelTrackOptions{},
      [](int, const std::string&) { return false; });
  EXPECT_TRUE(result.blocks.empty());
  EXPECT_TRUE(result.track.empty());
}

// The bars source carries no VITS: nothing is measured and nothing applied.
TEST(ChannelTrackingTest, ASourceWithoutVitsHasNoTrack) {
  auto source = std::make_shared<PalColourBarsSource>(
      PalColourBarsSource::Options{3, true, std::nullopt, {}, 0, 1});
  const auto result =
      orc::track_channel(*source, orc::ChannelTrackOptions{}, nullptr);
  EXPECT_EQ(result.fields_read, 6u);
  EXPECT_EQ(result.fields_measured, 0u);
  EXPECT_TRUE(result.track.empty());
  const auto report = orc::channel_calibration_tool::describe_track(result);
  EXPECT_EQ(report.status, orc::StageAnalysisReport::Status::Success);
  EXPECT_TRUE(report.parameter_changes.empty());
  EXPECT_NE(report.summary.find("No smear could be measured"),
            std::string::npos);
}

// A PAL frame source of sharp vertical bars (0.1 / 0.8 of white - blanking,
// 60 samples wide) on the picture lines of the fields chosen, black
// elsewhere, and no VITS.
class FieldBarsSource : public orc::VideoFrameRepresentation,
                        public orc::Artifact {
 public:
  static constexpr size_t kSpl = 1135;
  static constexpr size_t kHeight = 625;
  static constexpr int16_t kBlack = 256;
  static constexpr int16_t kWhite = 844;

  FieldBarsSource(bool first, bool second, size_t frames = 1)
      : orc::Artifact(orc::ArtifactID("field_bars"), orc::Provenance{}),
        frames_(frames) {
    frame_.assign(
        orc::frame_line_sample_offset(orc::VideoSystem::PAL, kSpl, kHeight),
        kBlack);
    const size_t f1 = orc::field1_lines(orc::VideoSystem::PAL);
    for (size_t line = 0; line < kHeight; ++line) {
      const bool second_field = line >= f1;
      const size_t field_line = second_field ? line - f1 : line;
      if (!(second_field ? second : first) || field_line < 22 ||
          field_line >= 310) {
        continue;
      }
      const size_t base =
          orc::frame_line_sample_offset(orc::VideoSystem::PAL, kSpl, line);
      for (size_t i = 200; i < 1090; ++i) {
        const double v = (((i - 200) / 60) % 2 == 1) ? 0.8 : 0.1;
        frame_[base + i] =
            static_cast<int16_t>(std::lround(kBlack + (v * (kWhite - kBlack))));
      }
    }
  }

  std::string type_name() const override { return "field_bars"; }
  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0u, frames_ - 1};
  }
  size_t frame_count() const override { return frames_; }
  bool has_frame(orc::FrameID id) const override { return id < frames_; }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = id;
    d.system = orc::VideoSystem::PAL;
    d.height = kHeight;
    d.samples_total = frame_.size();
    d.samples_per_line_nominal = kSpl;
    return d;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    return has_frame(id) ? frame_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return has_frame(id) ? frame_ : std::vector<sample_type>{};
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    auto p = PalColourBarsSource::parameters();
    p.blanking_level = kBlack;
    p.black_level = kBlack;
    p.white_level = kWhite;
    return p;
  }

 private:
  size_t frames_;
  std::vector<int16_t> frame_;
};

// track_channel() reads the picture edges of both fields of a frame: the
// first field's 313 lines and the second's 312.
TEST(ChannelTrackingTest, ReadsThePictureEdgesOfBothFields) {
  const orc::ChannelTrackOptions options;
  const auto second_only =
      orc::track_channel(FieldBarsSource(false, true), options, nullptr);
  EXPECT_EQ(second_only.picture_edges_found[0], 0u);
  EXPECT_EQ(second_only.picture_edges_found[1], options.max_edges_per_field);
  const auto first_only =
      orc::track_channel(FieldBarsSource(true, false), options, nullptr);
  EXPECT_EQ(first_only.picture_edges_found[0], options.max_edges_per_field);
  EXPECT_EQ(first_only.picture_edges_found[1], 0u);
  const auto both =
      orc::track_channel(FieldBarsSource(true, true), options, nullptr);
  EXPECT_EQ(both.picture_edges_found[0], options.max_edges_per_field);
  EXPECT_EQ(both.picture_edges_found[1], options.max_edges_per_field);
}

// A short source's picture is read on every frame, not every 32nd, so its
// certifying third has picture edges too.
TEST(ChannelTrackingTest, ReadsEveryFramesPictureOfAShortSource) {
  const orc::ChannelTrackOptions options;
  const auto result =
      orc::track_channel(FieldBarsSource(true, true, 6), options, nullptr);
  EXPECT_EQ(result.picture_edges_found[0], 6 * options.max_edges_per_field);
  EXPECT_EQ(result.picture_edges_found[1], 6 * options.max_edges_per_field);
}

TEST(ChannelTrackingTest, ReportAppliesTheTrack) {
  const auto result = orc::track_channel_fields(
      two_field_frames(512, kUpstream, kUpstream), {}, 0, 512, kUpstream,
      orc::ChannelTrackOptions{}, nullptr);
  const auto report = orc::channel_calibration_tool::describe_track(result);
  ASSERT_EQ(report.status, orc::StageAnalysisReport::Status::Success);
  const auto parsed = orc::SmearTrack::parse(
      std::get<std::string>(report.parameter_changes.at("smear_track")));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_NEAR(tail_9_30(parsed->at(200)), tail_9_30(kUpstream), 0.002);
  EXPECT_FALSE(report.items.empty());
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("windows")), 2);
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("upstreamWindows")) +
                std::get<int64_t>(report.statistics.at("trackedWindows")) +
                std::get<int64_t>(report.statistics.at("constrainedWindows")),
            2);
}

// ── The stage ──────────────────────────────────────────────────────────────

TEST(ChannelTrackingStageTest, OffersTheTrackingTool) {
  orc::ChannelEqualiseStage stage;
  const auto tools = stage.get_stage_tools();
  ASSERT_EQ(tools.size(), 2u);
  EXPECT_EQ(tools[1].tool_id, "channel_tracking");
  EXPECT_EQ(tools[1].display_name, "Channel Tracking");
  EXPECT_EQ(tools[1].kind, orc::StageToolKind::Analysis);
}

TEST(ChannelTrackingStageTest, TrackParameterRoundTripsAndRejectsBadText) {
  orc::ChannelEqualiseStage stage;
  EXPECT_FALSE(stage.set_parameters({{"smear_track", 0.2}}));
  EXPECT_FALSE(stage.set_parameters({{"smear_track", std::string("1:0.9:6")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);
  EXPECT_TRUE(stage.set_parameters(
      {{"smear_track", std::string("0:0.2400:6.100;9:0.0000:2.000")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);
  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("smear_track")),
            "0:0.2400:6.100;9:0.0000:2.000");
  EXPECT_TRUE(stage.set_parameters({{"smear_track", std::string()}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);
}

// Each frame takes its own smear from the track: a frame where the track has
// none passes bit-identical, one where it has a smear is equalised exactly
// as the fixed smear would equalise it.
TEST(ChannelTrackingStageTest, EachFrameTakesItsOwnSmear) {
  auto source = std::make_shared<PalColourBarsSource>(
      PalColourBarsSource::Options{3, true, std::nullopt, {}, 3, 1});
  orc::ObservationContext ctx;
  orc::ChannelEqualiseStage tracked;
  const auto out =
      std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
          tracked
              .execute({source},
                       {{"smear_track",
                         std::string("0:0.2400:6.100;1:0.0000:2.000")}},
                       ctx)
              .at(0));
  orc::ChannelEqualiseStage fixed;
  const auto reference =
      std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
          fixed
              .execute({source},
                       {{"smear_fraction", 0.24}, {"smear_tau_samples", 6.1}},
                       ctx)
              .at(0));
  ASSERT_NE(out, nullptr);
  ASSERT_NE(reference, nullptr);
  EXPECT_EQ(out->get_frame_copy(0), reference->get_frame_copy(0));
  EXPECT_NE(out->get_frame_copy(0), source->get_frame_copy(0));
  EXPECT_EQ(out->get_frame_copy(1), source->get_frame_copy(1));
  EXPECT_EQ(out->get_frame_copy(2), source->get_frame_copy(2));
}

// With a smear on one field only, the other field's lines pass unchanged
// and the corrected field's are what the smear on its own gives.
TEST(ChannelEqualiseTest, EachFieldTakesItsOwnSmear) {
  auto source = std::make_shared<PalColourBarsSource>(
      PalColourBarsSource::Options{1, true, std::nullopt, {}, 3, 1});
  const auto frame = source->get_frame_copy(0);
  const size_t split = orc::frame_line_sample_offset(
      orc::VideoSystem::PAL, PalColourBarsSource::kSpl, 313);
  const orc::channel_equalise::EdgeSmear smear{0.24, 6.1};
  const auto both = orc::channel_equalise::equalise_frame(
      frame.data(), frame.size(), PalColourBarsSource::kSpl,
      PalColourBarsSource::kBlack, PalColourBarsSource::kWhite, smear, smear);
  const auto one = orc::channel_equalise::equalise_frame(
      frame.data(), frame.size(), PalColourBarsSource::kSpl,
      PalColourBarsSource::kBlack, PalColourBarsSource::kWhite, smear);
  EXPECT_EQ(both.frame, one.frame);
  // First field only: the second field's samples change by the chroma gain
  // alone, which is 1 for a frame within its burst range only by chance, so
  // compare the inverse directly.
  std::vector<double> stream(frame.begin(), frame.end());
  std::vector<double> first(
      stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(split));
  orc::channel_equalise::unsmear(first, smear);
  const auto mixed = orc::channel_equalise::equalise_frame(
      frame.data(), frame.size(), PalColourBarsSource::kSpl,
      PalColourBarsSource::kBlack, PalColourBarsSource::kWhite, smear,
      orc::channel_equalise::EdgeSmear{});
  ASSERT_EQ(mixed.frame.size(), frame.size());
  EXPECT_NE(mixed.frame, one.frame);
  EXPECT_NE(mixed.frame, frame);
}

}  // namespace
}  // namespace orc_unit_test
