/*
 * File:        vbi_offset_calibration_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for capture-offset fitting and its health reporting
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_offset_calibration.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_line_placement.h"
#include "vbi_output_frame.h"
#include "vbi_source_format.h"
#include "vbi_synthetic_line.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

using orc::testing::render_synthetic_vbi_line;
using orc::testing::SyntheticVBILine;

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
  return format;
}

VBITeletextService wst_service() {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                   service, error))
      << error;
  return service;
}

// Position the service's nominal timing puts the run-in at for a given
// capture offset.
double run_in_position_for_offset(const VBISourceFormat& format,
                                  const VBITeletextService& service,
                                  double capture_offset_samples) {
  return service.cri_start_samples(format.sample_rate_hz,
                                   capture_offset_samples);
}

std::vector<VBICRIObservation> observations_at(
    const std::vector<double>& positions, uint64_t first_sequence = 0,
    uint64_t stride = 1) {
  std::vector<VBICRIObservation> observations;
  observations.reserve(positions.size());
  uint64_t sequence = first_sequence;
  for (const double position : positions) {
    VBICRIObservation observation;
    observation.line_sequence = sequence;
    observation.anchor_position_samples = position;
    observation.peak_correlation = 0.95;
    observations.push_back(observation);
    sequence += stride;
  }
  return observations;
}

// Positions scattered about a truth by a deterministic saw, so a test can set
// the spread it wants without depending on a random generator.
std::vector<double> scattered_positions(double truth, double half_spread,
                                        size_t count) {
  std::vector<double> positions;
  positions.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const double phase = (index % 4u) - 1.5;  // -1.5, -0.5, 0.5, 1.5
    positions.push_back(truth + half_spread * phase / 1.5);
  }
  return positions;
}

// ---------------------------------------------------------------------------
// Sampling the capture
// ---------------------------------------------------------------------------

TEST(VBIOffsetCalibration, SampledFramesAreSpreadAcrossTheCaptureNotItsHead) {
  const std::vector<uint64_t> frames =
      vbi_calibration_frame_indices(368007, 16);

  ASSERT_EQ(frames.size(), 16u);
  EXPECT_TRUE(std::is_sorted(frames.begin(), frames.end()));

  // Nothing from the opening segment, whose material is the most likely to be
  // atypical, and coverage all the way to the end.
  EXPECT_GT(frames.front(), 368007u / 64u);
  EXPECT_GT(frames.back(), 368007u - 368007u / 16u);
}

TEST(VBIOffsetCalibration, SamplingNeverAsksForMoreFramesThanExist) {
  const std::vector<uint64_t> frames = vbi_calibration_frame_indices(3, 16);
  ASSERT_FALSE(frames.empty());
  EXPECT_LE(frames.size(), 3u);
  EXPECT_LT(frames.back(), 3u);

  EXPECT_TRUE(vbi_calibration_frame_indices(0, 16).empty());
  EXPECT_TRUE(vbi_calibration_frame_indices(100, 0).empty());
}

TEST(VBIOffsetCalibration, LineSequenceCountsEveryStoredRecordOfTheCapture) {
  const VBISourceFormat format = bt8x8_pal_format();

  VBILineRecord record;
  record.frame_index = 3;
  record.field_index = 1;
  record.record_index = 4;

  // 3 frames of 32 records, then 16 records of the first field, then 4.
  EXPECT_EQ(vbi_line_sequence(format, record), 3u * 32u + 16u + 4u);
}

// ---------------------------------------------------------------------------
// The fit itself
// ---------------------------------------------------------------------------

TEST(VBIOffsetCalibration, FitsTheOffsetTheRunInPositionsImply) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_offset = 244.0;
  const double truth_position =
      run_in_position_for_offset(format, service, truth_offset);

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 0.2, 64)), 64);

  EXPECT_TRUE(calibration.converged) << calibration.summary;
  EXPECT_NEAR(calibration.capture_offset_samples, truth_offset, 0.05);
  EXPECT_NEAR(calibration.anchor_position_samples, truth_position, 0.05);
  EXPECT_EQ(calibration.records_accepted, 64u);
  EXPECT_EQ(calibration.records_examined, 64u);
  EXPECT_DOUBLE_EQ(calibration.acceptance_fraction, 1.0);
  EXPECT_EQ(calibration.spread_class, VBIOffsetSpreadClass::kTight);
  EXPECT_TRUE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.summary.find("Capture offset"), std::string::npos);
}

// A mean would follow a partial match on a damaged line; a median does not
// (design §5.3.4 step 5).
TEST(VBIOffsetCalibration, OutliersDoNotMoveTheFittedOffset) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  std::vector<double> positions = scattered_positions(truth_position, 0.2, 60);
  for (int index = 0; index < 6; ++index) {
    positions.push_back(truth_position + 40.0);
  }

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service, observations_at(positions), positions.size());

  EXPECT_NEAR(calibration.anchor_position_samples, truth_position, 0.2);
  EXPECT_NEAR(calibration.capture_offset_samples, 244.0, 0.2);
}

TEST(VBIOffsetCalibration, SpreadClassificationFollowsTheFormatsOwnThresholds) {
  VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  const VBIOffsetCalibration tight = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 0.2, 64)), 64);
  EXPECT_EQ(tight.spread_class, VBIOffsetSpreadClass::kTight);
  EXPECT_TRUE(tight.converged);
  EXPECT_TRUE(tight.warnings.empty());

  const VBIOffsetCalibration mild = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 3.0, 64)), 64);
  EXPECT_EQ(mild.spread_class, VBIOffsetSpreadClass::kMild);
  EXPECT_TRUE(mild.converged) << mild.summary;
  ASSERT_FALSE(mild.warnings.empty());
  EXPECT_NE(mild.warnings.front().find("jitter"), std::string::npos)
      << mild.warnings.front();

  const VBIOffsetCalibration unusable = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 16.0, 64)), 64);
  EXPECT_EQ(unusable.spread_class, VBIOffsetSpreadClass::kUnusable);
  EXPECT_FALSE(unusable.converged);
  ASSERT_FALSE(unusable.diagnostics.empty());
  EXPECT_NE(unusable.diagnostics.front().find("not time-base corrected"),
            std::string::npos)
      << unusable.diagnostics.front();

  // The same scatter on a format that expects it is not a failure: the
  // thresholds belong to the format, not to the stage (design §5.3.6).
  format.calibration.tight_spread_samples = 4.0;
  format.calibration.maximum_spread_samples = 24.0;
  const VBIOffsetCalibration tolerated = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 16.0, 64)), 64);
  EXPECT_EQ(tolerated.spread_class, VBIOffsetSpreadClass::kMild);
  EXPECT_TRUE(tolerated.converged) << tolerated.summary;
}

// A monotonic drift is diagnostic of a sampling-rate error specifically, and
// the slope gives the correction directly (design §5.3.4).
TEST(VBIOffsetCalibration, InjectedSampleRateErrorIsDetectedAndCorrected) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  // A configured rate 100 ppm below the truth walks the run-in later by
  // 100 ppm of a record's samples on every line.
  constexpr double kRateErrorFraction = 100e-6;
  const double slope =
      kRateErrorFraction * static_cast<double>(format.line_length);

  std::vector<VBICRIObservation> observations;
  for (uint64_t line = 0; line < 512; ++line) {
    VBICRIObservation observation;
    observation.line_sequence = line * 64u;
    observation.anchor_position_samples =
        truth_position + slope * static_cast<double>(line * 64u);
    observation.peak_correlation = 0.95;
    observations.push_back(observation);
  }

  const VBIOffsetCalibration calibration =
      fit_vbi_capture_offset(format, service, observations, 512);

  EXPECT_TRUE(calibration.drift_detected);
  EXPECT_FALSE(calibration.converged);
  ASSERT_FALSE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.diagnostics.back().find("drifts monotonically"),
            std::string::npos)
      << calibration.diagnostics.back();

  const double true_rate_hz =
      format.sample_rate_hz * (1.0 + kRateErrorFraction);
  const double error_ppm =
      1e6 * std::abs(calibration.suggested_sample_rate_hz - true_rate_hz) /
      true_rate_hz;
  EXPECT_LT(error_ppm, 1.0)
      << "suggested " << calibration.suggested_sample_rate_hz << " Hz against "
      << true_rate_hz << " Hz";
}

// Scatter is not drift: the estimator's own noise must not be reported as a
// rate error, however long the span it was fitted over.
TEST(VBIOffsetCalibration, ScatterAloneIsNotReportedAsDrift) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 0.4, 96), 0, 4096),
      96);

  EXPECT_FALSE(calibration.drift_detected);
  EXPECT_TRUE(calibration.converged) << calibration.summary;
  EXPECT_DOUBLE_EQ(calibration.suggested_sample_rate_hz, format.sample_rate_hz);
}

TEST(VBIOffsetCalibration, TooFewLocksIsReportedRatherThanFitted) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service, observations_at(scattered_positions(121.3, 0.1, 3)),
      512);

  EXPECT_FALSE(calibration.converged);
  EXPECT_EQ(calibration.records_accepted, 3u);
  ASSERT_FALSE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.diagnostics.front().find("too few"), std::string::npos)
      << calibration.diagnostics.front();
}

TEST(VBIOffsetCalibration, LowAcceptanceIsReportedWithItsFraction) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  // Sixteen locks out of 512 records examined: a tenth of what this format
  // expects.
  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 0.2, 16)), 512);

  EXPECT_FALSE(calibration.converged);
  EXPECT_NEAR(calibration.acceptance_fraction, 16.0 / 512.0, 1e-9);
  ASSERT_FALSE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.diagnostics.front().find("3.1%"), std::string::npos)
      << calibration.diagnostics.front();
}

// The offset is applied globally and never per line: every record of every
// frame is placed by exactly the same map, and a record that carried no
// teletext becomes ordinary blanking (design §5.3.4).
TEST(VBIOffsetCalibration, EveryRecordIsPlacedByTheOneGlobalOffset) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 0.2, 64)), 256);
  ASSERT_TRUE(calibration.converged) << calibration.summary;

  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(
      make_vbi_output_frame(format.tv_system, format.tt_system, output, error))
      << error;

  VBIDataPlacement placement;
  ASSERT_TRUE(make_vbi_data_placement(format, service, output,
                                      calibration.capture_offset_samples,
                                      placement, error))
      << error;

  // The fitted offset is what sample zero of every record is placed at, so the
  // run-in lands at the service's own anchor on the output lattice.
  EXPECT_NEAR(placement.source_position_at_output_zero,
              -calibration.capture_offset_samples, 1e-9);
  const double run_in_output_index =
      (calibration.anchor_position_samples -
       placement.source_position_at_output_zero) /
      placement.source_samples_per_output_sample;
  EXPECT_NEAR(run_in_output_index, placement.data_start_samples, 0.5);
}

// ---------------------------------------------------------------------------
// The whole procedure against a capture
// ---------------------------------------------------------------------------

// In-memory capture. Unit tests never touch the filesystem, and the transport
// presents identical bytes whatever the container.
class FakeByteSource : public IVBIByteSource {
 public:
  explicit FakeByteSource(std::vector<uint8_t> bytes)
      : bytes_(std::move(bytes)) {}

  std::optional<uint64_t> size_bytes() const override {
    return static_cast<uint64_t>(bytes_.size());
  }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& error_message) override {
    (void)error_message;
    if (byte_offset >= bytes_.size()) {
      return 0;
    }
    const size_t available = bytes_.size() - static_cast<size_t>(byte_offset);
    const size_t produced = std::min(count, available);
    std::memcpy(out_buffer, bytes_.data() + byte_offset, produced);
    return produced;
  }

 private:
  std::vector<uint8_t> bytes_;
};

// A capture whose teletext lines all sit at one true capture offset, with
// every fourth record carrying no data service at all.
std::vector<uint8_t> make_synthetic_capture(const VBISourceFormat& format,
                                            const VBITeletextService& service,
                                            uint64_t frame_count,
                                            double capture_offset_samples) {
  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0);

  const double position =
      run_in_position_for_offset(format, service, capture_offset_samples);

  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    for (uint32_t field = 0; field < 2u; ++field) {
      for (uint32_t index = 0; index < format.field_lines; ++index) {
        SyntheticVBILine line;
        line.sample_rate_hz = format.sample_rate_hz;
        line.valid_samples = format.valid_samples;
        line.anchor_position_samples = position;
        line.seed =
            static_cast<uint32_t>(frame * 64u + field * 32u + index + 1u);
        line.noise_amplitude = 0.5;
        line.carries_data = (index % 4u) != 3u;

        const std::vector<double> samples = render_synthetic_vbi_line(line);
        const uint64_t offset = frame * format.bytes_per_frame() +
                                field * format.bytes_per_field() +
                                index * format.bytes_per_record();
        for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
          const double value =
              std::clamp(std::round(samples[sample]), 0.0, 255.0);
          bytes[static_cast<size_t>(offset + sample)] =
              static_cast<uint8_t>(value);
        }
      }
    }
  }
  return bytes;
}

TEST(VBIOffsetCalibration, CalibratesACaptureFromItsOwnClockRunIn) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  constexpr double kTruthOffset = 231.5;

  FakeByteSource source(
      make_synthetic_capture(format, service, 6, kTruthOffset));
  VBILineReader reader(format, source);

  VBICalibrationConfig config;
  config.sample_frames = 4;

  VBIOffsetCalibration calibration;
  std::string error;
  ASSERT_TRUE(
      calibrate_vbi_capture_offset(reader, service, config, calibration, error))
      << error;

  EXPECT_TRUE(calibration.converged) << calibration.summary;
  EXPECT_NEAR(calibration.capture_offset_samples, kTruthOffset, 0.2);
  EXPECT_EQ(calibration.records_examined, 4u * 32u);

  // Three records in four carry data, and all of those should lock.
  EXPECT_EQ(calibration.records_accepted, 4u * 24u);
  EXPECT_NEAR(calibration.acceptance_fraction, 0.75, 1e-9);
  EXPECT_EQ(calibration.spread_class, VBIOffsetSpreadClass::kTight);
}

// The survey also reads the levels the data lines carry their data at, from
// each accepted record's own payload: a card capture's frames that have no
// data line of their own are mapped at them.
TEST(VBIOffsetCalibration, MeasuresTheLevelsTheDataLinesCarry) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  FakeByteSource source(make_synthetic_capture(format, service, 6, 231.5));
  VBILineReader reader(format, source);

  VBICalibrationConfig config;
  config.sample_frames = 4;

  VBIOffsetCalibration calibration;
  std::string error;
  ASSERT_TRUE(
      calibrate_vbi_capture_offset(reader, service, config, calibration, error))
      << error;
  ASSERT_TRUE(calibration.converged) << calibration.summary;

  // The synthetic data lines carry logic 0 at 40 counts and logic 1 at 200.
  ASSERT_TRUE(calibration.data_levels.usable);
  EXPECT_NEAR(calibration.data_levels.logic0, 40.0, 3.0);
  EXPECT_NEAR(calibration.data_levels.logic1, 200.0, 3.0);
}

// A 525-line bt8x8 dump carrying nothing but line-21 captions: a caption line
// of null pairs on record 11 of both fields (broadcast lines 21 and 284), its
// run-in at one true capture offset, blanking at 60 counts and 50 IRE at 140.
std::vector<uint8_t> make_captions_only_capture(const VBISourceFormat& format,
                                                uint64_t frame_count,
                                                double capture_offset_samples) {
  VBITeletextService caption;
  std::string error;
  EXPECT_TRUE(vbi_caption_service(VBITVSystem::kNTSC, caption, error)) << error;

  std::vector<bool> null_pair;
  for (uint32_t byte = 0; byte < 2u; ++byte) {
    for (uint32_t bit = 0; bit < 8u; ++bit) {
      null_pair.push_back(bit == 7u);
      null_pair.push_back(bit == 7u);
    }
  }

  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0);
  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    for (uint32_t field = 0; field < 2u; ++field) {
      for (uint32_t index = 0; index < format.field_lines; ++index) {
        SyntheticVBILine line;
        line.sample_rate_hz = format.sample_rate_hz;
        line.valid_samples = format.valid_samples;
        line.bit_rate_hz = caption.bit_rate_hz;
        line.pattern = caption.cri_frc_pattern;
        line.pattern_bits = caption.cri_bits + caption.frc_bits;
        line.run_in_bits = caption.cri_bits;
        line.payload = null_pair;
        line.anchor_position_samples = caption.cri_start_samples(
            format.sample_rate_hz, capture_offset_samples);
        line.logic0 = 60.0;
        line.logic1 = 140.0;
        line.noise_amplitude = 0.5;
        line.seed =
            static_cast<uint32_t>(frame * 64u + field * 32u + index + 1u);
        line.carries_data = index == 11u;

        const std::vector<double> samples = render_synthetic_vbi_line(line);
        const uint64_t offset = frame * format.bytes_per_frame() +
                                field * format.bytes_per_field() +
                                index * format.bytes_per_record();
        for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
          bytes[static_cast<size_t>(offset + sample)] = static_cast<uint8_t>(
              std::clamp(std::round(samples[sample]), 0.0, 255.0));
        }
      }
    }
  }
  return bytes;
}

// The teletext fit finds nothing on a captions-only dump; the caption fit
// correlates line 21 of both fields alone and finds its run-in where it is.
// Each field is fitted on its own, so each needs the eight records a fit
// needs: eight frames.
TEST(VBIOffsetCalibration, CalibratesACaptionsOnlyCaptureOnItsCaptionRunIn) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset(
      "bt8x8 card dump, 8-bit (NABTS, NTSC source)", format, error))
      << error;
  VBITeletextService nabts;
  ASSERT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC,
                                   VBITeletextSystem::kNABTS, nabts, error))
      << error;
  constexpr double kTruthOffset = 258.0;

  FakeByteSource source(make_captions_only_capture(format, 10, kTruthOffset));
  VBILineReader reader(format, source);

  VBICalibrationConfig config;
  config.sample_frames = 8;

  VBIOffsetCalibration teletext;
  ASSERT_TRUE(
      calibrate_vbi_capture_offset(reader, nabts, config, teletext, error))
      << error;
  EXPECT_FALSE(teletext.converged) << teletext.summary;

  VBIOffsetCalibration captions;
  ASSERT_TRUE(
      calibrate_vbi_capture_offset_on_captions(reader, config, captions, error))
      << error;
  ASSERT_TRUE(captions.converged) << captions.summary;
  EXPECT_NEAR(captions.capture_offset_samples, kTruthOffset, 0.5);

  // Line 21 of each field, in every sampled frame, and nothing else.
  EXPECT_EQ(captions.records_examined, 8u * 2u);
  EXPECT_EQ(captions.records_accepted, 8u * 2u);

  // The caption line's levels: blanking from its payload of padding, and
  // 50 IRE from the upper tenth of its run-in, a little under the peaks.
  ASSERT_TRUE(captions.data_levels.usable);
  EXPECT_NEAR(captions.data_levels.logic0, 60.0, 3.0);
  EXPECT_NEAR(captions.data_levels.logic1, 140.0, 8.0);
}

// A 625-line capture carries no line-21 captions to fit on.
TEST(VBIOffsetCalibration, A625LineCaptureHasNoCaptionsToCalibrateOn) {
  const VBISourceFormat format = bt8x8_pal_format();
  FakeByteSource source(
      std::vector<uint8_t>(static_cast<size_t>(format.bytes_per_frame()), 0u));
  VBILineReader reader(format, source);

  VBIOffsetCalibration calibration;
  std::string error;
  EXPECT_FALSE(calibrate_vbi_capture_offset_on_captions(
      reader, VBICalibrationConfig{}, calibration, error));
  EXPECT_NE(error.find("525-line"), std::string::npos) << error;
}

// ---------------------------------------------------------------------------
// The caption fit, field by field
// ---------------------------------------------------------------------------

VBISourceFormat cx23885_nabts_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset("cx23885 card dump, 8-bit (NABTS)",
                                       format, error))
      << error;
  return format;
}

VBITeletextService caption_service() {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_caption_service(VBITVSystem::kNTSC, service, error)) << error;
  return service;
}

// One stored field's run-in positions, a frame's worth of records apart.
std::vector<VBICRIObservation> field_observations(
    uint32_t field, const std::vector<double>& positions) {
  std::vector<VBICRIObservation> observations =
      observations_at(positions, field * 12u, 24u);
  for (VBICRIObservation& observation : observations) {
    observation.field_index = field;
  }
  return observations;
}

// Line 21's two fields are two caption streams, and their encoders need not
// time their run-ins alike: on one recording they sit at 4,0 and 19,6
// samples.  Pooled they look like a capture that cannot be time-base
// corrected; fitted field by field, each is tight, and the one global offset
// puts their midpoint on the anchor.
TEST(VBIOffsetCalibrationByField, FitsTwoFieldsWhoseRunInsSitApart) {
  const VBISourceFormat format = cx23885_nabts_format();
  const VBITeletextService captions = caption_service();

  std::vector<VBICRIObservation> observations =
      field_observations(0, scattered_positions(4.0, 0.4, 64));
  const std::vector<VBICRIObservation> second =
      field_observations(1, scattered_positions(19.6, 0.4, 64));
  observations.insert(observations.end(), second.begin(), second.end());

  const VBIOffsetCalibration pooled =
      fit_vbi_capture_offset(format, captions, observations, 128);
  EXPECT_FALSE(pooled.converged) << pooled.summary;

  const VBIOffsetCalibration calibration =
      fit_vbi_capture_offset_by_field(format, captions, observations, {64, 64});
  ASSERT_TRUE(calibration.converged) << calibration.summary;
  EXPECT_NEAR(calibration.anchor_position_samples, 11.8, 0.05);
  EXPECT_NEAR(calibration.capture_offset_samples,
              captions.t_offset_ns * 1e-9 * format.sample_rate_hz - 11.8, 0.05);
  EXPECT_LT(calibration.spread_samples, 1.0);
  EXPECT_EQ(calibration.records_accepted, 128u);
  EXPECT_EQ(calibration.records_examined, 128u);

  ASSERT_EQ(calibration.fields.size(), 2u);
  EXPECT_TRUE(calibration.fields[0].carries_service);
  EXPECT_TRUE(calibration.fields[1].carries_service);
  EXPECT_NEAR(calibration.fields[0].anchor_position_samples, 4.0, 0.05);
  EXPECT_NEAR(calibration.fields[1].anchor_position_samples, 19.6, 0.05);
  EXPECT_NE(calibration.summary.find("stored field 2 at 19.60"),
            std::string::npos)
      << calibration.summary;
}

// A field that carries no captions — or a handful of lines that correlated by
// chance — is left out, and the other field alone sets the offset.
TEST(VBIOffsetCalibrationByField, AFieldWithoutCaptionsIsLeftOut) {
  const VBISourceFormat format = cx23885_nabts_format();
  const VBITeletextService captions = caption_service();

  std::vector<VBICRIObservation> observations =
      field_observations(0, scattered_positions(14.6, 0.5, 40));
  const std::vector<VBICRIObservation> strays =
      field_observations(1, {43.0, 61.0, 61.0});
  observations.insert(observations.end(), strays.begin(), strays.end());

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset_by_field(
      format, captions, observations, {128, 128});
  ASSERT_TRUE(calibration.converged) << calibration.summary;
  EXPECT_NEAR(calibration.anchor_position_samples, 14.6, 0.05);
  ASSERT_EQ(calibration.fields.size(), 2u);
  EXPECT_TRUE(calibration.fields[0].carries_service);
  EXPECT_FALSE(calibration.fields[1].carries_service);
  EXPECT_EQ(calibration.fields[1].records_accepted, 3u);
  ASSERT_FALSE(calibration.warnings.empty());
  EXPECT_NE(calibration.warnings.back().find("stored field 2"),
            std::string::npos)
      << calibration.warnings.back();
}

TEST(VBIOffsetCalibrationByField, NeitherFieldFittingIsRefused) {
  const VBISourceFormat format = cx23885_nabts_format();
  const VBITeletextService captions = caption_service();

  std::vector<VBICRIObservation> observations =
      field_observations(0, {12.0, 30.0});
  const std::vector<VBICRIObservation> scattered =
      field_observations(1, scattered_positions(20.0, 16.0, 64));
  observations.insert(observations.end(), scattered.begin(), scattered.end());

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset_by_field(
      format, captions, observations, {128, 128});
  EXPECT_FALSE(calibration.converged);
  ASSERT_FALSE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.diagnostics.front().find("Neither"), std::string::npos)
      << calibration.diagnostics.front();
}

// One global offset puts each field within half of their disagreement of the
// anchor; fields further apart than the search tolerance cannot both be
// placed by it.
TEST(VBIOffsetCalibrationByField, FieldsTooFarApartForOneOffsetAreRefused) {
  const VBISourceFormat format = cx23885_nabts_format();
  const VBITeletextService captions = caption_service();

  std::vector<VBICRIObservation> observations =
      field_observations(0, scattered_positions(-20.0, 0.4, 64));
  const std::vector<VBICRIObservation> second =
      field_observations(1, scattered_positions(40.0, 0.4, 64));
  observations.insert(observations.end(), second.begin(), second.end());

  const VBIOffsetCalibration calibration =
      fit_vbi_capture_offset_by_field(format, captions, observations, {64, 64});
  EXPECT_FALSE(calibration.converged);
  ASSERT_FALSE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.diagnostics.front().find("apart"), std::string::npos)
      << calibration.diagnostics.front();
}

// A cx23885 dump of line-21 captions alone: a caption line of null pairs on
// record 11 of either field, its run-in where |field_anchor| puts it in the
// record (none for a field without captions), in the frames |has_captions|
// says.
std::vector<uint8_t> make_cx23885_captions_capture(
    const VBISourceFormat& format, uint64_t frame_count,
    const std::array<std::optional<double>, 2>& field_anchor,
    const std::function<bool(uint64_t)>& has_captions) {
  const VBITeletextService caption = caption_service();
  std::vector<bool> null_pair;
  for (uint32_t byte = 0; byte < 2u; ++byte) {
    for (uint32_t bit = 0; bit < 8u; ++bit) {
      null_pair.push_back(bit == 7u);
      null_pair.push_back(bit == 7u);
    }
  }

  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0);
  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    for (uint32_t field = 0; field < 2u; ++field) {
      for (uint32_t index = 0; index < format.field_lines; ++index) {
        SyntheticVBILine line;
        line.sample_rate_hz = format.sample_rate_hz;
        line.valid_samples = format.valid_samples;
        line.bit_rate_hz = caption.bit_rate_hz;
        line.pattern = caption.cri_frc_pattern;
        line.pattern_bits = caption.cri_bits + caption.frc_bits;
        line.run_in_bits = caption.cri_bits;
        line.payload = null_pair;
        line.logic0 = 60.0;
        line.logic1 = 140.0;
        line.noise_amplitude = 0.5;
        line.seed =
            static_cast<uint32_t>(frame * 64u + field * 32u + index + 1u);
        line.carries_data = index == 11u && field_anchor[field].has_value() &&
                            has_captions(frame);
        line.anchor_position_samples = field_anchor[field].value_or(0.0);

        const std::vector<double> samples = render_synthetic_vbi_line(line);
        const uint64_t offset = frame * format.bytes_per_frame() +
                                field * format.bytes_per_field() +
                                index * format.bytes_per_record();
        for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
          bytes[static_cast<size_t>(offset + sample)] = static_cast<uint8_t>(
              std::clamp(std::round(samples[sample]), 0.0, 255.0));
        }
      }
    }
  }
  return bytes;
}

bool every_frame(uint64_t /*frame*/) { return true; }

TEST(VBIOffsetCalibrationOnCaptions,
     FitsAFieldWhoseRunInSitsApartFromTheOther) {
  const VBISourceFormat format = cx23885_nabts_format();
  const VBITeletextService captions = caption_service();
  FakeByteSource source(
      make_cx23885_captions_capture(format, 8, {12.0, 26.0}, every_frame));
  VBILineReader reader(format, source);

  VBICalibrationConfig config;
  config.sample_frames = 8;
  VBIOffsetCalibration calibration;
  std::string error;
  ASSERT_TRUE(calibrate_vbi_capture_offset_on_captions(reader, config,
                                                       calibration, error))
      << error;
  ASSERT_TRUE(calibration.converged) << calibration.summary;
  ASSERT_EQ(calibration.fields.size(), 2u);
  EXPECT_NEAR(calibration.fields[0].anchor_position_samples, 12.0, 0.5);
  EXPECT_NEAR(calibration.fields[1].anchor_position_samples, 26.0, 0.5);
  EXPECT_NEAR(calibration.capture_offset_samples,
              captions.t_offset_ns * 1e-9 * format.sample_rate_hz - 19.0, 0.5);
}

// A cx23885 dump whose card window opens part-way into every run-in.  The
// template's part inside the record still finds it.
TEST(VBIOffsetCalibrationOnCaptions, FindsARunInTheCardWindowClipped) {
  const VBISourceFormat format = cx23885_nabts_format();
  const VBITeletextService captions = caption_service();
  constexpr double kClippedAnchor = -44.0;
  FakeByteSource source(make_cx23885_captions_capture(
      format, 8, {kClippedAnchor, std::nullopt}, every_frame));
  VBILineReader reader(format, source);

  VBICalibrationConfig config;
  config.sample_frames = 8;
  VBIOffsetCalibration calibration;
  std::string error;
  ASSERT_TRUE(calibrate_vbi_capture_offset_on_captions(reader, config,
                                                       calibration, error))
      << error;
  ASSERT_TRUE(calibration.converged) << calibration.summary;
  EXPECT_NEAR(calibration.anchor_position_samples, kClippedAnchor, 0.5);
  EXPECT_NEAR(
      calibration.capture_offset_samples,
      captions.t_offset_ns * 1e-9 * format.sample_rate_hz - kClippedAnchor,
      0.5);
  ASSERT_EQ(calibration.fields.size(), 2u);
  EXPECT_FALSE(calibration.fields[1].carries_service);
}

// Captions in the last quarter of a capture only, on one field, are too few in
// a sixteen-frame sample, and enough in one that samples each frame.
TEST(VBIOffsetCalibrationOnCaptions, ASparseCaptionStreamNeedsEnoughFrames) {
  const VBISourceFormat format = cx23885_nabts_format();
  FakeByteSource source(make_cx23885_captions_capture(
      format, 64, {14.0, std::nullopt},
      [](uint64_t frame) { return frame >= 48u; }));
  VBILineReader reader(format, source);

  VBICalibrationConfig config;
  config.sample_frames = 16;
  VBIOffsetCalibration sparse;
  std::string error;
  ASSERT_TRUE(
      calibrate_vbi_capture_offset_on_captions(reader, config, sparse, error))
      << error;
  EXPECT_FALSE(sparse.converged) << sparse.summary;
  EXPECT_EQ(sparse.records_accepted, 4u);

  config.sample_frames = 64;
  VBIOffsetCalibration dense;
  ASSERT_TRUE(
      calibrate_vbi_capture_offset_on_captions(reader, config, dense, error))
      << error;
  EXPECT_TRUE(dense.converged) << dense.summary;
  EXPECT_EQ(dense.records_accepted, 16u);
  EXPECT_NEAR(dense.anchor_position_samples, 14.0, 0.5);
}

// ---------------------------------------------------------------------------
// The marks of a teletext line
// ---------------------------------------------------------------------------

TEST(VBIOffsetCalibration, ATeletextLineShowsItsFramingCodeOrItsRunIn) {
  VBICRIObservation observation;
  observation.framing_errors = 1;
  observation.run_in_purity = 0.0;
  EXPECT_TRUE(vbi_shows_teletext_marks(observation));

  observation.framing_errors = 2;
  EXPECT_FALSE(vbi_shows_teletext_marks(observation));

  observation.run_in_purity = 0.3;
  EXPECT_TRUE(vbi_shows_teletext_marks(observation));
}

// A cx23885 dump of 525-line NABTS-format lines on records 1 to 4 of either
// field, at one true capture offset: |pattern| is each line's 24-bit run-in
// and framing code, |run_in| the run-in's amplitude, and |payload| what
// follows.
std::vector<uint8_t> make_cx23885_teletext_capture(
    const VBISourceFormat& format, uint32_t pattern, double run_in,
    const std::vector<bool>& payload, uint64_t frame_count,
    double capture_offset_samples) {
  VBITeletextService nabts;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC,
                                   VBITeletextSystem::kNABTS, nabts, error))
      << error;
  const double position =
      nabts.cri_start_samples(format.sample_rate_hz, capture_offset_samples);

  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0);
  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    for (uint32_t field = 0; field < 2u; ++field) {
      for (uint32_t index = 0; index < format.field_lines; ++index) {
        SyntheticVBILine line;
        line.sample_rate_hz = format.sample_rate_hz;
        line.valid_samples = format.valid_samples;
        line.bit_rate_hz = nabts.bit_rate_hz;
        line.pattern = pattern;
        line.run_in_amplitude_fraction = run_in;
        if (!payload.empty()) {
          line.payload = payload;
        } else {
          line.payload_bits = nabts.payload_bytes * 8u;
        }
        line.logic0 = 60.0;
        line.logic1 = 180.0;
        line.blur_bit_periods = 0.4;
        line.noise_amplitude = 0.5;
        line.seed =
            static_cast<uint32_t>(frame * 64u + field * 32u + index + 1u);
        line.carries_data = index >= 1u && index <= 4u;
        // The anchor is the pattern's first one bit; a pattern whose run-in
        // is no run-in is placed so its bit 0 sits where NABTS's would.
        uint32_t first_one = 0;
        while (first_one < 24u && ((pattern >> (23u - first_one)) & 1u) == 0u) {
          ++first_one;
        }
        line.anchor_position_samples =
            position + static_cast<double>(first_one) *
                           nabts.samples_per_bit(format.sample_rate_hz);

        const std::vector<double> samples = render_synthetic_vbi_line(line);
        const uint64_t offset = frame * format.bytes_per_frame() +
                                field * format.bytes_per_field() +
                                index * format.bytes_per_record();
        for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
          bytes[static_cast<size_t>(offset + sample)] = static_cast<uint8_t>(
              std::clamp(std::round(samples[sample]), 0.0, 255.0));
        }
      }
    }
  }
  return bytes;
}

VBIOffsetCalibration calibrate_nabts(const std::vector<uint8_t>& bytes) {
  const VBISourceFormat format = cx23885_nabts_format();
  VBITeletextService nabts;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC,
                                   VBITeletextSystem::kNABTS, nabts, error))
      << error;
  FakeByteSource source(bytes);
  VBILineReader reader(format, source);
  VBICalibrationConfig config;
  config.sample_frames = 4;
  VBIOffsetCalibration calibration;
  EXPECT_TRUE(
      calibrate_vbi_capture_offset(reader, nabts, config, calibration, error))
      << error;
  return calibration;
}

// A slow code with no run-in ahead of it, as some recordings carry on line
// 11, can correlate with the run-in and framing code template well enough to be
// accepted at one place on every line, and still be no teletext: the fit is
// refused, and the caption fit gets its turn.
TEST(VBIOffsetCalibration, RefusesAFitOnLinesShowingNeitherMarkOfTeletext) {
  std::vector<bool> slow_code;
  for (uint32_t bit = 0; bit < 300u; ++bit) {
    slow_code.push_back((bit / 6u) % 2u == 0u);
  }
  const VBIOffsetCalibration calibration =
      calibrate_nabts(make_cx23885_teletext_capture(
          cx23885_nabts_format(), 0x000023u, 1.0, slow_code, 6, 250.0));
  EXPECT_GE(calibration.records_accepted, 8u) << calibration.summary;
  EXPECT_LT(calibration.spread_samples, 1.0) << calibration.summary;
  EXPECT_FALSE(calibration.converged) << calibration.summary;
  ASSERT_FALSE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.diagnostics.back().find("framing code"),
            std::string::npos)
      << calibration.diagnostics.back();
}

// Either mark is enough.  A tape erases the run-in long before the data
// (design §5.3.6), and the framing code still reads; a line framed otherwise
// than the configured service — here WST's 0xE4 read as NABTS, two bits from
// 0xE7 — reads its framing code wrong, and its run-in still alternates.
TEST(VBIOffsetCalibration, KeepsAFitThatShowsEitherMarkOfTeletext) {
  const VBISourceFormat format = cx23885_nabts_format();

  const VBIOffsetCalibration collapsed = calibrate_nabts(
      make_cx23885_teletext_capture(format, 0xAAAAE7u, 0.0, {}, 6, 250.0));
  EXPECT_TRUE(collapsed.converged) << collapsed.summary;
  EXPECT_NEAR(collapsed.capture_offset_samples, 250.0, 0.5);

  const VBIOffsetCalibration other_framing = calibrate_nabts(
      make_cx23885_teletext_capture(format, 0xAAAAE4u, 1.0, {}, 6, 250.0));
  EXPECT_TRUE(other_framing.converged) << other_framing.summary;
  EXPECT_NEAR(other_framing.capture_offset_samples, 250.0, 0.5);
}

// A time-base corrected source has its origin at 0H by construction, so
// calibrating one would replace an exactly known value with a fitted one
// (design §5.3.3).
TEST(VBIOffsetCalibration, RefusesToCalibrateATimeBaseCorrectedSource) {
  VBISourceFormat format = bt8x8_pal_format();
  format.family = VBISourceFamily::kTBCDerived;

  FakeByteSource source(
      std::vector<uint8_t>(static_cast<size_t>(format.bytes_per_frame()), 0u));
  VBILineReader reader(format, source);

  VBIOffsetCalibration calibration;
  std::string error;
  EXPECT_FALSE(calibrate_vbi_capture_offset(
      reader, wst_service(), VBICalibrationConfig{}, calibration, error));
  EXPECT_NE(error.find("exactly zero"), std::string::npos) << error;
}

// The other half of that rule. Such a source still has an unknown — when the
// broadcaster transmitted, which the service table only tabulates for the
// captures it was measured on — and that one is measured from the same survey,
// read as an anchor rather than as an offset.
TEST(VBIOffsetCalibration, MeasuresTheServiceAnchorOfATimeBaseCorrectedSource) {
  VBISourceFormat format = bt8x8_pal_format();
  format.family = VBISourceFamily::kTBCDerived;
  format.capture_offset_is_auto = false;
  format.capture_offset_samples = 0.0;

  // A transmission that put its run-in 12 samples earlier than the table says.
  // Displacing the run-in produces the same bytes whichever way they are read;
  // what differs is the reading, and a record of this family starts at 0H, so
  // the displacement is the broadcaster's rather than the hardware's.
  constexpr double kShift = 12.0;
  const VBITeletextService service = wst_service();
  const double truth_anchor =
      service.cri_start_samples(format.sample_rate_hz, kShift);

  FakeByteSource source(make_synthetic_capture(format, service, 4, kShift));
  VBILineReader reader(format, source);

  VBICalibrationConfig config;
  config.sample_frames = 4;

  VBIOffsetCalibration calibration;
  std::string error;
  ASSERT_TRUE(
      measure_vbi_service_anchor(reader, service, config, calibration, error))
      << error;
  ASSERT_TRUE(calibration.converged) << calibration.summary;

  // The record starts at 0H, so where the run-in sits in the record is when it
  // was transmitted, with no further correction.
  EXPECT_NEAR(calibration.anchor_position_samples, truth_anchor, 0.2);

  const double measured_ns =
      vbi_measured_anchor_ns(calibration, format.sample_rate_hz);
  EXPECT_NEAR(measured_ns, truth_anchor / format.sample_rate_hz * 1e9, 1.0);
  EXPECT_LT(measured_ns, service.t_offset_ns);
}

// A card capture's fitted offset has already put its run-in on the tabulated
// anchor, so there is no second quantity there to measure: asking for one would
// be fitting the same unknown twice.
TEST(VBIOffsetCalibration, RefusesToMeasureTheAnchorOfACardCapture) {
  const VBISourceFormat format = bt8x8_pal_format();

  FakeByteSource source(
      std::vector<uint8_t>(static_cast<size_t>(format.bytes_per_frame()), 0u));
  VBILineReader reader(format, source);

  VBIOffsetCalibration calibration;
  std::string error;
  EXPECT_FALSE(measure_vbi_service_anchor(
      reader, wst_service(), VBICalibrationConfig{}, calibration, error));
  EXPECT_NE(error.find("time-base corrected"), std::string::npos) << error;
}

TEST(VBIOffsetCalibration, ReportsACaptureWithNoWholeFramesAsAnError) {
  const VBISourceFormat format = bt8x8_pal_format();

  FakeByteSource source(std::vector<uint8_t>(1024, 0u));
  VBILineReader reader(format, source);

  VBIOffsetCalibration calibration;
  std::string error;
  EXPECT_FALSE(calibrate_vbi_capture_offset(
      reader, wst_service(), VBICalibrationConfig{}, calibration, error));
  EXPECT_NE(error.find("no whole frames"), std::string::npos) << error;
}

}  // namespace
}  // namespace orc
