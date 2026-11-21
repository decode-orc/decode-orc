/*
 * File:        colour_frame_phase_observer_test.cpp
 * Module:      orc-tests/core/unit/observers
 * Purpose:     Unit tests for ColourFramePhaseObserver
 *
 * Tests: PAL 8-field, NTSC 4-field, PAL_M 8-field per-field phase
 * classification; absent-burst handling; observation-context storage.
 * Frame data is synthesised in memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/observers/colour_frame_phase_observer.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace orc {
namespace tests {

namespace {

// ---------------------------------------------------------------------------
// Minimal in-memory VFR backed by a flat sample buffer
// ---------------------------------------------------------------------------

class FlatBufferVFR : public VideoFrameRepresentation {
 public:
  FlatBufferVFR(VideoSystem system, std::vector<int16_t> samples,
                SourceParameters params)
      : system_(system),
        samples_(std::move(samples)),
        params_(std::move(params)) {}

  FrameIDRange frame_range() const override { return {0, 1}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(FrameID id) const override { return id == FrameID{0}; }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (id != FrameID{0}) return std::nullopt;
    FrameDescriptor d;
    d.frame_id = id;
    d.system = system_;
    d.height = static_cast<size_t>(params_.frame_height);
    d.samples_total = samples_.size();
    d.samples_per_line_nominal =
        static_cast<size_t>(params_.frame_width_nominal);
    return d;
  }

  const sample_type* get_frame(FrameID id) const override {
    return (id == FrameID{0}) ? samples_.data() : nullptr;
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return (id == FrameID{0}) ? samples_ : std::vector<sample_type>{};
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    return params_;
  }

 private:
  VideoSystem system_;
  std::vector<int16_t> samples_;
  SourceParameters params_;
};

// ---------------------------------------------------------------------------
// Parameter builders
// ---------------------------------------------------------------------------

SourceParameters make_pal_params() {
  SourceParameters p{};
  p.system = VideoSystem::PAL;
  p.frame_width_nominal = kPalSamplesPerLineNominal;
  p.frame_height = kPalFrameLines;
  p.blanking_level = kPalBlanking;
  p.white_level = kPalWhite;
  return p;
}

SourceParameters make_ntsc_params() {
  SourceParameters p{};
  p.system = VideoSystem::NTSC;
  p.frame_width_nominal = kNtscSamplesPerLine;
  p.frame_height = kNtscFrameLines;
  p.blanking_level = kNtscBlanking;
  p.white_level = kNtscWhite;
  return p;
}

SourceParameters make_palm_params() {
  SourceParameters p{};
  p.system = VideoSystem::PAL_M;
  p.frame_width_nominal = kPalMSamplesPerLine;
  p.frame_height = kPalMFrameLines;
  p.blanking_level = kNtscBlanking;
  p.white_level = kNtscWhite;
  return p;
}

// ---------------------------------------------------------------------------
// Burst injection
// ---------------------------------------------------------------------------

// Inject a demodulatable colour burst at frame-flat line |flat_line|.
//
// The 4FSC demodulator in measure_burst_angle() measures:
//   I = sum of (sample - blanking) weighted by cos(n*90°)
//   Q = sum of (sample - blanking) weighted by sin(n*90°)
//
// Injecting A*sin((off+n)%4 * 90°) always produces Q = 20A regardless of
// the line's phase offset, making rising/falling independent of geometry.
//
// rising=true  → Q > 0 (positive-going first zero crossing, "rising" burst)
// rising=false → Q < 0 (negative-going, "falling" burst)
// Demodulated amplitude is approximately 20*amp in both cases.
void inject_burst(std::vector<int16_t>& buf, VideoSystem sys, size_t spl_nom,
                  size_t burst_start, int32_t blanking, size_t flat_line,
                  int32_t amp, bool rising) {
  const size_t off =
      frame_line_sample_offset(sys, spl_nom, flat_line) + burst_start;
  for (size_t n = 0; n < 40; ++n) {
    const double phase = static_cast<double>((off + n) % 4) * (M_PI / 2.0);
    const double ac = static_cast<double>(amp) *
                      (rising ? std::sin(phase) : -std::sin(phase));
    buf[off + n] =
        static_cast<int16_t>(std::lround(static_cast<double>(blanking) + ac));
  }
}

// Inject a burst whose demodulated angle (the observer's atan2(Q, I)) is
// |angle_deg| at frame-flat line |flat_line|.
//
// |sequence_offset| is where this frame starts in a continuous 4FSC sample
// stream: the angle is then taken against the stream's grid, not the frame's,
// so consecutive frames of one subcarrier can be built (see
// make_continuous_frame()).
void inject_burst_at_angle(std::vector<int16_t>& buf, VideoSystem sys,
                           size_t spl_nom, size_t burst_start, int32_t blanking,
                           size_t flat_line, double angle_deg,
                           size_t sequence_offset = 0) {
  const size_t off =
      frame_line_sample_offset(sys, spl_nom, flat_line) + burst_start;
  for (size_t n = 0; n < 40; ++n) {
    const double grid =
        static_cast<double>((sequence_offset + off + n) % 4) * 90.0;
    const double ac = 100.0 * std::cos((angle_deg - grid) * (M_PI / 180.0));
    buf[off + n] =
        static_cast<int16_t>(std::lround(static_cast<double>(blanking) + ac));
  }
}

// -U against the frame's own 4FSC grid in frame |frame_pos| (0-3) of the
// 4-frame sequence at the standard sampling phase (EBU Tech. 3280-E §1.1.1),
// giving ld-decode field ids 2k+1 / 2k+2: 225, 315, 45, 135 degrees for PAL
// (709,379 samples a frame, ≡ 3 mod 4) and 225, 135, 45, 315 for PAL_M
// (477,225, ≡ 1 mod 4).  The burst swings +/-45 degrees about it, so it lands
// on the demodulator's axes.
double standard_minus_u(VideoSystem sys, int frame_pos) {
  const double step = (sys == VideoSystem::PAL) ? 90.0 : -90.0;
  return 225.0 + step * frame_pos;
}

// ---------------------------------------------------------------------------
// PAL field burst injection helpers
//
// PAL field mapping (EBU Tech. 3280-E / ld-decode FieldPAL):
//   ld-decode always writes the isFirstField=True field first in the TBC, so
//   VFR field 1 (flat_start=0)   → ld-decode isFirstField=True
//   VFR field 2 (flat_start=313) → ld-decode isFirstField=False
// TBC line = ld-decode field line - 1 for both fields (the per-field
// lineoffset cancels between scale_field and lineslice).
//
// The burst-blanking gate (ld-decode field line 6, TBC line 5) carries burst
// in fields 3, 5, 2 and 8 of the sequence (ld-decode map4) and none in fields
// 1, 7, 4 and 6.  It fixes the frame's parity; the -U angle picks between the
// two frames of that parity.
// ---------------------------------------------------------------------------

std::vector<int16_t> make_blank_pal_frame() {
  return std::vector<int16_t>(static_cast<size_t>(kPalFrameSamples),
                              static_cast<int16_t>(kPalBlanking));
}

// Inject a PAL field's burst for the field starting at |flat_start| with the
// given gate-line burst and -U angle, on every line from TBC line 7 to the end
// of the field.
void inject_pal_field(std::vector<int16_t>& buf, size_t flat_start,
                      size_t field_lines, bool line6_burst, double minus_u) {
  constexpr size_t spl = static_cast<size_t>(kPalSamplesPerLineNominal);
  constexpr size_t bs = static_cast<size_t>(kPalColourBurstStart);
  constexpr int32_t bl = kPalBlanking;
  if (line6_burst) {
    inject_burst_at_angle(buf, VideoSystem::PAL, spl, bs, bl, flat_start + 5,
                          minus_u + 45.0);
  }
  for (size_t fl = 6; fl < field_lines; ++fl) {
    inject_burst_at_angle(buf, VideoSystem::PAL, spl, bs, bl, flat_start + fl,
                          minus_u + ((fl % 2) != 0 ? 45.0 : -45.0));
  }
}

constexpr size_t kPalF1 = static_cast<size_t>(kPalField1Lines);
constexpr size_t kPalF2 = static_cast<size_t>(kPalFrameLines) - kPalF1;

// Gate-line burst for a frame at |frame_pos| of the sequence, per field.
bool pal_field1_gate(int frame_pos) { return (frame_pos % 2) != 0; }
bool pal_field2_gate(int frame_pos) { return (frame_pos % 2) == 0; }

// Inject both fields of a standard PAL frame at |frame_pos| (0-3), with the
// -U angle offset by |phase_error_deg| from the standard sampling phase.
std::vector<int16_t> make_pal_frame(int frame_pos,
                                    double phase_error_deg = 0.0) {
  auto buf = make_blank_pal_frame();
  const double minus_u =
      standard_minus_u(VideoSystem::PAL, frame_pos) + phase_error_deg;
  inject_pal_field(buf, 0, kPalF1, pal_field1_gate(frame_pos), minus_u);
  inject_pal_field(buf, kPalF1, kPalF2, pal_field2_gate(frame_pos), minus_u);
  return buf;
}

// ---------------------------------------------------------------------------
// NTSC field burst injection helpers
//
// NTSC field mapping (SMPTE 244M-2003):
//   VFR field 1 (flat lines 0..262,   263 lines) → ld-decode isFirstField=true
//   VFR field 2 (flat lines 263..524, 262 lines) → ld-decode isFirstField=false
//
// Phase ID table for each (is_ld_first, field14) combination:
//   {(T,T)→1, (F,F)→2, (T,F)→3, (F,T)→4}
// The frame is on the CVBS specification's line origin, two samples earlier
// than ld-decode's, so ld-decode's field14 (a rising vote on its own origin)
// is a falling vote here: field14 = falling_sum > even_count/2 on even
// field-relative lines 8+.
// ---------------------------------------------------------------------------

std::vector<int16_t> make_blank_ntsc_frame() {
  return std::vector<int16_t>(static_cast<size_t>(kNtscFrameSamples),
                              static_cast<int16_t>(kNtscBlanking));
}

// Inject rising or falling burst on 4 even field-relative lines (8,10,12,14).
// 4 lines → even_count=4; rising_sum=4 or 0 → unambiguous majority.
// |rising| uses ld-decode's physical convention: field 2 starts at an offset
// ≡ 2 (mod 4) samples, rotating the observer's demodulation reference 180°,
// so the injected Q polarity is inverted there to represent the same
// physical burst (the observer compensates symmetrically).
void inject_ntsc_field_burst(std::vector<int16_t>& buf, size_t flat_start,
                             bool rising) {
  constexpr size_t spl = static_cast<size_t>(kNtscSamplesPerLine);
  constexpr size_t bs = static_cast<size_t>(kNtscColourBurstStart);
  constexpr int32_t bl = kNtscBlanking;
  constexpr int32_t amp = 100;
  const bool q_rising = ((flat_start * spl) % 4 == 2) ? !rising : rising;
  for (size_t fl : {8u, 10u, 12u, 14u}) {
    inject_burst(buf, VideoSystem::NTSC, spl, bs, bl, flat_start + fl, amp,
                 q_rising);
  }
}

// A frame of colour frame A (|colour_frame| 0) or B (1) on the specification's
// line origin, built from one continuous subcarrier.  SMPTE 244M-2003 §4.1.2:
// at 0° SC/H, stored sample 125 of stored frame line 9 of colour frame A is a
// +I (123°) sample, so the subcarrier phase there is -33° (chroma at angle φ
// from +U is sin(ωt + φ)).  The phase advances 90° a sample, and a frame is
// 477,750 samples (≡ 2 mod 4), so colour frame B is 180° on.  The burst sits
// at 180° (SMPTE 170M-2004 §8) in its window on every field row from 8.
std::vector<int16_t> make_ntsc_colour_frame(int colour_frame) {
  constexpr size_t spl = static_cast<size_t>(kNtscSamplesPerLine);
  constexpr size_t kReference = 9 * spl + 125;
  auto buf = make_blank_ntsc_frame();
  const size_t f1 = static_cast<size_t>(kNtscField1Lines);
  for (size_t row = 0; row < static_cast<size_t>(kNtscFrameLines); ++row) {
    const size_t field_row = row < f1 ? row : row - f1;
    if (field_row < 8) continue;
    for (size_t x = static_cast<size_t>(kNtscColourBurstStart);
         x < static_cast<size_t>(kNtscColourBurstEnd); ++x) {
      const size_t s = row * spl + x;
      const double cycles =
          (static_cast<double>(s) - static_cast<double>(kReference)) / 4.0 +
          0.5 * colour_frame;
      const double omega_t = -33.0 * (M_PI / 180.0) + 2.0 * M_PI * cycles;
      buf[s] = static_cast<int16_t>(
          std::lround(kNtscBlanking + 100.0 * std::sin(omega_t + M_PI)));
    }
  }
  return buf;
}

// ---------------------------------------------------------------------------
// PAL_M field burst injection helpers
//
// ITU-R BT.1700-1 Annex 1 Part B: the burst swings +/-45 degrees about the -U
// axis line by line, and against the frame's own 4FSC sample grid the -U axis
// steps 90 degrees a frame (225, 135, 45, 315 for frames 1-4 of the sequence
// at PAL's sampling phase), the same for both fields of a frame. This is how
// videosynth's clean standard-phase PAL_M output reads.
// ---------------------------------------------------------------------------

std::vector<int16_t> make_blank_palm_frame() {
  return std::vector<int16_t>(static_cast<size_t>(kPalMFrameSamples),
                              static_cast<int16_t>(kNtscBlanking));
}

// Inject frame |frame_pos| (0-3) of the 4-frame sequence on every burst line
// of the field starting at |flat_start|.
void inject_palm_field_burst(std::vector<int16_t>& buf, size_t flat_start,
                             size_t field_lines, int frame_pos) {
  constexpr size_t spl = static_cast<size_t>(kPalMSamplesPerLine);
  constexpr size_t bs = static_cast<size_t>(kPalMColourBurstStart);
  const double minus_u = standard_minus_u(VideoSystem::PAL_M, frame_pos);
  for (size_t fl = 9; fl < field_lines; ++fl) {
    inject_burst_at_angle(buf, VideoSystem::PAL_M, spl, bs, kNtscBlanking,
                          flat_start + fl,
                          minus_u + ((fl % 2) != 0 ? 45.0 : -45.0));
  }
}

// ---------------------------------------------------------------------------
// Continuous-subcarrier frames
//
// Frame |frame_pos| of one subcarrier that runs on unbroken from frame 0,
// whose -U sits at 225 degrees (ld-decode fields 1/2).  Each frame's -U then
// follows from the frame length alone, so these frames check the per-frame
// step of the sequence against the arithmetic of the standard rather than
// against the observer's own constants.  The PAL burst-blanking gate follows
// the frame's parity.
// ---------------------------------------------------------------------------
std::vector<int16_t> make_continuous_frame(VideoSystem sys, int frame_pos) {
  const bool pal = (sys == VideoSystem::PAL);
  const size_t frame_samples =
      static_cast<size_t>(pal ? kPalFrameSamples : kPalMFrameSamples);
  const size_t spl = static_cast<size_t>(pal ? kPalSamplesPerLineNominal
                                             : kPalMSamplesPerLine);
  const size_t bs =
      static_cast<size_t>(pal ? kPalColourBurstStart : kPalMColourBurstStart);
  const int32_t bl = pal ? kPalBlanking : kNtscBlanking;
  const size_t f1 =
      static_cast<size_t>(pal ? kPalField1Lines : kPalMField1Lines);
  const size_t lines =
      static_cast<size_t>(pal ? kPalFrameLines : kPalMFrameLines);
  const size_t sequence_offset = static_cast<size_t>(frame_pos) * frame_samples;

  std::vector<int16_t> buf(frame_samples, static_cast<int16_t>(bl));
  for (size_t flat = 0; flat < lines; ++flat) {
    const bool field1 = flat < f1;
    const size_t fl = field1 ? flat : flat - f1;
    if (fl == 5 && pal) {
      const bool gate =
          field1 ? pal_field1_gate(frame_pos) : pal_field2_gate(frame_pos);
      if (!gate) continue;
    } else if (fl < 6) {
      continue;
    }
    inject_burst_at_angle(buf, sys, spl, bs, bl, flat,
                          225.0 + ((fl % 2) != 0 ? 45.0 : -45.0),
                          sequence_offset);
  }
  return buf;
}

// ---------------------------------------------------------------------------
// Observation extraction helper
// ---------------------------------------------------------------------------

struct FramePhaseResult {
  int32_t f1_phase_id = -999;
  int32_t f2_phase_id = -999;
  int32_t f1_colour_index = -999;
  int32_t f2_colour_index = -999;
};

FramePhaseResult observe_frame(const VideoFrameRepresentation& vfr) {
  ObservationContext ctx;
  ColourFramePhaseObserver obs;
  obs.process_frame(vfr, FrameID{0}, ctx);

  FramePhaseResult r;
  if (auto v = ctx.get(FieldID(0), "colour_frame_phase", "field_phase_id");
      v && std::holds_alternative<int32_t>(*v))
    r.f1_phase_id = std::get<int32_t>(*v);
  if (auto v = ctx.get(FieldID(1), "colour_frame_phase", "field_phase_id");
      v && std::holds_alternative<int32_t>(*v))
    r.f2_phase_id = std::get<int32_t>(*v);
  if (auto v = ctx.get(FieldID(0), "colour_frame_phase", "colour_frame_index");
      v && std::holds_alternative<int32_t>(*v))
    r.f1_colour_index = std::get<int32_t>(*v);
  if (auto v = ctx.get(FieldID(1), "colour_frame_phase", "colour_frame_index");
      v && std::holds_alternative<int32_t>(*v))
    r.f2_colour_index = std::get<int32_t>(*v);
  return r;
}

}  // namespace

// ===========================================================================
// Observer identity
// ===========================================================================

TEST(ColourFramePhaseObserverTest, ObserverName) {
  ColourFramePhaseObserver obs;
  EXPECT_EQ(obs.observer_name(), "ColourFramePhaseObserver");
}

TEST(ColourFramePhaseObserverTest, ProvidesFieldPhaseIdAndColourFrameIndex) {
  ColourFramePhaseObserver obs;
  const auto keys = obs.get_provided_observations();
  ASSERT_EQ(keys.size(), 2u);
  EXPECT_EQ(keys[0].namespace_, "colour_frame_phase");
  EXPECT_EQ(keys[0].name, "field_phase_id");
  EXPECT_EQ(keys[0].type, ObservationType::INT32);
  EXPECT_EQ(keys[1].namespace_, "colour_frame_phase");
  EXPECT_EQ(keys[1].name, "colour_frame_index");
  EXPECT_EQ(keys[1].type, ObservationType::INT32);
}

// ===========================================================================
// Context storage: both fields must be written per frame
// ===========================================================================

TEST(ColourFramePhaseObserverTest, StoresBothFieldsPerFrame) {
  // Only field 1 has burst; field 2 is blank (returns -1).
  // Both FieldID(0) and FieldID(1) must be written regardless.
  auto buf = make_blank_pal_frame();
  inject_pal_field(buf, 0, kPalF1, false,
                   standard_minus_u(VideoSystem::PAL, 0));
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  ObservationContext ctx;
  ColourFramePhaseObserver obs;
  obs.process_frame(vfr, FrameID{0}, ctx);

  EXPECT_TRUE(ctx.has(FieldID(0), "colour_frame_phase", "field_phase_id"));
  EXPECT_TRUE(ctx.has(FieldID(0), "colour_frame_phase", "colour_frame_index"));
  EXPECT_TRUE(ctx.has(FieldID(1), "colour_frame_phase", "field_phase_id"));
  EXPECT_TRUE(ctx.has(FieldID(1), "colour_frame_phase", "colour_frame_index"));
}

// ===========================================================================
// PAL 8-field phase classification
// ===========================================================================
// colour_frame_index = (field_phase_id - 1) / 2 + 1  (PAL/PAL_M)

TEST(ColourFramePhaseObserverTest,
     PAL_StandardSamplingPhase_FramePositionsGiveConsecutiveFieldIds) {
  // At the standard phase every burst lies on the demodulator's axes, half of
  // them on Q = 0; both fields of each frame must still agree (issue #380).
  for (int k = 0; k < 4; ++k) {
    SCOPED_TRACE(k);
    FlatBufferVFR vfr(VideoSystem::PAL, make_pal_frame(k), make_pal_params());
    const auto r = observe_frame(vfr);
    EXPECT_EQ(r.f1_phase_id, 2 * k + 1);
    EXPECT_EQ(r.f2_phase_id, 2 * k + 2);
    EXPECT_EQ(r.f1_colour_index, k + 1);
    EXPECT_EQ(r.f2_colour_index, k + 1);
  }
}

TEST(ColourFramePhaseObserverTest,
     PAL_ContinuousSubcarrier_ConsecutiveFramesRunThroughSequence) {
  // 709,379 samples a frame (≡ 3 mod 4): each frame starts a quarter cycle
  // further on, and the ids must run forwards, 1/2, 3/4, 5/6, 7/8.
  for (int k = 0; k < 4; ++k) {
    SCOPED_TRACE(k);
    FlatBufferVFR vfr(VideoSystem::PAL,
                      make_continuous_frame(VideoSystem::PAL, k),
                      make_pal_params());
    const auto r = observe_frame(vfr);
    EXPECT_EQ(r.f1_phase_id, 2 * k + 1);
    EXPECT_EQ(r.f2_phase_id, 2 * k + 2);
  }
}

TEST(ColourFramePhaseObserverTest,
     PAL_SamplingPhaseErrorWithinMargin_KeepsFieldIds) {
  // Real captures sit up to about 30 degrees from the standard phase.
  for (double error : {-35.0, 35.0}) {
    for (int k = 0; k < 4; ++k) {
      SCOPED_TRACE(testing::Message() << "error " << error << " frame " << k);
      FlatBufferVFR vfr(VideoSystem::PAL, make_pal_frame(k, error),
                        make_pal_params());
      const auto r = observe_frame(vfr);
      EXPECT_EQ(r.f1_phase_id, 2 * k + 1);
      EXPECT_EQ(r.f2_phase_id, 2 * k + 2);
    }
  }
}

TEST(ColourFramePhaseObserverTest, PAL_GridOneSampleOut_StoresMinusOne) {
  // A line origin one 4FSC sample out rotates -U by 90 degrees, midway
  // between the two frames the burst-blanking gate allows: neither is called.
  for (double error : {-90.0, 90.0}) {
    SCOPED_TRACE(error);
    FlatBufferVFR vfr(VideoSystem::PAL, make_pal_frame(0, error),
                      make_pal_params());
    const auto r = observe_frame(vfr);
    EXPECT_EQ(r.f1_phase_id, -1);
    EXPECT_EQ(r.f2_phase_id, -1);
  }
}

TEST(ColourFramePhaseObserverTest, PAL_GateFixesParity_AngleFixesHalf) {
  // The gate line says fields 1/5 for field 1; -U 180 degrees from frame 0's
  // picks field 5.
  auto buf = make_blank_pal_frame();
  inject_pal_field(buf, 0, kPalF1, false,
                   standard_minus_u(VideoSystem::PAL, 2));
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 5);
  EXPECT_EQ(r.f1_colour_index, 3);
  EXPECT_EQ(r.f2_phase_id, -1);
}

TEST(ColourFramePhaseObserverTest, PAL_FieldsFromDifferentFramesDisagree) {
  // Field 2 from a frame two positions on (a mid-frame skip) keeps its own id.
  auto buf = make_blank_pal_frame();
  inject_pal_field(buf, 0, kPalF1, pal_field1_gate(0),
                   standard_minus_u(VideoSystem::PAL, 0));
  inject_pal_field(buf, kPalF1, kPalF2, pal_field2_gate(2),
                   standard_minus_u(VideoSystem::PAL, 2));
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 1);
  EXPECT_EQ(r.f2_phase_id, 6);
}

TEST(ColourFramePhaseObserverTest, PAL_AmbiguousGateLevel_StoresMinusOne) {
  // A gate-line burst at half the reference level (a dropout or
  // interference) cannot give the parity.
  auto buf = make_pal_frame(0);
  constexpr size_t spl = static_cast<size_t>(kPalSamplesPerLineNominal);
  const size_t off = frame_line_sample_offset(VideoSystem::PAL, spl, 5) +
                     static_cast<size_t>(kPalColourBurstStart);
  for (size_t n = 0; n < 40; ++n) {
    const double grid = static_cast<double>((off + n) % 4) * 90.0;
    buf[off + n] = static_cast<int16_t>(std::lround(
        kPalBlanking + 50.0 * std::cos((270.0 - grid) * (M_PI / 180.0))));
  }
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, -1);
  EXPECT_EQ(r.f2_phase_id, 2);
}

TEST(ColourFramePhaseObserverTest, PAL_AbsentBurst_StoresMinusOne) {
  FlatBufferVFR vfr(VideoSystem::PAL, make_blank_pal_frame(),
                    make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, -1);
  EXPECT_EQ(r.f2_phase_id, -1);
  EXPECT_EQ(r.f1_colour_index, -1);
  EXPECT_EQ(r.f2_colour_index, -1);
}

// ===========================================================================
// NTSC 4-field phase classification
// ===========================================================================
// Phase map: {(isFirstField=T, field14=T)→1, (F,F)→2, (T,F)→3, (F,T)→4}
// colour_frame_index = (field_phase_id - 1) / 2  (NTSC)
// field14 is the falling vote on the specification's origin.

TEST(ColourFramePhaseObserverTest, NTSC_Field1_Falling_Phase1) {
  auto buf = make_blank_ntsc_frame();
  inject_ntsc_field_burst(buf, 0, false);
  FlatBufferVFR vfr(VideoSystem::NTSC, std::move(buf), make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 1);
  EXPECT_EQ(r.f1_colour_index, 0);
  EXPECT_EQ(r.f2_phase_id, -1);
}

TEST(ColourFramePhaseObserverTest, NTSC_Field1_Rising_Phase3) {
  auto buf = make_blank_ntsc_frame();
  inject_ntsc_field_burst(buf, 0, true);
  FlatBufferVFR vfr(VideoSystem::NTSC, std::move(buf), make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 3);
  EXPECT_EQ(r.f1_colour_index, 1);
  EXPECT_EQ(r.f2_phase_id, -1);
}

TEST(ColourFramePhaseObserverTest, NTSC_Field2_Rising_Phase2) {
  auto buf = make_blank_ntsc_frame();
  constexpr size_t kF2 = static_cast<size_t>(kNtscField1Lines);
  inject_ntsc_field_burst(buf, kF2, true);
  FlatBufferVFR vfr(VideoSystem::NTSC, std::move(buf), make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, -1);
  EXPECT_EQ(r.f2_phase_id, 2);
  EXPECT_EQ(r.f2_colour_index, 0);
}

TEST(ColourFramePhaseObserverTest, NTSC_Field2_Falling_Phase4) {
  auto buf = make_blank_ntsc_frame();
  constexpr size_t kF2 = static_cast<size_t>(kNtscField1Lines);
  inject_ntsc_field_burst(buf, kF2, false);
  FlatBufferVFR vfr(VideoSystem::NTSC, std::move(buf), make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f2_phase_id, 4);
  EXPECT_EQ(r.f2_colour_index, 1);
}

// On the specification's line origin the stored grid fixes the SMPTE 170M
// §13.2 colour frame: A reads fields 1/2 and B reads 3/4.
TEST(ColourFramePhaseObserverTest, NTSC_SpecOriginColourFrameA_ReadsOneTwo) {
  FlatBufferVFR vfr(VideoSystem::NTSC, make_ntsc_colour_frame(0),
                    make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 1);
  EXPECT_EQ(r.f2_phase_id, 2);
  EXPECT_EQ(r.f1_colour_index, 0);
  EXPECT_EQ(r.f2_colour_index, 0);
}

TEST(ColourFramePhaseObserverTest, NTSC_SpecOriginColourFrameB_ReadsThreeFour) {
  FlatBufferVFR vfr(VideoSystem::NTSC, make_ntsc_colour_frame(1),
                    make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 3);
  EXPECT_EQ(r.f2_phase_id, 4);
  EXPECT_EQ(r.f1_colour_index, 1);
  EXPECT_EQ(r.f2_colour_index, 1);
}

TEST(ColourFramePhaseObserverTest, NTSC_AbsentBurst_StoresMinusOne) {
  FlatBufferVFR vfr(VideoSystem::NTSC, make_blank_ntsc_frame(),
                    make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, -1);
  EXPECT_EQ(r.f2_phase_id, -1);
}

// ===========================================================================
// PAL_M 8-field phase classification
// ===========================================================================
// Frame k of the 4-frame sequence gives field ids 2k+1 and 2k+2.

TEST(ColourFramePhaseObserverTest, PALM_FramePositionsGiveConsecutiveFieldIds) {
  constexpr size_t kF1 = static_cast<size_t>(kPalMField1Lines);
  constexpr size_t kF2 = static_cast<size_t>(kPalMFrameLines) - kF1;
  for (int k = 0; k < 4; ++k) {
    SCOPED_TRACE(k);
    auto buf = make_blank_palm_frame();
    inject_palm_field_burst(buf, 0, kF1, k);
    inject_palm_field_burst(buf, kF1, kF2, k);
    FlatBufferVFR vfr(VideoSystem::PAL_M, std::move(buf), make_palm_params());
    const auto r = observe_frame(vfr);
    EXPECT_EQ(r.f1_phase_id, 2 * k + 1);
    EXPECT_EQ(r.f2_phase_id, 2 * k + 2);
    EXPECT_EQ(r.f1_colour_index, k + 1);
    EXPECT_EQ(r.f2_colour_index, k + 1);
  }
}

TEST(ColourFramePhaseObserverTest,
     PALM_ContinuousSubcarrier_ConsecutiveFramesRunThroughSequence) {
  // 477,225 samples a frame (≡ 1 mod 4): the step runs the other way from
  // PAL's, and the ids must still run forwards.
  for (int k = 0; k < 4; ++k) {
    SCOPED_TRACE(k);
    FlatBufferVFR vfr(VideoSystem::PAL_M,
                      make_continuous_frame(VideoSystem::PAL_M, k),
                      make_palm_params());
    const auto r = observe_frame(vfr);
    EXPECT_EQ(r.f1_phase_id, 2 * k + 1);
    EXPECT_EQ(r.f2_phase_id, 2 * k + 2);
  }
}

TEST(ColourFramePhaseObserverTest, PALM_FieldsFromDifferentFramesDisagree) {
  // A frame whose field 2 comes from another frame (a mid-frame skip) must
  // not read as a consecutive pair.
  constexpr size_t kF1 = static_cast<size_t>(kPalMField1Lines);
  constexpr size_t kF2 = static_cast<size_t>(kPalMFrameLines) - kF1;
  auto buf = make_blank_palm_frame();
  inject_palm_field_burst(buf, 0, kF1, 0);
  inject_palm_field_burst(buf, kF1, kF2, 2);
  FlatBufferVFR vfr(VideoSystem::PAL_M, std::move(buf), make_palm_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 1);
  EXPECT_EQ(r.f2_phase_id, 6);
}

TEST(ColourFramePhaseObserverTest, PALM_AbsentBurst_StoresMinusOne) {
  FlatBufferVFR vfr(VideoSystem::PAL_M, make_blank_palm_frame(),
                    make_palm_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, -1);
  EXPECT_EQ(r.f2_phase_id, -1);
}

// ===========================================================================
// Sources that are not subcarrier phase locked
// ===========================================================================
// CVBS file format spec v1.8.1 lets phase be computed from sample position
// only on a locked source; an unlocked one reads unknown even with a burst
// that would classify cleanly.

TEST(ColourFramePhaseObserverTest, Unlocked_EverySystemStoresMinusOne) {
  auto unlocked = [](SourceParameters p) {
    p.is_subcarrier_locked = false;
    return p;
  };
  constexpr size_t kF1 = static_cast<size_t>(kPalMField1Lines);
  constexpr size_t kF2 = static_cast<size_t>(kPalMFrameLines) - kF1;
  auto palm = make_blank_palm_frame();
  inject_palm_field_burst(palm, 0, kF1, 1);
  inject_palm_field_burst(palm, kF1, kF2, 1);

  FlatBufferVFR pal(VideoSystem::PAL, make_pal_frame(1),
                    unlocked(make_pal_params()));
  FlatBufferVFR ntsc(VideoSystem::NTSC, make_ntsc_colour_frame(0),
                     unlocked(make_ntsc_params()));
  FlatBufferVFR pal_m(VideoSystem::PAL_M, std::move(palm),
                      unlocked(make_palm_params()));
  for (const VideoFrameRepresentation* vfr :
       {static_cast<const VideoFrameRepresentation*>(&pal),
        static_cast<const VideoFrameRepresentation*>(&ntsc),
        static_cast<const VideoFrameRepresentation*>(&pal_m)}) {
    const auto r = observe_frame(*vfr);
    EXPECT_EQ(r.f1_phase_id, -1);
    EXPECT_EQ(r.f2_phase_id, -1);
    EXPECT_EQ(r.f1_colour_index, -1);
    EXPECT_EQ(r.f2_colour_index, -1);
  }
}

// The same frames on a locked source classify, so the gate is what makes
// them unknown above.
TEST(ColourFramePhaseObserverTest, Locked_SameFramesClassify) {
  FlatBufferVFR pal(VideoSystem::PAL, make_pal_frame(1), make_pal_params());
  EXPECT_EQ(observe_frame(pal).f1_phase_id, 3);
  FlatBufferVFR ntsc(VideoSystem::NTSC, make_ntsc_colour_frame(0),
                     make_ntsc_params());
  EXPECT_EQ(observe_frame(ntsc).f1_phase_id, 1);
}

}  // namespace tests
}  // namespace orc
