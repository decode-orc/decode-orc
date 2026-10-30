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

#include <colour_frame_phase_observer.h>
#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>

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
// Phase ID table for each (m4, is_firstfour) combination:
//   Field 1 (is_ld_first=true):
//     m4=1 (line6 absent):   rising→1, falling→5
//     m4=3 (line6 present):  rising→3, falling→7
//   Field 2 (is_ld_first=false):
//     m4=4 (line6 absent):   rising→4, falling→8
//     m4=2 (line6 present):  rising→6, falling→2  [m4==2 reverses is_firstfour]
// ---------------------------------------------------------------------------

std::vector<int16_t> make_blank_pal_frame() {
  return std::vector<int16_t>(static_cast<size_t>(kPalFrameSamples),
                              static_cast<int16_t>(kPalBlanking));
}

// Inject burst on all lines the PAL algorithm needs for one field.
// flat_start: 0 for field 1, kPalField1Lines for field 2.
// line6_burst: whether ld-decode gate line 6 carries burst.
// rising: polarity majority on ld-decode measurement lines 7,11,15,19.
//
// Injects at the TBC frame-flat positions the observer actually reads:
//   ref  = flat_start + 19          (ld-decode field line 20)
//   gate = flat_start + 5           (ld-decode field line 6)
//   meas = flat_start + {6,10,14,18} (ld-decode lines 7,11,15,19)
void inject_pal_field_burst(std::vector<int16_t>& buf, size_t flat_start,
                            bool line6_burst, bool rising) {
  constexpr size_t spl = static_cast<size_t>(kPalSamplesPerLineNominal);
  constexpr size_t bs = static_cast<size_t>(kPalColourBurstStart);
  constexpr int32_t bl = kPalBlanking;
  constexpr int32_t amp = 100;

  inject_burst(buf, VideoSystem::PAL, spl, bs, bl, flat_start + 19, amp, true);
  if (line6_burst) {
    inject_burst(buf, VideoSystem::PAL, spl, bs, bl, flat_start + 5, amp, true);
  }
  for (size_t l : {6u, 10u, 14u, 18u}) {
    inject_burst(buf, VideoSystem::PAL, spl, bs, bl, flat_start + l, amp,
                 rising);
  }
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
// field14 = rising_sum > even_count/2 on even field-relative lines 8+.
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

// ---------------------------------------------------------------------------
// PAL_M field burst injection helpers
//
// ITU-R BT.1700-1 Annex 1 Part B: the burst swings +/-45 degrees about the -U
// axis line by line, and against the frame's own 4FSC sample grid the -U axis
// steps 90 degrees a frame (270, 180, 90, 0 for frames 1-4 of the sequence),
// the same for both fields of a frame. This is how videosynth's clean PAL_M
// output reads.
// ---------------------------------------------------------------------------

std::vector<int16_t> make_blank_palm_frame() {
  return std::vector<int16_t>(static_cast<size_t>(kPalMFrameSamples),
                              static_cast<int16_t>(kNtscBlanking));
}

// Inject a burst whose demodulated angle (the observer's atan2(Q, I)) is
// |angle_deg| at frame-flat line |flat_line|.
void inject_palm_burst_at_angle(std::vector<int16_t>& buf, size_t flat_line,
                                double angle_deg) {
  constexpr size_t spl = static_cast<size_t>(kPalMSamplesPerLine);
  const size_t off =
      frame_line_sample_offset(VideoSystem::PAL_M, spl, flat_line) +
      static_cast<size_t>(kPalMColourBurstStart);
  for (size_t n = 0; n < 36; ++n) {
    const double grid = static_cast<double>((off + n) % 4) * 90.0;
    const double ac = 100.0 * std::cos((angle_deg - grid) * (M_PI / 180.0));
    buf[off + n] = static_cast<int16_t>(
        std::lround(static_cast<double>(kNtscBlanking) + ac));
  }
}

// Inject frame |frame_pos| (0-3) of the 4-frame sequence on every burst line
// of the field starting at |flat_start|.
void inject_palm_field_burst(std::vector<int16_t>& buf, size_t flat_start,
                             size_t field_lines, int frame_pos) {
  const double minus_u = 270.0 - 90.0 * frame_pos;
  for (size_t fl = 9; fl < field_lines; ++fl) {
    inject_palm_burst_at_angle(buf, flat_start + fl,
                               minus_u + ((fl % 2) != 0 ? 45.0 : -45.0));
  }
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
  inject_pal_field_burst(buf, 0, false, true);
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

TEST(ColourFramePhaseObserverTest, PAL_Field1_Line6Absent_Rising_Phase1) {
  // is_ld_first=true, m4=1 (no line6), rising → is_firstfour=true → 1+0=1
  auto buf = make_blank_pal_frame();
  inject_pal_field_burst(buf, 0, false, true);
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 1);
  EXPECT_EQ(r.f1_colour_index, 1);
  EXPECT_EQ(r.f2_phase_id, -1);
}

TEST(ColourFramePhaseObserverTest, PAL_Field1_Line6Absent_Falling_Phase5) {
  // is_ld_first=true, m4=1 (no line6), falling → is_firstfour=false → 1+4=5
  auto buf = make_blank_pal_frame();
  inject_pal_field_burst(buf, 0, false, false);
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 5);
  EXPECT_EQ(r.f1_colour_index, 3);
  EXPECT_EQ(r.f2_phase_id, -1);
}

TEST(ColourFramePhaseObserverTest, PAL_Field1_Line6Present_Rising_Phase3) {
  // is_ld_first=true, m4=3 (line6 present), rising → is_firstfour=true → 3+0=3
  auto buf = make_blank_pal_frame();
  inject_pal_field_burst(buf, 0, true, true);
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 3);
  EXPECT_EQ(r.f1_colour_index, 2);
  EXPECT_EQ(r.f2_phase_id, -1);
}

TEST(ColourFramePhaseObserverTest, PAL_Field1_Line6Present_Falling_Phase7) {
  // is_ld_first=true, m4=3 (line6 present), falling → is_firstfour=false →
  // 3+4=7
  auto buf = make_blank_pal_frame();
  inject_pal_field_burst(buf, 0, true, false);
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 7);
  EXPECT_EQ(r.f1_colour_index, 4);
  EXPECT_EQ(r.f2_phase_id, -1);
}

TEST(ColourFramePhaseObserverTest, PAL_Field2_Line6Absent_Rising_Phase4) {
  // is_ld_first=false, m4=4 (no line6), rising → is_firstfour=true → 4+0=4
  auto buf = make_blank_pal_frame();
  constexpr size_t kF2 = static_cast<size_t>(kPalField1Lines);
  inject_pal_field_burst(buf, kF2, false, true);
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, -1);
  EXPECT_EQ(r.f2_phase_id, 4);
  EXPECT_EQ(r.f2_colour_index, 2);
}

TEST(ColourFramePhaseObserverTest, PAL_Field2_Line6Absent_Falling_Phase8) {
  // is_ld_first=false, m4=4 (no line6), falling → is_firstfour=false → 4+4=8
  auto buf = make_blank_pal_frame();
  constexpr size_t kF2 = static_cast<size_t>(kPalField1Lines);
  inject_pal_field_burst(buf, kF2, false, false);
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f2_phase_id, 8);
  EXPECT_EQ(r.f2_colour_index, 4);
}

TEST(ColourFramePhaseObserverTest, PAL_Field2_Line6Present_Rising_Phase6) {
  // is_ld_first=false, m4=2 (line6 present), rising → reversed → false → 2+4=6
  auto buf = make_blank_pal_frame();
  constexpr size_t kF2 = static_cast<size_t>(kPalField1Lines);
  inject_pal_field_burst(buf, kF2, true, true);
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f2_phase_id, 6);
  EXPECT_EQ(r.f2_colour_index, 3);
}

TEST(ColourFramePhaseObserverTest, PAL_Field2_Line6Present_Falling_Phase2) {
  // is_ld_first=false, m4=2 (line6 present), falling → reversed → true → 2+0=2
  auto buf = make_blank_pal_frame();
  constexpr size_t kF2 = static_cast<size_t>(kPalField1Lines);
  inject_pal_field_burst(buf, kF2, true, false);
  FlatBufferVFR vfr(VideoSystem::PAL, std::move(buf), make_pal_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f2_phase_id, 2);
  EXPECT_EQ(r.f2_colour_index, 1);
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

TEST(ColourFramePhaseObserverTest, NTSC_Field1_Rising_Phase1) {
  auto buf = make_blank_ntsc_frame();
  inject_ntsc_field_burst(buf, 0, true);
  FlatBufferVFR vfr(VideoSystem::NTSC, std::move(buf), make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 1);
  EXPECT_EQ(r.f1_colour_index, 0);
  EXPECT_EQ(r.f2_phase_id, -1);
}

TEST(ColourFramePhaseObserverTest, NTSC_Field1_Falling_Phase3) {
  auto buf = make_blank_ntsc_frame();
  inject_ntsc_field_burst(buf, 0, false);
  FlatBufferVFR vfr(VideoSystem::NTSC, std::move(buf), make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, 3);
  EXPECT_EQ(r.f1_colour_index, 1);
  EXPECT_EQ(r.f2_phase_id, -1);
}

TEST(ColourFramePhaseObserverTest, NTSC_Field2_Falling_Phase2) {
  auto buf = make_blank_ntsc_frame();
  constexpr size_t kF2 = static_cast<size_t>(kNtscField1Lines);
  inject_ntsc_field_burst(buf, kF2, false);
  FlatBufferVFR vfr(VideoSystem::NTSC, std::move(buf), make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f1_phase_id, -1);
  EXPECT_EQ(r.f2_phase_id, 2);
  EXPECT_EQ(r.f2_colour_index, 0);
}

TEST(ColourFramePhaseObserverTest, NTSC_Field2_Rising_Phase4) {
  auto buf = make_blank_ntsc_frame();
  constexpr size_t kF2 = static_cast<size_t>(kNtscField1Lines);
  inject_ntsc_field_burst(buf, kF2, true);
  FlatBufferVFR vfr(VideoSystem::NTSC, std::move(buf), make_ntsc_params());
  const auto r = observe_frame(vfr);
  EXPECT_EQ(r.f2_phase_id, 4);
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

}  // namespace tests
}  // namespace orc
