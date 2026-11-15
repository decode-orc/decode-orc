/*
 * File:        secam_observers_test.cpp
 * Module:      orc-tests/core/unit/observers
 * Purpose:     SECAM colour sequence and sub-carrier reference observers
 *
 * Tests: ColourFramePhaseObserver identifies SECAM D'R/D'B lines from the
 * back-porch sub-carrier reference and the field identification lines, gives
 * the CVBS specification's 2-frame sequence, ignores the lock gate, reports a
 * mixed frame, and holds under noise; BurstLevelObserver reads the SECAM
 * sub-carrier reference.  Frames are synthesised in memory at fOR / fOB; no
 * I/O is performed.
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
#include <orc/support/observers/burst_level_observer.h>
#include <orc/support/observers/colour_frame_phase_observer.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace orc {
namespace tests {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kSpl = static_cast<size_t>(kSecamSamplesPerLineNominal);
constexpr size_t kF1 = static_cast<size_t>(kSecamField1Lines);
constexpr size_t kF2 = static_cast<size_t>(kSecamFrameLines) - kF1;

// BT.1700 Part C Table 4 item 10d: 2 M0 = 23 % of the 588-code luminance
// amplitude, times the precorrection gain at rest (1.3325 at fOR, 1.0356 at
// fOB).
constexpr double kLuma = static_cast<double>(kSecamWhite - kSecamBlanking);
constexpr double kDrReferencePeak = 0.115 * 1.3325 * kLuma;  // ~90 codes
constexpr double kDbReferencePeak = 0.115 * 1.0356 * kLuma;  // ~70 codes

class FlatBufferVFR : public VideoFrameRepresentation {
 public:
  FlatBufferVFR(std::vector<int16_t> samples, SourceParameters params)
      : samples_(std::move(samples)), params_(std::move(params)) {}

  FrameIDRange frame_range() const override { return {0, 1}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(FrameID id) const override { return id == FrameID{0}; }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (id != FrameID{0}) return std::nullopt;
    FrameDescriptor d;
    d.frame_id = id;
    d.system = params_.system;
    d.height = static_cast<size_t>(params_.frame_height);
    d.samples_total = samples_.size();
    d.samples_per_line_nominal = kSpl;
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
  std::vector<int16_t> samples_;
  SourceParameters params_;
};

// A SECAM source is never phase locked (the specification forbids it).
SourceParameters secam_params() {
  SourceParameters p{};
  p.system = VideoSystem::SECAM;
  p.frame_width_nominal = kSecamSamplesPerLineNominal;
  p.frame_height = kSecamFrameLines;
  p.blanking_level = kSecamBlanking;
  p.white_level = kSecamWhite;
  p.first_active_frame_line = kPalFirstActiveFrameLine;
  p.last_active_frame_line = kPalLastActiveFrameLine;
  p.is_subcarrier_locked = false;
  return p;
}

// What a synthetic SECAM frame carries.
struct SecamFrameSpec {
  // Sequence frame (1 or 2) of each field.
  int field1_frame = 1;
  int field2_frame = 1;
  bool reference = true;                 // back-porch sub-carrier reference
  bool identification = true;            // field identification lines
  bool identification_inverted = false;  // id lines say the other line type
  double noise_rms = 0.0;
  // Every Nth picture line (0 = none) has a reference at a random frequency
  // in the SECAM band, as a dropout or a head switch might leave it.
  int corrupt_every = 0;
};

// Deterministic Gaussian noise (Box-Muller on an LCG), independent of the
// standard library's distribution implementations.
class Noise {
 public:
  double uniform() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<double>(state_ >> 11) + 0.5) / 9007199254740992.0;
  }
  double gaussian() {
    return std::sqrt(-2.0 * std::log(uniform())) *
           std::cos(2.0 * kPi * uniform());
  }

 private:
  uint64_t state_ = 0x5ECA'0001ULL;
};

// Writes an FM sub-carrier onto samples [first, end) of |line|, following
// |freq_at(n)| Hz at sample n, with peak |peak| and a phase carried from the
// previous call through |phase|.
template <typename FreqAt>
void write_carrier(int16_t* line, size_t first, size_t end, double peak,
                   FreqAt freq_at, double& phase) {
  for (size_t n = first; n < end; ++n) {
    phase += 2.0 * kPi * freq_at(n) / kSecamSampleRate;
    line[n] = static_cast<int16_t>(
        std::lround(static_cast<double>(line[n]) + peak * std::cos(phase)));
  }
}

size_t us_to_sample(double us) {
  return static_cast<size_t>(std::lround(us * 1.0e-6 * kSecamSampleRate));
}

std::vector<int16_t> make_secam_frame(const SecamFrameSpec& spec) {
  std::vector<int16_t> frame(
      frame_line_sample_offset(VideoSystem::SECAM, kSpl, kSecamFrameLines),
      static_cast<int16_t>(kSecamBlanking));
  Noise noise;
  double phase = 0.0;

  for (size_t stored = 0; stored < kF1 + kF2; ++stored) {
    const bool field2 = stored >= kF1;
    const size_t fl = field2 ? stored - kF1 : stored;
    const int seq_frame = field2 ? spec.field2_frame : spec.field1_frame;
    // Sequence frame 1: even stored lines carry D'R.
    const bool dr = ((stored % 2) == 0) == (seq_frame == 1);
    const double rest = dr ? kSecamFor : kSecamFob;
    const double ref_peak = dr ? kDrReferencePeak : kDbReferencePeak;
    int16_t* line = frame.data() +
                    frame_line_sample_offset(VideoSystem::SECAM, kSpl, stored);

    const bool id_line = fl >= 6 && fl <= 14;
    const bool picture_line = fl >= 22 && fl + 3 < (field2 ? kF2 : kF1);
    const bool carries = (id_line && spec.identification) || picture_line;
    if (!carries) continue;

    // Back porch: the undeviated reference (sample 99 to 10.5 us).
    const size_t porch_first =
        static_cast<size_t>(kSecamSubcarrierReferenceStart);
    const size_t active_first = us_to_sample(10.5);
    const size_t active_end = us_to_sample(62.0);
    double porch_freq = rest;
    if (spec.corrupt_every > 0 && picture_line &&
        stored % static_cast<size_t>(spec.corrupt_every) == 0) {
      porch_freq = kSecamFob - 100e3 + noise.uniform() * 350e3;
    }
    if (spec.reference) {
      write_carrier(
          line, porch_first, active_first, ref_peak,
          [&](size_t) { return porch_freq; }, phase);
    }

    if (id_line) {
      // BT.1700 Part C Fig. 17: a trapezoid from rest to +350 kHz (D'R) over
      // 15 us or -350 kHz (D'B) over 18 us, then flat.
      const bool id_dr = spec.identification_inverted ? !dr : dr;
      const double id_rest = id_dr ? kSecamFor : kSecamFob;
      const double swing = id_dr ? 350e3 : -350e3;
      const double rise = us_to_sample(id_dr ? 15.0 : 18.0);
      write_carrier(
          line, active_first, active_end, ref_peak,
          [&](size_t n) {
            const double t =
                std::min(1.0, static_cast<double>(n - active_first) / rise);
            return id_rest + swing * t;
          },
          phase);
    } else {
      // Picture: a slow colour sweep, so the active line is not at rest.
      write_carrier(
          line, active_first, active_end, ref_peak,
          [&](size_t n) {
            return rest + 150e3 * std::sin(static_cast<double>(n) * 0.01);
          },
          phase);
    }
  }

  if (spec.noise_rms > 0.0) {
    for (int16_t& s : frame) {
      s = static_cast<int16_t>(std::lround(static_cast<double>(s) +
                                           spec.noise_rms * noise.gaussian()));
    }
  }
  return frame;
}

struct Phase {
  int32_t f1_phase = -999;
  int32_t f2_phase = -999;
  int32_t f1_index = -999;
  int32_t f2_index = -999;
};

int32_t read_int(const ObservationContext& ctx, uint64_t field,
                 const char* key) {
  const auto v = ctx.get(FieldID(field), "colour_frame_phase", key);
  return (v && std::holds_alternative<int32_t>(*v)) ? std::get<int32_t>(*v)
                                                    : -999;
}

Phase observe(const SecamFrameSpec& spec,
              SourceParameters params = secam_params()) {
  FlatBufferVFR vfr(make_secam_frame(spec), params);
  ObservationContext ctx;
  ColourFramePhaseObserver().process_frame(vfr, FrameID{0}, ctx);
  return {read_int(ctx, 0, "field_phase_id"),
          read_int(ctx, 1, "field_phase_id"),
          read_int(ctx, 0, "colour_frame_index"),
          read_int(ctx, 1, "colour_frame_index")};
}

}  // namespace

// ===========================================================================
// Sequence position (CVBS File Format Specification v1.8.1, Preset SECAM)
// ===========================================================================

TEST(SecamColourSequenceTest, EvenStoredLinesDr_IsSequenceFrame1) {
  const Phase p = observe({.field1_frame = 1, .field2_frame = 1});
  EXPECT_EQ(p.f1_phase, 1);
  EXPECT_EQ(p.f2_phase, 2);
  EXPECT_EQ(p.f1_index, 1);
  EXPECT_EQ(p.f2_index, 1);
}

TEST(SecamColourSequenceTest, EvenStoredLinesDb_IsSequenceFrame2) {
  const Phase p = observe({.field1_frame = 2, .field2_frame = 2});
  EXPECT_EQ(p.f1_phase, 3);
  EXPECT_EQ(p.f2_phase, 4);
  EXPECT_EQ(p.f1_index, 2);
  EXPECT_EQ(p.f2_index, 2);
}

// SECAM is never locked, and its identification is a frequency, so the lock
// gate that makes PAL and NTSC ids unknown does not apply.  Declaring the
// source locked (which the specification forbids) changes nothing either.
TEST(SecamColourSequenceTest, LockStateDoesNotGateSecam) {
  SourceParameters locked = secam_params();
  locked.is_subcarrier_locked = true;
  EXPECT_EQ(observe({}).f1_phase, 1);
  EXPECT_EQ(observe({}, locked).f1_phase, 1);
}

// ===========================================================================
// The two identification methods (BT.1700 Part C Table 4 item 13)
// ===========================================================================

TEST(SecamColourSequenceTest, BackPorchReferenceAlone_Decides) {
  const Phase p =
      observe({.field1_frame = 2, .field2_frame = 2, .identification = false});
  EXPECT_EQ(p.f1_phase, 3);
  EXPECT_EQ(p.f2_phase, 4);
}

TEST(SecamColourSequenceTest, FieldIdentificationAlone_Decides) {
  const Phase p =
      observe({.field1_frame = 2, .field2_frame = 2, .reference = false});
  EXPECT_EQ(p.f1_phase, 3);
  EXPECT_EQ(p.f2_phase, 4);
}

// When both are present they must agree; a disagreement is not guessed at.
TEST(SecamColourSequenceTest, MethodsDisagree_Unknown) {
  const Phase p = observe({.identification_inverted = true});
  EXPECT_EQ(p.f1_phase, -1);
  EXPECT_EQ(p.f2_phase, -1);
  EXPECT_EQ(p.f1_index, -1);
  EXPECT_EQ(p.f2_index, -1);
}

TEST(SecamColourSequenceTest, NoSubcarrier_Unknown) {
  const Phase p = observe({.reference = false, .identification = false});
  EXPECT_EQ(p.f1_phase, -1);
  EXPECT_EQ(p.f2_phase, -1);
  EXPECT_EQ(p.f1_index, -1);
}

// A reference that does not alternate (here, D'R on every line: a PAL burst
// near fOR would read the same) is not a SECAM sequence.
TEST(SecamColourSequenceTest, NonAlternatingReference_Unknown) {
  std::vector<int16_t> frame(
      frame_line_sample_offset(VideoSystem::SECAM, kSpl, kSecamFrameLines),
      static_cast<int16_t>(kSecamBlanking));
  double phase = 0.0;
  for (size_t stored = 22; stored < 300; ++stored) {
    int16_t* line = frame.data() +
                    frame_line_sample_offset(VideoSystem::SECAM, kSpl, stored);
    write_carrier(
        line, 99, 186, kDrReferencePeak, [](size_t) { return kSecamFor; },
        phase);
  }
  FlatBufferVFR vfr(std::move(frame), secam_params());
  ObservationContext ctx;
  ColourFramePhaseObserver().process_frame(vfr, FrameID{0}, ctx);
  EXPECT_EQ(read_int(ctx, 0, "field_phase_id"), -1);
}

// ===========================================================================
// Frames whose fields come from different sequence frames
// ===========================================================================

// Each field keeps its own id, which shows the break; the frame as a whole
// has no sequence position.
TEST(SecamColourSequenceTest, MixedFrame_FieldIdsShowIt_IndexUnknown) {
  const Phase p = observe({.field1_frame = 1, .field2_frame = 2});
  EXPECT_EQ(p.f1_phase, 1);
  EXPECT_EQ(p.f2_phase, 4);
  EXPECT_EQ(p.f1_index, -1);
  EXPECT_EQ(p.f2_index, -1);

  const Phase q = observe({.field1_frame = 2, .field2_frame = 1});
  EXPECT_EQ(q.f1_phase, 3);
  EXPECT_EQ(q.f2_phase, 2);
  EXPECT_EQ(q.f1_index, -1);
}

// One field unreadable: the other still gives the frame's position.
TEST(SecamColourSequenceTest, OneFieldUnreadable_OtherStands) {
  SecamFrameSpec spec{.field1_frame = 2, .field2_frame = 2};
  std::vector<int16_t> frame = make_secam_frame(spec);
  const size_t f2 = frame_line_sample_offset(VideoSystem::SECAM, kSpl, kF1);
  std::fill(frame.begin() + static_cast<std::ptrdiff_t>(f2), frame.end(),
            static_cast<int16_t>(kSecamBlanking));
  FlatBufferVFR vfr(std::move(frame), secam_params());
  ObservationContext ctx;
  ColourFramePhaseObserver().process_frame(vfr, FrameID{0}, ctx);
  EXPECT_EQ(read_int(ctx, 0, "field_phase_id"), 3);
  EXPECT_EQ(read_int(ctx, 0, "colour_frame_index"), 2);
  EXPECT_EQ(read_int(ctx, 1, "field_phase_id"), -1);
  EXPECT_EQ(read_int(ctx, 1, "colour_frame_index"), -1);
}

// ===========================================================================
// Noise margin
// ===========================================================================

// 20 codes RMS is about 29 dB below the 588-code luminance amplitude, far
// noisier than a usable capture; every line's reading still holds.
TEST(SecamColourSequenceTest, HeavyNoise_StillIdentified) {
  for (const int frame : {1, 2}) {
    const Phase p = observe(
        {.field1_frame = frame, .field2_frame = frame, .noise_rms = 20.0});
    EXPECT_EQ(p.f1_phase, frame == 1 ? 1 : 3) << "frame " << frame;
    EXPECT_EQ(p.f2_phase, frame == 1 ? 2 : 4) << "frame " << frame;
  }
}

// A quarter of the references corrupted, as dropouts would, is outvoted.
TEST(SecamColourSequenceTest, CorruptedReferences_Outvoted) {
  const Phase p = observe({.field1_frame = 2,
                           .field2_frame = 2,
                           .identification = false,
                           .corrupt_every = 4});
  EXPECT_EQ(p.f1_phase, 3);
  EXPECT_EQ(p.f2_phase, 4);
}

// Noise that swamps the sub-carrier gives unknown, never a wrong position.
TEST(SecamColourSequenceTest, NoiseSwampsSubcarrier_UnknownNotWrong) {
  const Phase p = observe({.noise_rms = 400.0});
  EXPECT_EQ(p.f1_phase, -1);
  EXPECT_EQ(p.f2_phase, -1);
}

// ===========================================================================
// BurstLevelObserver: the SECAM sub-carrier reference
// ===========================================================================

double observed_burst_level(const SecamFrameSpec& spec) {
  FlatBufferVFR vfr(make_secam_frame(spec), secam_params());
  ObservationContext ctx;
  BurstLevelObserver().process_frame(vfr, FrameID{0}, ctx);
  const auto v = ctx.get(FieldID(0), "burst_level", "median_burst_10bit");
  return (v && std::holds_alternative<double>(*v)) ? std::get<double>(*v)
                                                   : -1.0;
}

// The reading is the mean of a D'R and a D'B reference, whichever frame of
// the sequence it is, and with or without identification lines.
TEST(SecamBurstLevelTest, ReadsMeanOfDrAndDbReference) {
  const double expected = (kDrReferencePeak + kDbReferencePeak) / 2.0;
  for (const int frame : {1, 2}) {
    for (const bool id : {true, false}) {
      const double level = observed_burst_level(
          {.field1_frame = frame, .field2_frame = frame, .identification = id});
      EXPECT_NEAR(level, expected, 0.03 * expected)
          << "frame " << frame << " id " << id;
    }
  }
}

TEST(SecamBurstLevelTest, NoReference_ReadsNearZero) {
  EXPECT_LT(observed_burst_level({.reference = false, .identification = false}),
            1.0);
}

}  // namespace tests
}  // namespace orc
