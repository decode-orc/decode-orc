/*
 * File:        vbi_ntsc_source_test.cpp
 * Module:      orc-tests
 * Purpose:     End-to-end validation of the VBI source stage on real 525-line
 *              media
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vbi_cri_correlator.h"
#include "vbi_cri_template.h"
#include "vbi_output_frame.h"
#include "vbi_source_format.h"
#include "vbi_source_stage.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

// A real 525-line WST capture: the TBS Electra recordings are the circulating
// material that carries System B on 525 lines, which is the service the stage
// places.  Like every sample under test-data/ it is not checked in, so the
// tests skip when it is absent.
const char* kElectraCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/TBS Electra WST 525-60/"
    "TBS_1988-01-03_Electra_teletext_NTSC_SP_vbi_only_u16-002.flac";

// A real NABTS capture, in the same container as the Electra ones: the two
// services differ in framing code and packet length, not in how they are
// stored, so the same preset reads both and only the service is configured
// differently.
const char* kExtraVisionCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/NTSC NABTS Teletext samples/"
    "CBS_1985-12-10_ExtraVision_teletext_NTSC_SP_vbi_only_part4_u16.flac";

// A real 525-line bt8x8 card dump: the same card as the PAL reference capture,
// on the driver's other television norm — a different sampling clock, a
// different valid sample count and a different first stored line, in the same
// 2048-byte record and 65 536-byte frame.  This one carries 525-line WST.
const char* kKeyfaxCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/bt8x8 NTSC sample/"
    "TBS_1984-02-01_Keyfax_teletext_NTSC_WST_bt8x8.vbi";

// A real 525-line cx2388x card dump (issue #332): the bt8x8's clock, record
// stride and valid sample count, in a twelve-record field with no frame
// counter.  This one carries NABTS.
const char* kCx2388xCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/cx2388x NTSC sample/"
    "CBS_198x-xx-xx_ExtraVision_teletext_NTSC_NABTS_cx88.vbi.flac";

// Stored frame lines a 525-line capture carries data on: broadcast frame lines
// 10-21 and 273-284 (design §5.1).
const std::vector<size_t> kTeletextFrameLines = [] {
  std::vector<size_t> lines;
  for (size_t line = 9; line <= 20; ++line) lines.push_back(line);
  for (size_t line = 272; line <= 283; ++line) lines.push_back(line);
  return lines;
}();

// Frames examined, spread evenly across the middle of the capture rather than
// taken from its head: the opening of a tape is a lead-in that may carry
// nothing at all, and the captures differ in length by a factor of two.
constexpr uint32_t kFramesUnderTest = 6;

std::vector<FrameID> frames_under_test(uint64_t frame_count) {
  std::vector<FrameID> frames;
  for (uint32_t index = 0; index < kFramesUnderTest; ++index) {
    const uint64_t span = frame_count / 4;
    const uint64_t step = span / kFramesUnderTest;
    frames.push_back(static_cast<FrameID>(span + index * step));
  }
  return frames;
}

bool electra_capture_available() {
  return std::filesystem::exists(kElectraCapture);
}

std::map<std::string, ParameterValue> electra_parameters() {
  return {
      {"input_path", std::string(kElectraCapture)},
      {"format", std::string(".tbc VBI crop, 16-bit (WST)")},
  };
}

std::shared_ptr<VideoFrameRepresentation> load_electra_capture(
    VBISourceStage& stage, ObservationContext& observations) {
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, electra_parameters(), observations);
  if (outputs.empty()) return nullptr;
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
}

bool keyfax_capture_available() {
  return std::filesystem::exists(kKeyfaxCapture);
}

std::shared_ptr<VideoFrameRepresentation> load_keyfax_capture(
    VBISourceStage& stage, ObservationContext& observations) {
  const std::map<std::string, ParameterValue> parameters = {
      {"input_path", std::string(kKeyfaxCapture)},
      {"format", std::string("bt8x8 card dump, 8-bit (WST, NTSC source)")},
  };
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, parameters, observations);
  if (outputs.empty()) return nullptr;
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
}

// The container this capture is read with, expressed on the output lattice so
// the placed line can be searched with the same machinery a source record is.
VBISourceFormat output_lattice_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (WST)", format, error))
      << error;
  // The output lattice: same rate, one whole line, and the run-in expected at
  // the standard's own position because that is where the stage puts it.
  format.line_length = static_cast<uint32_t>(kNtscSamplesPerLine);
  format.valid_samples = static_cast<uint32_t>(kNtscSamplesPerLine);
  format.capture_offset_samples = 0.0;
  // A tape's line-to-line timing scatters, so the search is the wide one.
  format.calibration.search_tolerance_samples = 12.0;
  return format;
}

// ---------------------------------------------------------------------------
// The stage against a real 525-line capture
// ---------------------------------------------------------------------------

TEST(VBINTSCSource, OpensTheCaptureAndReportsNTSCFrames) {
  if (!electra_capture_available()) {
    GTEST_SKIP() << "525-line capture not present: " << kElectraCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_electra_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  // 2 244 409 440 samples of u16 at 910 x 16 x 2 samples per frame: 77 074
  // whole frames and one trailing field, which is not emitted.
  EXPECT_EQ(representation->frame_count(), 77074u);

  const auto parameters = representation->get_video_parameters();
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters->system, VideoSystem::NTSC);
  EXPECT_EQ(parameters->frame_width_nominal, kNtscSamplesPerLine);
  EXPECT_EQ(parameters->frame_height, kNtscFrameLines);

  const auto descriptor = representation->get_frame_descriptor(0);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->system, VideoSystem::NTSC);
  EXPECT_EQ(descriptor->samples_total, static_cast<size_t>(kNtscFrameSamples));

  // A TBC-derived capture is never calibrated: its records already start at
  // 0H, so there is no offset to fit and none is reported.
  EXPECT_FALSE(
      observations.get(FieldID(0), "vbi_source", "capture_offset").has_value());

  const auto system =
      observations.get(FieldID(0), "vbi_source", "video_system");
  ASSERT_TRUE(system.has_value());
  EXPECT_EQ(std::get<std::string>(*system), "NTSC");
}

// Search a loaded capture's placed lines for the configured service's run-in
// and framing code, and report where they were found.
//
// This is the acceptance criterion available before a 525-line slicer exists:
// the run-in and framing code survive the u16 decode, the level mapping and the
// placement, and land on the output line where the configured service says they
// should.  That is everything this stage is responsible for; recovering the
// packet bytes is the slicer's job.  It is also the check that distinguishes
// the two 525-line services, because their framing codes differ.
struct RunInSurvey {
  uint64_t candidate_lines = 0;
  uint64_t detected = 0;
  double median_position = 0.0;
  double expected_position = 0.0;
};

RunInSurvey survey_run_in(const VideoFrameRepresentation& representation,
                          VBITeletextSystem tt_system) {
  RunInSurvey survey;

  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(
      vbi_teletext_service(VBITVSystem::kNTSC, tt_system, service, error))
      << error;

  // The template is generated at the output rate, because it is the output
  // line being searched, and blurred to match what survives a tape: the run-in
  // has been very nearly filtered away (design §5.3.6).
  VBICRITemplateConfig template_config;
  template_config.blur_bit_periods = 0.8;
  VBICRITemplate tmpl;
  EXPECT_TRUE(make_vbi_cri_frc_template(service, kNtscSampleRate,
                                        template_config, tmpl, error))
      << error;

  const VBISourceFormat lattice = output_lattice_format();
  const VBICRISearchWindow window = vbi_cri_search_window(lattice, service);
  EXPECT_FALSE(window.empty());

  std::vector<double> positions;
  for (const FrameID frame : frames_under_test(representation.frame_count())) {
    for (const size_t line : kTeletextFrameLines) {
      const std::vector<int16_t> samples =
          representation.get_line_samples(frame, line);
      EXPECT_FALSE(samples.empty()) << "frame " << frame << " line " << line;
      if (samples.empty()) continue;
      ++survey.candidate_lines;

      const std::vector<double> as_doubles(samples.begin(), samples.end());
      const VBICRIDetection detection =
          detect_vbi_cri_position(as_doubles, tmpl, window, 0.5);
      if (!detection.accepted) continue;
      ++survey.detected;
      positions.push_back(detection.anchor_position_samples);
    }
  }

  std::sort(positions.begin(), positions.end());
  survey.median_position =
      positions.empty() ? 0.0 : positions[positions.size() / 2];
  survey.expected_position = service.t_offset_ns * 1e-9 * kNtscSampleRate;
  return survey;
}

void report(const char* what, const RunInSurvey& survey) {
  std::cout << "[ INFO     ] " << what << ": " << survey.detected << " of "
            << survey.candidate_lines
            << " placed lines carried a locatable run-in, median at output "
               "sample "
            << survey.median_position << " (configured "
            << survey.expected_position << ")" << std::endl;
}

TEST(VBINTSCSource, TheWSTRunInLandsAtItsConfiguredPositionOnTheOutputLines) {
  if (!electra_capture_available()) {
    GTEST_SKIP() << "525-line WST capture not present: " << kElectraCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_electra_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RunInSurvey survey =
      survey_run_in(*representation, VBITeletextSystem::kWST);
  report("Electra (WST)", survey);

  // These broadcasts use six of the twelve lines the standard gives them, and
  // a tape does not deliver every one of those cleanly, so the bar is set for
  // what such material really carries — well above what a mis-placed or
  // mis-scaled output could reach by accident.
  ASSERT_GT(survey.detected, survey.candidate_lines / 8)
      << survey.detected << " of " << survey.candidate_lines << " located";
  EXPECT_NEAR(survey.median_position, survey.expected_position, 3.0);
}

// ---------------------------------------------------------------------------
// bt8x8 card dumps of a 525-line source
//
// The other way 525-line teletext reaches this stage, and the one the stage
// has to do real work for: unlike the .tbc crops above, a card dump's records
// do not start at 0H, so where they start has to be fitted from the clock
// run-in before anything can be placed.
// ---------------------------------------------------------------------------

TEST(VBINTSCSource, ABt8x8CardDumpOfA525LineSourceLoadsAsNTSCFrames) {
  if (!keyfax_capture_available()) {
    GTEST_SKIP() << "525-line bt8x8 capture not present: " << kKeyfaxCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_keyfax_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  // 910 819 328 bytes at 2048 x 16 x 2 bytes per frame: 13 898 whole frames
  // and nothing over.
  EXPECT_EQ(representation->frame_count(), 13898u);

  const auto parameters = representation->get_video_parameters();
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters->system, VideoSystem::NTSC);
  EXPECT_EQ(parameters->frame_width_nominal, kNtscSamplesPerLine);
  EXPECT_EQ(parameters->frame_height, kNtscFrameLines);

  const auto descriptor = representation->get_frame_descriptor(0);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->samples_total, static_cast<size_t>(kNtscFrameSamples));

  const auto service =
      observations.get(FieldID(0), "vbi_source", "teletext_system");
  ASSERT_TRUE(service.has_value());
  EXPECT_EQ(std::get<std::string>(*service), "WST");
}

// A card capture is calibrated, and this is the check that the container is
// described correctly: a wrong stride, sampling rate or television system
// scatters the run-in far beyond the spread limit and stops the run.  The fit
// settles 5,1 samples from the driver's folkloric 244, which is as close as
// that constant gets on either of its norms.
TEST(VBINTSCSource, ABt8x8CardDumpFitsItsCaptureOffsetFromTheRunIn) {
  if (!keyfax_capture_available()) {
    GTEST_SKIP() << "525-line bt8x8 capture not present: " << kKeyfaxCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_keyfax_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const auto offset =
      observations.get(FieldID(0), "vbi_source", "capture_offset");
  ASSERT_TRUE(offset.has_value())
      << "a card capture's offset must be fitted, not taken from the preset";
  EXPECT_NEAR(std::get<double>(*offset), 249.1, 2.0);
}

TEST(VBINTSCSource, TheRunInOfABt8x8CardDumpLandsAtItsConfiguredPosition) {
  if (!keyfax_capture_available()) {
    GTEST_SKIP() << "525-line bt8x8 capture not present: " << kKeyfaxCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_keyfax_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RunInSurvey survey =
      survey_run_in(*representation, VBITeletextSystem::kWST);
  report("Keyfax (bt8x8, WST)", survey);

  // This broadcast uses five of the twelve lines in one field and three in the
  // other, and a tape does not deliver every one of those cleanly, so the bar
  // is what such material really carries rather than what the standard allows.
  ASSERT_GT(survey.detected, survey.candidate_lines / 8)
      << survey.detected << " of " << survey.candidate_lines << " located";
  EXPECT_NEAR(survey.median_position, survey.expected_position, 3.0);
}

// ---------------------------------------------------------------------------
// cx2388x card dumps of a 525-line source
// ---------------------------------------------------------------------------

bool cx2388x_capture_available() {
  return std::filesystem::exists(kCx2388xCapture);
}

std::shared_ptr<VideoFrameRepresentation> load_cx2388x_capture(
    VBISourceStage& stage, ObservationContext& observations) {
  const std::map<std::string, ParameterValue> parameters = {
      {"input_path", std::string(kCx2388xCapture)},
      {"format", std::string("cx2388x card dump, 8-bit (NABTS, NTSC source)")},
  };
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, parameters, observations);
  if (outputs.empty()) return nullptr;
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
}

TEST(VBINTSCSource, ACx2388xCardDumpLoadsAsNTSCFrames) {
  if (!cx2388x_capture_available()) {
    GTEST_SKIP() << "cx2388x capture not present: " << kCx2388xCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_cx2388x_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  // 316 735 488 bytes at 2048 x 12 x 2 bytes per frame: 6444 whole frames and
  // nothing over.
  EXPECT_EQ(representation->frame_count(), 6444u);

  const auto parameters = representation->get_video_parameters();
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters->system, VideoSystem::NTSC);
  EXPECT_EQ(parameters->frame_height, kNtscFrameLines);

  const auto service =
      observations.get(FieldID(0), "vbi_source", "teletext_system");
  ASSERT_TRUE(service.has_value());
  EXPECT_EQ(std::get<std::string>(*service), "NABTS");
}

// The fit is the check that the container is described correctly: a wrong
// stride, rate or system scatters the run-in past the spread limit.  The next
// line's sync pulse puts sample 0 about 242 samples after 0H, but what the fit
// measures is the offset that puts the run-in on the service table's NABTS
// anchor, and this transmission's run-in is 0,42 us later than that anchor —
// so it settles at 230,1, twelve samples short of the sync figure.
TEST(VBINTSCSource, ACx2388xCardDumpFitsItsCaptureOffsetFromTheRunIn) {
  if (!cx2388x_capture_available()) {
    GTEST_SKIP() << "cx2388x capture not present: " << kCx2388xCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_cx2388x_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const auto offset =
      observations.get(FieldID(0), "vbi_source", "capture_offset");
  ASSERT_TRUE(offset.has_value())
      << "a card capture's offset must be fitted, not taken from the preset";
  EXPECT_NEAR(std::get<double>(*offset), 230.1, 2.0);
}

TEST(VBINTSCSource, TheRunInOfACx2388xCardDumpLandsAtItsConfiguredPosition) {
  if (!cx2388x_capture_available()) {
    GTEST_SKIP() << "cx2388x capture not present: " << kCx2388xCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_cx2388x_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RunInSurvey as_nabts =
      survey_run_in(*representation, VBITeletextSystem::kNABTS);
  const RunInSurvey as_wst =
      survey_run_in(*representation, VBITeletextSystem::kWST);
  report("ExtraVision (cx2388x) as NABTS", as_nabts);
  report("ExtraVision (cx2388x) as WST", as_wst);

  ASSERT_GT(as_nabts.detected, as_nabts.candidate_lines / 8)
      << as_nabts.detected << " of " << as_nabts.candidate_lines << " located";
  EXPECT_NEAR(as_nabts.median_position, as_nabts.expected_position, 3.0);
  EXPECT_GT(as_nabts.detected, as_wst.detected * 2);
}

// ---------------------------------------------------------------------------
// NABTS
// ---------------------------------------------------------------------------

bool extravision_capture_available() {
  return std::filesystem::exists(kExtraVisionCapture);
}

std::shared_ptr<VideoFrameRepresentation> load_extravision_capture(
    VBISourceStage& stage, ObservationContext& observations) {
  const std::map<std::string, ParameterValue> parameters = {
      {"input_path", std::string(kExtraVisionCapture)},
      {"format", std::string(".tbc VBI crop, 16-bit (NABTS)")},
  };
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, parameters, observations);
  if (outputs.empty()) return nullptr;
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
}

// The same container as the WST captures, read with the other service.
TEST(VBINTSCSource, ANABTSCaptureLoadsThroughTheSamePreset) {
  if (!extravision_capture_available()) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_extravision_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  // 1 037 327 200 samples of u16 at 910 x 16 x 2 per frame: 35 622 whole
  // frames and one trailing field.
  EXPECT_EQ(representation->frame_count(), 35622u);

  const auto parameters = representation->get_video_parameters();
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters->system, VideoSystem::NTSC);

  const auto system =
      observations.get(FieldID(0), "vbi_source", "teletext_system");
  ASSERT_TRUE(system.has_value());
  EXPECT_EQ(std::get<std::string>(*system), "NABTS");
}

// The framing code is the only thing that tells the two 525-line services
// apart on a capture, so this is also what says the service was configured
// correctly: the NABTS template locks on this material and the WST one, which
// differs from it only in the framing code, largely does not.
TEST(VBINTSCSource, TheNABTSFramingCodeIsWhatThisCaptureCarries) {
  if (!extravision_capture_available()) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_extravision_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RunInSurvey as_nabts =
      survey_run_in(*representation, VBITeletextSystem::kNABTS);
  const RunInSurvey as_wst =
      survey_run_in(*representation, VBITeletextSystem::kWST);
  report("ExtraVision as NABTS", as_nabts);
  report("ExtraVision as WST", as_wst);

  ASSERT_GT(as_nabts.detected, as_nabts.candidate_lines / 8)
      << as_nabts.detected << " of " << as_nabts.candidate_lines << " located";
  EXPECT_NEAR(as_nabts.median_position, as_nabts.expected_position, 3.0);

  // Read as the wrong service, the same lines largely stop matching.
  EXPECT_GT(as_nabts.detected, as_wst.detected * 2)
      << "the WST framing code matched this NABTS capture nearly as well as "
         "the NABTS one, so the two are not being told apart";
}

}  // namespace
}  // namespace orc
