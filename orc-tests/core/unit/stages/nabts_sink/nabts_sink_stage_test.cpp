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
      {"tolerant_framing", false},
      {"require_valid_prefix", true},
      {"pin_data_phase", true},
      {"learn_active_lines", true},
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

  // ITU-R BT.653 §2 broadcast lines 10-21, presented 1-based.
  ASSERT_TRUE(first->constraints.default_value.has_value());
  ASSERT_TRUE(last->constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*first->constraints.default_value), 10);
  EXPECT_EQ(std::get<int32_t>(*last->constraints.default_value), 21);
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

// The grammar tie-break changes recovered record data rather than a reading of
// it, which is why it is a stage parameter and not a viewer toggle — so it has
// to reach the recovery, and it has to default to on there as well as in the
// descriptor.
TEST_F(NabtsSinkStage, Trigger_CarriesTheGrammarAssistedVoteToTheRecovery) {
  for (const bool asked : {true, false}) {
    orc::NabtsSinkStage stage{nullptr};
    auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
    orc::NabtsSinkOptions captured;
    EXPECT_CALL(*deps, init(_, _));
    EXPECT_CALL(*deps, analyse(_, _))
        .WillOnce(testing::DoAll(SaveArg<1>(&captured),
                                 Return(orc::NabtsSinkResult{})));
    stage.set_deps_override(deps);

    auto parameters = default_parameters();
    parameters["grammar_assisted_vote"] = asked;

    MockObservationContext observations;
    auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
    stage.trigger({input}, parameters, observations);
    EXPECT_EQ(captured.grammar_assisted_vote, asked);
  }

  // And a parameter set that predates it — a project saved before the stage
  // offered it — recovers as the descriptor says it should.
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkOptions captured;
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _))
      .WillOnce(testing::DoAll(SaveArg<1>(&captured),
                               Return(orc::NabtsSinkResult{})));
  stage_.set_deps_override(deps);
  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  stage_.trigger({input}, default_parameters(), observations);
  EXPECT_TRUE(captured.grammar_assisted_vote);
}

// On by default: it only ever settles positions nothing else could, and only
// where the grammar leaves one answer.
TEST_F(NabtsSinkStage, GrammarAssistedVotingIsAdvertisedAndOnByDefault) {
  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);
  const auto* found = find_descriptor(descriptors, "grammar_assisted_vote");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->type, orc::ParameterType::BOOL);
  ASSERT_TRUE(found->constraints.default_value.has_value());
  ASSERT_TRUE(std::holds_alternative<bool>(*found->constraints.default_value));
  EXPECT_TRUE(std::get<bool>(*found->constraints.default_value));
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

TEST_F(NabtsSinkStage, Trigger_ReportsTheDepsResult) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkResult result;
  result.success = true;
  result.packets_written = 1234;
  result.fields_with_data = 567;
  result.output_path = "/tmp/out.t33";
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _)).WillOnce(Return(result));
  stage_.set_deps_override(deps);

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_TRUE(stage_.trigger({input}, default_parameters(), observations));

  const std::string status = stage_.get_trigger_status();
  EXPECT_NE(status.find("1234"), std::string::npos);
  EXPECT_NE(status.find("567"), std::string::npos);
  EXPECT_NE(status.find("/tmp/out.t33"), std::string::npos);
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
  record.data = {0x0E, 0x6B, 0x7F};  // SO, then a filled rectangle
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

// Whether a damaged page is presented as recovered or as transmitted is the
// reader's to switch while looking, so the browser is offered it.
TEST_F(NabtsSinkStage, Catalogue_OffersSyntaxRepairToTheReader) {
  stage_.set_parameters(default_parameters());
  triggerWithOneRecord(stage_);

  const auto& catalogue = stage_.catalogue();
  ASSERT_EQ(catalogue.schema.toggles.size(), 1u);
  EXPECT_EQ(catalogue.schema.toggles.front().id, orc::kNabtsRepairToggleId);
  // Off unless the reader asks: what the viewer shows to begin with is what
  // the recording carried, and a reading of a damaged recording — however
  // disciplined — is this program's argument about it rather than the thing
  // itself.
  EXPECT_FALSE(catalogue.schema.toggles.front().active);
}

// The toggle is part of what the cached catalogue was built under, so switching
// it rebuilds rather than serving the other reading.
TEST_F(NabtsSinkStage, Catalogue_RebuildsWhenTheRepairToggleChanges) {
  stage_.set_parameters(default_parameters());
  triggerWithOneRecord(stage_);

  const std::vector<std::string> on{std::string(orc::kNabtsRepairToggleId)};
  const std::vector<std::string> off{};

  EXPECT_TRUE(stage_.catalogue("256 x 200", on).schema.toggles.front().active);
  EXPECT_FALSE(
      stage_.catalogue("256 x 200", off).schema.toggles.front().active);
  EXPECT_TRUE(stage_.catalogue("256 x 200", on).schema.toggles.front().active);

  // And the receiver is still the other axis: changing one keeps the other.
  const auto& fine = stage_.catalogue("512 x 400", off);
  EXPECT_EQ(fine.schema.view_option, "512 x 400");
  EXPECT_FALSE(fine.schema.toggles.front().active);
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
