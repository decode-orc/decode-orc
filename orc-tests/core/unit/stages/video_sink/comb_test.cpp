/*
 * File:        comb_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit test(s) for Comb decoder core paths
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/decoders/comb.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <cmath>

namespace orc_unit_test {
namespace {

orc::SourceParameters make_ntsc_video_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::NTSC;
  p.frame_width_nominal = 32;
  p.active_video_start = 16;
  p.active_video_end = 24;
  p.first_active_frame_line = 0;
  p.last_active_frame_line = 6;
  return p;
}

// Helper owning wrapper for tests: holds the sample buffers and the
// non-owning SourceField that points into them.
struct OwnedField {
  std::vector<int16_t> composite_buf;
  std::vector<int16_t> luma_buf;
  std::vector<int16_t> chroma_buf;
  SourceField field;

  static OwnedField makeYC(bool is_first_field, int16_t luma_base,
                           int16_t chroma_base, int width, int height) {
    OwnedField of;
    of.field.is_yc = true;
    of.field.is_first_field = is_first_field;
    of.field.field_phase_id = is_first_field ? 1 : 2;
    of.field.line_count = static_cast<size_t>(height);
    of.field.samples_per_line = static_cast<size_t>(width);

    of.luma_buf.reserve(static_cast<size_t>(width * height));
    of.chroma_buf.reserve(static_cast<size_t>(width * height));
    for (int line = 0; line < height; ++line) {
      for (int x = 0; x < width; ++x) {
        of.luma_buf.push_back(static_cast<int16_t>(luma_base + line * 4 + x));
        of.chroma_buf.push_back(
            static_cast<int16_t>(chroma_base + ((x % 4) * 10)));
      }
    }
    // Pointers remain valid: std::vector move preserves data address.
    of.field.luma_data = of.luma_buf.data();
    of.field.chroma_data = of.chroma_buf.data();
    return of;
  }

  static OwnedField makeComposite(bool is_first_field, int16_t base, int width,
                                  int height) {
    OwnedField of;
    of.field.is_yc = false;
    of.field.is_first_field = is_first_field;
    of.field.field_phase_id = is_first_field ? 1 : 2;
    of.field.line_count = static_cast<size_t>(height);
    of.field.samples_per_line = static_cast<size_t>(width);

    of.composite_buf.reserve(static_cast<size_t>(width * height));
    for (int line = 0; line < height; ++line) {
      for (int x = 0; x < width; ++x) {
        of.composite_buf.push_back(static_cast<int16_t>(base + line * 8 + x));
      }
    }
    of.field.data = of.composite_buf.data();
    return of;
  }
};

}  // namespace

TEST(CombTest, Configuration_LookAroundDependsOnDimensions) {
  Comb::Configuration c2d;
  c2d.dimensions = 2;

  Comb::Configuration c3d;
  c3d.dimensions = 3;

  EXPECT_EQ(c2d.getLookBehind(), 0);
  EXPECT_EQ(c2d.getLookAhead(), 0);
  EXPECT_EQ(c3d.getLookBehind(), 1);
  EXPECT_EQ(c3d.getLookAhead(), 1);
}

TEST(CombTest, DecodeFramesYcPath_PreservesLumaInActiveRegion) {
  const auto params = make_ntsc_video_params();

  Comb::Configuration config;
  config.dimensions = 2;
  config.phaseCompensation = false;

  Comb decoder;
  decoder.updateConfiguration(params, config);

  auto first_owned = OwnedField::makeYC(true, 1000, 2000, 32, 4);
  auto second_owned = OwnedField::makeYC(false, 3000, 4000, 32, 4);

  std::vector<SourceField> fields = {first_owned.field, second_owned.field};
  std::vector<ComponentFrame> output(1);

  decoder.decodeFrames(fields, 0, 2, output);

  EXPECT_EQ(output[0].getWidth(), 32);
  EXPECT_EQ(output[0].getHeight(), 525);  // NTSC: 263*2-1

  const double* line0 = output[0].y(0);
  const double* line1 = output[0].y(1);

  EXPECT_DOUBLE_EQ(line0[16],
                   static_cast<double>(first_owned.field.luma_data[16]));
  EXPECT_DOUBLE_EQ(line0[20],
                   static_cast<double>(first_owned.field.luma_data[20]));
  EXPECT_DOUBLE_EQ(line1[16],
                   static_cast<double>(second_owned.field.luma_data[16]));
  EXPECT_DOUBLE_EQ(line1[20],
                   static_cast<double>(second_owned.field.luma_data[20]));
}

TEST(CombTest, DecodeFramesCompositePath_MatchesGoldenVector) {
  const auto params = make_ntsc_video_params();

  Comb::Configuration config;
  config.dimensions = 2;
  config.phaseCompensation = false;

  Comb decoder;
  decoder.updateConfiguration(params, config);

  auto first_owned = OwnedField::makeComposite(true, 2500, 32, 4);
  auto second_owned = OwnedField::makeComposite(false, 2600, 32, 4);

  std::vector<SourceField> fields = {first_owned.field, second_owned.field};
  std::vector<ComponentFrame> output(1);

  decoder.decodeFrames(fields, 0, 2, output);

  EXPECT_EQ(output[0].getWidth(), 32);
  EXPECT_EQ(output[0].getHeight(), 525);  // NTSC: 263*2-1

  const double* y0 = output[0].y(0);
  const double* u0 = output[0].u(0);
  const double* v0 = output[0].v(0);

  EXPECT_DOUBLE_EQ(y0[16], 2516.0);
  EXPECT_DOUBLE_EQ(y0[20], 2520.0);
  EXPECT_DOUBLE_EQ(u0[16], 0.0);
  EXPECT_DOUBLE_EQ(u0[20], 0.0);
  EXPECT_DOUBLE_EQ(v0[16], 0.0);
  EXPECT_DOUBLE_EQ(v0[20], 0.0);
  EXPECT_TRUE(std::isfinite(y0[16]));
  EXPECT_TRUE(std::isfinite(u0[16]));
  EXPECT_TRUE(std::isfinite(v0[16]));
}

TEST(CombTest, InvalidConfiguration_DoesNotAttemptDecode) {
  auto params = make_ntsc_video_params();
  params.frame_width_nominal = 8;

  Comb::Configuration config;
  config.dimensions = 2;

  Comb decoder;
  std::vector<SourceField> fields(2);
  std::vector<ComponentFrame> output(1);

  EXPECT_NO_FATAL_FAILURE({
    decoder.updateConfiguration(params, config);
    decoder.decodeFrames(fields, 0, 2, output);
  });

  EXPECT_EQ(output[0].getWidth(), -1);
  EXPECT_EQ(output[0].getHeight(), -1);
}

// ----------------------------------------------------------------------------
// NTSC burst detection at CVBS_U10_4FSC 10-bit levels
// ----------------------------------------------------------------------------
// Regression test: the burst-norm underflow guard in detectBurst() must be
// below the actual burst amplitude so that phase-compensated I/Q demodulation
// (splitIQlocked) produces non-zero chroma from a valid NTSC composite signal.
//
// The old guard (≈ 1016) was calibrated for the legacy 16-bit TBC domain; in
// the CVBS_U10_4FSC 10-bit domain the nominal burst norm is ≈ 56, so the old
// value clamped burstNorm to 1016 and reduced the normalised burst vector to
// ≈ 5 % of its true value, erasing all colour.
//
// Signal construction (SMPTE 170M-2004):
//   – Frame width = 200; burst window = samples 72–107 (kNtscColourBurstStart
//     to kNtscColourBurstEnd).
//   – Burst amplitude = 40 IRE p-p (20 IRE half-amplitude) at CVBS_U10_4FSC
//     levels: half_amp = 20 × (kNtscWhite − kNtscBlanking) / 100 ≈ 112.
//     Pattern: sin(π i/2) → {0, +half_amp, 0, −half_amp} per sample.
//   – Active video (samples 120–159): blanking + chroma × sin(π h / 2) with
//     chroma = 10 IRE × ire_scale ≈ 56 units.
//   – Both fields carry the same signal to exercise the 1D path.
//
// Expected result: at least one sample in the active area has |U| > threshold
// after phase-compensated 1D decoding.
TEST(CombTest, PhaseCompensation_BurstAtNtscCvbsLevels_ProducesNonZeroChroma) {
  // SMPTE 170M-2004 §4.1: 525-line/60 Hz system at 4FSC.
  orc::SourceParameters params;
  params.system = orc::VideoSystem::NTSC;
  params.frame_width_nominal = 200;
  params.active_video_start = 120;
  params.active_video_end = 160;
  params.first_active_frame_line = 0;
  params.last_active_frame_line = 4;

  // phaseCompensation = true exercises splitIQlocked() → detectBurst(),
  // which is the path that contained the incorrect underflow guard.
  // dimensions = 1 uses only the 1D comb result so the test is self-contained.
  Comb::Configuration config;
  config.dimensions = 1;
  config.phaseCompensation = true;

  Comb decoder;
  decoder.updateConfiguration(params, config);

  // SMPTE 170M-2004 Table 4: burst = 40 IRE p-p → half-amplitude = 20 IRE.
  const double ire_scale =
      static_cast<double>(orc::kNtscWhite - orc::kNtscBlanking) / 100.0;
  const auto burst_half_amp =
      static_cast<int16_t>(std::lround(20.0 * ire_scale));  // ≈ 112
  const auto chroma_amp =
      static_cast<int16_t>(std::lround(10.0 * ire_scale));  // ≈ 56

  // Build a single-line template: blanking everywhere, with burst at 72–107
  // and chroma subcarrier in the active area.
  // sin(π i/2) pattern → {0, +1, 0, −1} for i%4 = {0, 1, 2, 3}.
  auto make_ntsc_line = [&]() -> std::vector<int16_t> {
    std::vector<int16_t> line(200, orc::kNtscBlanking);

    // SMPTE 170M-2004 §6.4: colour burst at positions 72–107.
    for (int i = orc::kNtscColourBurstStart; i < orc::kNtscColourBurstEnd;
         ++i) {
      const int ph = i % 4;
      const int16_t s = (ph == 1)   ? burst_half_amp
                        : (ph == 3) ? -burst_half_amp
                                    : 0;
      line[i] = static_cast<int16_t>(orc::kNtscBlanking + s);
    }

    // Active video: luma at black level + subcarrier chroma.
    for (int h = params.active_video_start; h < params.active_video_end; ++h) {
      const int ph = h % 4;
      const int16_t c = (ph == 1) ? chroma_amp : (ph == 3) ? -chroma_amp : 0;
      line[h] = static_cast<int16_t>(orc::kNtscBlack + c);
    }

    return line;
  };

  // Build two 4-line fields with the same NTSC signal on every line.
  auto build_owned_field = [&](bool is_first_field) -> OwnedField {
    OwnedField of;
    of.field.is_yc = false;
    of.field.is_first_field = is_first_field;
    of.field.field_phase_id = is_first_field ? 1 : 2;
    of.field.line_count = 4;
    of.field.samples_per_line = 200;

    const auto line = make_ntsc_line();
    for (int l = 0; l < 4; ++l) {
      of.composite_buf.insert(of.composite_buf.end(), line.begin(), line.end());
    }
    of.field.data = of.composite_buf.data();
    return of;
  };

  auto field1 = build_owned_field(true);
  auto field2 = build_owned_field(false);

  std::vector<SourceField> fields = {field1.field, field2.field};
  std::vector<ComponentFrame> output(1);
  decoder.decodeFrames(fields, 0, 2, output);

  // With a correctly normalised burst vector the demodulated U channel must
  // be significantly non-zero in the active video area.  The old guard
  // (≈ 1016) would reduce the burst vector to ≈ 5 % of its true value,
  // producing |U| < 1; the corrected guard (≈ 6.65) lets the full burst
  // through (burstNorm ≈ 56) giving |U| > 10 for 10 IRE chroma.
  const double kChromaThreshold = 1.0;
  bool found_chroma = false;
  const double* u0 = output[0].u(0);
  if (u0 != nullptr) {
    for (int h = params.active_video_start; h < params.active_video_end; ++h) {
      if (std::abs(u0[h]) > kChromaThreshold) {
        found_chroma = true;
        break;
      }
    }
  }

  EXPECT_TRUE(found_chroma)
      << "Phase-compensated NTSC decode must produce non-zero chroma from a "
         "40 IRE p-p burst at CVBS_U10_4FSC 10-bit levels (kNtscBlanking="
      << orc::kNtscBlanking << ", kNtscWhite=" << orc::kNtscWhite
      << "). burstNorm for 20 IRE half-amplitude ≈ " << (20.0 * ire_scale / 2)
      << "; the underflow guard must remain well below this value.";
}

// ----------------------------------------------------------------------------
// Per-field colour phase (issue #324)
// ----------------------------------------------------------------------------
// The comb derives each line's burst phase from its field's phase id.  The two
// fields of an NTSC frame sit at opposite subcarrier phase on the same field
// line (field 2 line 0 starts 263 x 910 samples after field 1 line 0, and
// 910 samples is 227.5 subcarrier cycles), so each field needs its own id.
// Handing both fields one frame-level colour_frame_index inverts every field-2
// line in the non-compensated demodulator, and makes the 3D filter accept
// in-phase cross-field candidates as out-of-phase ones, leaving subcarrier in
// the luma along moving edges.
namespace {

constexpr int kNtscSamplesPerLine = 910;
constexpr int kNtscFieldLines = 263;
constexpr double kPi = 3.14159265358979323846;

orc::SourceParameters make_full_ntsc_video_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::NTSC;
  p.frame_width_nominal = kNtscSamplesPerLine;
  p.active_video_start = 200;
  p.active_video_end = 700;
  p.first_active_frame_line = 40;
  p.last_active_frame_line = 60;
  return p;
}

// One field of a flat colour: black-level luma plus a subcarrier that runs
// continuously through the frame's time line, starting at field_start_sample.
OwnedField make_flat_colour_field(bool is_first_field, int32_t field_phase_id,
                                  int64_t field_start_sample) {
  const double ire_scale =
      static_cast<double>(orc::kNtscWhite - orc::kNtscBlanking) / 100.0;
  const double chroma_amp = 20.0 * ire_scale;
  const double theta = kPi / 3.0;  // puts energy on both demodulator axes

  OwnedField of;
  of.field.is_yc = false;
  of.field.is_first_field = is_first_field;
  of.field.field_phase_id = field_phase_id;
  of.field.line_count = kNtscFieldLines;
  of.field.samples_per_line = kNtscSamplesPerLine;
  of.composite_buf.reserve(
      static_cast<size_t>(kNtscFieldLines * kNtscSamplesPerLine));
  for (int line = 0; line < kNtscFieldLines; ++line) {
    for (int h = 0; h < kNtscSamplesPerLine; ++h) {
      const int64_t n = field_start_sample +
                        (static_cast<int64_t>(line) * kNtscSamplesPerLine) + h;
      const double c =
          chroma_amp *
          std::cos((kPi / 2.0) * static_cast<double>(n % 4) + theta);
      of.composite_buf.push_back(
          static_cast<int16_t>(std::lround(orc::kNtscBlack + c)));
    }
  }
  of.field.data = of.composite_buf.data();
  return of;
}

// Decode one flat-colour frame with the given per-field ids and return the
// U/V at the centre of a field-1 line and of the field-2 line below it.
struct FieldPairChroma {
  double u_field1, v_field1, u_field2, v_field2;
};

FieldPairChroma decode_flat_colour(int32_t field1_phase_id,
                                   int32_t field2_phase_id) {
  const auto params = make_full_ntsc_video_params();

  Comb::Configuration config;
  config.dimensions = 2;
  config.phaseCompensation = false;  // demodulate from getLinePhase()

  Comb decoder;
  decoder.updateConfiguration(params, config);

  auto field1 = make_flat_colour_field(true, field1_phase_id, 0);
  auto field2 = make_flat_colour_field(
      false, field2_phase_id,
      static_cast<int64_t>(kNtscFieldLines) * kNtscSamplesPerLine);

  std::vector<SourceField> fields = {field1.field, field2.field};
  std::vector<ComponentFrame> output(1);
  decoder.decodeFrames(fields, 0, 2, output);

  const int h = (params.active_video_start + params.active_video_end) / 2;
  const int line = 50;  // even frame line: field 1; line 51: field 2
  return {output[0].u(line)[h], output[0].v(line)[h], output[0].u(line + 1)[h],
          output[0].v(line + 1)[h]};
}

}  // namespace

TEST(CombTest, PerFieldPhaseIds_DecodeBothFieldsToTheSameColour) {
  const auto c = decode_flat_colour(1, 2);

  const double magnitude = std::hypot(c.u_field1, c.v_field1);
  ASSERT_GT(magnitude, 10.0);
  EXPECT_NEAR(c.u_field2, c.u_field1, magnitude * 0.05);
  EXPECT_NEAR(c.v_field2, c.v_field1, magnitude * 0.05);
}

TEST(CombTest, SharedFrameLevelPhaseId_InvertsFieldTwoChroma) {
  // Both fields tagged with the same id, as a frame-level colour_frame_index
  // would: field 2 decodes to the complementary colour.
  const auto c = decode_flat_colour(1, 1);

  const double magnitude = std::hypot(c.u_field1, c.v_field1);
  ASSERT_GT(magnitude, 10.0);
  EXPECT_NEAR(c.u_field2, -c.u_field1, magnitude * 0.05);
  EXPECT_NEAR(c.v_field2, -c.v_field1, magnitude * 0.05);
}
}  // namespace orc_unit_test
