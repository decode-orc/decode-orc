/*
 * File:        nabts_sink_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NABTS sink stage
 *
 * Covers: parameter descriptors and project-format filtering, parsing
 * (including the 1-based UI to 0-based field-line conversion),
 * configuration-status transitions, trigger validation and dispatch to the
 * deps seam, and execute() returning no artifacts.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_sink_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"
#include "nabts_data_group.h"
#include "nabts_sink_deps_interface_mock.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::Return;
using testing::SaveArg;

namespace orc_unit_test {
namespace {

const orc::ParameterDescriptor* find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  for (const auto& descriptor : descriptors) {
    if (descriptor.name == name) {
      return &descriptor;
    }
  }
  return nullptr;
}

std::map<std::string, orc::ParameterValue> default_parameters() {
  return {
      {"output_path", std::string("/tmp/nabts-test")},
      {"first_vbi_line", int32_t{10}},
      {"last_vbi_line", int32_t{21}},
      {"keep_empty_packets", false},
      {"detector", std::string("Automatic")},
      {"decode_threads", int32_t{0}},
      {"write_report", false},
  };
}

class NabtsSinkStage : public ::testing::Test {
 protected:
  orc::NabtsSinkStage stage_{nullptr};
};

// ---------------------------------------------------------------------------
// Node type and parameters
// ---------------------------------------------------------------------------

TEST_F(NabtsSinkStage, NodeType_IsASinkWithOneInputAndNoOutputs) {
  const auto info = stage_.get_node_type_info();
  EXPECT_EQ(info.type, orc::NodeType::SINK);
  EXPECT_EQ(info.stage_name, "nabts_sink");
  EXPECT_EQ(stage_.required_input_count(), 1u);
  EXPECT_EQ(stage_.output_count(), 0u);
}

// CEA-516 §1.1.1 specifies NABTS on the 525-line NTSC signal, so a 625-line
// project has nothing this stage can recover and is offered nothing to
// configure rather than a parameter set that cannot be used.
TEST_F(NabtsSinkStage, Descriptors_AreWithheldFromA625LineProject) {
  EXPECT_TRUE(stage_
                  .get_parameter_descriptors(orc::VideoSystem::PAL,
                                             orc::SourceType::Unknown)
                  .empty());

  for (const auto system : {orc::VideoSystem::NTSC, orc::VideoSystem::PAL_M,
                            orc::VideoSystem::Unknown}) {
    EXPECT_FALSE(
        stage_.get_parameter_descriptors(system, orc::SourceType::Unknown)
            .empty())
        << "video system " << static_cast<int>(system);
  }
}

TEST_F(NabtsSinkStage, Descriptors_DefaultToTheBt653LineWindow) {
  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Unknown);

  const auto* first = find_descriptor(descriptors, "first_vbi_line");
  const auto* last = find_descriptor(descriptors, "last_vbi_line");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(last, nullptr);

  // ITU-R BT.653 §2 broadcast lines 10-21, presented 1-based, short of line
  // 21: the caption line (47 CFR 73.682(a)(22); kNabtsLastFieldLine).
  ASSERT_TRUE(first->constraints.default_value.has_value());
  ASSERT_TRUE(last->constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*first->constraints.default_value), 10);
  EXPECT_EQ(std::get<int32_t>(*last->constraints.default_value), 20);
}

TEST_F(NabtsSinkStage, Descriptors_NameTheT33Stream) {
  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Unknown);
  const auto* path = find_descriptor(descriptors, "output_path");
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(path->file_extension_hint, ".t33");
  EXPECT_FALSE(path->constraints.required);
}

// CEA-516 §3.3 makes byte parity conditional on the data group type, so there
// is no parity-repair option to offer — nor a row squasher, which is a Level 1
// teletext idea with no System C equivalent.
TEST_F(NabtsSinkStage, Descriptors_OfferNoLevel1TeletextOptions) {
  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Unknown);
  for (const char* absent : {"repair_damaged_bytes", "squash_repeated_rows",
                             "export_subtitles", "subtitle_page"}) {
    EXPECT_EQ(find_descriptor(descriptors, absent), nullptr) << absent;
  }
}

// ---------------------------------------------------------------------------
// Configuration status
// ---------------------------------------------------------------------------

TEST_F(NabtsSinkStage, Status_IsYellowWithoutAnOutputPathAndGreenWithOne) {
  EXPECT_EQ(stage_.get_configuration_status(),
            orc::ConfigurationStatus::Yellow);

  stage_.set_parameters({{"output_path", std::string("/tmp/out.t33")}});
  EXPECT_EQ(stage_.get_configuration_status(), orc::ConfigurationStatus::Green);

  // An empty path is the report-only run, which is supported, not a fault.
  stage_.set_parameters({{"output_path", std::string("")}});
  EXPECT_EQ(stage_.get_configuration_status(),
            orc::ConfigurationStatus::Yellow);
}

// ---------------------------------------------------------------------------
// Trigger
// ---------------------------------------------------------------------------

TEST_F(NabtsSinkStage, Trigger_FailsWithNoInput) {
  MockObservationContext observations;
  EXPECT_FALSE(stage_.trigger({}, default_parameters(), observations));
  EXPECT_NE(stage_.get_trigger_status().find("No input"), std::string::npos);
  EXPECT_FALSE(stage_.is_trigger_in_progress());
}

TEST_F(NabtsSinkStage, Trigger_ConvertsTheUiLineWindowToFieldLines) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkOptions captured;
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _))
      .WillOnce(testing::DoAll(SaveArg<1>(&captured),
                               Return(orc::NabtsSinkResult{})));
  stage_.set_deps_override(deps);

  auto parameters = default_parameters();
  parameters["first_vbi_line"] = int32_t{10};
  parameters["last_vbi_line"] = int32_t{21};

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  stage_.trigger({input}, parameters, observations);

  // 1-based UI lines, 0-based field lines: the ITU-R BT.653 §2 window.
  EXPECT_EQ(captured.first_field_line, 9);
  EXPECT_EQ(captured.last_field_line, 20);
}

// A project saved while the stage still offered tolerant framing, the prefix
// switch, phase pinning, line learning and the grammar-assisted vote carries
// their keys. The stage reads
// only the parameters it declares, so the project runs as one that never set
// them did.
TEST_F(NabtsSinkStage, Trigger_IgnoresRetiredParameters) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkOptions captured;
  orc::NabtsSinkResult result;
  result.success = true;
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _))
      .WillOnce(testing::DoAll(SaveArg<1>(&captured), Return(result)));
  stage_.set_deps_override(deps);

  auto parameters = default_parameters();
  parameters["tolerant_framing"] = true;
  parameters["require_valid_prefix"] = false;
  parameters["pin_data_phase"] = false;
  parameters["learn_active_lines"] = false;
  parameters["grammar_assisted_vote"] = true;
  EXPECT_TRUE(stage_.set_parameters(parameters));

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_TRUE(stage_.trigger({input}, parameters, observations))
      << stage_.get_trigger_status();

  EXPECT_EQ(captured.first_field_line, 9);
  EXPECT_EQ(captured.last_field_line, 20);
  EXPECT_EQ(captured.detector, orc::TeletextDetector::kAuto);
  EXPECT_TRUE(captured.calibrate_bit_clock);
}

// Ignoring a retired key is not the same as dropping it silently: the stage
// names each one it finds so set_parameters() can warn about it, and nothing
// it still reads.
TEST(NabtsSinkStageRetiredParameters, NamesEachRetiredKeyAndNothingElse) {
  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")}, {"grammar_assisted_vote", true},
      {"learn_active_lines", true},        {"pin_data_phase", true},
      {"require_valid_prefix", true},      {"tolerant_framing", true}};
  std::vector<std::string> found =
      orc::NabtsSinkStage::retired_parameters_in(params);
  std::sort(found.begin(), found.end());
  EXPECT_EQ(found,
            (std::vector<std::string>{
                "grammar_assisted_vote", "learn_active_lines", "pin_data_phase",
                "require_valid_prefix", "tolerant_framing"}));
  EXPECT_TRUE(orc::NabtsSinkStage::retired_parameters_in(
                  {{"output_path", std::string("out")}})
                  .empty());
}

TEST_F(NabtsSinkStage, Trigger_RejectsAnInvertedLineWindow) {
  auto parameters = default_parameters();
  parameters["first_vbi_line"] = int32_t{18};
  parameters["last_vbi_line"] = int32_t{12};

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_FALSE(stage_.trigger({input}, parameters, observations));
  EXPECT_NE(stage_.get_trigger_status().find("Invalid VBI line window"),
            std::string::npos);
}

// The report is named after the packet stream and written beside it, so it has
// nowhere to go without one. Refused rather than dropped.
TEST_F(NabtsSinkStage, Trigger_RefusesAReportWithNoOutputFile) {
  auto parameters = default_parameters();
  parameters["output_path"] = std::string("");
  parameters["write_report"] = true;

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_FALSE(stage_.trigger({input}, parameters, observations));
  EXPECT_NE(stage_.get_trigger_status().find("needs an output file"),
            std::string::npos);
}

// The caption document is named after the packet stream too, so it is refused
// on the same grounds — an export silently not happening is the worse outcome.
TEST_F(NabtsSinkStage, Trigger_RefusesCaptionExportWithNoOutputFile) {
  auto parameters = default_parameters();
  parameters["output_path"] = std::string("");
  parameters["export_captions"] = true;

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_FALSE(stage_.trigger({input}, parameters, observations));
  EXPECT_NE(stage_.get_trigger_status().find("needs an output file"),
            std::string::npos);
}

// Every parameter the stage advertises has to be readable back, or the GUI
// dialogue would drop it on save.
TEST_F(NabtsSinkStage, ExportCaptionsIsAdvertisedAsAParameter) {
  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);
  const auto found = std::find_if(descriptors.begin(), descriptors.end(),
                                  [](const orc::ParameterDescriptor& d) {
                                    return d.name == "export_captions";
                                  });
  ASSERT_NE(found, descriptors.end());
  EXPECT_EQ(found->type, orc::ParameterType::BOOL);
  ASSERT_TRUE(found->constraints.default_value.has_value());
  ASSERT_TRUE(std::holds_alternative<bool>(*found->constraints.default_value));
  EXPECT_FALSE(std::get<bool>(*found->constraints.default_value));
}

// Reconstruction draws on other recordings' packet streams, which the stage
// is told of as a list the GUI can hold in one text field.
TEST_F(NabtsSinkStage, ReferenceRecordingsAreAdvertisedAsAParameter) {
  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);
  const auto found = std::find_if(descriptors.begin(), descriptors.end(),
                                  [](const orc::ParameterDescriptor& d) {
                                    return d.name == "reference_records";
                                  });
  ASSERT_NE(found, descriptors.end());
  EXPECT_EQ(found->type, orc::ParameterType::STRING);
  ASSERT_TRUE(found->constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*found->constraints.default_value), "");
}

TEST_F(NabtsSinkStage, Trigger_PassesEachReferenceRecordingToTheDeps) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkOptions captured;
  orc::NabtsSinkResult result;
  result.success = true;
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _))
      .WillOnce(testing::DoAll(SaveArg<1>(&captured), Return(result)));
  stage_.set_deps_override(deps);

  auto parameters = default_parameters();
  parameters["reference_records"] =
      std::string(" /captures/first.t33 ;;\n/captures/second \n");
  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_TRUE(stage_.trigger({input}, parameters, observations));
  EXPECT_EQ(
      captured.reference_records,
      (std::vector<std::string>{"/captures/first.t33", "/captures/second"}));
}

TEST_F(NabtsSinkStage, Trigger_ReportsTheDepsResult) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkResult result;
  result.success = true;
  result.packets_written = 1234;
  result.fields_with_data = 567;
  result.output_path = "/tmp/out.t33";
  result.message =
      "Recovered 1234 NABTS packets (567 fields with data) to /tmp/out.t33";
  result.lost_packets_estimate = 89;
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _)).WillOnce(Return(result));
  stage_.set_deps_override(deps);

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_TRUE(stage_.trigger({input}, default_parameters(), observations));

  EXPECT_EQ(stage_.get_trigger_status(),
            result.message + "; 89 packets estimated lost");
}

TEST_F(NabtsSinkStage, Trigger_ReportsAFailedRun) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkResult result;
  result.success = false;
  result.message = "Input carries no NABTS service";
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _)).WillOnce(Return(result));
  stage_.set_deps_override(deps);

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_FALSE(stage_.trigger({input}, default_parameters(), observations));
  EXPECT_NE(stage_.get_trigger_status().find("no NABTS service"),
            std::string::npos);
}

// A run that found no records has still run. Saying it has no results sends the
// reader back to trigger it again; saying what the recording carried instead —
// here, another service's data on a channel of its own — is the answer.
TEST_F(NabtsSinkStage, Trigger_AnEmptyCatalogueIsAResultThatSaysWhy) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkResult result;
  result.success = true;
  result.dataset.summary.frames_analysed = 5857;
  result.dataset.summary.foreign_groups.push_back(
      {0xCFD, orc::kNabtsPrivateGroupType, 34});
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _)).WillOnce(Return(result));
  stage_.set_deps_override(deps);
  stage_.set_parameters(default_parameters());

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  ASSERT_TRUE(stage_.trigger({input}, default_parameters(), observations));
  ASSERT_TRUE(stage_.has_results());

  const auto& catalogue = stage_.catalogue();
  EXPECT_TRUE(catalogue.items.empty());
  EXPECT_NE(catalogue.schema.empty_message.find("another service"),
            std::string::npos);
  ASSERT_EQ(catalogue.summary.notices.size(), 1u);
  const std::string& notice = catalogue.summary.notices.front();
  EXPECT_NE(notice.find("Channel CFD carried 34 data groups of type 15"),
            std::string::npos)
      << notice;
  EXPECT_NE(notice.find("private use"), std::string::npos) << notice;
}

// A recording whose NABTS is all datacast catalogues no
// record, and the list says what its addresses carried instead.
TEST_F(NabtsSinkStage, Trigger_ARecordingOfOtherServicesSaysWhatTheyAre) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkResult result;
  result.success = true;
  result.dataset.summary.frames_analysed = 54767;
  orc::NabtsOtherService datacast;
  datacast.kind = orc::NabtsOtherService::Kind::kFecDatacast;
  datacast.channels = "0F0-0FF";
  datacast.packets = 159553;
  datacast.description = "16 data rows to each check row";
  orc::NabtsOtherService clock;
  clock.kind = orc::NabtsOtherService::Kind::kClock;
  clock.channels = "EF0, FC0";
  clock.packets = 3874;
  clock.description = "Hamming-coded time of day";
  result.dataset.summary.other_services = {datacast, clock};
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _)).WillOnce(Return(result));
  stage_.set_deps_override(deps);
  stage_.set_parameters(default_parameters());

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  ASSERT_TRUE(stage_.trigger({input}, default_parameters(), observations));
  ASSERT_TRUE(stage_.has_results());

  const auto& catalogue = stage_.catalogue();
  EXPECT_TRUE(catalogue.items.empty());
  EXPECT_NE(catalogue.schema.empty_message.find("another service"),
            std::string::npos);
  ASSERT_EQ(catalogue.summary.notices.size(), 2u);
  EXPECT_NE(catalogue.summary.notices[0].find(
                "Channels 0F0-0FF carried an FEC datacast"),
            std::string::npos)
      << catalogue.summary.notices[0];
  EXPECT_NE(catalogue.summary.notices[0].find("159553 packets"),
            std::string::npos)
      << catalogue.summary.notices[0];
  EXPECT_NE(catalogue.summary.notices[1].find(
                "Channels EF0, FC0 carried clock packets"),
            std::string::npos)
      << catalogue.summary.notices[1];
}

// ---------------------------------------------------------------------------
// The catalogue and the receiver it is drawn for
// ---------------------------------------------------------------------------

/// Trigger |stage| with a result carrying one presentation record, so it has a
/// catalogue to serve. The record's data draws a filled rectangle, which is
/// enough for a display list either mode can emit.
void triggerWithOneRecord(orc::NabtsSinkStage& stage,
                          const std::map<std::string, orc::ParameterValue>&
                              parameters = default_parameters()) {
  orc::NabtsSinkResult result;
  result.success = true;
  orc::NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address_text = "000";
  record.record_type = 1;  // §5.2.2.3 non-cyclic presentation
  record.times_seen = 1;
  record.times_intact = 1;
  record.complete = true;
  // A letter: a record that draws nothing is named rather than drawn.
  record.data = {0xC1};
  result.dataset.records.push_back(record);

  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _)).WillOnce(Return(result));
  stage.set_deps_override(deps);

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  ASSERT_TRUE(stage.trigger({input}, parameters, observations));
  ASSERT_TRUE(stage.has_results());
}

// Asked for nothing in particular, the stage draws for the receiver X3.110
// Table D1 requires — what a set-top decoder of the period put on screen. There
// is no parameter behind this: which receiver a page is drawn for changes
// nothing the recovery found, so it is a way of looking rather than a setting.
TEST_F(NabtsSinkStage, Catalogue_DrawsForTheReferenceReceiverByDefault) {
  stage_.set_parameters(default_parameters());
  triggerWithOneRecord(stage_);

  EXPECT_EQ(stage_.catalogue().schema.view_option, "256 x 200");
  EXPECT_EQ(stage_.catalogue().payloads[0].display_list.nominal_width, 256);

  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);
  EXPECT_EQ(find_descriptor(descriptors, "render_resolution"), nullptr)
      << "the receiver is offered as a parameter, so changing it costs a "
         "re-run of a recovery pass that cannot see it";
}

// A reader who picks a receiver in the viewer gets it, and the records are not
// read again to give it to them: the same trigger's data is drawn a second way.
TEST_F(NabtsSinkStage, Catalogue_DrawsForTheReceiverTheViewerAsksFor) {
  stage_.set_parameters(default_parameters());
  triggerWithOneRecord(stage_);

  const auto& fine = stage_.catalogue("512 x 400");
  EXPECT_EQ(fine.schema.view_option, "512 x 400");
  ASSERT_FALSE(fine.payloads.empty());
  EXPECT_EQ(fine.payloads[0].display_list.nominal_width, 512);

  // And back again, which is what comparing two receivers amounts to.
  const auto& coarse = stage_.catalogue("256 x 200");
  EXPECT_EQ(coarse.schema.view_option, "256 x 200");
  EXPECT_EQ(coarse.payloads[0].display_list.nominal_width, 256);
}

// An option the stage does not know is not worth refusing to draw over: the
// host round-trips what a schema gave it, and anything else means "the
// receiver the viewer opens on".
TEST_F(NabtsSinkStage, Catalogue_FallsBackToTheReferenceForAnUnknownOption) {
  stage_.set_parameters(default_parameters());
  triggerWithOneRecord(stage_);

  EXPECT_EQ(stage_.catalogue("144p").schema.view_option, "256 x 200");
  EXPECT_EQ(stage_.catalogue(std::string()).schema.view_option, "256 x 200");
}

// Whether a damaged page is presented with its lost bytes rebuilt is the
// reader's to switch while looking, so the browser is offered it — off to begin
// with, because what the viewer shows first should be what the recording
// carried. The toggle is part of what the cached catalogue was built under, so
// switching it rebuilds rather than serving the other reading.
// Which receiver paces the pages is the reader's to switch while looking:
// Receiver pace plays the Norpak TTX 650's, TU-2000x pace the Panasonic
// TU-2000x's, with or without Receiver pace. Each box reports what the reader
// set, including when two requests come to the same receiver and the pages
// already built are served again.
TEST_F(NabtsSinkStage, Catalogue_OffersBothReceiversPaces) {
  stage_.set_parameters(default_parameters());
  triggerWithOneRecord(stage_);

  const auto& toggles = stage_.catalogue().schema.toggles;
  ASSERT_EQ(toggles.size(), 3u);
  EXPECT_EQ(toggles[1].id, orc::kNabtsReceiverPaceToggleId);
  EXPECT_EQ(toggles[2].id, orc::kNabtsTu2000xPaceToggleId);
  EXPECT_FALSE(toggles[1].active);
  EXPECT_FALSE(toggles[2].active);

  const std::string pace(orc::kNabtsReceiverPaceToggleId);
  const std::string tu(orc::kNabtsTu2000xPaceToggleId);
  const auto& alone = stage_.catalogue("256 x 200", {tu}).schema.toggles;
  EXPECT_FALSE(alone[1].active);
  EXPECT_TRUE(alone[2].active);
  const auto& both = stage_.catalogue("256 x 200", {pace, tu}).schema.toggles;
  EXPECT_TRUE(both[1].active);
  EXPECT_TRUE(both[2].active);
  const auto& ttx = stage_.catalogue("256 x 200", {pace}).schema.toggles;
  EXPECT_TRUE(ttx[1].active);
  EXPECT_FALSE(ttx[2].active);
}

TEST_F(NabtsSinkStage, Catalogue_RebuildsWhenTheReconstructToggleChanges) {
  stage_.set_parameters(default_parameters());
  triggerWithOneRecord(stage_);

  ASSERT_EQ(stage_.catalogue().schema.toggles.size(), 3u);
  EXPECT_EQ(stage_.catalogue().schema.toggles.front().id,
            orc::kNabtsReconstructToggleId);
  EXPECT_FALSE(stage_.catalogue().schema.toggles.front().active);

  const std::vector<std::string> on{
      std::string(orc::kNabtsReconstructToggleId)};
  const std::vector<std::string> off{};
  EXPECT_TRUE(stage_.catalogue("256 x 200", on).schema.toggles.front().active);
  EXPECT_FALSE(
      stage_.catalogue("256 x 200", off).schema.toggles.front().active);
  EXPECT_TRUE(stage_.catalogue("256 x 200", on).schema.toggles.front().active);

  // And the receiver is still the other axis: changing one keeps the other.
  const auto& fine = stage_.catalogue("512 x 400", on);
  EXPECT_EQ(fine.schema.view_option, "512 x 400");
  EXPECT_TRUE(fine.schema.toggles.front().active);
}

// The receiver's pace is the other toggle, keyed into the cache the same way:
// the two switch independently, and each rebuild carries both.
TEST_F(NabtsSinkStage, Catalogue_RebuildsWhenTheReceiverPaceToggleChanges) {
  stage_.set_parameters(default_parameters());
  triggerWithOneRecord(stage_);

  const auto pace_active = [](const orc::CatalogueDataset& catalogue) {
    for (const auto& toggle : catalogue.schema.toggles) {
      if (toggle.id == orc::kNabtsReceiverPaceToggleId) {
        return toggle.active;
      }
    }
    ADD_FAILURE() << "no receiver pace toggle";
    return false;
  };
  EXPECT_FALSE(pace_active(stage_.catalogue()));

  const std::vector<std::string> pace{
      std::string(orc::kNabtsReceiverPaceToggleId)};
  const std::vector<std::string> both{
      std::string(orc::kNabtsReconstructToggleId),
      std::string(orc::kNabtsReceiverPaceToggleId)};
  EXPECT_TRUE(pace_active(stage_.catalogue("256 x 200", pace)));
  EXPECT_FALSE(
      stage_.catalogue("256 x 200", pace).schema.toggles.front().active);
  const auto& paced = stage_.catalogue("256 x 200", both);
  EXPECT_TRUE(pace_active(paced));
  EXPECT_TRUE(paced.schema.toggles.front().active);
  EXPECT_FALSE(pace_active(stage_.catalogue("256 x 200", {})));
}

// A toggle id the stage does not know is not worth refusing to draw over, for
// the reason an unknown view option is not: the host round-trips what a schema
// gave it.
TEST_F(NabtsSinkStage, Catalogue_TreatsAnUnknownToggleAsOff) {
  stage_.set_parameters(default_parameters());
  triggerWithOneRecord(stage_);

  const auto& catalogue =
      stage_.catalogue("256 x 200", {std::string("something_else")});
  EXPECT_FALSE(catalogue.schema.toggles.front().active);
}

// ---------------------------------------------------------------------------
// execute()
// ---------------------------------------------------------------------------

TEST_F(NabtsSinkStage, Execute_ProducesNoArtifacts) {
  orc::ObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_TRUE(
      stage_.execute({input}, default_parameters(), observations).empty());
}

}  // namespace
}  // namespace orc_unit_test
