/*
 * File:        teletext_slicer_test.cpp
 * Module:      orc-tests/core/unit/support
 * Purpose:     Unit tests for the WST TeletextSlicer (support tier)
 *
 * Lines are synthesized in memory at the PAL 4FSC sample rate from known
 * packet bytes; no filesystem, network, or clock access.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi-services/teletext_slicer.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <set>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "teletext_line_synthesizer.h"

namespace orc {
namespace tests {
namespace {

TeletextSlicer make_slicer(TeletextSlicerOptions options = {}) {
  return TeletextSlicer(kPalSampleRate, kTeletextBitRate, options);
}

TeletextLineResult slice_line(const std::vector<int16_t>& line,
                              TeletextSlicerOptions options = {},
                              const TeletextPhaseHint& hint = {}) {
  return make_slicer(options).slice(line.data(), line.size(),
                                    static_cast<int16_t>(kPalBlack),
                                    static_cast<int16_t>(kPalWhite), hint);
}

// The 525-line service on an NTSC line: the constructor derives the ITU-R
// BT.653 Table 1b bit rate from the system, so nothing here has to state it
// twice.
TeletextLineResult slice_ntsc_line(const std::vector<int16_t>& line,
                                   TeletextSlicerOptions options = {}) {
  const TeletextSlicer slicer(kNtscSampleRate, TeletextSystem::kWst525,
                              options);
  return slicer.slice(line.data(), line.size(),
                      static_cast<int16_t>(kNtscBlack),
                      static_cast<int16_t>(kNtscWhite));
}

std::vector<int16_t> synthesize_ntsc_wst_line(
    const std::array<uint8_t, kTeletextPacketBytes>& payload,
    TeletextLineSynthOptions opt = ntsc_wst_synth_options()) {
  return synthesize_teletext_line(payload, opt, kTeletext525PacketBytes);
}

// ---------------------------------------------------------------------------
// Hamming 8/4 and hex helpers
// ---------------------------------------------------------------------------

TEST(TeletextHamming84, EncodeDecodeRoundTrip_AllValues) {
  for (int value = 0; value < 16; ++value) {
    const uint8_t code = teletext_hamming84_encode(static_cast<uint8_t>(value));
    EXPECT_EQ(teletext_hamming84_decode(code), value);
  }
}

TEST(TeletextHamming84, SingleBitErrorsAreCorrected) {
  for (int value = 0; value < 16; ++value) {
    const uint8_t code = teletext_hamming84_encode(static_cast<uint8_t>(value));
    for (int bit = 0; bit < 8; ++bit) {
      EXPECT_EQ(teletext_hamming84_decode(code ^ (1u << bit)), value)
          << "value=" << value << " flipped bit=" << bit;
    }
  }
}

TEST(TeletextHamming84, DoubleBitErrorsAreRejected) {
  // EN 300 706 §8.2: double-bit errors are detected, not corrected.
  const uint8_t code = teletext_hamming84_encode(0x5);
  EXPECT_EQ(teletext_hamming84_decode(code ^ 0b00000011), -1);
  EXPECT_EQ(teletext_hamming84_decode(code ^ 0b00010100), -1);
  EXPECT_EQ(teletext_hamming84_decode(code ^ 0b10000001), -1);
}

TEST(TeletextHamming84, CleanAcceptsCodewordsOnly) {
  for (int value = 0; value < 16; ++value) {
    const uint8_t code = teletext_hamming84_encode(static_cast<uint8_t>(value));
    EXPECT_TRUE(teletext_hamming84_clean(code)) << "value=" << value;
    for (int bit = 0; bit < 8; ++bit) {
      // Corrected, and correctly — but corrected, which is what this reports.
      const auto damaged = static_cast<uint8_t>(code ^ (1u << bit));
      EXPECT_EQ(teletext_hamming84_decode(damaged), value);
      EXPECT_FALSE(teletext_hamming84_clean(damaged))
          << "value=" << value << " flipped bit=" << bit;
    }
  }
}

TEST(TeletextHamming84, CleanRejectsAnUncorrectableByte) {
  // Nothing was received cleanly in a byte the code cannot even resolve.
  const uint8_t code = teletext_hamming84_encode(0x5);
  ASSERT_EQ(teletext_hamming84_decode(code ^ 0b00000011), -1);
  EXPECT_FALSE(teletext_hamming84_clean(code ^ 0b00000011));
}

TEST(TeletextHamming84, CleanFlagsTheMisCorrectionThatInventsAnIdentity) {
  // The failure issue #267 is about: §8.2 gives the code minimum distance 4, so
  // three bit errors land inside a *neighbouring* codeword's correction sphere
  // and are resolved there silently. The measured case is codeword 0 — which is
  // an alternating bit pattern, and so exactly what an MLSE detector's
  // alternating error runs move — arriving as the neighbour of codeword 7.
  const uint8_t zero = teletext_hamming84_encode(0x0);
  const auto misread = static_cast<uint8_t>(zero ^ 0b00111000);
  ASSERT_EQ(teletext_hamming84_decode(misread), 0x7);
  EXPECT_FALSE(teletext_hamming84_clean(misread));
}

TEST(TeletextHamming2418, EncodeDecodeRoundTrip) {
  // The 18 data bits go out and come back unchanged, at the extremes and over
  // a spread of patterns that exercises every bit in both states.
  for (const uint32_t value : {0x00000u, 0x3FFFFu, 0x2AAAAu, 0x15555u, 0x00001u,
                               0x20000u, 0x12345u, 0x2CB9Eu}) {
    uint8_t bytes[3] = {};
    teletext_hamming2418_encode(value, bytes);
    EXPECT_EQ(teletext_hamming2418_decode(bytes[0], bytes[1], bytes[2]),
              static_cast<int32_t>(value))
        << "value=" << std::hex << value;
  }
}

TEST(TeletextHamming2418, ProtectionBitsSitWhereTheStandardPutsThem) {
  // EN 300 706 §8.3: transmission-order bits 1, 2, 4, 8, 16 and 24 are P1-P6
  // and the other eighteen carry D1-D18 in order. Encoding a single data bit
  // therefore has to light exactly one of the data positions.
  constexpr std::array<int, 18> kDataBits = {
      3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23};
  for (size_t index = 0; index < kDataBits.size(); ++index) {
    uint8_t bytes[3] = {};
    teletext_hamming2418_encode(1u << index, bytes);
    const uint32_t bits = static_cast<uint32_t>(bytes[0]) |
                          (static_cast<uint32_t>(bytes[1]) << 8) |
                          (static_cast<uint32_t>(bytes[2]) << 16);
    uint32_t data_mask = 0;
    for (const int position : kDataBits) {
      data_mask |= 1u << (position - 1);
    }
    EXPECT_EQ(bits & data_mask, 1u << (kDataBits[index] - 1))
        << "D" << (index + 1);
  }
}

TEST(TeletextHamming2418, SingleBitErrorsAreCorrectedAtEveryPosition) {
  // Including errors in the protection bits themselves, which must leave the
  // data intact rather than be "corrected" into it.
  for (const uint32_t value : {0x00000u, 0x3FFFFu, 0x12345u, 0x2CB9Eu}) {
    uint8_t bytes[3] = {};
    teletext_hamming2418_encode(value, bytes);
    for (int bit = 0; bit < 24; ++bit) {
      uint8_t damaged[3] = {bytes[0], bytes[1], bytes[2]};
      damaged[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      EXPECT_EQ(teletext_hamming2418_decode(damaged[0], damaged[1], damaged[2]),
                static_cast<int32_t>(value))
          << "value=" << std::hex << value << " flipped bit=" << std::dec
          << bit;
    }
  }
}

TEST(TeletextHamming2418, DoubleBitErrorsAreRejected) {
  // §8.3: two errors are detected, not corrected. Every pair of positions is
  // checked, because a code that corrected one of them would put a silently
  // wrong triplet into the decoder.
  uint8_t bytes[3] = {};
  teletext_hamming2418_encode(0x12345u, bytes);
  for (int first = 0; first < 24; ++first) {
    for (int second = first + 1; second < 24; ++second) {
      uint8_t damaged[3] = {bytes[0], bytes[1], bytes[2]};
      damaged[first / 8] ^= static_cast<uint8_t>(1u << (first % 8));
      damaged[second / 8] ^= static_cast<uint8_t>(1u << (second % 8));
      EXPECT_EQ(teletext_hamming2418_decode(damaged[0], damaged[1], damaged[2]),
                -1)
          << "flipped bits " << first << " and " << second;
    }
  }
}

TEST(TeletextHex, PacketRoundTrip) {
  const auto payload = make_test_payload();
  const std::string hex = teletext_packet_to_hex(payload);
  EXPECT_EQ(hex.size(), kTeletextPacketBytes * 2);
  const auto decoded = teletext_hex_to_packet(hex);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, payload);
}

TEST(TeletextHex, RejectsBadLengthAndBadCharacters) {
  EXPECT_FALSE(teletext_hex_to_packet("").has_value());
  EXPECT_FALSE(teletext_hex_to_packet("abc").has_value());
  std::string hex = teletext_packet_to_hex(make_test_payload());
  hex[10] = 'g';
  EXPECT_FALSE(teletext_hex_to_packet(hex).has_value());
}

TEST(TeletextHex, AcceptsUppercaseHex) {
  const auto payload = make_test_payload();
  std::string hex = teletext_packet_to_hex(payload);
  for (auto& c : hex) c = static_cast<char>(std::toupper(c));
  const auto decoded = teletext_hex_to_packet(hex);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, payload);
}

// ---------------------------------------------------------------------------
// Clean recovery
// ---------------------------------------------------------------------------

TEST(TeletextSlicer, CleanLine_RecoversBytesExactly) {
  const auto payload = make_test_payload();
  const auto result = slice_line(synthesize_teletext_line(payload));
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
  EXPECT_EQ(result.framing_bit_errors, 0);
  EXPECT_GT(result.data_start_sample, 0.0);
}

TEST(TeletextSlicer, PhaseOffsetSweep_RecoversBytesExactly) {
  // Sub-sample timing sweep covering more than one full bit period
  // (≈ 2.556 samples at 444 × fH, EN 300 706 §5.3).
  const auto payload = make_test_payload();
  for (int step = 0; step <= 26; ++step) {
    const double offset = step * 0.1;
    TeletextLineSynthOptions opt;
    opt.phase_offset_samples = offset;
    const auto result = slice_line(synthesize_teletext_line(payload, opt));
    ASSERT_TRUE(result.valid) << "offset=" << offset;
    EXPECT_EQ(result.bytes, payload) << "offset=" << offset;
  }
}

TEST(TeletextSlicer, AmplitudeSweep_RecoversAcrossSpecRange) {
  // EN 300 706 §5.2: '1' level 66 ± 6 % of black-to-white; sweep well beyond
  // the tolerance range in both directions.
  const auto payload = make_test_payload();
  for (int step = 0; step <= 9; ++step) {
    const double fraction = 0.45 + step * 0.05;
    TeletextLineSynthOptions opt;
    opt.amplitude_fraction = fraction;
    const auto result = slice_line(synthesize_teletext_line(payload, opt));
    ASSERT_TRUE(result.valid) << "fraction=" << fraction;
    EXPECT_EQ(result.bytes, payload) << "fraction=" << fraction;
  }
}

TEST(TeletextSlicer, LowAmplitudeLine_IsRejected) {
  // Data burst far below the amplitude gate reads as an empty line.
  const auto payload = make_test_payload();
  TeletextLineSynthOptions opt;
  opt.amplitude_fraction = 0.1;
  EXPECT_FALSE(slice_line(synthesize_teletext_line(payload, opt)).valid);
}

TEST(TeletextSlicer, NoiseSweep_RecoversBytesExactly) {
  const auto payload = make_test_payload();
  for (int32_t noise = 10; noise <= 50; noise += 10) {
    TeletextLineSynthOptions opt;
    opt.noise_amplitude = noise;
    opt.noise_seed = static_cast<uint32_t>(noise) * 977u + 1u;
    opt.phase_offset_samples = 0.7;
    const auto result = slice_line(synthesize_teletext_line(payload, opt));
    ASSERT_TRUE(result.valid) << "noise=" << noise;
    EXPECT_EQ(result.bytes, payload) << "noise=" << noise;
  }
}

// ---------------------------------------------------------------------------
// Empty and noise-only rejection
// ---------------------------------------------------------------------------

TEST(TeletextSlicer, BlankLine_IsRejected) {
  const std::vector<int16_t> line(
      static_cast<size_t>(kPalSamplesPerLineNominal),
      static_cast<int16_t>(kPalBlack));
  EXPECT_FALSE(slice_line(line).valid);
}

TEST(TeletextSlicer, NoiseOnlyLines_AreNeverValid) {
  // Deterministic noise-only lines across seeds and amplitudes must never
  // yield a packet (run-in match, framing code, and MRAG filters all gate).
  const std::array<uint8_t, kTeletextTransmissionBytes> silent{};
  for (uint32_t seed = 1; seed <= 20; ++seed) {
    for (int32_t noise : {30, 80, 150}) {
      TeletextLineSynthOptions opt;
      opt.amplitude_fraction = 0.0;  // no data burst, baseline only
      opt.noise_amplitude = noise;
      opt.noise_seed = seed;
      const auto line = synthesize_teletext_line_raw(silent, opt);
      EXPECT_FALSE(slice_line(line).valid)
          << "seed=" << seed << " noise=" << noise;
    }
  }
}

TEST(TeletextSlicer, NullOrShortInput_IsRejected) {
  const auto slicer = make_slicer();
  EXPECT_FALSE(slicer
                   .slice(nullptr, 1135, static_cast<int16_t>(kPalBlack),
                          static_cast<int16_t>(kPalWhite))
                   .valid);
  const std::vector<int16_t> short_line(64, static_cast<int16_t>(kPalBlack));
  EXPECT_FALSE(slicer
                   .slice(short_line.data(), short_line.size(),
                          static_cast<int16_t>(kPalBlack),
                          static_cast<int16_t>(kPalWhite))
                   .valid);
}

// ---------------------------------------------------------------------------
// Framing code
// ---------------------------------------------------------------------------

// ETSI EN 300 706 §6.2 defines the framing code bit for bit, and the threshold
// detector accepts it only as defined.
TEST(TeletextSlicer, OneFramingBitError_IsRejected) {
  auto packet = make_transmission_packet(make_test_payload());
  packet[2] ^= 0x04;  // flip one framing-code bit (byte 3, EN 300 706 §6.2)
  const auto line = synthesize_teletext_line_raw(packet);
  const auto result = slice_line(line);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.reject_reason, TeletextRejectReason::kFramingCodeMiss);
}

// ---------------------------------------------------------------------------
// MRAG plausibility filter
// ---------------------------------------------------------------------------

TEST(TeletextSlicer, UncorrectableMrag_IsRejected) {
  // Both MRAG bytes given double-bit errors: uncorrectable per §8.2.
  auto payload = make_test_payload();
  payload[0] ^= 0b00000011;
  payload[1] ^= 0b00011000;
  ASSERT_EQ(teletext_hamming84_decode(payload[0]), -1);
  ASSERT_EQ(teletext_hamming84_decode(payload[1]), -1);

  const auto result = slice_line(synthesize_teletext_line(payload));
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.reject_reason, TeletextRejectReason::kInvalidMrag);
}

TEST(TeletextSlicer, SingleBitDamagedMrag_PassesFilterUncorrected) {
  // A correctable (single-bit) MRAG error passes the plausibility filter and
  // the output byte stays as transmitted — no correction applied (T42
  // contract: transmission coding preserved).
  auto payload = make_test_payload();
  payload[0] ^= 0b00000010;
  ASSERT_GE(teletext_hamming84_decode(payload[0]), 0);
  const auto result = slice_line(synthesize_teletext_line(payload));
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
}

// ---------------------------------------------------------------------------
// Run-in without framing code
// ---------------------------------------------------------------------------

TEST(TeletextSlicer, RunInWithoutFramingCode_IsRejected) {
  // Valid clock run-in followed by a corrupted framing byte: the run-in lock
  // succeeds but no framing code is found within ± 2 bit positions.
  auto packet = make_transmission_packet(make_test_payload());
  packet[2] = 0x00;
  const auto line = synthesize_teletext_line_raw(packet);
  EXPECT_FALSE(slice_line(line).valid);
}

// ---------------------------------------------------------------------------
// Band-limited channels and the MLSE detector
// ---------------------------------------------------------------------------

// Consumer VHS luma rolls off around 3 MHz. Measured on PAL SP captures the
// clock run-in (3,47 MHz fundamental) comes back 16-26 dB down while the
// payload survives, which is exactly the regime the MLSE detector exists for.
constexpr double kTapeLikeCutoffHz = 2.8e6;

TeletextLineSynthOptions tape_like_options() {
  TeletextLineSynthOptions options;
  options.low_pass_cutoff_hz = kTapeLikeCutoffHz;
  return options;
}

TEST(TeletextSlicerMlse, BandLimitedLine_RejectedByThresholdDetector) {
  const auto line = synthesize_teletext_line(make_parity_coded_payload(),
                                             tape_like_options());
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kThreshold;
  EXPECT_FALSE(slice_line(line, options).valid);
}

TEST(TeletextSlicerMlse, BandLimitedLine_RecoveredByMlseDetector) {
  const auto payload = make_parity_coded_payload();
  const auto line = synthesize_teletext_line(payload, tape_like_options());

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto result = slice_line(line, options);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.detector, TeletextDetector::kMlse);
  EXPECT_EQ(result.bytes, payload);
}

TEST(TeletextSlicerMlse, BandLimitedLine_RecoveredByAutoDetector) {
  const auto payload = make_parity_coded_payload();
  const auto line = synthesize_teletext_line(payload, tape_like_options());

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kAuto;
  const auto result = slice_line(line, options);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.detector, TeletextDetector::kMlse);
  EXPECT_EQ(result.bytes, payload);
}

TEST(TeletextSlicerMlse, CleanLine_AutoStopsAtThresholdDetector) {
  // A source the threshold detector handles must not pay for the fallback,
  // and must decode identically to kThreshold.
  const auto payload = make_test_payload();
  const auto line = synthesize_teletext_line(payload);

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kAuto;
  const auto result = slice_line(line, options);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.detector, TeletextDetector::kThreshold);
  EXPECT_EQ(result.bytes, payload);
}

TEST(TeletextSlicerMlse, CleanLine_RecoveredByMlseDetectorToo) {
  // The MLSE detector is a band-limited-channel remedy, not a tape-only one:
  // it must stay exact on a signal the threshold detector also handles.
  const auto payload = make_parity_coded_payload();
  const auto line = synthesize_teletext_line(payload);

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto result = slice_line(line, options);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
}

TEST(TeletextSlicerMlse, BandLimitedNoisyLine_RecoversBytesExactly) {
  const auto payload = make_parity_coded_payload();
  auto synth = tape_like_options();
  synth.noise_amplitude = 12;
  const auto line = synthesize_teletext_line(payload, synth);

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto result = slice_line(line, options);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
}

TEST(TeletextSlicerMlse, BlankLine_IsRejectedByEveryDetector) {
  const std::vector<int16_t> line(
      static_cast<size_t>(kPalSamplesPerLineNominal),
      static_cast<int16_t>(kPalBlack));
  for (const auto detector :
       {TeletextDetector::kThreshold, TeletextDetector::kMlse,
        TeletextDetector::kAuto}) {
    TeletextSlicerOptions options;
    options.detector = detector;
    EXPECT_FALSE(slice_line(line, options).valid);
  }
}

std::vector<int16_t> make_noise_line(uint32_t seed, double cutoff_hz) {
  std::vector<int16_t> line(static_cast<size_t>(kPalSamplesPerLineNominal),
                            static_cast<int16_t>(kPalBlack));
  uint32_t state = seed;
  for (auto& sample : line) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    // Full data-amplitude noise: as hostile as a VBI line can plausibly be.
    sample = static_cast<int16_t>(kPalBlack + (state % 400));
  }
  if (cutoff_hz > 0.0) {
    band_limit_line(line, kPalSampleRate, cutoff_hz);
  }
  return line;
}

TEST(TeletextSlicerMlse, NoiseOnlyLines_RarelyLock) {
  // Unlike the threshold detector, which demands an exact framing-code match
  // (§6.2) and so is absolute about noise, the MLSE detector *fits* the
  // preamble, and enough noise will occasionally fit. What holds the rate
  // down is the MRAG filter (§8.2) plus the parity plausibility of the
  // recovered payload (§9.3.1) — measured at a few per cent of noise-only
  // lines, and the survivors land almost entirely in the rows that carry
  // Hamming 24/18 or independent data and so have no parity to check.
  //
  // This is why kAuto reaches the MLSE detector only for lines the threshold
  // detector rejected or read with a closed eye: on a source it handles, the
  // fallback never runs. On real captures the observed false-lock count is
  // zero — 2800 LaserDisc VBI lines carrying no teletext, the blank lines of
  // two VHS captures, and the 35 776 lines of those two captures that carry a
  // data burst which is not a teletext packet (0-based field lines 18 and 19,
  // in every field of both).
  //
  // The bound is one line above the observed count rather than at it. None
  // of these lines locks at either band limit (one in 640 at 2,8 MHz, against
  // 11 when the detector read one shift of its best fit and read nothing
  // back: every reading must now read its framing code back within a bit and,
  // where its MRAG needed correction, its clock run-in still alternating).
  constexpr int kSeeds = 64;
  constexpr int kMaxFalseLocks = 1;

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;

  for (const double cutoff_hz : {0.0, kTapeLikeCutoffHz}) {
    int false_locks = 0;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
      if (slice_line(make_noise_line(seed, cutoff_hz), options).valid) {
        ++false_locks;
      }
    }
    EXPECT_LE(false_locks, kMaxFalseLocks) << "cutoff_hz=" << cutoff_hz;
  }
}

TEST(TeletextSlicerMlse, NoiseAfterARealPreamble_IsRejected) {
  // The payload gates have to work even when the line genuinely starts with a
  // preamble: a dropout mid-line must not yield 42 bytes of invented data.
  auto packet = make_transmission_packet(make_parity_coded_payload());
  auto line = synthesize_teletext_line_raw(packet, tape_like_options());

  uint32_t state = 12345;
  const auto burst_start = static_cast<size_t>(14.0 * kPalSampleRate / 1e6);
  for (size_t s = burst_start; s < line.size(); ++s) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    line[s] = static_cast<int16_t>(kPalBlack + (state % 400));
  }

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  EXPECT_FALSE(slice_line(line, options).valid);
}

// ---------------------------------------------------------------------------
// Fractionally-spaced sampling
//
// The MLSE detector reads each bit at three positions within its bit period,
// so the interpolation kernel sits in the signal path. Linear interpolation
// is a low-pass in its own right and attenuates the transition band a
// band-limited channel has already eaten into; these tests pin the kernel's
// shape and measure its amplitude error against the linear alternative.
// ---------------------------------------------------------------------------

// Linear interpolation, the alternative the cubic kernel is measured against.
double linear_interpolate(const std::vector<int16_t>& line, double t) {
  const auto i = static_cast<size_t>(t);
  const double frac = t - static_cast<double>(i);
  return line[i] + (line[i + 1] - line[i]) * frac;
}

TEST(TeletextInterpolateSample, ReproducesInputSamplesAtIntegerPositions) {
  // An interpolating kernel passes through its input; a smoothing one does
  // not, and would shift every bit-centre reading of the detector.
  const std::vector<int16_t> line{100, -40, 900, 12, -700, 355, 0, 88};
  for (size_t i = 0; i < line.size(); ++i) {
    EXPECT_NEAR(teletext_interpolate_sample(line.data(), line.size(),
                                            static_cast<double>(i)),
                line[i], 1e-9)
        << "i=" << i;
  }
}

TEST(TeletextInterpolateSample, ImpulseResponseIsTheCatmullRomKernel) {
  // The four-point cubic evaluates to (-1, 9, 9, -1)/16 at the half sample,
  // so an isolated impulse must come back with that kernel's shape — the
  // negative outer lobes are the passband correction linear interpolation
  // lacks.
  std::vector<int16_t> line(16, 0);
  constexpr int kImpulse = 8;
  constexpr double kAmplitude = 1000.0;
  line[kImpulse] = static_cast<int16_t>(kAmplitude);

  const auto at = [&line](double t) {
    return teletext_interpolate_sample(line.data(), line.size(), t);
  };
  EXPECT_NEAR(at(kImpulse - 0.5), 0.5625 * kAmplitude, 1e-9);
  EXPECT_NEAR(at(kImpulse + 0.5), 0.5625 * kAmplitude, 1e-9);
  EXPECT_NEAR(at(kImpulse - 1.5), -0.0625 * kAmplitude, 1e-9);
  EXPECT_NEAR(at(kImpulse + 1.5), -0.0625 * kAmplitude, 1e-9);
}

TEST(TeletextInterpolateSample, PositionsOutsideTheLineClamp) {
  const std::vector<int16_t> line{50, 60, 70, 80};
  EXPECT_NEAR(teletext_interpolate_sample(line.data(), line.size(), -4.0), 50.0,
              1e-9);
  EXPECT_NEAR(teletext_interpolate_sample(line.data(), line.size(), 9.0), 80.0,
              1e-9);
  EXPECT_EQ(teletext_interpolate_sample(nullptr, 0, 1.0), 0.0);
}

// Peak absolute reconstruction error of each kernel against |truth|, sampled
// on a fine sub-sample sweep across the interior of the line.
struct InterpolationError {
  double cubic = 0.0;
  double linear = 0.0;
};

InterpolationError measure_interpolation_error(
    const std::vector<int16_t>& line,
    const std::function<double(double)>& truth) {
  InterpolationError error;
  // Twenty sub-sample steps per sample across the interior of the line. The
  // sweep is driven by an integer count rather than an accumulating double so
  // the step lands on the same positions however long the line is.
  constexpr double kFirstSample = 4.0;
  constexpr int kStepsPerSample = 20;
  const int steps =
      (static_cast<int>(line.size()) - 5 - static_cast<int>(kFirstSample)) *
      kStepsPerSample;
  for (int step = 0; step < steps; ++step) {
    const double t = kFirstSample + static_cast<double>(step) /
                                        static_cast<double>(kStepsPerSample);
    const double expected = truth(t);
    error.cubic = std::max(
        error.cubic,
        std::abs(teletext_interpolate_sample(line.data(), line.size(), t) -
                 expected));
    error.linear = std::max(error.linear,
                            std::abs(linear_interpolate(line, t) - expected));
  }
  return error;
}

TEST(TeletextInterpolateSample, SineErrorIsFarBelowLinearInterpolation) {
  // A tone at the top of the VHS luma band: the frequencies that survive the
  // channel are exactly the ones linear interpolation damages most.
  constexpr double kToneHz = 2.5e6;
  constexpr double kAmplitude = 400.0;
  constexpr double kPi = 3.14159265358979323846;
  const auto truth = [&](double t) {
    return kAmplitude * std::sin(2.0 * kPi * kToneHz * t / kPalSampleRate);
  };

  std::vector<int16_t> line(256);
  for (size_t s = 0; s < line.size(); ++s) {
    line[s] = static_cast<int16_t>(std::lround(truth(static_cast<double>(s))));
  }

  const auto error = measure_interpolation_error(line, truth);
  // 2,5 MHz is where the cubic's margin is *narrowest* — its error grows as
  // the fourth power of frequency against the linear kernel's square — and it
  // is still better by more than six times, at 1,5 % of the tone amplitude
  // against 10 %.
  EXPECT_LT(error.cubic, 0.2 * error.linear)
      << "cubic=" << error.cubic << " linear=" << error.linear;
  EXPECT_LT(error.cubic, 0.02 * kAmplitude);
}

TEST(TeletextInterpolateSample, BandLimitedFixtureErrorIsBelowLinear) {
  // Three tones spanning the band a consumer VHS luma channel passes.
  constexpr double kPi = 3.14159265358979323846;
  const std::array<double, 3> tones{1.0e6, 2.0e6, 2.8e6};
  const std::array<double, 3> amplitudes{200.0, 120.0, 80.0};
  const auto truth = [&](double t) {
    double sum = 0.0;
    for (size_t i = 0; i < tones.size(); ++i) {
      sum += amplitudes[i] *
             std::sin(2.0 * kPi * tones[i] * t / kPalSampleRate + 0.4 * i);
    }
    return sum;
  };

  std::vector<int16_t> line(512);
  for (size_t s = 0; s < line.size(); ++s) {
    line[s] = static_cast<int16_t>(std::lround(truth(static_cast<double>(s))));
  }

  const auto error = measure_interpolation_error(line, truth);
  EXPECT_LT(error.cubic, 0.15 * error.linear)
      << "cubic=" << error.cubic << " linear=" << error.linear;
}

// ---------------------------------------------------------------------------
// Fractionally-spaced channel fit and branch metric
// ---------------------------------------------------------------------------

// A channel that is exactly a five-bit-tap response per sample phase.
//
// Driving the slicer at a bit rate of a third the sample rate puts three
// samples in every bit period, so the three positions the detector reads land
// on integer sample offsets and a sample-spaced FIR spanning ± 2 bit periods
// *is* the polyphase model the fit claims to recover — a different bit-spaced
// tap vector at each phase, sharing one DC level. If the fit recovers it, the
// preamble it was fitted to and the payload it was never shown both reconstruct
// to within the line's quantisation.
constexpr double kIntegerGridBitRate = kPalSampleRate / 3.0;

// Deliberately asymmetric so the three phases see genuinely different
// responses: a symmetric one would make two of them equal and the test
// vacuous. Normalised to unit DC gain, so the recovered data amplitude still
// sits at the EN 300 706 §5.2 level.
constexpr std::array<double, 13> kPolyphaseChannel{0.02, 0.05, 0.09, 0.14, 0.18,
                                                   0.16, 0.13, 0.09, 0.06, 0.04,
                                                   0.02, 0.01, 0.01};

std::vector<int16_t> apply_polyphase_channel(const std::vector<int16_t>& line) {
  double gain = 0.0;
  for (const double tap : kPolyphaseChannel) {
    gain += tap;
  }
  std::vector<int16_t> out(line.size());
  for (size_t s = 0; s < line.size(); ++s) {
    double sum = 0.0;
    for (size_t m = 0; m < kPolyphaseChannel.size(); ++m) {
      const auto index = static_cast<long>(s) - static_cast<long>(m);
      const size_t clamped = static_cast<size_t>(std::max(index, 0L));
      sum += kPolyphaseChannel[m] * line[clamped];
    }
    out[s] = static_cast<int16_t>(std::lround(sum / gain));
  }
  return out;
}

std::vector<int16_t> synthesize_polyphase_channel_line(
    const std::array<uint8_t, kTeletextPacketBytes>& payload,
    int32_t noise_amplitude = 0, uint32_t noise_seed = 1) {
  TeletextLineSynthOptions opt;
  opt.bit_rate = kIntegerGridBitRate;
  // 360 bits at three samples each, plus the burst offset and the channel's
  // group delay (EN 300 706 §7.1).
  opt.sample_count = 1500;
  auto line = apply_polyphase_channel(synthesize_teletext_line(payload, opt));
  if (noise_amplitude > 0) {
    uint32_t state = noise_seed != 0 ? noise_seed : 1;
    for (auto& sample : line) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      const int32_t span = 2 * noise_amplitude + 1;
      const auto noise =
          static_cast<int32_t>(state % static_cast<uint32_t>(span)) -
          noise_amplitude;
      sample = static_cast<int16_t>(sample + noise);
    }
  }
  return line;
}

TeletextLineResult slice_integer_grid_line(const std::vector<int16_t>& line,
                                           int samples_per_bit) {
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  options.mlse_samples_per_bit = samples_per_bit;
  return TeletextSlicer(kPalSampleRate, kIntegerGridBitRate, options)
      .slice(line.data(), line.size(), static_cast<int16_t>(kPalBlack),
             static_cast<int16_t>(kPalWhite));
}

TEST(TeletextSlicerMlse, PolyphaseFitReproducesTheChannelAtEveryPhase) {
  const auto payload = make_parity_coded_payload();
  const auto line = synthesize_polyphase_channel_line(payload);

  const auto result = slice_integer_grid_line(line, /*samples_per_bit=*/3);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
  // The known preamble and the unknown payload both reconstruct to within a
  // few counts of the ≈ 388-count data step: no phase is left mismodelled. A
  // fit that used one tap vector for all three phases could not reach this —
  // the phases of this channel differ by far more than a rounding step. The
  // lock sits where the response of a bit peaks, one bit before the alignment
  // that fits this four-bit-long channel best, so a little of its tail spills
  // past the five taps and into the payload residual.
  EXPECT_LT(result.preamble_residual, 0.01);
  EXPECT_LT(result.payload_residual, 0.02);
}

// Byte-level accuracy of |result| against |payload|; an invalid result scores
// nothing. Counting bytes rather than packets makes the comparison degrade
// smoothly instead of collapsing at the gates.
int correct_bytes(const TeletextLineResult& result,
                  const std::array<uint8_t, kTeletextPacketBytes>& payload) {
  if (!result.valid) {
    return 0;
  }
  int correct = 0;
  for (size_t i = 0; i < kTeletextPacketBytes; ++i) {
    correct += result.bytes[i] == payload[i] ? 1 : 0;
  }
  return correct;
}

TEST(TeletextSlicerMlse, PolyphaseMetricBeatsBitCentreUnderNoise) {
  // Task 2.3's graded-SNR comparison on the exactly-modelled channel above:
  // three phases of evidence per bit against one, at matched noise. Bytes are
  // counted rather than packets so the comparison degrades smoothly.
  const auto payload = make_parity_coded_payload();

  // The sweep stops short of the noise level at which both detectors lose the
  // packet outright: past that point the totals are two small numbers of
  // surviving bytes and compare nothing.
  bool strictly_better_somewhere = false;
  for (const int32_t noise : {0, 40, 80, 120}) {
    int centre_total = 0;
    int polyphase_total = 0;
    for (uint32_t seed = 1; seed <= 12; ++seed) {
      const auto line =
          synthesize_polyphase_channel_line(payload, noise, seed * 7919u + 13u);
      centre_total += correct_bytes(slice_integer_grid_line(line, 1), payload);
      polyphase_total +=
          correct_bytes(slice_integer_grid_line(line, 3), payload);
    }
    EXPECT_GE(polyphase_total, centre_total) << "noise=" << noise;
    strictly_better_somewhere |= polyphase_total > centre_total;
  }
  EXPECT_TRUE(strictly_better_somewhere);
}

TEST(TeletextSlicerMlse, PolyphaseMetricBeatsBitCentreOnATapeLikeChannel) {
  // The same comparison at the real ≈ 2.556 samples per bit, where the three
  // grid phases fall between captured samples and are interpolated: the
  // operational case, against the 2,8 MHz roll-off of consumer VHS luma.
  //
  // Packets recovered exactly is the metric here rather than bytes, because the
  // detector's second pass (see the channel refit in slice_mlse) turned what
  // used to be a graded byte-error comparison into a mostly all-or-nothing one:
  // below the noise level at which a line is lost outright, both configurations
  // now decode nearly every byte of nearly every accepted packet, and counting
  // bytes measures which lines happened to be accepted rather than how well
  // they were read. Where the extra phases still tell is at the top of the
  // range, and there they tell decisively — two and a half times the packets
  // at noise 80.
  const auto payload = make_parity_coded_payload();

  // Where the channel is stressed enough that noise, not model mismatch,
  // decides the bits (10-bit level counts, against a ≈ 388-count data step).
  constexpr int32_t kNoiseDominatedFrom = 80;
  constexpr uint32_t kSeeds = 96;

  bool strictly_better_somewhere = false;
  for (const int32_t noise : {0, 20, 40, 60, 70, 80}) {
    int centre_exact = 0;
    int polyphase_exact = 0;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
      auto synth = tape_like_options();
      synth.noise_amplitude = noise;
      synth.noise_seed = seed * 7919u + 13u;
      // Spread the burst across a whole bit period so no single sub-sample
      // alignment dominates the comparison.
      synth.phase_offset_samples = 0.2 * static_cast<double>(seed % 13);
      const auto line = synthesize_teletext_line(payload, synth);

      TeletextSlicerOptions options;
      options.detector = TeletextDetector::kMlse;
      options.mlse_samples_per_bit = 1;
      centre_exact += correct_bytes(slice_line(line, options), payload) ==
                              static_cast<int>(kTeletextPacketBytes)
                          ? 1
                          : 0;
      options.mlse_samples_per_bit = 3;
      polyphase_exact += correct_bytes(slice_line(line, options), payload) ==
                                 static_cast<int>(kTeletextPacketBytes)
                             ? 1
                             : 0;
    }

    // Never materially worse, at any noise level.
    EXPECT_GE(100 * polyphase_exact, 95 * centre_exact) << "noise=" << noise;
    if (noise >= kNoiseDominatedFrom) {
      // And decisively better wherever noise is what is costing the packets.
      EXPECT_GT(100 * polyphase_exact, 150 * centre_exact) << "noise=" << noise;
      strictly_better_somewhere = true;
    }
  }
  EXPECT_TRUE(strictly_better_somewhere);
}

// ---------------------------------------------------------------------------
// Two-pass detection: terminated trellis and decision-directed channel refit
// ---------------------------------------------------------------------------

TEST(TeletextSlicerMlse, BandLimitedLine_RecoversExactlyAtEveryBitPhase) {
  // The channel refit is what makes an undisturbed band-limited line decode
  // exactly rather than nearly. Five taps fitted to the 24 preamble bits alone
  // describe the head of the channel's pulse response and not its tail, and on
  // this fixture that mismodelling cost bytes at most sub-sample alignments;
  // refitting the same five taps against all 360 bits removes them.
  const auto payload = make_parity_coded_payload();
  for (int step = 0; step < 10; ++step) {
    auto synth = tape_like_options();
    synth.phase_offset_samples = 0.25 * static_cast<double>(step);
    const auto line = synthesize_teletext_line(payload, synth);

    TeletextSlicerOptions options;
    options.detector = TeletextDetector::kMlse;
    const auto result = slice_line(line, options);
    ASSERT_TRUE(result.valid) << "phase step " << step;
    EXPECT_EQ(result.bytes, payload) << "phase step " << step;
  }
}

TEST(TeletextSlicerMlse, FinalDataByteIsNoWorseThanTheRestOfThePacket) {
  // ETSI EN 300 706 §7.1: nothing follows the 360th bit, so the line is back at
  // black there and the trellis is terminated in that known state. Without it
  // the final bits are chosen to explain the last samples rather than pinned by
  // evidence on both sides, and the last data byte decodes several times worse
  // than any other position — measured at 30,7 % against a 7,3 % packet average
  // on the reference VHS LP capture.
  const auto payload = make_parity_coded_payload();
  for (const int32_t noise : {40, 60, 80}) {
    int accepted = 0;
    int final_byte_errors = 0;
    int other_byte_errors = 0;
    for (uint32_t seed = 1; seed <= 96; ++seed) {
      auto synth = tape_like_options();
      synth.noise_amplitude = noise;
      synth.noise_seed = seed * 7919u + 13u;
      synth.phase_offset_samples = 0.2 * static_cast<double>(seed % 13);
      const auto line = synthesize_teletext_line(payload, synth);

      TeletextSlicerOptions options;
      options.detector = TeletextDetector::kMlse;
      const auto result = slice_line(line, options);
      if (!result.valid) {
        continue;
      }
      ++accepted;
      const size_t last = kTeletextPacketBytes - 1;
      final_byte_errors += result.bytes[last] != payload[last] ? 1 : 0;
      for (size_t i = 2; i < last; ++i) {
        other_byte_errors += result.bytes[i] != payload[i] ? 1 : 0;
      }
    }
    ASSERT_GT(accepted, 0) << "noise=" << noise;
    // The final byte sits with the rest rather than apart from it. The bound is
    // twice the mean error count of the other data positions plus one, which is
    // wide enough for a single unlucky line and an order of magnitude below the
    // rate an unterminated trellis leaves behind.
    const double mean_other = static_cast<double>(other_byte_errors) /
                              static_cast<double>(kTeletextPacketBytes - 3);
    EXPECT_LE(static_cast<double>(final_byte_errors), 1.0 + 2.0 * mean_other)
        << "noise=" << noise << ", accepted=" << accepted;
  }
}

// A playback chain that rings past the tap window. The response to one bit
// rises over a bit and a half, falls in under one, and then undershoots by a
// fifth of its peak four bit periods later: the shape of a capture card dump
// whose recorder and card undershoot by 4-6 % of the data step three to six
// bits after the peak, made deeper here so that the tests
// turn on it. |tau| is in bit periods from the bit's centre.
double ringing_channel_pulse(double tau) {
  const double width = tau < 0.0 ? 1.2 : 0.7;
  const double dip = tau - 4.0;
  return std::exp(-tau * tau / (2.0 * width * width)) -
         0.2 * std::exp(-dip * dip / 2.0);
}

// The transmission |bytes| through that channel, at the levels and timing of
// |opt|, with uniform noise of ± |noise| counts drawn as the line synthesizer
// draws it.
std::vector<int16_t> synthesize_ringing_line(
    const std::vector<uint8_t>& bytes, const TeletextLineSynthOptions& opt,
    int32_t noise, uint32_t seed, double phase_offset_samples) {
  const double spb = opt.sample_rate / opt.bit_rate;
  const double t0 =
      opt.first_bit_centre_us * opt.sample_rate / 1e6 + phase_offset_samples;
  const double one_step =
      opt.amplitude_fraction *
      static_cast<double>(opt.white_level - opt.black_level);
  // Unit DC gain, so a long run of ones settles at the '1' level.
  double gain = 0.0;
  for (int j = -12; j <= 12; ++j) {
    gain += ringing_channel_pulse(static_cast<double>(j));
  }
  const int bits = static_cast<int>(bytes.size()) * 8;
  std::vector<int16_t> line(opt.sample_count, opt.black_level);
  uint32_t state = seed != 0 ? seed : 1;
  for (size_t n = 0; n < line.size(); ++n) {
    const double t = (static_cast<double>(n) - t0) / spb;
    const int first = std::max(0, static_cast<int>(std::floor(t)) - 12);
    const int last = std::min(bits - 1, static_cast<int>(std::ceil(t)) + 12);
    double response = 0.0;
    for (int j = first; j <= last; ++j) {
      if (((bytes[static_cast<size_t>(j) >> 3] >> (j & 7)) & 1) != 0) {
        response += ringing_channel_pulse(t - static_cast<double>(j));
      }
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const auto drawn =
        static_cast<int32_t>(state % static_cast<uint32_t>(2 * noise + 1)) -
        noise;
    line[n] = static_cast<int16_t>(
        std::lround(static_cast<double>(opt.black_level) +
                    one_step * response / gain + static_cast<double>(drawn)));
  }
  return line;
}

TEST(TeletextSlicerMlse, ReadsAChannelThatRingsPastItsTapWindow) {
  // The refits that model the bits before the five-tap window read rows a
  // ringing playback chain leaves no five-tap channel able to explain. At this
  // noise the five taps alone read 3 of these 64 rows exactly and lost 5
  // outright; with the tail all 64 come back exactly. The tail refits are
  // among the wider readings (TeletextSlicerOptions::extended_search).
  const auto payload = make_parity_coded_payload();
  const auto bytes = make_transmission_bytes(payload);
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  options.extended_search = true;
  constexpr uint32_t kSeeds = 64;
  int exact = 0;
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    const auto line = synthesize_ringing_line(
        bytes, TeletextLineSynthOptions{}, /*noise=*/60, seed * 7919u + 13u,
        0.2 * static_cast<double>(seed % 13));
    const auto result = slice_line(line, options);
    exact += result.valid && result.bytes == payload ? 1 : 0;
  }
  EXPECT_GE(exact, 60);
}

TEST(TeletextSlicerMlse, APacketThatRunsToTheEndOfTheLineIsReadWhole) {
  // The detector interpolates every position whose four-point window lies
  // inside the line from coefficients worked out once per line, and the rest
  // through teletext_interpolate_sample, which clamps the window. Cutting the
  // line just after the last payload bit puts that bit's window over the end,
  // so one packet is read across both paths.
  const auto payload = make_parity_coded_payload();
  const auto line = synthesize_teletext_line(payload, tape_like_options());
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto full = slice_line(line, options);
  ASSERT_TRUE(full.valid);
  ASSERT_EQ(full.bytes, payload);

  const double last_bit =
      full.data_start_sample +
      (kTeletextPayloadBits - 1) * make_slicer(options).samples_per_bit();
  const auto cut = static_cast<size_t>(std::floor(last_bit)) + 2;
  ASSERT_LT(cut, line.size());
  const std::vector<int16_t> short_line(
      line.begin(), line.begin() + static_cast<ptrdiff_t>(cut));

  const auto result = slice_line(short_line, options);
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
}

TEST(TeletextSlicerMlse, SamplesPerBitIsClampedToTheSupportedRange) {
  // Out-of-range settings must degrade to a working detector rather than read
  // off the end of the phase tables.
  const auto payload = make_parity_coded_payload();
  const auto line = synthesize_teletext_line(payload, tape_like_options());
  for (const int samples_per_bit : {-1, 0, 1, 2, 3, 4, 99}) {
    TeletextSlicerOptions options;
    options.detector = TeletextDetector::kMlse;
    options.mlse_samples_per_bit = samples_per_bit;
    const auto result = slice_line(line, options);
    ASSERT_TRUE(result.valid) << "samples_per_bit=" << samples_per_bit;
    EXPECT_EQ(result.bytes, payload) << "samples_per_bit=" << samples_per_bit;
  }
}

// ---------------------------------------------------------------------------
// Per-bit reconstruction error (timing diagnostic)
//
// The profile TeletextLineResult::payload_bit_errors reports is what separates
// a bit clock running at the wrong rate — an error that grows along the packet
// as each bit is read further from its centre than the last — from noise and
// intersymbol interference, which cost the same everywhere.
// ---------------------------------------------------------------------------

// Mean reconstruction error over each quarter of the payload. Quarters rather
// than raw bits because a single bit's error is dominated by which bits sit
// around it; the drift question is about the trend along the packet.
std::array<double, 4> quarter_error_profile(const TeletextLineResult& result) {
  constexpr size_t kQuarter = kTeletextPayloadBits / 4;
  std::array<double, 4> means{};
  for (size_t q = 0; q < means.size(); ++q) {
    double sum = 0.0;
    for (size_t bit = q * kQuarter; bit < (q + 1) * kQuarter; ++bit) {
      sum += static_cast<double>(result.payload_bit_errors[bit]);
    }
    means[q] = sum / static_cast<double>(kQuarter);
  }
  return means;
}

TEST(TeletextSlicerDrift, MatchedTimingGivesAFlatErrorProfile) {
  const auto payload = make_parity_coded_payload();
  const auto line = synthesize_teletext_line(payload, tape_like_options());

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto result = slice_line(line, options);

  ASSERT_TRUE(result.valid);
  ASSERT_EQ(result.bytes, payload);
  const auto quarters = quarter_error_profile(result);
  // Flat: the model mismatch of the band-limited channel costs the same at both
  // ends of the packet, so no quarter rises above the first. Quarters do differ
  // by which bits happen to sit in them — hence a bound rather than equality —
  // but nothing here trends upwards.
  for (size_t q = 1; q < quarters.size(); ++q) {
    EXPECT_LT(quarters[q], 1.1 * quarters[0]) << "quarter " << q;
  }
}

TEST(TeletextSlicerDrift, SamplesPerBitErrorGrowsTheErrorProfile) {
  // The line is synthesized at a bit rate 0,1 % away from the ETSI EN 300 706
  // §5.3 rate the slicer assumes — far outside the ± 25 ppm the spec allows,
  // and the signature of a tape whose transport is not running at the speed it
  // was recorded at. The bit phase is acquired on the preamble at the start of
  // the line, so the error accumulates from there across the packet: a third of
  // a bit period by the last byte.
  const auto payload = make_parity_coded_payload();
  auto synth = tape_like_options();
  synth.bit_rate = kTeletextBitRate * 1.001;
  const auto line = synthesize_teletext_line(payload, synth);

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto result = slice_line(line, options);

  const auto quarters = quarter_error_profile(result);
  for (size_t q = 1; q < quarters.size(); ++q) {
    EXPECT_GT(quarters[q], quarters[q - 1]) << "quarter " << q;
  }
  // And by the end of the packet the rise is unmistakable against the flat
  // profile of the matched line above.
  EXPECT_GT(quarters[3], 1.3 * quarters[0]);
}

// The same line read at the bit clock it was sent at: the recording's clock
// taken as the slicer's own (TeletextSlicerOptions::bit_clock) puts every bit
// back where the detector looks for it.
TEST(TeletextSlicerDrift, ReadingAtTheLinesOwnClockFlattensTheProfile) {
  const auto payload = make_parity_coded_payload();
  auto synth = tape_like_options();
  synth.bit_rate = kTeletextBitRate * 1.001;
  const auto line = synthesize_teletext_line(payload, synth);

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  options.bit_clock = 1.001;
  const auto result = slice_line(line, options);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
  const auto quarters = quarter_error_profile(result);
  for (size_t q = 1; q < quarters.size(); ++q) {
    EXPECT_LT(quarters[q], 1.1 * quarters[0]) << "quarter " << q;
  }
}

TEST(TeletextSlicerDrift, NoDetectionLeavesTheProfileEmpty) {
  // Lines the MLSE detector never ran on report no profile rather than a stale
  // one: the threshold detector reconstructs nothing to measure.
  const auto payload = make_test_payload();
  const auto line = synthesize_teletext_line(payload);
  const auto result = slice_line(line);

  ASSERT_TRUE(result.valid);
  ASSERT_EQ(result.detector, TeletextDetector::kThreshold);
  for (const float error : result.payload_bit_errors) {
    EXPECT_EQ(error, 0.0F);
  }
}

// ---------------------------------------------------------------------------
// Rejection reasons
//
// One synthesized line per early-return path of slice(), slice_threshold()
// and slice_mlse(), asserting the reason each reports. These are diagnostics:
// what is being pinned here is that a discarded line says which gate
// discarded it, so a recovery profile can tell "nothing on this line" from
// "data that did not survive".
// ---------------------------------------------------------------------------

// A line rising monotonically from black to white across its whole length.
// It clears the coarse amplitude gate, but the run-in correlation (§6.1) is
// negative at every phase — each +/- kernel pair straddles a rising edge — so
// the threshold detector never locks.
std::vector<int16_t> make_ramp_line() {
  const auto count = static_cast<size_t>(kPalSamplesPerLineNominal);
  std::vector<int16_t> line(count);
  for (size_t s = 0; s < count; ++s) {
    const double fraction = static_cast<double>(s) / (count - 1);
    line[s] =
        static_cast<int16_t>(kPalBlack + fraction * (kPalWhite - kPalBlack));
  }
  return line;
}

// A line held at white: bright enough to pass the coarse gate, but with no
// structure for a channel fit to explain, so every fitted response has zero
// gain.
std::vector<int16_t> make_flat_white_line() {
  return std::vector<int16_t>(static_cast<size_t>(kPalSamplesPerLineNominal),
                              static_cast<int16_t>(kPalWhite));
}

TEST(TeletextSlicerRejectReason, ValidLineReportsNoReason) {
  const auto result = slice_line(synthesize_teletext_line(make_test_payload()));
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.reject_reason, TeletextRejectReason::kNone);
}

TEST(TeletextSlicerRejectReason, NullOrShortLine_ReportsInsufficientSamples) {
  const auto slicer = make_slicer();
  EXPECT_EQ(slicer
                .slice(nullptr, 1135, static_cast<int16_t>(kPalBlack),
                       static_cast<int16_t>(kPalWhite))
                .reject_reason,
            TeletextRejectReason::kInsufficientSamples);
  const std::vector<int16_t> short_line(64, static_cast<int16_t>(kPalBlack));
  EXPECT_EQ(slicer
                .slice(short_line.data(), short_line.size(),
                       static_cast<int16_t>(kPalBlack),
                       static_cast<int16_t>(kPalWhite))
                .reject_reason,
            TeletextRejectReason::kInsufficientSamples);
}

TEST(TeletextSlicerRejectReason, DegenerateLevels_ReportsInsufficientSamples) {
  // White at or below black leaves no §5.2 data level to measure against.
  const auto line = synthesize_teletext_line(make_test_payload());
  const auto result = make_slicer().slice(line.data(), line.size(),
                                          static_cast<int16_t>(kPalWhite),
                                          static_cast<int16_t>(kPalBlack));
  EXPECT_EQ(result.reject_reason, TeletextRejectReason::kInsufficientSamples);
}

TEST(TeletextSlicerRejectReason, BlankLine_ReportsAmplitudeGate) {
  const std::vector<int16_t> line(
      static_cast<size_t>(kPalSamplesPerLineNominal),
      static_cast<int16_t>(kPalBlack));
  EXPECT_EQ(slice_line(line).reject_reason,
            TeletextRejectReason::kAmplitudeGate);
}

TEST(TeletextSlicerRejectReason, RampWithoutRunIn_ReportsNoRunInLock) {
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kThreshold;
  EXPECT_EQ(slice_line(make_ramp_line(), options).reject_reason,
            TeletextRejectReason::kNoRunInLock);
}

TEST(TeletextSlicerRejectReason, BandLimitedLine_ReportsRunInAmplitude) {
  // The tape case: the channel has attenuated the 3,47 MHz run-in fundamental
  // (§6.1), so the 1 and 0 levels recovered at the locked phase are too close
  // together to slice between. This reason is what says "try MLSE here".
  const auto line = synthesize_teletext_line(make_parity_coded_payload(),
                                             tape_like_options());
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kThreshold;
  EXPECT_EQ(slice_line(line, options).reject_reason,
            TeletextRejectReason::kRunInAmplitude);
}

TEST(TeletextSlicerRejectReason, DamagedRunIn_ReportsRunInPattern) {
  // Second run-in byte reduced to a single one: the correlation still locks
  // with usable amplitude, but three of the sixteen bits no longer alternate
  // (§6.1), one more than the note's allowance.
  auto packet = make_transmission_packet(make_test_payload());
  packet[1] = 0x40;
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kThreshold;
  EXPECT_EQ(
      slice_line(synthesize_teletext_line_raw(packet), options).reject_reason,
      TeletextRejectReason::kRunInPattern);
}

TEST(TeletextSlicerRejectReason, NoFramingCode_ReportsFramingCodeMiss) {
  auto packet = make_transmission_packet(make_test_payload());
  packet[2] = 0x00;
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kThreshold;
  EXPECT_EQ(
      slice_line(synthesize_teletext_line_raw(packet), options).reject_reason,
      TeletextRejectReason::kFramingCodeMiss);
}

TEST(TeletextSlicerRejectReason, UncorrectableMrag_ReportsInvalidMrag) {
  auto payload = make_test_payload();
  payload[0] ^= 0b00000011;
  payload[1] ^= 0b00011000;
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kThreshold;
  EXPECT_EQ(
      slice_line(synthesize_teletext_line(payload), options).reject_reason,
      TeletextRejectReason::kInvalidMrag);
}

TEST(TeletextSlicerRejectReasonMlse, FlatLine_ReportsNoPreambleLock) {
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  EXPECT_EQ(slice_line(make_flat_white_line(), options).reject_reason,
            TeletextRejectReason::kNoPreambleLock);
}

TEST(TeletextSlicerRejectReasonMlse, NoiseLine_ReportsPreambleResidual) {
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto result = slice_line(make_noise_line(1, 0.0), options);
  EXPECT_EQ(result.reject_reason, TeletextRejectReason::kPreambleResidual);
  // The residual is reported even though the line was rejected — the rejected
  // tail is what the gate is tuned against.
  EXPECT_GT(result.preamble_residual, 0.0);
}

TEST(TeletextSlicerRejectReasonMlse,
     DropoutAfterPreamble_ReportsPayloadResidual) {
  // A real preamble followed by a dropout that saturates the luma to peak
  // white: the fitted channel cannot produce a level anywhere near that, so
  // the reconstruction of the recovered bits misses the samples by far more
  // than the gate allows.
  auto packet = make_transmission_packet(make_parity_coded_payload());
  auto line = synthesize_teletext_line_raw(packet, tape_like_options());
  const auto dropout_start = static_cast<size_t>(14.0 * kPalSampleRate / 1e6);
  for (size_t s = dropout_start; s < line.size(); ++s) {
    line[s] = static_cast<int16_t>(kPalPeak);
  }

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto result = slice_line(line, options);
  EXPECT_EQ(result.reject_reason, TeletextRejectReason::kPayloadResidual);
  EXPECT_GT(result.payload_residual, 0.0);
}

TEST(TeletextSlicerRejectReasonMlse, UncorrectableMrag_ReportsInvalidMrag) {
  auto payload = make_parity_coded_payload();
  payload[0] ^= 0b00000011;
  payload[1] ^= 0b00011000;
  const auto line = synthesize_teletext_line(payload, tape_like_options());

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  EXPECT_EQ(slice_line(line, options).reject_reason,
            TeletextRejectReason::kInvalidMrag);
}

TEST(TeletextSlicerRejectReasonMlse, UncodedDataBytes_ReportsParityFraction) {
  // Row 0 is parity coded (§9.3.1), so a payload whose data bytes are not
  // odd-parity coded fails the plausibility gate however cleanly it decoded.
  const auto line =
      synthesize_teletext_line(make_test_payload(), tape_like_options());
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  EXPECT_EQ(slice_line(line, options).reject_reason,
            TeletextRejectReason::kParityFraction);
}

// ---------------------------------------------------------------------------
// Soft decisions: per-byte confidence
//
// The Viterbi decides a bit by how much better the best sequence carrying it
// one way is than the best sequence carrying it the other. That margin is a
// measurement of the bit, and what these pin is that it means what it claims:
// high where the detector is right, low where it is wrong.
// ---------------------------------------------------------------------------

TEST(TeletextSlicerConfidence, ThresholdDetectorReportsTheEyeMargin) {
  // A clean line reads every bit centre at a full level, so every byte is as
  // sure as a byte can be; band limiting pulls the bit centres in towards the
  // slicing level, and the margin says so. Reported only when asked for
  // (TeletextSlicerOptions::extended_search).
  const auto payload = make_test_payload();
  TeletextSlicerOptions extended;
  extended.extended_search = true;
  const auto clean = slice_line(synthesize_teletext_line(payload), extended);
  ASSERT_TRUE(clean.valid);
  ASSERT_EQ(clean.detector, TeletextDetector::kThreshold);
  ASSERT_TRUE(clean.has_byte_confidence);
  for (size_t i = 0; i < kTeletextPacketBytes; ++i) {
    EXPECT_GE(clean.byte_confidence[i], 0.99F) << "byte " << i;
    EXPECT_LE(clean.byte_confidence[i], 1.0F) << "byte " << i;
  }

  TeletextLineSynthOptions synth;
  synth.low_pass_cutoff_hz = 5.0e6;
  const auto limited =
      slice_line(synthesize_teletext_line(payload, synth), extended);
  ASSERT_TRUE(limited.valid);
  ASSERT_EQ(limited.detector, TeletextDetector::kThreshold);
  ASSERT_TRUE(limited.has_byte_confidence);
  const float weakest = *std::min_element(limited.byte_confidence.begin(),
                                          limited.byte_confidence.end());
  EXPECT_GT(weakest, 0.0F);
  EXPECT_LT(weakest, 1.0F);
}

// ---------------------------------------------------------------------------
// TeletextSlicerOptions::extended_search: off by default, so a disc or direct
// capture reads and costs as it did before the wider readings existed
// ---------------------------------------------------------------------------

// Without the option a threshold-read packet carries no confidence, so a row
// squasher fed it counts every copy alike, as it did before.
TEST(TeletextSlicerExtended, ThresholdDetectorReportsNoConfidenceByDefault) {
  TeletextLineSynthOptions synth;
  synth.low_pass_cutoff_hz = 5.0e6;
  const auto result =
      slice_line(synthesize_teletext_line(make_test_payload(), synth));
  ASSERT_TRUE(result.valid);
  ASSERT_EQ(result.detector, TeletextDetector::kThreshold);
  EXPECT_FALSE(result.has_byte_confidence);
  EXPECT_EQ(result.byte_confidence, TeletextPacketConfidence{});
}

// Lines the threshold detector reads, but some with an eye too closed to
// trust: channels just wide enough to pass the clock run-in, under noise, as
// on a worn tape.
std::vector<std::vector<int16_t>> closed_eye_lines(
    const std::array<uint8_t, kTeletextPacketBytes>& payload) {
  std::vector<std::vector<int16_t>> lines;
  for (const double cutoff : {3.6e6, 3.8e6, 4.0e6, 4.2e6, 4.4e6}) {
    for (uint32_t seed = 1; seed <= 20; ++seed) {
      TeletextLineSynthOptions synth;
      synth.low_pass_cutoff_hz = cutoff;
      synth.noise_amplitude = 80;
      synth.noise_seed = seed;
      synth.phase_offset_samples = 0.25 * static_cast<double>(seed % 10);
      lines.push_back(synthesize_teletext_line(payload, synth));
    }
  }
  return lines;
}

// By default Automatic keeps every line the threshold detector read as it
// read it, so it reads exactly as Threshold does wherever that locks.
TEST(TeletextSlicerExtended, AutoKeepsEveryThresholdReadingByDefault) {
  TeletextSlicerOptions threshold;
  threshold.detector = TeletextDetector::kThreshold;
  TeletextSlicerOptions automatic;
  automatic.detector = TeletextDetector::kAuto;
  int compared = 0;
  for (const auto& line : closed_eye_lines(make_parity_coded_payload())) {
    const auto direct = slice_line(line, threshold);
    if (!direct.valid) {
      continue;
    }
    ++compared;
    const auto chosen = slice_line(line, automatic);
    ASSERT_TRUE(chosen.valid);
    EXPECT_EQ(chosen.detector, TeletextDetector::kThreshold);
    EXPECT_EQ(chosen.bytes, direct.bytes);
  }
  EXPECT_GT(compared, 0);
}

TEST(TeletextSlicerExtended, AutoReadsALowMarginLineAgainWithMlseWhenAsked) {
  TeletextSlicerOptions threshold;
  threshold.detector = TeletextDetector::kThreshold;
  TeletextSlicerOptions automatic;
  automatic.detector = TeletextDetector::kAuto;
  automatic.extended_search = true;
  int fell_back = 0;
  for (const auto& line : closed_eye_lines(make_parity_coded_payload())) {
    const auto chosen = slice_line(line, automatic);
    fell_back += (slice_line(line, threshold).valid && chosen.valid &&
                  chosen.detector == TeletextDetector::kMlse)
                     ? 1
                     : 0;
  }
  EXPECT_GT(fell_back, 0);
}

// The MLSE detector's cost per line: by default one alignment of the run-in
// and no tail refits, as before; asked to, it reads several alignments and
// refits the channel with its tail.
TEST(TeletextSlicerExtended, MlseReadsOneAlignmentWithoutTailRefitsByDefault) {
  const auto payload = make_parity_coded_payload();
  const auto line = synthesize_teletext_line(payload, tape_like_options());
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto result = slice_line(line, options);
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
  EXPECT_EQ(result.mlse_alignments_read, 1);
  EXPECT_EQ(result.mlse_tail_refits, 0);
}

TEST(TeletextSlicerExtended, MlseWidensItsSearchWhenAsked) {
  const auto payload = make_parity_coded_payload();
  const auto line = synthesize_teletext_line(payload, tape_like_options());
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  options.extended_search = true;
  const auto result = slice_line(line, options);
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
  EXPECT_GT(result.mlse_alignments_read, 1);
  EXPECT_GE(result.mlse_tail_refits, 1);
}

TEST(TeletextSlicerConfidence, UndamagedLineIsConfidentInEveryByte) {
  const auto payload = make_parity_coded_payload();
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;

  // Both ends of the range the detector meets: a signal that arrives intact,
  // and one a tape has band-limited into intersymbol interference. The second
  // is inherently the less certain of the two — where the channel smears bits
  // together, flipping one is partly answered by its neighbours — so it is
  // held to a lower bar, not to none.
  struct Case {
    TeletextLineSynthOptions synth;
    double floor;
  };
  const Case cases[] = {{TeletextLineSynthOptions{}, 0.5},
                        {tape_like_options(), 0.25}};

  for (const auto& test_case : cases) {
    const auto line = synthesize_teletext_line(payload, test_case.synth);
    const auto result = slice_line(line, options);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.bytes, payload);
    ASSERT_TRUE(result.has_byte_confidence);
    for (size_t i = 0; i < kTeletextPacketBytes; ++i) {
      EXPECT_GE(result.byte_confidence[i], test_case.floor) << "byte " << i;
      EXPECT_LE(result.byte_confidence[i], 1.0F) << "byte " << i;
    }
  }
}

TEST(TeletextSlicerConfidence, MisreadBytesSitInTheLowConfidenceTail) {
  // The property that matters is ordering, not calibration: the bytes the
  // detector got wrong must be the ones it was least sure of. Measured over a
  // spread of noise levels so the comparison rests on hundreds of errors
  // rather than on whichever few a single fixture happens to produce — up to
  // a level the detector still misreads often enough to rank, which the
  // refits that model the channel's tail pushed higher.
  const auto payload = make_parity_coded_payload();
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;

  double correct_confidence = 0.0;
  double wrong_confidence = 0.0;
  int correct_bytes_seen = 0;
  int wrong_bytes_seen = 0;
  for (const int32_t noise : {40, 80, 120, 160}) {
    for (uint32_t seed = 1; seed <= 64; ++seed) {
      auto synth = tape_like_options();
      synth.noise_amplitude = noise;
      synth.noise_seed = seed * 7919u + 13u;
      synth.phase_offset_samples = 0.2 * static_cast<double>(seed % 13);
      const auto result =
          slice_line(synthesize_teletext_line(payload, synth), options);
      if (!result.valid || !result.has_byte_confidence) {
        continue;
      }
      for (size_t i = 0; i < kTeletextPacketBytes; ++i) {
        const double confidence =
            static_cast<double>(result.byte_confidence[i]);
        if (result.bytes[i] == payload[i]) {
          correct_confidence += confidence;
          ++correct_bytes_seen;
        } else {
          wrong_confidence += confidence;
          ++wrong_bytes_seen;
        }
      }
    }
  }

  ASSERT_GT(correct_bytes_seen, 0);
  ASSERT_GT(wrong_bytes_seen, 50) << "too few errors to rank";
  const double correct_mean =
      correct_confidence / static_cast<double>(correct_bytes_seen);
  const double wrong_mean =
      wrong_confidence / static_cast<double>(wrong_bytes_seen);
  EXPECT_LT(wrong_mean, 0.5 * correct_mean)
      << "correct mean " << correct_mean << ", wrong mean " << wrong_mean;
}

// ---------------------------------------------------------------------------
// Parity-guided repair
// ---------------------------------------------------------------------------

// A parity-coded payload addressed to |row|, with |even_parity_bytes| of its
// data bytes deliberately left at even parity — a byte that arrived damaged.
std::array<uint8_t, kTeletextPacketBytes> make_payload_with_damage(
    int row, std::initializer_list<size_t> even_parity_bytes) {
  auto payload = make_parity_coded_payload();
  const auto mrag = make_mrag(/*magazine=*/1, /*packet_number=*/row);
  payload[0] = mrag[0];
  payload[1] = mrag[1];
  for (const size_t index : even_parity_bytes) {
    // ETSI EN 300 706 §8.1: bit 8 is the parity bit, so flipping it is exactly
    // "the same character, transmitted with the wrong parity".
    payload[index] = static_cast<uint8_t>(payload[index] ^ 0x80);
  }
  return payload;
}

TEST(TeletextSlicerRepair, OffByDefault_DamagedBytesAreLeftAsTransmitted) {
  // The T42 contract is transmission coding: without being asked, the slicer
  // hands back what it read, parity failures and all.
  const auto payload = make_payload_with_damage(/*row=*/1, {5, 17, 33});
  const auto line = synthesize_teletext_line(payload, tape_like_options());

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto result = slice_line(line, options);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
  EXPECT_EQ(result.repaired_bytes, 0);
}

TEST(TeletextSlicerRepair, RestoresBytesTheDetectorMisread) {
  // The case the option exists for: noise flips a bit, parity says the byte is
  // wrong, and the detector's own margins say which bit to doubt. Counted over
  // a sweep, because whether any single line has a repairable error is a
  // property of the noise rather than of the detector.
  const auto payload = make_parity_coded_payload();
  TeletextSlicerOptions plain;
  plain.detector = TeletextDetector::kMlse;
  TeletextSlicerOptions repairing = plain;
  repairing.parity_repair = true;

  bool strictly_better_somewhere = false;
  for (const int32_t noise : {40, 80, 120}) {
    int plain_correct = 0;
    int repaired_correct = 0;
    for (uint32_t seed = 1; seed <= 64; ++seed) {
      auto synth = tape_like_options();
      synth.noise_amplitude = noise;
      synth.noise_seed = seed * 7919u + 13u;
      synth.phase_offset_samples = 0.2 * static_cast<double>(seed % 13);
      const auto line = synthesize_teletext_line(payload, synth);

      const auto before = slice_line(line, plain);
      const auto after = slice_line(line, repairing);
      if (!before.valid || !after.valid) {
        continue;
      }
      plain_correct += correct_bytes(before, payload);
      repaired_correct += correct_bytes(after, payload);

      // Whatever it changed, the packet it emits is still valid transmission
      // coding: a repaired byte satisfies the odd parity of §8.1.
      for (size_t i = 2; i < kTeletextPacketBytes; ++i) {
        if (after.bytes[i] != before.bytes[i]) {
          EXPECT_TRUE(teletext_odd_parity_valid(after.bytes[i]))
              << "byte " << i << " at noise " << noise;
        }
      }
    }
    EXPECT_GE(repaired_correct, plain_correct) << "noise=" << noise;
    strictly_better_somewhere |= repaired_correct > plain_correct;
  }
  EXPECT_TRUE(strictly_better_somewhere);
}

TEST(TeletextSlicerRepair, HeaderAddressAndControlBytesAreLeftAlone) {
  // ETSI EN 300 706 §9.3.1: the first ten bytes of a header are Hamming 8/4
  // coded. Those carry their own correction, which the page decoder applies
  // with the whole codeword in view; guessing a bit of one here would only
  // take that decision away from it.
  const auto payload = make_payload_with_damage(/*row=*/0, {2, 5, 9});
  const auto line = synthesize_teletext_line(payload, tape_like_options());

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  options.parity_repair = true;
  const auto result = slice_line(line, options);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
  EXPECT_EQ(result.repaired_bytes, 0);
}

TEST(TeletextSlicerRepair, NonParityCodedRowsAreLeftAlone) {
  // Rows above 25 carry Hamming 24/18 triplets or independent data (§9.6,
  // §9.8), which are not byte-wise odd parity: reading a parity failure there
  // as damage would be reading a coding it does not use.
  auto payload = make_test_payload();
  const auto mrag = make_mrag(/*magazine=*/1, /*packet_number=*/26);
  payload[0] = mrag[0];
  payload[1] = mrag[1];
  const auto line = synthesize_teletext_line(payload, tape_like_options());

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  options.parity_repair = true;
  const auto result = slice_line(line, options);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
  EXPECT_EQ(result.repaired_bytes, 0);
}

// ---------------------------------------------------------------------------
// Observation-string encoding
// ---------------------------------------------------------------------------

TEST(TeletextHex, ConfidenceSuffixRoundTrip) {
  const auto payload = make_test_payload();
  TeletextPacketConfidence confidence{};
  for (size_t i = 0; i < confidence.size(); ++i) {
    // Every quantisation level, sampled across the packet.
    confidence[i] = static_cast<float>(i % kTeletextConfidenceLevels) /
                    static_cast<float>(kTeletextConfidenceLevels - 1);
  }

  const std::string hex = teletext_packet_to_hex(payload, confidence);
  ASSERT_EQ(hex.size(), kTeletextPacketBytes * 3);

  const auto decoded = teletext_hex_to_observed_packet(hex);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->bytes, payload);
  EXPECT_TRUE(decoded->has_confidence);
  for (size_t i = 0; i < confidence.size(); ++i) {
    EXPECT_NEAR(decoded->confidence[i], confidence[i], 1e-6) << "byte " << i;
  }

  // The bytes are the same 84 characters they always were, so a consumer that
  // only wants the packet gets it from either form.
  EXPECT_EQ(hex.substr(0, kTeletextPacketBytes * 2),
            teletext_packet_to_hex(payload));
  EXPECT_EQ(teletext_hex_to_packet(hex), payload);
}

TEST(TeletextHex, LegacyStringDecodesAsFullConfidence) {
  // An observation stored before confidences existed must stay usable, and
  // must not be weighted below one that measured itself.
  const auto payload = make_test_payload();
  const auto decoded =
      teletext_hex_to_observed_packet(teletext_packet_to_hex(payload));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->bytes, payload);
  EXPECT_FALSE(decoded->has_confidence);
  for (const float value : decoded->confidence) {
    EXPECT_EQ(value, 1.0F);
  }
}

TEST(TeletextHex, RejectsPartialAndMalformedConfidenceSuffixes) {
  const std::string full =
      teletext_packet_to_hex(make_test_payload(), TeletextPacketConfidence{});
  EXPECT_FALSE(teletext_hex_to_observed_packet(full.substr(0, full.size() - 1))
                   .has_value());
  EXPECT_FALSE(teletext_hex_to_observed_packet(full.substr(0, full.size() - 20))
                   .has_value());
  std::string bad = full;
  bad.back() = 'z';
  EXPECT_FALSE(teletext_hex_to_observed_packet(bad).has_value());
}

TEST(TeletextSlicerConfidence, RecoveredPacketSurvivesTheObservationRoundTrip) {
  // What the observer stores and what the sink and the previewer read back are
  // the same measurement, to a quantisation step.
  const auto payload = make_parity_coded_payload();
  const auto line = synthesize_teletext_line(payload, tape_like_options());
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto result = slice_line(line, options);
  ASSERT_TRUE(result.valid);
  ASSERT_TRUE(result.has_byte_confidence);

  const auto decoded = teletext_hex_to_observed_packet(
      teletext_packet_to_hex(result.bytes, result.byte_confidence));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->bytes, result.bytes);
  ASSERT_TRUE(decoded->has_confidence);
  const float step = 1.0F / static_cast<float>(kTeletextConfidenceLevels - 1);
  for (size_t i = 0; i < kTeletextPacketBytes; ++i) {
    EXPECT_NEAR(decoded->confidence[i], result.byte_confidence[i], step)
        << "byte " << i;
  }
}

TEST(TeletextRejectReasonName, EveryReasonHasItsOwnName) {
  std::set<std::string_view> names;
  for (size_t index = 0; index < kTeletextRejectReasonCount; ++index) {
    const auto name =
        teletext_reject_reason_name(static_cast<TeletextRejectReason>(index));
    EXPECT_FALSE(name.empty()) << "index=" << index;
    EXPECT_NE(name, "unknown") << "index=" << index;
    names.insert(name);
  }
  EXPECT_EQ(names.size(), kTeletextRejectReasonCount);
}

// ---------------------------------------------------------------------------
// 525-line WST (ITU-R BT.653 Table 1b)
// ---------------------------------------------------------------------------

TEST(Teletext525Slicer, CleanLineRecoversThe34ByteExactly) {
  const auto payload = make_525_test_payload();
  const auto result = slice_ntsc_line(synthesize_ntsc_wst_line(payload));
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.packet_bytes, kTeletext525PacketBytes);
  EXPECT_EQ(result.bytes, payload);
  EXPECT_EQ(result.framing_bit_errors, 0);
}

TEST(Teletext525Slicer, PhaseOffsetSweep_RecoversBytesExactly) {
  // Sub-sample timing sweep covering more than one full bit period, which is
  // exactly 2,5 samples at 4FSC NTSC (BT.653 Table 1b: 364 x fH).
  const auto payload = make_525_test_payload();
  for (int step = 0; step <= 26; ++step) {
    const double offset = step * 0.1;
    TeletextLineSynthOptions opt = ntsc_wst_synth_options();
    opt.phase_offset_samples = offset;
    const auto result = slice_ntsc_line(synthesize_ntsc_wst_line(payload, opt));
    ASSERT_TRUE(result.valid) << "offset=" << offset;
    EXPECT_EQ(result.bytes, payload) << "offset=" << offset;
  }
}

TEST(Teletext525Slicer, TimingSweepCoversTheBt653Tolerance) {
  // BT.653 Table 1b puts the timing reference at 11,7 us +/- 0,175, so the
  // first run-in bit centre moves over 9,44 to 9,78 us. The measured 525-line
  // captures sit a little earlier still (9,3 to 9,4 us leading edge), so the
  // sweep runs wider than the tolerance in both directions.
  const auto payload = make_525_test_payload();
  for (int step = 0; step <= 12; ++step) {
    const double centre_us = 9.2 + step * 0.05;
    TeletextLineSynthOptions opt = ntsc_wst_synth_options();
    opt.first_bit_centre_us = centre_us;
    const auto result = slice_ntsc_line(synthesize_ntsc_wst_line(payload, opt));
    ASSERT_TRUE(result.valid) << "centre=" << centre_us;
    EXPECT_EQ(result.bytes, payload) << "centre=" << centre_us;
  }
}

TEST(Teletext525Slicer, AmplitudeSweep_RecoversAcrossSpecRange) {
  // BT.653 Table 1b: logical '1' at 70 +/- 6 % of the black-to-white
  // excursion; swept well beyond the tolerance in both directions.
  const auto payload = make_525_test_payload();
  for (int step = 0; step <= 9; ++step) {
    const double fraction = 0.50 + step * 0.05;
    TeletextLineSynthOptions opt = ntsc_wst_synth_options();
    opt.amplitude_fraction = fraction;
    const auto result = slice_ntsc_line(synthesize_ntsc_wst_line(payload, opt));
    ASSERT_TRUE(result.valid) << "fraction=" << fraction;
    EXPECT_EQ(result.bytes, payload) << "fraction=" << fraction;
  }
}

TEST(Teletext525Slicer, BandLimitedLineIsRecoveredByMlse) {
  // The tape case, on the 525-line service: a channel rolling off below the
  // clock run-in fundamental, which the threshold detector cannot lock.
  const auto payload = make_525_test_payload();
  TeletextLineSynthOptions opt = ntsc_wst_synth_options();
  opt.low_pass_cutoff_hz = 2.6e6;

  TeletextSlicerOptions threshold;
  threshold.detector = TeletextDetector::kThreshold;
  EXPECT_FALSE(
      slice_ntsc_line(synthesize_ntsc_wst_line(payload, opt), threshold).valid);

  TeletextSlicerOptions mlse;
  mlse.detector = TeletextDetector::kMlse;
  const auto result =
      slice_ntsc_line(synthesize_ntsc_wst_line(payload, opt), mlse);
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.packet_bytes, kTeletext525PacketBytes);
  EXPECT_EQ(result.bytes, payload);
  ASSERT_TRUE(result.has_byte_confidence);
  // Confidence is reported for the bytes that were transmitted and no further.
  for (size_t i = 0; i < kTeletext525PacketBytes; ++i) {
    EXPECT_GT(result.byte_confidence[i], 0.0F) << "byte " << i;
  }
}

// A row of VirText pages as recordings carry it on 525 lines:
// the frame byte 0x55, thirty spaces and 0x55 again. A run of spaces is a
// pattern that repeats every eight bits, so a reading a bit early from part
// way along stays parity-clean (0x20 becomes 0x10); that is the slip.
std::array<uint8_t, kTeletextPacketBytes> make_virtext_spaces_row() {
  std::array<uint8_t, kTeletextPacketBytes> payload{};
  const auto mrag = make_mrag(/*magazine=*/1, /*packet_number=*/3);
  payload[0] = mrag[0];
  payload[1] = mrag[1];
  payload[2] = 0x55;
  for (size_t i = 3; i + 1 < kTeletext525PacketBytes; ++i) {
    payload[i] = teletext_odd_parity_encode(' ');
  }
  payload[kTeletext525PacketBytes - 1] = 0x55;
  return payload;
}

// The line sent at the clock quest_for_fire was once read at, 0,24 % slow:
// read at the standard's clock the grid is most of a bit away from the bits by
// the end of the packet, and the row does not come through intact. Read at the
// clock it was sent at, it is exact.
TEST(Teletext525Slicer, ARowSentAtAnotherClockIsReadExactlyAtThatClock) {
  constexpr double kSent = 0.9976;
  const auto payload = make_virtext_spaces_row();
  TeletextLineSynthOptions opt = ntsc_wst_synth_options();
  opt.low_pass_cutoff_hz = 2.6e6;
  opt.bit_rate = kTeletext525BitRate * kSent;
  const auto line = synthesize_ntsc_wst_line(payload, opt);

  TeletextSlicerOptions mlse;
  mlse.detector = TeletextDetector::kMlse;
  const auto nominal = slice_ntsc_line(line, mlse);
  EXPECT_FALSE(nominal.valid && nominal.bytes == payload);

  mlse.bit_clock = kSent;
  const auto matched = slice_ntsc_line(line, mlse);
  ASSERT_TRUE(matched.valid);
  EXPECT_EQ(matched.bytes, payload);
}

// A time-base step part way along the line — the second half of the burst
// arriving a quarter of a bit late, as a tape's playback might jump — is
// absorbed: the trellis reads its bits against one channel fitted to the
// whole packet, and a quarter of a bit is well inside what that channel's
// phase-by-phase taps take up. Measured on VirText recordings no packet steps
// at all (a straight line through its segments' timing explains every
// packet but 0.1 to 1.5 % of them as well as a line with a step).
TEST(Teletext525Slicer, AQuarterBitTimingStepPartWayAlongIsReadThrough) {
  const auto payload = make_525_test_payload();
  TeletextLineSynthOptions opt = ntsc_wst_synth_options();
  opt.low_pass_cutoff_hz = 2.6e6;
  const auto early = synthesize_ntsc_wst_line(payload, opt);
  opt.phase_offset_samples = 0.25 * kNtscSampleRate / kTeletext525BitRate;
  const auto late = synthesize_ntsc_wst_line(payload, opt);
  // The step falls in the packet's sixteenth byte: run-in and framing code
  // (24 bits) and 15 bytes of payload from the first bit's centre.
  const double first_bit = opt.first_bit_centre_us * kNtscSampleRate / 1e6;
  const auto step = static_cast<size_t>(
      first_bit + (24 + 15 * 8 + 3) * kNtscSampleRate / kTeletext525BitRate);
  std::vector<int16_t> line(early.begin(),
                            early.begin() + static_cast<ptrdiff_t>(step));
  line.insert(line.end(), late.begin() + static_cast<ptrdiff_t>(step),
              late.end());

  TeletextSlicerOptions mlse;
  mlse.detector = TeletextDetector::kMlse;
  const auto result = slice_ntsc_line(line, mlse);
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
}

TEST(Teletext525Slicer, A625LineBurstAtItsOwnBitRateIsNotRecovered) {
  // The framing code and clock run-in are shared (BT.653 Tables 1a and 1b row
  // 2.1), so it is the bit rate and packet length that keep the two services
  // apart.
  TeletextLineSynthOptions opt = ntsc_wst_synth_options();
  opt.bit_rate = kTeletextBitRate;
  const auto line = synthesize_teletext_line(make_parity_coded_payload(), opt);
  EXPECT_FALSE(slice_ntsc_line(line).valid);
}

TEST(Teletext525Hex, RoundTripsAtItsOwnLength) {
  const auto payload = make_525_test_payload();
  const std::string hex =
      teletext_packet_to_hex(payload, kTeletext525PacketBytes);
  EXPECT_EQ(hex.size(), kTeletext525PacketBytes * 2);

  const auto observed = teletext_hex_to_observed_packet(hex);
  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(observed->byte_count, kTeletext525PacketBytes);
  EXPECT_FALSE(observed->has_confidence);
  EXPECT_EQ(observed->bytes, payload);

  // The fixed-length helper is 625-line only, so a 34-byte packet can never
  // reach a caller that has nowhere to record its length.
  EXPECT_FALSE(teletext_hex_to_packet(hex).has_value());
}

TEST(Teletext525Hex, ConfidenceSuffixRoundTripsAtItsOwnLength) {
  const auto payload = make_525_test_payload();
  TeletextPacketConfidence confidence{};
  for (size_t i = 0; i < kTeletext525PacketBytes; ++i) {
    confidence[i] = static_cast<float>(i % kTeletextConfidenceLevels) /
                    static_cast<float>(kTeletextConfidenceLevels - 1);
  }

  const std::string hex =
      teletext_packet_to_hex(payload, confidence, kTeletext525PacketBytes);
  ASSERT_EQ(hex.size(), kTeletext525PacketBytes * 3);

  const auto observed = teletext_hex_to_observed_packet(hex);
  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(observed->byte_count, kTeletext525PacketBytes);
  ASSERT_TRUE(observed->has_confidence);
  EXPECT_EQ(observed->bytes, payload);
  for (size_t i = 0; i < kTeletext525PacketBytes; ++i) {
    EXPECT_NEAR(observed->confidence[i], confidence[i], 1e-6) << "byte " << i;
  }
}

TEST(Teletext525Hex, TheAcceptedLengthsAreDistinct) {
  // What lets a string be decoded without being told which system produced it.
  // Three services, each with a plain and a confidence-suffixed form.
  const std::set<size_t> lengths{
      kTeletextPacketBytes * 2,    kTeletextPacketBytes * 3,
      kTeletext525PacketBytes * 2, kTeletext525PacketBytes * 3,
      kNabtsPacketBytes * 2,       kNabtsPacketBytes * 3};
  EXPECT_EQ(lengths.size(), 6u);

  // And a length that is no service's is refused rather than truncated. Two
  // bytes shorter than a 525-line packet is a 33-byte System C one and so is
  // accepted; four shorter is nobody's.
  const std::string good =
      teletext_packet_to_hex(make_525_test_payload(), kTeletext525PacketBytes);
  EXPECT_FALSE(teletext_hex_to_observed_packet(good.substr(0, good.size() - 4))
                   .has_value());
  EXPECT_FALSE(teletext_hex_to_observed_packet(good + "00").has_value());

  // The one length this change did move: 66 characters used to be nobody's and
  // is now a System C packet.
  EXPECT_TRUE(teletext_hex_to_observed_packet(good.substr(0, good.size() - 2))
                  .has_value());
}

////////////////////////////////////////////////////////////////////////////////////////////
// Pinned acquisition (TeletextPhaseHint)
////////////////////////////////////////////////////////////////////////////////////////////

// The lock position is what a caller accumulates to build a hint, so it has to
// be reported and it has to be where the burst actually is. The synthesizer
// puts the first run-in bit centre at first_bit_centre_us.
TEST(TeletextPhaseHint, TheReportedLockIsWhereTheBurstStarts) {
  const auto result = slice_line(synthesize_teletext_line(make_test_payload()));
  ASSERT_TRUE(result.valid);

  TeletextLineSynthOptions opt;
  const double expected_samples =
      opt.first_bit_centre_us * kPalSampleRate / 1e6;
  // Within the quarter-sample step the correlation sweep advances by.
  EXPECT_NEAR(result.lock_sample, expected_samples, 0.3);
}

// A hint centred on the real lock recovers exactly what the full sweep does:
// pinning is a saving, not a different decode.
TEST(TeletextPhaseHint, APinnedSweepRecoversWhatTheFullSweepDoes) {
  const auto payload = make_test_payload();
  const auto line = synthesize_teletext_line(payload);
  const TeletextLineResult unpinned = slice_line(line);
  ASSERT_TRUE(unpinned.valid);

  TeletextPhaseHint hint;
  hint.valid = true;
  hint.centre = unpinned.lock_sample;
  hint.radius = 3.0;

  const TeletextLineResult pinned = slice_line(line, {}, hint);
  ASSERT_TRUE(pinned.valid);
  EXPECT_EQ(pinned.bytes, payload);
  EXPECT_DOUBLE_EQ(pinned.lock_sample, unpinned.lock_sample);
}

// The contract that makes pinning safe to leave on: a hint pointing at the
// wrong end of the line costs the pinned attempt and then finds the packet
// anyway over the full window.
TEST(TeletextPhaseHint, AWrongHintFallsBackToTheFullSweep) {
  const auto payload = make_test_payload();
  const auto line = synthesize_teletext_line(payload);

  TeletextPhaseHint hint;
  hint.valid = true;
  hint.centre = 210.0;  // Late in the §6.3 window, past the real burst
  hint.radius = 3.0;

  const TeletextLineResult result = slice_line(line, {}, hint);
  ASSERT_TRUE(result.valid)
      << "a wrong hint lost a packet the full sweep finds";
  EXPECT_EQ(result.bytes, payload);
}

// A hint that lands entirely outside the timing window the system permits is
// not a narrowing of anything, so it is dropped rather than searched.
TEST(TeletextPhaseHint, AHintOutsideTheTimingWindowIsIgnored) {
  const auto payload = make_test_payload();
  const auto line = synthesize_teletext_line(payload);

  TeletextPhaseHint hint;
  hint.valid = true;
  hint.centre = 5000.0;
  hint.radius = 1.0;

  const TeletextLineResult result = slice_line(line, {}, hint);
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
}

// The MLSE detector acquires its own bit phase and so has its own sweep to
// pin; the band-limited line is the one it exists for.
TEST(TeletextPhaseHint, PinsTheMlseAcquisitionToo) {
  const auto payload = make_parity_coded_payload();

  TeletextSlicerOptions slicer_options;
  slicer_options.detector = TeletextDetector::kMlse;

  const auto line = synthesize_teletext_line(payload, tape_like_options());
  const TeletextLineResult unpinned = slice_line(line, slicer_options);
  ASSERT_TRUE(unpinned.valid);
  ASSERT_GE(unpinned.lock_sample, 0.0);

  TeletextPhaseHint hint;
  hint.valid = true;
  hint.centre = unpinned.lock_sample;
  hint.radius = 3.0;

  const TeletextLineResult pinned = slice_line(line, slicer_options, hint);
  ASSERT_TRUE(pinned.valid);
  EXPECT_EQ(pinned.bytes, unpinned.bytes);
}

// ---------------------------------------------------------------------------
// NABTS (ITU-R BT.653 System C, CEA-516)
// ---------------------------------------------------------------------------

// Read as the NABTS sink reads, with the wider readings always on
// (TeletextSlicerOptions::extended_search).
TeletextLineResult slice_nabts_line(const std::vector<int16_t>& line,
                                    TeletextSlicerOptions options = {},
                                    const TeletextPhaseHint& hint = {}) {
  options.extended_search = true;
  const TeletextSlicer slicer(kNtscSampleRate, TeletextSystem::kNabts525,
                              options);
  return slicer.slice(line.data(), line.size(),
                      static_cast<int16_t>(kNtscBlack),
                      static_cast<int16_t>(kNtscWhite), hint);
}

// A band-limited NABTS line: the same 3 MHz roll-off the WST tape fixture
// uses, which is below the 3,47 MHz clock run-in fundamental and so is what
// the MLSE detector exists for.
TeletextLineSynthOptions nabts_tape_like_options() {
  TeletextLineSynthOptions opt = nabts_synth_options();
  opt.low_pass_cutoff_hz = 3.0e6;
  return opt;
}

// The 525-line System B equivalent, so the two services are compared under
// identical channel conditions.
TeletextLineSynthOptions ntsc_tape_like_options() {
  TeletextLineSynthOptions opt = ntsc_wst_synth_options();
  opt.low_pass_cutoff_hz = 3.0e6;
  return opt;
}

TEST(TeletextSlicerNabts, RecoversThe33BytePacketFromACleanLine) {
  const auto payload = make_nabts_test_payload();
  const auto result = slice_nabts_line(synthesize_nabts_line(payload));

  ASSERT_TRUE(result.valid)
      << "reject reason: " << teletext_reject_reason_name(result.reject_reason);
  EXPECT_EQ(result.packet_bytes, kNabtsPacketBytes);
  EXPECT_EQ(result.system, TeletextSystem::kNabts525);
  EXPECT_EQ(result.framing_bit_errors, 0);
  for (size_t i = 0; i < kNabtsPacketBytes; ++i) {
    EXPECT_EQ(result.bytes[i], payload[i]) << "byte " << i;
  }
  // The eight bytes past the packet were never transmitted.
  for (size_t i = kNabtsPacketBytes; i < kTeletextPacketBytes; ++i) {
    EXPECT_EQ(result.bytes[i], 0) << "byte " << i;
  }
}

TEST(TeletextSlicerNabts, MlseRecoversABandLimitedLine) {
  const auto payload = make_nabts_test_payload();
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;

  const auto result = slice_nabts_line(
      synthesize_nabts_line(payload, nabts_tape_like_options()), options);

  ASSERT_TRUE(result.valid)
      << "reject reason: " << teletext_reject_reason_name(result.reject_reason);
  EXPECT_EQ(result.detector, TeletextDetector::kMlse);
  for (size_t i = 0; i < kNabtsPacketBytes; ++i) {
    EXPECT_EQ(result.bytes[i], payload[i]) << "byte " << i;
  }
}

// CEA-516 §3.3 makes byte parity conditional on the data group type, which one
// packet cannot establish, so parity-guided repair never runs on System C.
TEST(TeletextSlicerNabts, ParityRepairIsNeverAppliedToSystemC) {
  auto payload = make_nabts_test_payload();
  // Break the parity of a data-block byte the way a channel error would.
  payload[10] = static_cast<uint8_t>(payload[10] ^ 0x01);

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  options.parity_repair = true;

  const auto result = slice_nabts_line(
      synthesize_nabts_line(payload, nabts_tape_like_options()), options);

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.repaired_bytes, 0);
  EXPECT_EQ(result.bytes[10], payload[10]);
}

// The threshold detector's prefix gate covers all five Hamming-coded bytes of
// the packet prefix (CEA-516 §3.2.1), not just the leading two.
TEST(TeletextSlicerNabts, RejectsAPacketWhoseContinuityIndexIsUnrecoverable) {
  auto payload = make_nabts_test_payload();
  // Two bit errors in CI: Hamming 8/4 detects but cannot correct them.
  payload[3] = static_cast<uint8_t>(payload[3] ^ 0x03);
  const auto line = synthesize_nabts_line(payload);

  const auto rejected = slice_nabts_line(line);
  EXPECT_FALSE(rejected.valid);
  EXPECT_EQ(rejected.reject_reason, TeletextRejectReason::kInvalidMrag);
}

// CEA-516 §1.3 allows the first transition to sit anywhere in 10,48 ± 0,34 µs,
// and the ExtraVision captures measure half a bit period below nominal.
TEST(TeletextSlicerNabts, ToleratesTheSpecifiedTimingSpread) {
  const auto payload = make_nabts_test_payload();
  for (const double centre : {10.23, 10.39, 10.57, 10.91}) {
    TeletextLineSynthOptions opt = nabts_synth_options();
    opt.first_bit_centre_us = centre;
    const auto result = slice_nabts_line(synthesize_nabts_line(payload, opt));
    EXPECT_TRUE(result.valid) << "first bit centre " << centre << " us";
  }
}

TEST(TeletextSlicerNabts, RejectsNoise) {
  // The threshold detector matches the framing code exactly (CEA-516 §2.2.3)
  // and so is absolute about noise. The MLSE detector fits the preamble
  // instead, and a full-amplitude noise line occasionally fits it: none of the
  // 200 here, and four in 2000 synthesized the same way, against 24 when the
  // detector read one shift of its best fit and read nothing back — every
  // reading now reads its synchronization sequence back. The bound is one
  // above the observed count.
  constexpr uint32_t kSeeds = 200;
  constexpr int kMaxMlseFalseLocks = 1;
  TeletextLineSynthOptions opt = nabts_synth_options();
  opt.noise_amplitude = 200;
  TeletextSlicerOptions mlse;
  mlse.detector = TeletextDetector::kMlse;
  int mlse_false_locks = 0;
  // Reuse the synthesizer's noise generator by asking it for a line with no
  // packet in it: an all-zero transmission still leaves the noise on top.
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    opt.noise_seed = seed;
    const auto noisy = synthesize_teletext_line_bytes({}, opt);
    EXPECT_FALSE(slice_nabts_line(noisy).valid) << "seed " << seed;
    mlse_false_locks += slice_nabts_line(noisy, mlse).valid ? 1 : 0;
  }
  EXPECT_LE(mlse_false_locks, kMaxMlseFalseLocks);
}

TEST(TeletextSlicerNabts, MlseLocksOnTheFirstRunInBit) {
  // The lock the phase tracker pins the recording's timing to has to be the
  // first run-in bit. A channel fit left to choose its own phase settles up to
  // two bits early, because the free taps absorb a shift of the period-2
  // run-in; settling the lock by where the fitted response sits in the taps
  // does not.
  const auto payload = make_nabts_test_payload();
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const double half_bit = 0.5 * kNtscSampleRate / kTeletext525BitRate;
  for (const double phase : {0.0, 0.7, 1.3, 2.1}) {
    auto synth = nabts_tape_like_options();
    synth.phase_offset_samples = phase;
    const auto result =
        slice_nabts_line(synthesize_nabts_line(payload, synth), options);
    ASSERT_TRUE(result.valid) << "phase " << phase;
    const double expected =
        synth.first_bit_centre_us * synth.sample_rate / 1e6 + phase;
    EXPECT_NEAR(result.lock_sample, expected, half_bit) << "phase " << phase;
  }
}

TEST(TeletextSlicerNabts, MlseRecoversNearlyEveryLightlyNoisyLine) {
  // Light noise on a 5 MHz channel is well inside what the detector handles;
  // what used to lose one line in seven here was the two-bit-early lock, which
  // left the channel model unable to describe the pre-cursor interference and
  // the residual gates then refused the line.
  const auto payload = make_nabts_test_payload();
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  constexpr uint32_t kSeeds = 40;
  int recovered = 0;
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    auto synth = nabts_synth_options();
    synth.low_pass_cutoff_hz = 5.0e6;
    synth.noise_amplitude = 20;
    synth.noise_seed = seed;
    synth.phase_offset_samples = 0.25 * static_cast<double>(seed % 10);
    const auto result =
        slice_nabts_line(synthesize_nabts_line(payload, synth), options);
    if (result.valid && result.bytes == payload) {
      ++recovered;
    }
  }
  EXPECT_GE(recovered, 37);
}

TEST(TeletextSlicerNabts, MlseReadsAChannelThatRingsPastItsTapWindow) {
  // Five taps cannot describe an undershoot four bits after a bit's peak, and
  // what they leave of it lands where runs of ones end — first of all on the
  // first prefix byte, after the framing code's three ones, which is the
  // channel 0x000 read as 0x200 or 0x100. The refit's
  // decision-feedback tail, fitted to the packet and repeated while it changes
  // the decisions, is what reads these lines: without it 6 of these 64 lines
  // came back exact and 54 with the first byte right, with it all 64.
  const auto payload =
      make_nabts_test_payload(/*channel=*/0x000, /*continuity=*/5, /*ps=*/0);
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  constexpr uint32_t kSeeds = 64;
  int exact = 0;
  int first_byte = 0;
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    const auto line = synthesize_ringing_line(
        make_nabts_transmission_bytes(payload), nabts_synth_options(),
        /*noise=*/60, seed * 7919u + 13u,
        0.25 * static_cast<double>(seed % 10));
    const auto result = slice_nabts_line(line, options);
    if (!result.valid) {
      continue;
    }
    exact += result.bytes == payload ? 1 : 0;
    first_byte += result.bytes[0] == payload[0] ? 1 : 0;
  }
  EXPECT_GE(exact, 60);
  EXPECT_GE(first_byte, 62);
}

TEST(TeletextSlicerNabts, MlseReadsAChannelWithoutATailAsBefore) {
  // A channel whose response is spent within the five-tap window gives the
  // refits that model the bits before it nothing to find: the tail fits to
  // noise, and a refit that reads no better than the five-tap one is not kept.
  // Lines through the 3 MHz tape fixture read at least as well as the five
  // taps alone read them — 92 and 86 of these 96 exactly at the two noise
  // levels below.
  const auto payload = make_nabts_test_payload();
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const std::array<std::pair<int32_t, int>, 2> levels = {{{80, 92}, {100, 86}}};
  for (const auto& [noise, five_tap_exact] : levels) {
    int exact = 0;
    for (uint32_t seed = 1; seed <= 96; ++seed) {
      auto synth = nabts_tape_like_options();
      synth.noise_amplitude = noise;
      synth.noise_seed = seed * 7919u + 13u;
      synth.phase_offset_samples = 0.25 * static_cast<double>(seed % 10);
      const auto result =
          slice_nabts_line(synthesize_nabts_line(payload, synth), options);
      exact += result.valid && result.bytes == payload ? 1 : 0;
    }
    EXPECT_GE(exact, five_tap_exact) << "noise=" << noise;
  }
}

TEST(TeletextSlicerNabts, AutoFallsBackToMlseWhenTheThresholdEyeIsClosed) {
  // A 3 MHz channel under noise still lets the threshold detector lock, but
  // its bit-centre samples sit close enough to the slicing level that many of
  // its packets are wrong. Automatic mode reads the eye margin and hands
  // those lines to the MLSE detector rather than emitting the guess.
  const auto payload = make_nabts_test_payload();
  TeletextSlicerOptions threshold;
  threshold.detector = TeletextDetector::kThreshold;
  TeletextSlicerOptions automatic;
  automatic.detector = TeletextDetector::kAuto;
  constexpr uint32_t kSeeds = 40;
  int threshold_exact = 0;
  int automatic_exact = 0;
  int fell_back = 0;
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    auto synth = nabts_synth_options();
    synth.low_pass_cutoff_hz = 3.0e6;
    synth.noise_amplitude = 60;
    synth.noise_seed = seed;
    synth.phase_offset_samples = 0.25 * static_cast<double>(seed % 10);
    const auto line = synthesize_nabts_line(payload, synth);
    const auto direct = slice_nabts_line(line, threshold);
    const auto chosen = slice_nabts_line(line, automatic);
    threshold_exact += (direct.valid && direct.bytes == payload) ? 1 : 0;
    automatic_exact += (chosen.valid && chosen.bytes == payload) ? 1 : 0;
    fell_back +=
        (chosen.valid && chosen.detector == TeletextDetector::kMlse) ? 1 : 0;
  }
  EXPECT_LE(threshold_exact, 28);
  EXPECT_GE(automatic_exact, 34);
  EXPECT_GT(fell_back, 0);
}

TEST(TeletextSlicerNabts, MlseRejectsRandomBurstsAtTheBitRate) {
  // A burst of random bits at the data rate is the hardest thing to tell from
  // a packet: the channel model fits it exactly, and the five-byte prefix gate
  // alone passes one such burst in eighteen, since it accepts corrected bytes.
  // Reading five shifts of the best fit gives such a burst several chances at
  // the gate, which is why a shift without a claim of its own is kept only on
  // a prefix that arrived as sent, and why every reading must read its
  // synchronization sequence back: the framing code within a bit (CEA-516
  // §2.2.3) and, where the prefix needed correction, the clock run-in still
  // alternating (§2.2.2). None in the hundred here and four in a thousand
  // synthesized the same way, against three and 37 with the framing code alone
  // and eight and 48 with a single reading and nothing read back. The bound is
  // one above the observed count.
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  constexpr uint32_t kSeeds = 100;
  int accepted = 0;
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    uint32_t state = seed * 2654435761u;
    std::vector<uint8_t> bytes(kNabtsTransmissionBytes);
    for (auto& byte : bytes) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      byte = static_cast<uint8_t>(state & 0xFF);
    }
    auto synth = nabts_synth_options();
    synth.low_pass_cutoff_hz = 4.0e6;
    synth.noise_amplitude = 30;
    synth.noise_seed = seed;
    synth.phase_offset_samples = 0.25 * static_cast<double>(seed % 10);
    const auto line = synthesize_teletext_line_bytes(bytes, synth);
    accepted += slice_nabts_line(line, options).valid ? 1 : 0;
  }
  EXPECT_LE(accepted, 1);
}

// Flips the lowest bit of the first packet-address byte, which Hamming 8/4
// corrects (ETSI EN 300 706 §8.2): the packet still decodes to the same
// address, but its prefix no longer arrives as sent.
void damage_nabts_prefix(std::vector<uint8_t>& transmission) {
  constexpr size_t kFirstPrefixByte = 3;  // after the run-in and framing code
  transmission[kFirstPrefixByte] =
      static_cast<uint8_t>(transmission[kFirstPrefixByte] ^ 0x01);
}

TEST(TeletextSlicerNabts, MlseAcceptsAFramingCodeWithOneBitWrong) {
  // CEA-516 §2.2.3 chose the framing code so that byte synchronization is
  // established even when one of its bits is wrongly received. The MLSE
  // detector reads the code back at the alignment it has fitted when the
  // packet's prefix needed correction, and reports and accepts that one
  // error. The last four bits are the ones a wrong bit leaves the preamble fit
  // undisturbed by; an error among the first four, next to the run-in, fails
  // the preamble residual gate on most lines.
  const auto payload = make_nabts_test_payload();
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  constexpr uint32_t kSeeds = 40;
  for (const int bit : {4, 7}) {
    int recovered = 0;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
      auto bytes = make_nabts_transmission_bytes(payload);
      bytes[2] = static_cast<uint8_t>(bytes[2] ^ (1u << bit));
      damage_nabts_prefix(bytes);
      auto synth = nabts_tape_like_options();
      synth.noise_amplitude = 20;
      synth.noise_seed = seed;
      synth.phase_offset_samples = 0.25 * static_cast<double>(seed % 10);
      const auto result = slice_nabts_line(
          synthesize_teletext_line_bytes(bytes, synth), options);
      auto expected = payload;
      expected[0] = static_cast<uint8_t>(expected[0] ^ 0x01);
      if (result.valid && result.bytes == expected) {
        ++recovered;
        EXPECT_EQ(result.framing_bit_errors, 1) << "bit " << bit;
      }
    }
    EXPECT_GE(recovered, 38) << "bit " << bit;
  }
}

TEST(TeletextSlicerNabts, MlseRejectsAFramingCodeWithTwoBitsWrong) {
  // Two wrong bits are more than §2.2.3 provides for. A channel fitted with
  // the framing code assumed absorbs the difference, so a packet whose prefix
  // needed correction has the code read back with a channel fitted to the
  // payload alone; the lines are rejected rather than decoded as a packet
  // whose byte synchronization cannot be trusted.
  const auto payload = make_nabts_test_payload();
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  constexpr uint32_t kSeeds = 40;
  for (const auto& bits : {std::array<int, 2>{1, 4}, std::array<int, 2>{2, 5},
                           std::array<int, 2>{3, 6}}) {
    int accepted = 0;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
      auto bytes = make_nabts_transmission_bytes(payload);
      for (const int bit : bits) {
        bytes[2] = static_cast<uint8_t>(bytes[2] ^ (1u << bit));
      }
      damage_nabts_prefix(bytes);
      auto synth = nabts_tape_like_options();
      synth.noise_amplitude = 20;
      synth.noise_seed = seed;
      synth.phase_offset_samples = 0.25 * static_cast<double>(seed % 10);
      const auto result = slice_nabts_line(
          synthesize_teletext_line_bytes(bytes, synth), options);
      accepted += result.valid ? 1 : 0;
    }
    EXPECT_EQ(accepted, 0) << "bits " << bits[0] << " and " << bits[1];
  }
}

// CEA-516 §4.2.1 puts the data group header in the data block of the
// synchronizing packet that opens a group, and §3.4 gives a packet whose suffix
// bits are 1,1 no data block at all. A prefix claiming both is not a packet:
// it is what a test signal repeating one byte, 0xB6 — the Hamming 8/4 codeword
// for 1101 — decodes as, with every prefix byte a codeword. Both detectors
// refuse it.
TEST(TeletextSlicerNabts, RejectsASynchronizingPacketWithNoDataBlock) {
  auto payload = make_nabts_test_payload();
  payload[4] = teletext_hamming84_encode(0xD);  // b2 = 1, b8,b6 = 1,1
  TeletextSlicerOptions threshold;
  threshold.detector = TeletextDetector::kThreshold;
  TeletextSlicerOptions mlse;
  mlse.detector = TeletextDetector::kMlse;

  const auto clean =
      slice_nabts_line(synthesize_nabts_line(payload), threshold);
  EXPECT_FALSE(clean.valid);
  EXPECT_EQ(clean.reject_reason, TeletextRejectReason::kInvalidMrag);

  auto synth = nabts_tape_like_options();
  synth.noise_amplitude = 20;
  synth.noise_seed = 3;
  const auto tape =
      slice_nabts_line(synthesize_teletext_line_bytes(
                           make_nabts_transmission_bytes(payload), synth),
                       mlse);
  EXPECT_FALSE(tape.valid);
  EXPECT_EQ(tape.reject_reason, TeletextRejectReason::kInvalidMrag);

  // A synchronizing packet with an ordinary suffix is untouched.
  payload[4] = teletext_hamming84_encode(0x1);
  EXPECT_TRUE(
      slice_nabts_line(synthesize_nabts_line(payload), threshold).valid);
}

TEST(TeletextSlicerNabts, MlseTrustsAPrefixThatArrivedAsSent) {
  // A prefix whose five Hamming 8/4 bytes all arrived as codewords is itself
  // what identifies a packet: a line carrying none matches that about once in
  // a million (ETSI EN 300 706 §8.2). Such a reading is not read back, which
  // is what keeps a tape whose ringing makes the framing code read back wrong
  // from losing its cleanly received packets. Two wrong framing bits that the
  // fit absorbs therefore cost nothing here, and nothing is reported against
  // the code.
  const auto payload = make_nabts_test_payload();
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  constexpr uint32_t kSeeds = 40;
  int recovered = 0;
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    auto bytes = make_nabts_transmission_bytes(payload);
    for (const int bit : {4, 7}) {
      bytes[2] = static_cast<uint8_t>(bytes[2] ^ (1u << bit));
    }
    auto synth = nabts_tape_like_options();
    synth.noise_amplitude = 20;
    synth.noise_seed = seed;
    synth.phase_offset_samples = 0.25 * static_cast<double>(seed % 10);
    const auto result =
        slice_nabts_line(synthesize_teletext_line_bytes(bytes, synth), options);
    if (result.valid && result.bytes == payload) {
      ++recovered;
      EXPECT_EQ(result.framing_bit_errors, 0);
    }
  }
  EXPECT_GE(recovered, 38);
}

// The test that holds the whole design up. System B and System C share the
// clock run-in, the bit rate, the lines and the levels on a 525-line capture:
// the framing code is the only thing that tells them apart, so each slicer
// must be blind to the other's lines.
TEST(TeletextSlicerNabts, SystemBAndSystemCLinesDoNotCrossDecode) {
  // Both a clean line and a band-limited one. The band-limited case is the
  // hard one and the reason the gate exists: the MLSE detector fits the
  // framing code rather than matching it, so on a tape — where the framing
  // bits are smeared into each other — nothing but the fit tells the two
  // services apart.
  struct Case {
    const char* name;
    TeletextLineSynthOptions wst;
    TeletextLineSynthOptions nabts;
  };
  const Case cases[] = {
      {"clean", ntsc_wst_synth_options(), nabts_synth_options()},
      {"band-limited", ntsc_tape_like_options(), nabts_tape_like_options()},
  };

  for (const Case& c : cases) {
    const auto wst_line =
        synthesize_ntsc_wst_line(make_525_test_payload(), c.wst);
    const auto nabts_line =
        synthesize_nabts_line(make_nabts_test_payload(), c.nabts);

    for (const TeletextDetector detector :
         {TeletextDetector::kThreshold, TeletextDetector::kMlse}) {
      TeletextSlicerOptions options;
      options.detector = detector;

      EXPECT_FALSE(slice_nabts_line(wst_line, options).valid)
          << c.name << ": a System B line decoded as System C (detector "
          << static_cast<int>(detector) << ")";
      EXPECT_FALSE(slice_ntsc_line(nabts_line, options).valid)
          << c.name << ": a System C line decoded as System B (detector "
          << static_cast<int>(detector) << ")";
    }
  }
}

// The observation-string encoding has to carry a 33-byte packet without being
// told which service produced it (teletext_hex_to_observed_packet).
TEST(TeletextSlicerNabts, ObservationStringsRoundTripA33BytePacket) {
  const auto payload = make_nabts_test_payload();

  const std::string plain = teletext_packet_to_hex(payload, kNabtsPacketBytes);
  EXPECT_EQ(plain.size(), kNabtsPacketBytes * 2);
  const auto decoded = teletext_hex_to_observed_packet(plain);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->byte_count, kNabtsPacketBytes);
  EXPECT_FALSE(decoded->has_confidence);
  for (size_t i = 0; i < kNabtsPacketBytes; ++i) {
    EXPECT_EQ(decoded->bytes[i], payload[i]) << "byte " << i;
  }

  TeletextPacketConfidence confidence{};
  for (size_t i = 0; i < kNabtsPacketBytes; ++i) {
    confidence[i] = static_cast<float>(i % 16) / 15.0F;
  }
  const std::string suffixed =
      teletext_packet_to_hex(payload, confidence, kNabtsPacketBytes);
  EXPECT_EQ(suffixed.size(), kNabtsPacketBytes * 3);
  const auto decoded_suffixed = teletext_hex_to_observed_packet(suffixed);
  ASSERT_TRUE(decoded_suffixed.has_value());
  EXPECT_EQ(decoded_suffixed->byte_count, kNabtsPacketBytes);
  EXPECT_TRUE(decoded_suffixed->has_confidence);
}

// Every string length the encoding accepts must name exactly one packet
// length, or a stored observation would decode as the wrong service.
TEST(TeletextSlicerNabts, EveryAcceptedObservationLengthIsUnambiguous) {
  std::set<size_t> lengths;
  for (const size_t bytes :
       {kTeletextPacketBytes, kTeletext525PacketBytes, kNabtsPacketBytes}) {
    EXPECT_TRUE(lengths.insert(bytes * 2).second)
        << "plain length collision at " << bytes << " bytes";
    EXPECT_TRUE(lengths.insert(bytes * 3).second)
        << "confidence-suffixed length collision at " << bytes << " bytes";
  }
  EXPECT_EQ(lengths.size(), 6u);
}

// ---------------------------------------------------------------------------
// System C: the second look under the packet's own checks (CEA-516 §3.3, §3.4)
// ---------------------------------------------------------------------------

// A packet whose structure byte announces the one-byte longitudinal suffix of
// CEA-516 §3.4 (PS b8,b6 = 0,1), carrying the test payload's odd-parity data
// bytes with |data| written over them, and the suffix that brings the
// exclusive-or of everything after the prefix to 0xFF. |ps| 0x0 gives the same
// packet with no suffix at all, its last byte left as data.
std::array<uint8_t, kTeletextPacketBytes> make_nabts_checked_payload(
    std::initializer_list<std::pair<size_t, uint8_t>> data = {}, int ps = 0x4) {
  auto payload = make_nabts_test_payload(0x123, 5, ps);
  for (const auto& [index, value] : data) {
    payload[index] = value;
  }
  if (ps == 0x4) {
    uint8_t check = 0xFF;
    for (size_t i = 5; i + 1 < kNabtsPacketBytes; ++i) {
      check = static_cast<uint8_t>(check ^ payload[i]);
    }
    payload[kNabtsPacketBytes - 1] = check;
  }
  return payload;
}

// A band-limited line carrying |sent|, moved a fraction |toward| of the way to
// the line that would carry |misread|: past halfway the samples sit nearer the
// misreading than the transmission, which is what a tape's noise and
// intersymbol interference do to a marginal line — here without the noise, so
// the outcome is the same every run.
std::vector<int16_t> nabts_line_toward(
    const std::array<uint8_t, kTeletextPacketBytes>& sent,
    const std::array<uint8_t, kTeletextPacketBytes>& misread, double toward) {
  const auto from = synthesize_nabts_line(sent, nabts_tape_like_options());
  const auto to = synthesize_nabts_line(misread, nabts_tape_like_options());
  std::vector<int16_t> line(from.size());
  for (size_t i = 0; i < line.size(); ++i) {
    line[i] = static_cast<int16_t>(
        std::lround((1.0 - toward) * static_cast<double>(from[i]) +
                    toward * static_cast<double>(to[i])));
  }
  return line;
}

TeletextSlicerOptions nabts_mlse_options() {
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  return options;
}

// A recorded page's byte 0x76 read as 0x75 after a
// byte that failed parity — one error event across the byte boundary, the
// parity failure saying where it is. The samples lean toward the misreading;
// the packet's checks put both bytes back, and the bytes the second look
// changed carry no confidence.
TEST(TeletextSlicerNabts, CheckedReadingMendsAnErrorAcrossAByteBoundary) {
  const auto sent = make_nabts_checked_payload({{16, 0xC1}, {17, 0x76}});
  auto misread = sent;
  misread[16] = 0x41;  // parity bit lost: fails odd parity
  misread[17] = 0x75;  // a transposition: keeps it
  const auto result = slice_nabts_line(nabts_line_toward(sent, misread, 0.55),
                                       nabts_mlse_options());

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes[16], 0xC1);
  EXPECT_EQ(result.bytes[17], 0x76);
  for (size_t i = 0; i < kNabtsPacketBytes; ++i) {
    EXPECT_EQ(result.bytes[i], sent[i]) << "byte " << i;
  }
  EXPECT_GT(result.checked_bits, 0);
  EXPECT_EQ(result.byte_confidence[16], 0.0F);
  EXPECT_EQ(result.byte_confidence[17], 0.0F);
}

// A transposition inside one byte keeps its parity, and only the longitudinal
// check sees it (0x58 read as 0x38). With no failing byte to place it, the
// second look changes a data byte only where the samples were nearly
// undecided between the two readings — as they are here.
TEST(TeletextSlicerNabts, CheckedReadingMendsATranspositionOnlyTheCheckSees) {
  const auto sent = make_nabts_checked_payload({{17, 0x58}});
  auto misread = sent;
  misread[17] = 0x38;
  const auto result = slice_nabts_line(nabts_line_toward(sent, misread, 0.51),
                                       nabts_mlse_options());

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes[17], 0x58);
  EXPECT_EQ(result.checked_bits, 2);
}

// The same line with no suffix: nothing to check the reading against, so the
// detector's own decision stands — the misreading the samples lean toward.
TEST(TeletextSlicerNabts, APacketWithoutACheckKeepsTheDecidedReading) {
  const auto sent = make_nabts_checked_payload({{17, 0x58}}, /*ps=*/0x0);
  auto misread = sent;
  misread[17] = 0x38;
  const auto result = slice_nabts_line(nabts_line_toward(sent, misread, 0.51),
                                       nabts_mlse_options());

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes[17], 0x38);
  EXPECT_EQ(result.checked_bits, 0);
}

// A structure byte is believed only as sent: three bits wrong in a Hamming
// 8/4 byte decode as another value with one corrected, so one that needed
// correcting may announce a check the packet never carried. The line above,
// its PS byte with one bit wrong, is left as the detector read it.
TEST(TeletextSlicerNabts, AStructureByteThatNeededCorrectingIsNotBelieved) {
  auto sent = make_nabts_checked_payload({{17, 0x58}});
  sent[4] = static_cast<uint8_t>(sent[4] ^ 0x01);  // PS, CEA-516 §3.2.5
  auto misread = sent;
  misread[17] = 0x38;
  const auto result = slice_nabts_line(nabts_line_toward(sent, misread, 0.51),
                                       nabts_mlse_options());

  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes[4], sent[4]);
  EXPECT_EQ(result.bytes[17], 0x38);
  EXPECT_EQ(result.checked_bits, 0);
}

// Where the samples clearly say otherwise, a failing check is left failing:
// a packet sent with its check byte one bit wrong comes back exactly as sent,
// not mended into one that passes.
TEST(TeletextSlicerNabts, CheckedReadingNeverOverridesWhatTheSamplesSay) {
  auto sent = make_nabts_checked_payload();
  sent[kNabtsPacketBytes - 1] =
      static_cast<uint8_t>(sent[kNabtsPacketBytes - 1] ^ 0x01);
  const auto result =
      slice_nabts_line(synthesize_nabts_line(sent, nabts_tape_like_options()),
                       nabts_mlse_options());

  ASSERT_TRUE(result.valid);
  for (size_t i = 0; i < kNabtsPacketBytes; ++i) {
    EXPECT_EQ(result.bytes[i], sent[i]) << "byte " << i;
  }
  EXPECT_EQ(result.checked_bits, 0);
}

// A tape's playback speed drifts within a line: here the bits arrive 0,3 %
// faster than the standard's rate, nearly a bit early by the end of the packet.
// Read at the nominal clock 19 of its bytes come out wrong, far more than the
// packet's checks can mend; read again along the bits' own timing it comes
// back whole, with nothing left for the checked reading to change.
TEST(TeletextSlicerNabts, MlseFollowsABitClockThatDriftsAcrossTheLine) {
  const auto payload = make_nabts_checked_payload();
  auto synth = nabts_tape_like_options();
  synth.bit_rate = kTeletext525BitRate * 1.003;
  const auto result = slice_nabts_line(synthesize_nabts_line(payload, synth),
                                       nabts_mlse_options());

  ASSERT_TRUE(result.valid);
  for (size_t i = 0; i < kNabtsPacketBytes; ++i) {
    EXPECT_EQ(result.bytes[i], payload[i]) << "byte " << i;
  }
  EXPECT_EQ(result.checked_bits, 0);
}

// ---------------------------------------------------------------------------
// The detector's working storage
// ---------------------------------------------------------------------------

// Lines that take the MLSE detector through each of its passes: the tape
// fixture, a channel that rings past its tap window (the tail refits), a
// packet the second look under its checks mends, and one whose bit clock
// drifts across the line (the timing look).
// A transfer whose channel responds to a bit three bit periods before the bit's
// own sample as well as for six after it, and then pulls the line down a
// little for ten more: the shape of some VHS EP recordings, fitted against the
// true bits of their packets, which no window of five bits and a tail can hold
// wherever it is placed. |tau| is in bit periods from the bit's centre.
double precursor_channel_pulse(double tau) {
  const double width = tau < 0.0 ? 0.75 : 2.2;
  const double before = tau + 2.6;
  double response = std::exp(-tau * tau / (2.0 * width * width)) -
                    0.3 * std::exp(-before * before / (2.0 * 0.5 * 0.5));
  if (tau > 5.5 && tau < 16.5) {
    response -= 0.02;
  }
  return response;
}

// The transmission |bytes| through that channel, as synthesize_ringing_line()
// puts them through its own.
std::vector<int16_t> synthesize_precursor_line(
    const std::vector<uint8_t>& bytes, const TeletextLineSynthOptions& opt,
    int32_t noise, uint32_t seed, double phase_offset_samples) {
  constexpr int kReach = 20;
  const double spb = opt.sample_rate / opt.bit_rate;
  const double t0 =
      opt.first_bit_centre_us * opt.sample_rate / 1e6 + phase_offset_samples;
  const double one_step =
      opt.amplitude_fraction *
      static_cast<double>(opt.white_level - opt.black_level);
  double gain = 0.0;
  for (int j = -kReach; j <= kReach; ++j) {
    gain += precursor_channel_pulse(static_cast<double>(j));
  }
  const int bits = static_cast<int>(bytes.size()) * 8;
  std::vector<int16_t> line(opt.sample_count, opt.black_level);
  uint32_t state = seed != 0 ? seed : 1;
  for (size_t n = 0; n < line.size(); ++n) {
    const double t = (static_cast<double>(n) - t0) / spb;
    const int first = std::max(0, static_cast<int>(std::floor(t)) - kReach);
    const int last =
        std::min(bits - 1, static_cast<int>(std::ceil(t)) + kReach);
    double response = 0.0;
    for (int j = first; j <= last; ++j) {
      if (((bytes[static_cast<size_t>(j) >> 3] >> (j & 7)) & 1) != 0) {
        response += precursor_channel_pulse(t - static_cast<double>(j));
      }
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const auto drawn =
        static_cast<int32_t>(state % static_cast<uint32_t>(2 * noise + 1)) -
        noise;
    line[n] = static_cast<int16_t>(
        std::lround(static_cast<double>(opt.black_level) +
                    one_step * response / gain + static_cast<double>(drawn)));
  }
  return line;
}

// Packets sent without a check, as some services sent theirs, through the
// precursor channel above, at 64 noise seeds and bit phases.
std::vector<std::vector<int16_t>> precursor_lines(
    const std::array<uint8_t, kTeletextPacketBytes>& payload) {
  std::vector<std::vector<int16_t>> lines;
  constexpr uint32_t kSeeds = 64;
  for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
    lines.push_back(synthesize_precursor_line(
        make_nabts_transmission_bytes(payload), nabts_synth_options(),
        /*noise=*/40, seed * 7919u + 13u,
        0.2 * static_cast<double>(seed % 13)));
  }
  return lines;
}

// The tail refits' window reaches two bits past the sampled one, or three where
// the reading's alignment slides it a bit earlier — and then loses the last
// bit of the tail behind it. These packets need both ends at once. Of the 60
// the detector delivers, the readings before the wide one leave 216 bytes
// wrong and read 8 exactly; the wide reading, taken where a byte still fails
// its parity, leaves 100 and reads 24.
TEST(TeletextSlicerNabts,
     TheWideReadingReadsAChannelThatRespondsAheadOfTheBit) {
  const auto payload =
      make_nabts_test_payload(/*channel=*/0x000, /*continuity=*/5, /*ps=*/0);
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  int valid = 0;
  int exact = 0;
  int wrong_bytes = 0;
  int widened = 0;
  for (const auto& line : precursor_lines(payload)) {
    const auto result = slice_nabts_line(line, options);
    if (!result.valid) {
      continue;
    }
    ++valid;
    int wrong = 0;
    for (size_t i = 0; i < kNabtsPacketBytes; ++i) {
      wrong += result.bytes[i] != payload[i] ? 1 : 0;
    }
    wrong_bytes += wrong;
    exact += wrong == 0 ? 1 : 0;
    widened += result.wide_bytes != 0 ? 1 : 0;
  }
  EXPECT_GE(valid, 58);
  EXPECT_LE(wrong_bytes, 120);
  EXPECT_GE(exact, 20);
  EXPECT_GT(widened, 0);
}

// A packet whose reading fails a check the samples bear out — a byte sent
// failing its parity — is read the same through the wide channel, and the
// reading is left as it was, with nothing reported changed.
TEST(TeletextSlicerNabts, TheWideReadingLeavesAReadingTheSamplesBearOut) {
  auto payload =
      make_nabts_test_payload(/*channel=*/0x000, /*continuity=*/5, /*ps=*/0);
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const auto clean = slice_nabts_line(synthesize_nabts_line(payload), options);
  ASSERT_TRUE(clean.valid);
  EXPECT_EQ(clean.wide_bytes, 0u);

  payload[12] = static_cast<uint8_t>(payload[12] ^ 0x80);
  const auto failing =
      slice_nabts_line(synthesize_nabts_line(payload), options);
  ASSERT_TRUE(failing.valid);
  for (size_t i = 0; i < kNabtsPacketBytes; ++i) {
    EXPECT_EQ(failing.bytes[i], payload[i]) << "byte " << i;
  }
  EXPECT_EQ(failing.wide_bytes, 0u);
}

std::vector<std::vector<int16_t>> nabts_lines_for_every_pass() {
  std::vector<std::vector<int16_t>> lines;
  lines.push_back(synthesize_nabts_line(make_nabts_checked_payload(),
                                        nabts_tape_like_options()));
  lines.push_back(synthesize_ringing_line(
      make_nabts_transmission_bytes(make_nabts_test_payload(
          /*channel=*/0x000, /*continuity=*/5, /*ps=*/0)),
      nabts_synth_options(), /*noise=*/60, /*seed=*/101u,
      /*phase_offset=*/0.5));
  const auto sent = make_nabts_checked_payload({{16, 0xC1}, {17, 0x76}});
  auto misread = sent;
  misread[16] = 0x41;
  misread[17] = 0x75;
  lines.push_back(nabts_line_toward(sent, misread, 0.55));
  auto drifting = nabts_tape_like_options();
  drifting.bit_rate = kTeletext525BitRate * 1.003;
  lines.push_back(
      synthesize_nabts_line(make_nabts_checked_payload(), drifting));
  // The fifth precursor line is one the wide reading is kept on.
  lines.push_back(precursor_lines(make_nabts_test_payload(
      /*channel=*/0x000, /*continuity=*/5, /*ps=*/0))[4]);
  return lines;
}

void expect_same_reading(const TeletextLineResult& a,
                         const TeletextLineResult& b) {
  EXPECT_EQ(a.valid, b.valid);
  EXPECT_EQ(a.reject_reason, b.reject_reason);
  EXPECT_EQ(a.detector, b.detector);
  EXPECT_EQ(a.system, b.system);
  EXPECT_EQ(a.packet_bytes, b.packet_bytes);
  EXPECT_EQ(a.data_start_sample, b.data_start_sample);
  EXPECT_EQ(a.repaired_bytes, b.repaired_bytes);
  EXPECT_EQ(a.bytes, b.bytes);
  EXPECT_EQ(a.has_byte_confidence, b.has_byte_confidence);
  EXPECT_EQ(a.byte_confidence, b.byte_confidence);
  EXPECT_EQ(a.checked_bits, b.checked_bits);
  EXPECT_EQ(a.wide_bytes, b.wide_bytes);
  EXPECT_EQ(a.lock_sample, b.lock_sample);
  EXPECT_EQ(a.framing_bit_errors, b.framing_bit_errors);
  EXPECT_EQ(a.preamble_residual, b.preamble_residual);
  EXPECT_EQ(a.payload_residual, b.payload_residual);
  EXPECT_EQ(a.payload_bit_errors, b.payload_bit_errors);
}

// The detector keeps the storage of its trellis passes on each thread from one
// line to the next, neither cleared nor reallocated: a line reads the same
// whatever the thread read before it.
TEST(TeletextSlicerNabts, ALineReadsTheSameWhateverWasReadBeforeIt) {
  const auto lines = nabts_lines_for_every_pass();
  std::vector<TeletextLineResult> in_order;
  for (const auto& line : lines) {
    in_order.push_back(slice_nabts_line(line, nabts_mlse_options()));
    ASSERT_TRUE(in_order.back().valid);
  }
  EXPECT_GT(in_order[2].checked_bits, 0);
  EXPECT_NE(in_order.back().wide_bytes, 0u);
  for (size_t i = lines.size(); i-- > 0;) {
    SCOPED_TRACE(i);
    expect_same_reading(slice_nabts_line(lines[i], nabts_mlse_options()),
                        in_order[i]);
  }
}

// Each thread keeps storage of its own, so threads slicing at once with one
// slicer read every line as a single thread does.
TEST(TeletextSlicerNabts, ThreadsSlicingAtOnceReadWhatOneThreadReads) {
  const auto lines = nabts_lines_for_every_pass();
  const TeletextSlicer slicer(kNtscSampleRate, TeletextSystem::kNabts525,
                              nabts_mlse_options());
  const auto read = [&slicer](const std::vector<int16_t>& line) {
    return slicer.slice(line.data(), line.size(),
                        static_cast<int16_t>(kNtscBlack),
                        static_cast<int16_t>(kNtscWhite));
  };
  std::vector<TeletextLineResult> alone;
  for (const auto& line : lines) {
    alone.push_back(read(line));
  }

  constexpr size_t kThreads = 4;
  constexpr size_t kRounds = 3;
  std::vector<std::vector<TeletextLineResult>> together(kThreads);
  std::vector<std::thread> threads;
  for (size_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (size_t round = 0; round < kRounds; ++round) {
        for (size_t i = 0; i < lines.size(); ++i) {
          together[t].push_back(read(lines[(i + t) % lines.size()]));
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (size_t t = 0; t < kThreads; ++t) {
    ASSERT_EQ(together[t].size(), kRounds * lines.size());
    for (size_t n = 0; n < together[t].size(); ++n) {
      SCOPED_TRACE(t * 1000 + n);
      expect_same_reading(together[t][n],
                          alone[(n % lines.size() + t) % lines.size()]);
    }
  }
}

// The detector's AVX2 forms (TeletextSlicerOptions::use_avx2) read a line as
// its other forms do, to the bit — the same arithmetic in the same order — on
// every pass a reading can take: the lines above, the lines the wide reading
// is tried on, and System B's at both rates with their parity repaired. On a
// processor without AVX2 both readings come from the same forms.
TEST(TeletextSlicerNabts, AVX2ReadsALineAsTheOtherFormsDo) {
  TeletextSlicerOptions with = nabts_mlse_options();
  TeletextSlicerOptions without = with;
  without.use_avx2 = false;
  std::vector<std::vector<int16_t>> lines = nabts_lines_for_every_pass();
  for (auto& line : precursor_lines(make_nabts_test_payload(
           /*channel=*/0x000, /*continuity=*/5, /*ps=*/0))) {
    lines.push_back(std::move(line));
  }
  int wide = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    SCOPED_TRACE(i);
    const TeletextLineResult reading = slice_nabts_line(lines[i], with);
    expect_same_reading(reading, slice_nabts_line(lines[i], without));
    wide += reading.wide_bytes != 0 ? 1 : 0;
  }
  EXPECT_GT(wide, 1);

  TeletextSlicerOptions repairing;
  repairing.detector = TeletextDetector::kMlse;
  repairing.parity_repair = true;
  TeletextSlicerOptions repairing_without = repairing;
  repairing_without.use_avx2 = false;
  const auto pal = synthesize_teletext_line(make_parity_coded_payload(),
                                            tape_like_options());
  expect_same_reading(slice_line(pal, repairing),
                      slice_line(pal, repairing_without));
  const auto ntsc = synthesize_ntsc_wst_line(make_525_test_payload(),
                                             ntsc_tape_like_options());
  expect_same_reading(slice_ntsc_line(ntsc, repairing),
                      slice_ntsc_line(ntsc, repairing_without));
}

}  // namespace
}  // namespace tests
}  // namespace orc
