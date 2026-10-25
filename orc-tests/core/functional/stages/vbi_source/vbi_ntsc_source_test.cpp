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

#include "teletext_frame_slicer.h"
#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/teletext_row_squasher.h"
#include "vbi-services/teletext_slicer.h"
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

// ---------------------------------------------------------------------------
// The recovery chain against a real 525-line capture: frame slicer -> page
// decoder. The stage supplies the frames; what is under test is that the
// 525-line service survives the whole path the teletext sink takes it
// through.
// ---------------------------------------------------------------------------

namespace {

// Frames read. A page is transmitted a row at a time and this capture carries
// up to a dozen packets a field, so a few hundred frames span several carousel
// cycles — enough for whole pages to assemble.
constexpr uint32_t kPageFrames = 300;

// Where to start reading. The head of a tape is lead-in, so this is a quarter
// of the way in, as the other tests here do.
FrameID page_survey_start(uint64_t frame_count) {
  return static_cast<FrameID>(frame_count / 4);
}

struct RecoverySurvey {
  int packets = 0;
  // Packets the slicer read at the 34-byte length the 525-line service
  // transmits, rather than the 42-byte 625-line packet.
  int packets_at_525_length = 0;
  // Odd-parity display bytes per recovered packet, as a fraction of the 32 the
  // service carries (ITU-R BT.653 Table 1b §2.4.1). An undamaged packet is
  // 1,0 throughout; noise sits at 0,5.
  std::vector<double> parity_fractions;
  std::vector<TeletextPageSnapshot> pages;
};

RecoverySurvey survey_recovery(VideoFrameRepresentation& representation) {
  RecoverySurvey survey;
  TeletextFrameSlicer frame_slicer;
  TeletextPageDecoder decoder;
  // As the analysis sink reads it: with a squasher, so repeated copies of a
  // row — and of the packets that extend it — correct each other.
  TeletextRowSquasher squasher;
  decoder.set_row_squasher(&squasher);
  decoder.set_page_callback([&survey](const TeletextPageSnapshot& page) {
    survey.pages.push_back(page);
  });

  TeletextFieldScan scan;
  const FrameID first = page_survey_start(representation.frame_count());
  for (uint32_t index = 0; index < kPageFrames; ++index) {
    const FrameID frame = first + index;
    if (!representation.has_frame(frame)) break;

    for (size_t field_index = 0; field_index < 2; ++field_index) {
      const FieldID field(frame * 2 + field_index);
      // slice_field() reports in ascending field-line order, which is the
      // order a real consumer feeds and the order the packets were
      // transmitted in.
      frame_slicer.slice_field(representation, frame, field_index, scan);
      for (const auto& line : scan.lines) {
        if (!line.sliced.valid) continue;

        ++survey.packets;
        if (line.sliced.packet_bytes == kTeletext525PacketBytes) {
          ++survey.packets_at_525_length;
        }

        int odd = 0;
        for (size_t i = 2; i < line.sliced.packet_bytes; ++i) {
          odd += teletext_odd_parity_valid(line.sliced.bytes[i]) ? 1 : 0;
        }
        survey.parity_fractions.push_back(
            static_cast<double>(odd) /
            static_cast<double>(line.sliced.packet_bytes - 2));

        // A stable source id per recovered line, which is what a consumer
        // re-reading the same line has to supply for the squasher to replace
        // rather than recount it.
        decoder.process_packet(
            line.sliced.bytes, static_cast<int64_t>(field.value()),
            static_cast<int64_t>(field.value()) * 32 + line.field_line,
            line.sliced.has_byte_confidence ? &line.sliced.byte_confidence
                                            : nullptr,
            line.sliced.packet_bytes);
      }
    }
  }
  decoder.finalize(static_cast<int64_t>((first + kPageFrames) * 2));
  return survey;
}

// Printable display text of a page, one string per received row.
std::vector<std::string> page_rows(const TeletextPageSnapshot& page) {
  std::vector<std::string> rows;
  for (int row = 0; row < TeletextPageSnapshot::kRows; ++row) {
    if (!page.row_received[static_cast<size_t>(row)]) continue;
    std::string text;
    for (int column = 0; column < page.columns; ++column) {
      const auto& cell =
          page.cells[static_cast<size_t>(row)][static_cast<size_t>(column)];
      // Mosaic cells are graphics, not text, and are not what is being read.
      const bool printable = !cell.mosaic && !cell.held_mosaic &&
                             cell.character >= 0x20 && cell.character < 0x7F;
      text.push_back(printable ? static_cast<char>(cell.character) : ' ');
    }
    rows.push_back(text);
  }
  return rows;
}

// Short alphabetic words on a page. This is what separates a recovered
// broadcast from a page assembled out of false locks: noise decoded as
// characters gives long runs of one letter and very few word-shaped tokens,
// while real text gives many.
size_t word_count(const TeletextPageSnapshot& page) {
  size_t words = 0;
  for (const auto& row : page_rows(page)) {
    size_t run = 0;
    for (size_t i = 0; i <= row.size(); ++i) {
      const bool alpha = i < row.size() &&
                         std::isalpha(static_cast<unsigned char>(row[i])) != 0;
      if (alpha) {
        ++run;
      } else {
        if (run >= 2 && run <= 12) ++words;
        run = 0;
      }
    }
  }
  return words;
}

}  // namespace

TEST(VBINTSCSource, TheObserverRecoversPacketsAtTheServicesOwnLength) {
  if (!electra_capture_available()) {
    GTEST_SKIP() << "525-line capture not present: " << kElectraCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_electra_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RecoverySurvey survey = survey_recovery(*representation);
  ASSERT_GT(survey.packets, 0)
      << "the observer recovered nothing from a capture the stage placed "
         "teletext on";

  // Every packet must be the 34-byte one: a 42-byte string here would mean the
  // observer had read this capture as the 625-line service.
  EXPECT_EQ(survey.packets_at_525_length, survey.packets);

  // The bits have to be right, not merely present. Odd parity is the standard's
  // own check on the display bytes and it is content-independent: noise sits at
  // 0,5, so a median this high says the packets carry the broadcast.
  auto fractions = survey.parity_fractions;
  std::sort(fractions.begin(), fractions.end());
  const double median = fractions[fractions.size() / 2];
  std::cout << "Electra: " << survey.packets << " packets, median parity "
            << median << ", " << survey.pages.size() << " pages\n";
  EXPECT_GT(median, 0.9);
}

// The same recovery chain against the bt8x8 card dump, which is where the
// whole of this preset's work shows up: the records were resampled from the
// card's 8 x fsc clock onto the output lattice and placed at a fitted offset,
// and if any part of that were wrong the slicer would read noise.
TEST(VBINTSCSource, TheObserverRecoversPacketsFromABt8x8CardDump) {
  if (!keyfax_capture_available()) {
    GTEST_SKIP() << "525-line bt8x8 capture not present: " << kKeyfaxCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_keyfax_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RecoverySurvey survey = survey_recovery(*representation);
  ASSERT_GT(survey.packets, 0)
      << "the observer recovered nothing from a capture the stage placed "
         "teletext on";

  // 34-byte packets throughout: a 42-byte string would mean the 625-line
  // service had been read off a 525-line capture.
  EXPECT_EQ(survey.packets_at_525_length, survey.packets);

  auto fractions = survey.parity_fractions;
  std::sort(fractions.begin(), fractions.end());
  const double median = fractions[fractions.size() / 2];
  std::cout << "Keyfax (bt8x8): " << survey.packets
            << " packets, median parity " << median << ", "
            << survey.pages.size() << " pages\n";
  EXPECT_GT(median, 0.9);
}

TEST(VBINTSCSource, ThePageDecoderAssemblesReadablePagesFromABt8x8CardDump) {
  if (!keyfax_capture_available()) {
    GTEST_SKIP() << "525-line bt8x8 capture not present: " << kKeyfaxCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_keyfax_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RecoverySurvey survey = survey_recovery(*representation);
  ASSERT_FALSE(survey.pages.empty())
      << "no page completed over " << kPageFrames << " frames";

  const TeletextPageSnapshot* best = nullptr;
  for (const auto& page : survey.pages) {
    ASSERT_EQ(page.columns, TeletextPageSnapshot::kColumns)
        << "page assembled at the wrong width";
    if (best == nullptr || word_count(page) > word_count(*best)) best = &page;
  }
  ASSERT_NE(best, nullptr);

  std::cout << "Keyfax (bt8x8): best page is " << best->magazine << std::hex
            << best->page_number << std::dec << " with " << word_count(*best)
            << " word-shaped tokens over " << survey.pages.size() << " pages\n";

  // Noise decoded as characters gives long runs of one letter and very few
  // word-shaped tokens; a recovered broadcast page gives many.
  EXPECT_GT(word_count(*best), 20u);
}

TEST(VBINTSCSource, ThePageDecoderAssemblesReadable40ColumnPages) {
  if (!electra_capture_available()) {
    GTEST_SKIP() << "525-line capture not present: " << kElectraCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_electra_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RecoverySurvey survey = survey_recovery(*representation);
  ASSERT_FALSE(survey.pages.empty())
      << "no page completed over " << kPageFrames << " frames";

  // 40 columns from 34-byte packets: this service sends the last 8 columns of
  // each row in row-extension packets (teletext_page_decoder.h). A page still
  // 32 wide here would mean they had stopped being recognised.
  for (const auto& page : survey.pages) {
    ASSERT_EQ(page.columns, TeletextPageSnapshot::kColumns)
        << "page assembled at the wrong width";
  }

  // Page 100 — magazine 1, page 00 — is the service's front page, so a decode
  // that addresses packets correctly finds it. Recovering it at all exercises
  // the Hamming 8/4 MRAG and page-number path on real bytes.
  const TeletextPageSnapshot* front_page = nullptr;
  for (const auto& page : survey.pages) {
    if (page.magazine != 1 || page.page_number != 0x00) continue;
    if (front_page == nullptr || word_count(page) > word_count(*front_page)) {
      front_page = &page;
    }
  }
  ASSERT_NE(front_page, nullptr) << "page 100 never completed";

  for (const auto& row : page_rows(*front_page)) {
    std::cout << "  |" << row << "|\n";
  }

  // Real text, not a page assembled out of false locks. The front page of this
  // capture carries its index (\"News index ....101\" and its neighbours) plus
  // a headline block, which is comfortably above this; a noise page scores a
  // handful.
  EXPECT_GT(word_count(*front_page), 20u);

  // The extension columns carry the page, not a blank margin: this service
  // fills its lines to the right-hand edge, so several rows must have content
  // past the 32 columns one packet brings.
  size_t rows_using_extension_columns = 0;
  for (const auto& row : page_rows(*front_page)) {
    const auto last = row.find_last_not_of(' ');
    if (last != std::string::npos &&
        static_cast<int>(last) >=
            static_cast<int>(kTeletext525PacketBytes) - 2) {
      ++rows_using_extension_columns;
    }
  }
  EXPECT_GT(rows_using_extension_columns, 3u)
      << "no row reached past column 32 — the row-extension packets are not "
         "reaching the page";

  // And they land on the right rows. A misattributed extension packet still
  // passes odd parity — it is eight valid characters in the wrong place — so
  // only content says the block mapping is right. This page's copyright line
  // spans columns 27 to 38, crossing the boundary between what the display
  // packet brought and what the extension packet did.
  bool joins_across_the_boundary = false;
  for (const auto& row : page_rows(*front_page)) {
    if (row.find("Broadcasting") != std::string::npos) {
      joins_across_the_boundary = true;
    }
  }
  EXPECT_TRUE(joins_across_the_boundary)
      << "the copyright line did not read across column 32 — the extension "
         "packets are being attributed to the wrong rows";

  // And they arrive as cleanly as the columns the display packets bring. The
  // extension columns are squashed over their own repeats like any others, so
  // damage in them is evidence of a mis-read packet rather than of the tape:
  // the extension packets each serve four rows, and one attributed to the wrong
  // block puts eight characters into four wrong places at once.
  //
  // Only the magazines that carry the service's content. This capture also
  // carries test pages — magazine 8's rolling character ramp and magazine 4's
  // "CLOCK CRACKER", whose display bytes are chosen to stress a receiver's
  // clock recovery rather than to satisfy odd parity — and neither has an
  // extension carrier, so they would only dilute both figures.
  const int kHeadColumns = static_cast<int>(kTeletext525PacketBytes) - 2;
  size_t head_cells = 0;
  size_t damaged_head_cells = 0;
  size_t extension_cells = 0;
  size_t damaged_extension_cells = 0;
  for (const auto& page : survey.pages) {
    if (page.magazine != 1 && page.magazine != 2) continue;
    for (int row = 1; row < TeletextPageSnapshot::kRows; ++row) {
      if (!page.row_received[static_cast<size_t>(row)]) continue;
      for (int column = 0; column < page.columns; ++column) {
        const bool damaged =
            page.cells[static_cast<size_t>(row)][static_cast<size_t>(column)]
                .parity_error;
        size_t& cells = column < kHeadColumns ? head_cells : extension_cells;
        size_t& damaged_cells = column < kHeadColumns ? damaged_head_cells
                                                      : damaged_extension_cells;
        ++cells;
        damaged_cells += damaged ? 1 : 0;
      }
    }
  }
  // And they reach every page, not just the one being looked at. A single
  // mis-corrected address in the carrier magazine used to disable extensions
  // for every page that followed it, which shows up here and nowhere else: a
  // page that lost them still renders, just 32 columns wide.
  size_t pages_with_extension_content = 0;
  size_t pages_with_a_carrier = 0;
  for (const auto& page : survey.pages) {
    if (page.magazine != 1 && page.magazine != 2) continue;
    ++pages_with_a_carrier;
    bool any = false;
    for (int row = 0; row < TeletextPageSnapshot::kRows && !any; ++row) {
      for (int column = kHeadColumns; column < page.columns; ++column) {
        if (page.cells[static_cast<size_t>(row)][static_cast<size_t>(column)]
                .character != 0x20) {
          any = true;
          break;
        }
      }
    }
    pages_with_extension_content += any ? 1 : 0;
  }
  std::cout << "Electra: " << pages_with_extension_content << " of "
            << pages_with_a_carrier
            << " pages in a magazine with a carrier hold extension content\n";
  ASSERT_GT(pages_with_a_carrier, 20u);
  EXPECT_GT(pages_with_extension_content, pages_with_a_carrier * 3 / 4);

  ASSERT_GT(extension_cells, 1000u);
  const double head_damage =
      static_cast<double>(damaged_head_cells) / static_cast<double>(head_cells);
  const double extension_damage = static_cast<double>(damaged_extension_cells) /
                                  static_cast<double>(extension_cells);
  std::cout << "Electra: " << damaged_head_cells << "/" << head_cells
            << " display cells damaged (" << 100.0 * head_damage << "%), "
            << damaged_extension_cells << "/" << extension_cells
            << " extension cells (" << 100.0 * extension_damage << "%)\n";

  // The bar is what an unsquashed extension column reaches on this capture,
  // which is several times this; a regression that stopped the extension
  // packets voting — or attributed them to the wrong block, which is the same
  // thing seen from the other end — lands well above it.
  EXPECT_LT(extension_damage, 0.02);
}

}  // namespace
}  // namespace orc
