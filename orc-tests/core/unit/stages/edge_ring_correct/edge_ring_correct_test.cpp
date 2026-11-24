/*
 * File:        edge_ring_correct_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for EdgeRingCorrectStage: the ring track's text
 *              form, the ring and halo removed from synthetic pictures, the
 *              certificate refusing a correction that does not help, and
 *              untouched frames passed bit-identical
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../../../orc/plugins/stages/edge_ring_correct/edge_ring.h"
#include "../../../../orc/plugins/stages/edge_ring_correct/edge_ring_calibration.h"
#include "../../../../orc/plugins/stages/edge_ring_correct/edge_ring_certify.h"
#include "../../../../orc/plugins/stages/edge_ring_correct/edge_ring_correct_stage.h"
#include "../../../../orc/plugins/stages/edge_ring_correct/edge_ring_track.h"

namespace orc_unit_test {
namespace {

using orc::edge_ring::FieldImage;
using orc::edge_ring::RingTrack;
using orc::edge_ring::WindowCorrection;

constexpr size_t kSpl = 1135;
constexpr size_t kHeight = 625;
constexpr int16_t kBlack = 282;
constexpr int16_t kWhite = 844;

// The ring after a rising step, per unit step, t samples after its crossing.
double ring(double t) {
  return t < 1.0 ? 0.0
                 : -0.09 * std::exp(-t / 4.0) *
                       std::cos(2.0 * M_PI * 0.28 * (t - 1.0));
}
// The halo after a thin dark line, per unit depth.
double halo(double t) {
  return t < 2.0 ? 0.0
                 : 0.12 * std::exp(-(t - 2.0) / 3.0) *
                       std::sin(M_PI * (t - 2.0) / 4.0 + 0.6);
}

// A step from a to b with its crossing at c, 1.5 samples wide.
double step(double n, double c, double a, double b) {
  const double u = std::clamp((n - c) / 1.5 + 0.5, 0.0, 1.0);
  return a + (b - a) * (0.5 - 0.5 * std::cos(M_PI * u));
}

// A black and white picture: on the upper half, bars of white on grey and
// black (sharp rising and falling steps); on the lower half, thin dark lines
// on white. Crossings drift by 0.03 samples a line so the fractional
// positions are all represented. Optionally the ring and halo, and noise.
std::vector<int16_t> picture(size_t frame, bool ringing, int noise) {
  std::vector<int16_t> x(
      orc::frame_line_sample_offset(orc::VideoSystem::PAL, kSpl, kHeight),
      kBlack);
  const size_t f1 = orc::field1_lines(orc::VideoSystem::PAL);
  uint32_t seed = 12345u + 7919u * static_cast<uint32_t>(frame);
  auto rnd = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
  };
  for (size_t line = 0; line < kHeight; ++line) {
    const size_t fl = line < f1 ? line : line - f1;
    const size_t base =
        orc::frame_line_sample_offset(orc::VideoSystem::PAL, kSpl, line);
    const double drift = 0.03 * static_cast<double>(fl) + 0.37 * frame;
    for (size_t n = 0; n < kSpl; ++n) {
      double y = 0.0;  // 0..1 of black..white
      if (fl >= 20 && fl < 310 && n >= 180 && n < 1105) {
        const double d = static_cast<double>(n);
        if (fl < 160) {
          // period 90: grey 0.3 -> white at +0, white -> black at +45,
          // black -> grey at +70 (each step with flat grounds of 20+)
          const double p =
              std::fmod(d - 180.0 + 90.0 - std::fmod(drift, 90.0), 90.0);
          const double c0 = d - p;
          y = 0.3;
          y = step(d, c0 + 10.0, y, 1.0);
          if (d > c0 + 30.0) y = step(d, c0 + 45.0, 1.0, 0.0);
          if (d > c0 + 60.0) y = step(d, c0 + 70.0, 0.0, 0.3);
          if (ringing) {
            for (double c : {c0 + 10.0, c0 + 70.0}) {
              const double s = c == c0 + 10.0 ? 0.7 : 0.3;
              y += s * ring(d - c);
            }
            y += -1.0 * 0.5 * ring(d - (c0 + 45.0));  // falling: half as much
          }
        } else {
          const double p =
              std::fmod(d - 180.0 + 60.0 - std::fmod(drift, 60.0), 60.0);
          const double c = d - p + 30.0;
          const double t = d - c;
          y = 0.85 - 0.6 * std::exp(-0.5 * t * t / 0.8);
          if (ringing) y += 0.6 * halo(t);
        }
      }
      double v = kBlack + y * (kWhite - kBlack);
      if (noise > 0) v += (rnd() - 0.5) * 2.0 * noise;
      x[base + n] = static_cast<int16_t>(std::lround(v));
    }
  }
  return x;
}

class PictureSource : public orc::VideoFrameRepresentation,
                      public orc::Artifact {
 public:
  PictureSource(size_t frames, bool ringing, int noise)
      : orc::Artifact(orc::ArtifactID("edge_ring_pictures"),
                      orc::Provenance{}) {
    for (size_t f = 0; f < frames; ++f)
      frames_.push_back(picture(f, ringing, noise));
  }
  std::string type_name() const override { return "edge_ring_pictures"; }
  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0u, frames_.size() - 1};
  }
  size_t frame_count() const override { return frames_.size(); }
  bool has_frame(orc::FrameID id) const override { return id < frames_.size(); }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = id;
    d.system = orc::VideoSystem::PAL;
    d.height = kHeight;
    d.samples_total = frames_[id].size();
    d.samples_per_line_nominal = kSpl;
    return d;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    return has_frame(id) ? frames_[id].data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return has_frame(id) ? frames_[id] : std::vector<sample_type>{};
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters p;
    p.system = orc::VideoSystem::PAL;
    p.frame_width_nominal = static_cast<int32_t>(kSpl);
    p.frame_height = static_cast<int32_t>(kHeight);
    p.blanking_level = 256;
    p.black_level = kBlack;
    p.white_level = kWhite;
    p.active_video_start = orc::kPalActiveVideoStart;
    p.active_video_end = orc::kPalActiveVideoEnd;
    return p;
  }

 private:
  std::vector<std::vector<int16_t>> frames_;
};

FieldImage field(const std::vector<int16_t>& frame, int which) {
  const size_t f1 = orc::field1_lines(orc::VideoSystem::PAL);
  return orc::edge_ring::field_image(frame.data(), kSpl, which ? f1 : 0,
                                     which ? kHeight - f1 : f1, kBlack, kWhite);
}

orc::edge_ring::ProfileSums sums_of(const std::vector<int16_t>& frame) {
  orc::edge_ring::ProfileSums s;
  for (int k = 0; k < 2; ++k) {
    const auto f = field(frame, k);
    s += orc::edge_ring::measure(f, orc::edge_ring::find_features(f, true));
  }
  return s;
}

// Mean |excess| of the rising steps at +2..+8 samples and of the lines'
// asymmetry at +2..+8, per unit step / depth.
struct Residual {
  double rise = 0.0;
  double line = 0.0;
};
Residual residual(const orc::edge_ring::ProfileSums& s) {
  Residual r;
  int nr = 0, nl = 0;
  for (int j = 0; j < orc::edge_ring::kStepBins; ++j) {
    const double t =
        -orc::edge_ring::kPre + static_cast<double>(j) / orc::edge_ring::kUp;
    if (t < 2.0 || t > 8.0 || s.rise_count[j] <= 0) continue;
    r.rise += std::abs(s.rise_sum[j] / s.rise_count[j]);
    ++nr;
  }
  for (int j = 0; j < orc::edge_ring::kLineBins; ++j) {
    const double t = static_cast<double>(j) / orc::edge_ring::kUp;
    if (t < 2.0 || t > 8.0 || s.line_count[j] <= 0) continue;
    r.line += std::abs(s.line_sum[j] / s.line_count[j]);
    ++nl;
  }
  if (nr) r.rise /= nr;
  if (nl) r.line /= nl;
  return r;
}

std::shared_ptr<const orc::VideoFrameRepresentation> run(
    orc::EdgeRingCorrectStage& stage,
    const std::shared_ptr<PictureSource>& source, const std::string& track) {
  orc::ObservationContext ctx;
  const auto outputs = stage.execute({source}, {{"ring_track", track}}, ctx);
  EXPECT_EQ(outputs.size(), 1u);
  return std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs.at(0));
}

TEST(EdgeRingTrackTest, TextFormRoundTrips) {
  WindowCorrection w;
  w.first_frame = 10;
  w.end_frame = 20;
  w.adapt = true;
  w.gains = {1.0, 0.5, 0.0};
  for (int j = 0; j < orc::edge_ring::kStepBins; ++j)
    w.templates.rise[j] = 0.001 * (j % 7) - 0.003;
  w.templates.line[40] = -0.123456;
  const auto t = RingTrack::from_windows({w});
  ASSERT_TRUE(t.has_value());
  const auto back = RingTrack::parse(t->to_string());
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->to_string(), t->to_string());
  ASSERT_EQ(back->windows().size(), 1u);
  EXPECT_TRUE(back->windows()[0].adapt);
  EXPECT_DOUBLE_EQ(back->windows()[0].gains.fall, 0.5);
  EXPECT_NEAR(back->windows()[0].templates.line[40], -0.12346, 1e-9);
  EXPECT_EQ(back->at(9), nullptr);
  EXPECT_NE(back->at(10), nullptr);
  EXPECT_NE(back->at(19), nullptr);
  EXPECT_EQ(back->at(20), nullptr);
}

TEST(EdgeRingTrackTest, RefusesMalformedOrOverlappingWindows) {
  EXPECT_FALSE(RingTrack::parse("garbage").has_value());
  EXPECT_FALSE(RingTrack::parse("1-2:x:1,1,1:0:0:0").has_value());
  WindowCorrection a, b;
  a.first_frame = 0;
  a.end_frame = 10;
  b.first_frame = 5;
  b.end_frame = 15;
  EXPECT_FALSE(RingTrack::from_windows({a, b}).has_value());
  a.gains.rise = 1.5;
  EXPECT_FALSE(RingTrack::from_windows({a}).has_value());
  EXPECT_TRUE(RingTrack::parse("").has_value());
}

TEST(EdgeRingTest, FindsStepsAndLinesOnTheLuma) {
  const auto f = field(picture(0, false, 0), 0);
  const auto feats = orc::edge_ring::find_features(f, true);
  int rises = 0, falls = 0;
  for (const auto& s : feats.steps) (s.size > 0 ? rises : falls) += 1;
  EXPECT_GT(rises, 500);
  EXPECT_GT(falls, 200);
  EXPECT_GT(feats.lines.size(), 500u);
  // No chroma: most are measured on the composite line itself (a step whose
  // ground holds a neighbouring edge's energy at the subcarrier goes to the
  // comb instead).
  size_t on_line = 0;
  for (const auto& s : feats.steps) on_line += s.mode == 0;
  EXPECT_GT(on_line, feats.steps.size() / 2);
}

TEST(EdgeRingTest, FittedTemplatesRemoveTheRingAndHalo) {
  std::vector<std::vector<int16_t>> fit_frames, test_frames;
  // eight line-art frames: the line term needs kMinLineArtFrames
  orc::edge_ring::ProfileSums fit_sums;
  for (size_t f = 0; f < 8; ++f) fit_sums += sums_of(picture(f, true, 2));
  auto w = orc::edge_ring::fit(fit_sums, fit_sums, 8);
  ASSERT_TRUE(w.has_value());
  EXPECT_EQ(w->gains.rise, 1.0);
  EXPECT_EQ(w->gains.line, 1.0);
  const auto input = picture(5, true, 2);
  auto output = input;
  ASSERT_TRUE(orc::edge_ring::correct_frame(input.data(), output.data(), kSpl,
                                            kHeight, kBlack, kWhite, *w));
  const auto before = residual(sums_of(input));
  const auto after = residual(sums_of(output));
  EXPECT_GT(before.rise, 0.01);
  EXPECT_LT(after.rise, 0.25 * before.rise);
  EXPECT_GT(before.line, 0.01);
  EXPECT_LT(after.line, 0.25 * before.line);
  // Outside the picture nothing changes.
  for (size_t n = 0; n < kSpl; ++n) EXPECT_EQ(output[n], input[n]);
}

std::vector<orc::edge_ring::CertifyField> certify_fields(size_t first,
                                                         size_t frames,
                                                         bool ringing) {
  std::vector<orc::edge_ring::CertifyField> out;
  for (size_t f = first; f < first + frames; ++f) {
    const auto x = picture(f, ringing, 2);
    out.emplace_back(field(x, 0));
    out.emplace_back(field(x, 1));
  }
  return out;
}

TEST(EdgeRingCertifyTest, CertifiesAWindowThatRings) {
  std::vector<FieldImage> fit;
  for (size_t f = 0; f < 8; ++f) {
    const auto x = picture(f, true, 2);
    fit.push_back(field(x, 0));
    fit.push_back(field(x, 1));
  }
  const auto r = orc::calibrate_window(0, 100, fit, certify_fields(10, 8, true),
                                       certify_fields(20, 8, true));
  EXPECT_TRUE(r.fitted);
  ASSERT_TRUE(r.accepted);
  const auto& m = r.held_out.measures.at("rise_post");
  EXPECT_LT(m.after, 0.5 * m.before);
  EXPECT_LT(r.held_out.measures.at("halo").after,
            0.5 * r.held_out.measures.at("halo").before);
}

// Three line-art frames are too few to certify the line term on: the ring is
// still corrected, the halo is not.
TEST(EdgeRingCertifyTest, NoLineTermWithFewLineArtFrames) {
  std::vector<FieldImage> fit;
  for (size_t f = 0; f < 8; ++f) {
    const auto x = picture(f, true, 2);
    fit.push_back(field(x, 0));
    fit.push_back(field(x, 1));
  }
  const auto r = orc::calibrate_window(0, 100, fit, certify_fields(10, 3, true),
                                       certify_fields(20, 3, true));
  ASSERT_TRUE(r.accepted);
  EXPECT_EQ(r.chosen.gains.line, 0.0);
  EXPECT_GT(r.chosen.gains.rise, 0.0);
}

// Clean pictures: whatever the fit finds is noise, and nothing is certain to
// help, so the window stays uncorrected.
TEST(EdgeRingCertifyTest, LeavesACleanWindowAlone) {
  std::vector<FieldImage> fit;
  for (size_t f = 0; f < 3; ++f) {
    const auto x = picture(f, false, 2);
    fit.push_back(field(x, 0));
    fit.push_back(field(x, 1));
  }
  const auto r = orc::calibrate_window(
      0, 100, fit, certify_fields(10, 4, false), certify_fields(20, 4, false));
  EXPECT_FALSE(r.accepted);
  EXPECT_FALSE(r.reasons.empty());
  for (const auto& [name, m] : r.held_out.measures) {
    if (m.measured) {
      EXPECT_EQ(m.after, m.before) << name;
    }
  }
}

TEST(EdgeRingCorrectStageTest, EmptyTrackPassesFramesThrough) {
  auto source = std::make_shared<PictureSource>(2, true, 2);
  orc::EdgeRingCorrectStage stage;
  const auto out = run(stage, source, "");
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->get_frame(0), source->get_frame(0));
  EXPECT_EQ(out->get_frame_copy(1), source->get_frame_copy(1));
}

TEST(EdgeRingCorrectStageTest, CalibratedTrackCorrectsItsWindowsOnly) {
  auto source = std::make_shared<PictureSource>(36, true, 2);
  orc::EdgeRingCalibrationOptions o;
  o.window_frames = 18;
  o.frames_per_third = 3;
  o.probe_stride = 1;
  o.min_spacing = 1;
  const auto c = orc::calibrate_edge_ring(*source, o, nullptr);
  ASSERT_EQ(c.windows.size(), 2u);
  ASSERT_FALSE(c.track.empty());
  orc::EdgeRingCorrectStage stage;
  const auto out = run(stage, source, c.track.to_string());
  const auto input = source->get_frame_copy(30);
  const auto output = out->get_frame_copy(30);
  ASSERT_EQ(output.size(), input.size());
  EXPECT_LT(residual(sums_of(output)).rise,
            0.3 * residual(sums_of(input)).rise);
  // get_line reads the same corrected frame.
  const int16_t* line = out->get_line(30, 100);
  ASSERT_NE(line, nullptr);
  const size_t base =
      orc::frame_line_sample_offset(orc::VideoSystem::PAL, kSpl, 100);
  for (size_t n = 0; n < kSpl; ++n) EXPECT_EQ(line[n], output[base + n]);
  // A track whose window holds only frame 0 leaves frame 1 as it was.
  const auto& w0 = c.track.windows().front();
  auto only = w0;
  only.first_frame = 0;
  only.end_frame = 1;
  const auto t = RingTrack::from_windows({only});
  ASSERT_TRUE(t.has_value());
  orc::EdgeRingCorrectStage stage2;
  const auto out2 = run(stage2, source, t->to_string());
  EXPECT_EQ(out2->get_frame(1), source->get_frame(1));
  EXPECT_NE(out2->get_frame_copy(0), source->get_frame_copy(0));
}

TEST(EdgeRingCorrectStageTest, RefusesAMalformedTrack) {
  orc::EdgeRingCorrectStage stage;
  EXPECT_FALSE(stage.set_parameters({{"ring_track", std::string("1-2:f")}}));
  EXPECT_FALSE(stage.set_parameters({{"other", std::string()}}));
  EXPECT_TRUE(stage.set_parameters({{"ring_track", std::string()}}));
}

}  // namespace
}  // namespace orc_unit_test
