/*
 * File:        teletext_sink_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the teletext sink stage
 *
 * Covers: parameter descriptors and parsing (including the 1-based UI to
 * 0-based field-line conversion), configuration-status transitions, trigger
 * validation and dispatch to the deps seam, and execute() returning no
 * artifacts.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_sink_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/logging.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"
#include "teletext_sink_deps_interface_mock.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::Ref;
using testing::Return;
using testing::SaveArg;
using testing::StrictMock;

// using different namespace from module-under-test so that we can use the same
// class names in the tests as in the module-under-test
namespace orc_unit_test {

// Captures what the stage logs while it is alive, so a diagnostic that exists
// only as a log line can be asserted at the boundary it is emitted from. No
// file or console I/O: the sink writes into an in-memory stream.
class LogCapture {
 public:
  LogCapture() : logger_(orc::get_logger()) {
    sink_ = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream_);
    previous_level_ = logger_->level();
    logger_->sinks().push_back(sink_);
    logger_->set_level(spdlog::level::trace);
  }

  ~LogCapture() {
    auto& sinks = logger_->sinks();
    sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
    logger_->set_level(previous_level_);
  }

  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;

  std::string text() const { return stream_.str(); }

 private:
  std::ostringstream stream_;
  std::shared_ptr<spdlog::logger> logger_;
  std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink_;
  spdlog::level::level_enum previous_level_{spdlog::level::info};
};

class TeletextSinkStage : public ::testing::Test {
 public:
  void SetUp() override {
    pMockDeps_ = std::make_shared<StrictMock<MockTeletextSinkStageDeps>>();
    pMockRepresentation_ =
        std::make_shared<StrictMock<MockVideoFrameRepresentationArtifact>>();

    instance_ = std::make_unique<orc::TeletextSinkStage>(
        static_cast<orc::IStageServices*>(nullptr));
  }

  void TearDown() override { instance_.reset(); }

 protected:
  std::vector<orc::ArtifactPtr> make_valid_input() const {
    return {std::static_pointer_cast<orc::Artifact>(pMockRepresentation_)};
  }

  // Expect a dispatch to the deps seam and capture the options it receives.
  void expect_export(const orc::TeletextSinkResult& result,
                     orc::TeletextSinkOptions& captured_options) {
    instance_->set_deps_override(pMockDeps_);
    EXPECT_CALL(*pMockDeps_, init(_, _)).Times(1);
    EXPECT_CALL(*pMockDeps_, analyse(pMockRepresentation_.get(), _))
        .Times(1)
        .WillOnce(
            testing::DoAll(SaveArg<1>(&captured_options), Return(result)));
  }

  std::shared_ptr<StrictMock<MockTeletextSinkStageDeps>> pMockDeps_;
  std::shared_ptr<StrictMock<MockVideoFrameRepresentationArtifact>>
      pMockRepresentation_;
  MockObservationContext mockObservationContext_;

  std::unique_ptr<orc::TeletextSinkStage> instance_;
};

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkStage, NodeTypeInfo_MatchesDesign) {
  const auto info = instance_->get_node_type_info();

  // A sink rather than an analysis sink: the packet stream is the product,
  // and the page catalogue rides along on a batch-analysis stage tool.
  EXPECT_EQ(info.type, orc::NodeType::SINK);
  EXPECT_EQ(orc::stage_category_for(info.type), orc::StageCategory::SINK);
  EXPECT_EQ(info.stage_name, "teletext_sink");
  EXPECT_EQ(info.display_name, "Teletext Sink");
  EXPECT_EQ(info.min_inputs, 1u);
  EXPECT_EQ(info.max_inputs, 1u);
  EXPECT_EQ(info.min_outputs, 0u);
  EXPECT_EQ(info.max_outputs, 0u);
  // ITU-R BT.653 System B is defined on 625- and 525-line systems alike, which
  // between them are every system the project models.
  EXPECT_EQ(info.compatible_formats, orc::VideoFormatCompatibility::ALL);
}

// The host routes the viewer on the tool kind and the contract string alone —
// it knows nothing about teletext — so both must name the generic catalogue
// browser.
TEST_F(TeletextSinkStage, StageTools_AdvertiseTheCatalogueContract) {
  const auto tools = instance_->get_stage_tools();
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].tool_id, "teletext_analysis");
  EXPECT_EQ(tools[0].kind, orc::StageToolKind::CatalogueBrowser);
  EXPECT_EQ(tools[0].contract_id, orc::kCatalogueBrowserContractId);
}

// The host discovers the viewer through StageToolProvider and reads it through
// ICatalogueResults; AnalysisToolProvider is the other, unrelated seam and must
// not resolve.
TEST_F(TeletextSinkStage, Mixins_ExposeStageToolProviderOnly) {
  EXPECT_NE(dynamic_cast<orc::StageToolProvider*>(instance_.get()), nullptr);
  EXPECT_NE(dynamic_cast<orc::ICatalogueResults*>(instance_.get()), nullptr);
  EXPECT_EQ(dynamic_cast<orc::AnalysisToolProvider*>(instance_.get()), nullptr);
}

// An untriggered stage has an empty catalogue rather than no catalogue: the
// host asks for one whenever a viewer opens, and a null would be a crash.
TEST_F(TeletextSinkStage, Catalogue_IsEmptyUntilTriggered) {
  const auto& catalogue = instance_->catalogue();
  EXPECT_TRUE(catalogue.items.empty());
  EXPECT_TRUE(catalogue.consistent());
  // The schema is a property of the stage, not of the run, so it is filled in
  // even with nothing to show — that is what labels the empty viewer.
  EXPECT_FALSE(catalogue.schema.columns.empty());
  EXPECT_EQ(catalogue.schema.item_noun, "Page");
}

TEST_F(TeletextSinkStage, Results_AreEmptyUntilTriggered) {
  EXPECT_FALSE(instance_->has_results());
  EXPECT_TRUE(instance_->dataset().pages.empty());
}

TEST_F(TeletextSinkStage, ParameterDescriptors_MatchSpecTable) {
  const auto descriptors = instance_->get_parameter_descriptors();
  ASSERT_EQ(descriptors.size(), 19u);

  EXPECT_EQ(descriptors[0].name, "output_path");
  EXPECT_EQ(descriptors[0].type, orc::ParameterType::FILE_PATH);
  // Optional: an empty path is the browse-only run (see the trigger tests).
  EXPECT_FALSE(descriptors[0].constraints.required);
  EXPECT_EQ(descriptors[0].file_extension_hint, ".t42");

  EXPECT_EQ(descriptors[1].name, "first_vbi_line");
  EXPECT_EQ(descriptors[1].type, orc::ParameterType::INT32);
  ASSERT_TRUE(descriptors[1].constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*descriptors[1].constraints.default_value), 6);

  EXPECT_EQ(descriptors[2].name, "last_vbi_line");
  EXPECT_EQ(descriptors[2].type, orc::ParameterType::INT32);
  ASSERT_TRUE(descriptors[2].constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*descriptors[2].constraints.default_value), 22);

  EXPECT_EQ(descriptors[3].name, "keep_empty_packets");
  EXPECT_EQ(descriptors[3].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[3].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[3].constraints.default_value), false);

  EXPECT_EQ(descriptors[4].name, "detector");
  EXPECT_EQ(descriptors[4].type, orc::ParameterType::STRING);
  EXPECT_EQ(descriptors[4].constraints.allowed_strings,
            (std::vector<std::string>{"Automatic", "Threshold", "MLSE"}));
  ASSERT_TRUE(descriptors[4].constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*descriptors[4].constraints.default_value),
            "Automatic");

  EXPECT_EQ(descriptors[5].name, "character_set");
  EXPECT_EQ(descriptors[5].type, orc::ParameterType::STRING);
  // Latin first because it is the default and the overwhelmingly common case,
  // then the Cyrillic sets in the order material for them is likely to turn up.
  EXPECT_EQ(descriptors[5].constraints.allowed_strings,
            (std::vector<std::string>{"Latin", "Cyrillic (Russian/Bulgarian)",
                                      "Cyrillic (Ukrainian)",
                                      "Cyrillic (Serbian/Croatian)"}));
  ASSERT_TRUE(descriptors[5].constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*descriptors[5].constraints.default_value),
            "Latin");

  // The set the ESC control character switches into, offered in the same order
  // behind the "None" that keeps a page in one alphabet — the default, and the
  // only value that leaves ESC doing nothing.
  EXPECT_EQ(descriptors[6].name, "second_character_set");
  EXPECT_EQ(descriptors[6].type, orc::ParameterType::STRING);
  EXPECT_EQ(descriptors[6].constraints.allowed_strings,
            (std::vector<std::string>{
                "None", "Latin", "Cyrillic (Russian/Bulgarian)",
                "Cyrillic (Ukrainian)", "Cyrillic (Serbian/Croatian)"}));
  ASSERT_TRUE(descriptors[6].constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*descriptors[6].constraints.default_value),
            "None");

  EXPECT_EQ(descriptors[7].name, "tolerant_framing");
  EXPECT_EQ(descriptors[7].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[7].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[7].constraints.default_value), false);

  EXPECT_EQ(descriptors[8].name, "require_valid_mrag");
  EXPECT_EQ(descriptors[8].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[8].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[8].constraints.default_value), true);

  EXPECT_EQ(descriptors[9].name, "repair_damaged_bytes");
  EXPECT_EQ(descriptors[9].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[9].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[9].constraints.default_value), true);
  ASSERT_TRUE(descriptors[9].constraints.depends_on.has_value());
  EXPECT_EQ(descriptors[9].constraints.depends_on->parameter_name, "detector");

  EXPECT_EQ(descriptors[10].name, "pin_data_phase");
  EXPECT_EQ(descriptors[10].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[10].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[10].constraints.default_value), true);

  EXPECT_EQ(descriptors[11].name, "calibrate_bit_clock");
  EXPECT_EQ(descriptors[11].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[11].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[11].constraints.default_value), true);

  EXPECT_EQ(descriptors[12].name, "learn_active_lines");
  EXPECT_EQ(descriptors[12].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[12].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[12].constraints.default_value), true);

  EXPECT_EQ(descriptors[13].name, "decode_threads");
  EXPECT_EQ(descriptors[13].type, orc::ParameterType::INT32);
  ASSERT_TRUE(descriptors[13].constraints.default_value.has_value());
  // 0 is "one per processor"; the recovered stream does not depend on it.
  EXPECT_EQ(std::get<int32_t>(*descriptors[13].constraints.default_value), 0);
  ASSERT_TRUE(descriptors[13].constraints.min_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*descriptors[13].constraints.min_value), 0);

  EXPECT_EQ(descriptors[14].name, "squash_repeated_rows");
  EXPECT_EQ(descriptors[14].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[14].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[14].constraints.default_value), true);

  EXPECT_EQ(descriptors[15].name, "write_report");
  EXPECT_EQ(descriptors[15].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[15].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[15].constraints.default_value), false);

  EXPECT_EQ(descriptors[16].name, "export_subtitles");
  EXPECT_EQ(descriptors[16].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[16].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[16].constraints.default_value), false);

  EXPECT_EQ(descriptors[17].name, "subtitle_page");
  EXPECT_EQ(descriptors[17].type, orc::ParameterType::STRING);
  ASSERT_TRUE(descriptors[17].constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*descriptors[17].constraints.default_value),
            "888");
  ASSERT_TRUE(descriptors[17].constraints.depends_on.has_value());
  EXPECT_EQ(descriptors[17].constraints.depends_on->parameter_name,
            "export_subtitles");

  EXPECT_EQ(descriptors[18].name, "subtitle_format");
  EXPECT_EQ(descriptors[18].type, orc::ParameterType::STRING);
  ASSERT_EQ(descriptors[18].constraints.allowed_strings.size(), 1u);
  EXPECT_EQ(descriptors[18].constraints.allowed_strings[0], "SRT");
  ASSERT_TRUE(descriptors[18].constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*descriptors[18].constraints.default_value),
            "SRT");
  ASSERT_TRUE(descriptors[18].constraints.depends_on.has_value());
  EXPECT_EQ(descriptors[18].constraints.depends_on->parameter_name,
            "export_subtitles");
}

// Without a path the stage still runs and still fills the viewer, so it is the
// reduced behaviour Yellow stands for rather than Red's missing requirement.
TEST_F(TeletextSinkStage, ConfigurationStatus_YellowWithoutOutputPath) {
  EXPECT_EQ(instance_->get_configuration_status(),
            orc::ConfigurationStatus::Yellow);

  EXPECT_TRUE(
      instance_->set_parameters({{"output_path", std::string("out.t42")}}));
  EXPECT_EQ(instance_->get_configuration_status(),
            orc::ConfigurationStatus::Green);

  EXPECT_TRUE(instance_->set_parameters({{"output_path", std::string("")}}));
  EXPECT_EQ(instance_->get_configuration_status(),
            orc::ConfigurationStatus::Yellow);
}

TEST_F(TeletextSinkStage, SetParameters_RoundTripsValues) {
  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out.t42")},
      {"first_vbi_line", int32_t{7}},
      {"keep_empty_packets", true}};

  EXPECT_TRUE(instance_->set_parameters(params));
  EXPECT_EQ(instance_->get_parameters(), params);
}

TEST_F(TeletextSinkStage, Execute_ReturnsNoArtifacts) {
  orc::ObservationContext context;
  EXPECT_TRUE(instance_->execute(make_valid_input(), {}, context).empty());
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkStage, Trigger_ReturnsFalseWhenNoInputConnected) {
  const bool result = instance_->trigger(
      {}, {{"output_path", std::string("out")}}, mockObservationContext_);

  EXPECT_FALSE(result);
  EXPECT_EQ(instance_->get_trigger_status(), "Error: No input connected");
  EXPECT_FALSE(instance_->is_trigger_in_progress());
}

TEST_F(TeletextSinkStage, Trigger_ReturnsFalseWhenInputNotRepresentation) {
  const std::vector<orc::ArtifactPtr> inputs = {nullptr};

  const bool result = instance_->trigger(
      inputs, {{"output_path", std::string("out")}}, mockObservationContext_);

  EXPECT_FALSE(result);
  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: Input is not a video frame representation");
}

// Browsing the pages is a reason to trigger the stage on its own, so no output
// path means "decode, catalogue, write nothing" rather than a failed run.
TEST_F(TeletextSinkStage, Trigger_RunsBrowseOnlyWhenOutputPathMissing) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.packets_written = 84;
  deps_result.fields_with_data = 2;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(
      instance_->trigger(make_valid_input(), {}, mockObservationContext_));

  EXPECT_TRUE(captured.output_path.empty());
  EXPECT_EQ(instance_->get_trigger_status(),
            "Recovered 84 teletext packets (2 fields with data); no packet "
            "stream written (no output file set); 0 pages");
}

TEST_F(TeletextSinkStage, Trigger_RunsBrowseOnlyWhenOutputPathEmpty) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("")}},
                                 mockObservationContext_));

  EXPECT_TRUE(captured.output_path.empty());
}

// The report and the subtitle document are both named after the packet stream
// and written beside it, so neither can be honoured without one. Refused up
// front rather than silently dropped.
TEST_F(TeletextSinkStage, Trigger_RejectsFileExportsWithoutOutputPath) {
  EXPECT_FALSE(instance_->trigger(
      make_valid_input(),
      {{"output_path", std::string("")}, {"export_subtitles", true}},
      mockObservationContext_));
  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: Subtitle export needs an output file (the cues are "
            "written beside the packet stream)");

  EXPECT_FALSE(instance_->trigger(make_valid_input(), {{"write_report", true}},
                                  mockObservationContext_));
  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: The report file needs an output file (it is written "
            "beside the packet stream)");
}

TEST_F(TeletextSinkStage, Trigger_RejectsInvalidLineWindows) {
  const std::vector<std::map<std::string, orc::ParameterValue>> bad_params = {
      // first > last
      {{"output_path", std::string("out")},
       {"first_vbi_line", int32_t{10}},
       {"last_vbi_line", int32_t{5}}},
      // below the 1-based window
      {{"output_path", std::string("out")}, {"first_vbi_line", int32_t{0}}},
      // above the EN 300 706 §4.1 window (1-based field line 22)
      {{"output_path", std::string("out")}, {"last_vbi_line", int32_t{23}}},
  };

  for (const auto& params : bad_params) {
    const bool result =
        instance_->trigger(make_valid_input(), params, mockObservationContext_);
    EXPECT_FALSE(result);
    EXPECT_EQ(instance_->get_trigger_status(),
              "Error: Invalid VBI line window");
  }
}

TEST_F(TeletextSinkStage, Trigger_ConvertsUiLinesToZeroBasedFieldLines) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")}, {"first_vbi_line", int32_t{7}},
      {"last_vbi_line", int32_t{20}},      {"keep_empty_packets", true},
      {"tolerant_framing", true},          {"require_valid_mrag", false}};

  EXPECT_TRUE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));

  EXPECT_EQ(captured.output_path, "out");
  EXPECT_EQ(captured.first_field_line, 6);  // 1-based 7 → 0-based 6
  EXPECT_EQ(captured.last_field_line, 19);  // 1-based 20 → 0-based 19
  EXPECT_TRUE(captured.keep_empty_packets);
  EXPECT_TRUE(captured.tolerant_framing);
  EXPECT_FALSE(captured.require_valid_mrag);
}

TEST_F(TeletextSinkStage, Trigger_PassesParityRepairChoiceToDeps) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  instance_->trigger(
      make_valid_input(),
      {{"output_path", std::string("out")}, {"repair_damaged_bytes", false}},
      mockObservationContext_);

  EXPECT_FALSE(captured.parity_repair);
}

TEST_F(TeletextSinkStage, Trigger_UsesSpecDefaultsWhenParametersAbsent) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  // EN 300 706 §4.1 window: 1-based 6-22 → 0-based field lines 5-21.
  EXPECT_EQ(captured.first_field_line, 5);
  EXPECT_EQ(captured.last_field_line, 21);
  EXPECT_FALSE(captured.keep_empty_packets);
  EXPECT_FALSE(captured.tolerant_framing);
  EXPECT_TRUE(captured.require_valid_mrag);
  EXPECT_TRUE(captured.parity_repair);
  EXPECT_EQ(captured.detector, orc::TeletextDetector::kAuto);
}

TEST_F(TeletextSinkStage, Trigger_PassesDetectorChoiceToDeps) {
  for (const auto& [name, detector] :
       std::vector<std::pair<std::string, orc::TeletextDetector>>{
           {"Automatic", orc::TeletextDetector::kAuto},
           {"Threshold", orc::TeletextDetector::kThreshold},
           {"MLSE", orc::TeletextDetector::kMlse}}) {
    orc::TeletextSinkResult deps_result;
    deps_result.success = true;
    deps_result.output_path = "out.t42";
    orc::TeletextSinkOptions captured;
    expect_export(deps_result, captured);

    EXPECT_TRUE(instance_->trigger(
        make_valid_input(),
        {{"output_path", std::string("out")}, {"detector", name}},
        mockObservationContext_));
    EXPECT_EQ(captured.detector, detector) << name;
  }
}

TEST_F(TeletextSinkStage, Trigger_RejectsUnknownDetector) {
  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")}, {"detector", std::string("Magic")}};

  EXPECT_FALSE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));
  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: Unknown bit detector: Magic");
}

TEST_F(TeletextSinkStage, Trigger_ReportsCountsFromDepsResult) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  deps_result.packets_written = 84;
  deps_result.fields_with_data = 2;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  EXPECT_EQ(instance_->get_trigger_status(),
            "Recovered 84 teletext packets (2 fields with data) to out.t42; "
            "0 pages");
  EXPECT_FALSE(instance_->is_trigger_in_progress());
}

TEST_F(TeletextSinkStage, Trigger_ReportsRepairedBytesFromDepsResult) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  deps_result.packets_written = 84;
  deps_result.fields_with_data = 2;
  deps_result.bytes_repaired = 17;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(
      make_valid_input(),
      {{"output_path", std::string("out")}, {"repair_damaged_bytes", true}},
      mockObservationContext_));

  EXPECT_EQ(instance_->get_trigger_status(),
            "Recovered 84 teletext packets (2 fields with data) to out.t42; "
            "0 pages; repaired 17 damaged bytes");
}

TEST_F(TeletextSinkStage, Trigger_PassesSubtitleOptionsToDeps) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")},
      {"export_subtitles", true},
      {"subtitle_page", std::string("150")},
      {"subtitle_format", std::string("SRT")}};

  EXPECT_TRUE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));

  EXPECT_TRUE(captured.export_subtitles);
  EXPECT_EQ(captured.subtitle_page, "150");
}

TEST_F(TeletextSinkStage, Trigger_SubtitleExportDisabledByDefault) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  EXPECT_FALSE(captured.export_subtitles);
}

TEST_F(TeletextSinkStage, Trigger_RejectsMalformedSubtitlePage) {
  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")},
      {"export_subtitles", true},
      {"subtitle_page", std::string("98X")}};

  EXPECT_FALSE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));
  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: Invalid subtitle page \"98X\" (expected magazine digit "
            "1-8 plus two hex digits, e.g. 888)");
}

TEST_F(TeletextSinkStage, Trigger_RejectsUnsupportedSubtitleFormat) {
  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")},
      {"export_subtitles", true},
      {"subtitle_format", std::string("VTT")}};

  EXPECT_FALSE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));
  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: Unsupported subtitle format: VTT");
}

TEST_F(TeletextSinkStage, Trigger_ReportsSubtitleCountsFromDepsResult) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  deps_result.packets_written = 84;
  deps_result.fields_with_data = 2;
  deps_result.subtitle_path = "out.srt";
  deps_result.subtitle_cues_written = 3;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")}, {"export_subtitles", true}};

  EXPECT_TRUE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));

  EXPECT_EQ(instance_->get_trigger_status(),
            "Recovered 84 teletext packets (2 fields with data) to out.t42; "
            "0 pages; 3 subtitle cues to out.srt");
}

TEST_F(TeletextSinkStage, Trigger_EmitsTheReportFromDeps) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  deps_result.report =
      "Teletext recovery: 12 candidate lines, 4 with a data burst";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  LogCapture log;
  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  EXPECT_NE(log.text().find(deps_result.report), std::string::npos)
      << log.text();
  // The report is a log-level diagnostic; the user-facing status is unchanged.
  EXPECT_EQ(instance_->get_trigger_status().find("candidate lines"),
            std::string::npos);
}

TEST_F(TeletextSinkStage, Trigger_EmitsNothingWhenDepsReportNoProfile) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  LogCapture log;
  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  EXPECT_EQ(log.text().find("Teletext recovery"), std::string::npos)
      << log.text();
}

// The report file is the sink's own product, so the path it lands at is worth
// saying in the status: a reader who asked for it needs to know where it went.
TEST_F(TeletextSinkStage, Trigger_ReportsTheSquashAndReportPathInTheStatus) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  deps_result.packets_written = 84;
  deps_result.fields_with_data = 2;
  deps_result.packets_corrected = 31;
  deps_result.report_path = "out.t42.txt";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(
      make_valid_input(),
      {{"output_path", std::string("out")}, {"write_report", true}},
      mockObservationContext_));

  EXPECT_EQ(instance_->get_trigger_status(),
            "Recovered 84 teletext packets (2 fields with data) to out.t42; "
            "0 pages; combined repeated rows corrected 31 packets; report to "
            "out.t42.txt");
  EXPECT_TRUE(captured.write_report);
}

// The result in the terms a reader thinks in: characters, and how many of them
// are known damaged.
TEST_F(TeletextSinkStage, Trigger_ReportsDataLossInTheStatus) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  deps_result.packets_written = 84;
  deps_result.fields_with_data = 2;
  deps_result.characters_written = 10234;
  deps_result.characters_damaged = 111;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  EXPECT_EQ(instance_->get_trigger_status(),
            "Recovered 84 teletext packets (2 fields with data) to out.t42; "
            "0 pages; data loss 1.08% (111 of 10234 characters damaged)");
}

// A run that wrote no display row has no denominator, so it claims no figure
// rather than reporting a loss of zero out of zero.
TEST_F(TeletextSinkStage, Trigger_OmitsDataLossWhenNoCharactersWereWritten) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  EXPECT_EQ(instance_->get_trigger_status().find("data loss"),
            std::string::npos)
      << instance_->get_trigger_status();
}

TEST_F(TeletextSinkStage, Trigger_ReportsDepsFailure) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = false;
  deps_result.message = "Input has no frames";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_FALSE(instance_->trigger(make_valid_input(),
                                  {{"output_path", std::string("out")}},
                                  mockObservationContext_));

  EXPECT_EQ(instance_->get_trigger_status(), "Error: Input has no frames");
  EXPECT_FALSE(instance_->is_trigger_in_progress());
}

////////////////////////////////////////////////////////////////////////////////////////////

// A 525-line project carries the 34-byte service, whose stream is named .t34,
// on the window ITU-R BT.653 §2 defines. Subtitle export is not offered: the
// cue timing assumes 50 fields per second.
TEST_F(TeletextSinkStage, ParameterDescriptors_FollowTheProjectFormat) {
  for (const auto system : {orc::VideoSystem::NTSC, orc::VideoSystem::PAL_M}) {
    const auto descriptors =
        instance_->get_parameter_descriptors(system, orc::SourceType::Unknown);
    ASSERT_EQ(descriptors.size(), 16u) << static_cast<int>(system);

    EXPECT_EQ(descriptors[0].name, "output_path");
    EXPECT_EQ(descriptors[0].file_extension_hint, ".t34");

    EXPECT_EQ(descriptors[1].name, "first_vbi_line");
    ASSERT_TRUE(descriptors[1].constraints.default_value.has_value());
    EXPECT_EQ(std::get<int32_t>(*descriptors[1].constraints.default_value), 10);

    EXPECT_EQ(descriptors[2].name, "last_vbi_line");
    ASSERT_TRUE(descriptors[2].constraints.default_value.has_value());
    EXPECT_EQ(std::get<int32_t>(*descriptors[2].constraints.default_value), 21);

    for (const auto& descriptor : descriptors) {
      EXPECT_EQ(descriptor.name.find("subtitle"), std::string::npos)
          << descriptor.name;
    }
  }
}

TEST_F(TeletextSinkStage, ParameterDescriptors_DefaultToThe625Service) {
  const auto descriptors = instance_->get_parameter_descriptors(
      orc::VideoSystem::PAL, orc::SourceType::Unknown);
  ASSERT_EQ(descriptors.size(), 19u);
  EXPECT_EQ(descriptors[0].file_extension_hint, ".t42");
  EXPECT_EQ(std::get<int32_t>(*descriptors[1].constraints.default_value), 6);
  EXPECT_EQ(std::get<int32_t>(*descriptors[2].constraints.default_value), 22);
}

// The last two links of the character-set chain, which nothing else covers:
// the parameter reaching the options the pass is given, and the pass's pages
// reaching the grid the viewer draws.
TEST_F(TeletextSinkStage, Trigger_PassesTheCharacterSetToThePass) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(
      make_valid_input(),
      {{"character_set", std::string("Cyrillic (Russian/Bulgarian)")}},
      mockObservationContext_));

  EXPECT_EQ(captured.character_set, orc::TeletextG0Set::Cyrillic2);
  // Left unset, so nothing the ESC control character could switch into: the
  // parameter's default and the behaviour of every page before it existed.
  EXPECT_FALSE(captured.second_character_set.has_value());
}

TEST_F(TeletextSinkStage, Trigger_PassesTheSecondCharacterSetToThePass) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(
      make_valid_input(),
      {{"character_set", std::string("Cyrillic (Russian/Bulgarian)")},
       {"second_character_set", std::string("Latin")}},
      mockObservationContext_));

  EXPECT_EQ(captured.character_set, orc::TeletextG0Set::Cyrillic2);
  ASSERT_TRUE(captured.second_character_set.has_value());
  EXPECT_EQ(captured.second_character_set->g0_set, orc::TeletextG0Set::Latin);
  // ETSI EN 300 706 §15.3: the second set's national option sub-set is
  // designated with the set and never taken from the page header, and with no
  // designation to read there is nothing but English to use.
  EXPECT_EQ(captured.second_character_set->national_option_subset, 0);
}

TEST_F(TeletextSinkStage, Trigger_RejectsAnUnknownSecondCharacterSet) {
  EXPECT_FALSE(
      instance_->trigger(make_valid_input(),
                         {{"output_path", std::string("out")},
                          {"second_character_set", std::string("Klingon")}},
                         mockObservationContext_));
  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: Unknown second character set: Klingon");
}

TEST_F(TeletextSinkStage, Catalogue_DrawsThePagesInTheirOwnCharacterSet) {
  orc::TeletextPageSnapshot snapshot;
  snapshot.magazine = 1;
  snapshot.page_number = 0x00;
  snapshot.g0_set = orc::TeletextG0Set::Cyrillic2;
  snapshot.row_received[1] = true;
  const std::string codes = "Wtornik";
  for (size_t column = 0; column < codes.size(); ++column) {
    snapshot.cells[1][column].character = static_cast<uint8_t>(codes[column]);
    // Cells carry their own set so an ESC-switched run keeps its alphabet; the
    // decoder stamps them and a hand-built snapshot must too.
    snapshot.cells[1][column].g0_set = snapshot.g0_set;
  }

  orc::TeletextCataloguedSubPage subpage;
  subpage.page = snapshot;
  orc::TeletextCataloguedPage page;
  page.magazine = snapshot.magazine;
  page.page_number = snapshot.page_number;
  page.times_seen = 1;
  page.subpages.push_back(std::move(subpage));

  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.dataset.pages.push_back(std::move(page));
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(
      make_valid_input(),
      {{"character_set", std::string("Cyrillic (Russian/Bulgarian)")}},
      mockObservationContext_));

  const orc::CatalogueDataset& catalogue = instance_->catalogue();
  const orc::CatalogueCellGrid* grid = nullptr;
  for (const orc::CataloguePayload& payload : catalogue.payloads) {
    if (payload.kind == orc::CataloguePayload::Kind::kCellGrid) {
      grid = &payload.grid;
      break;
    }
  }
  ASSERT_NE(grid, nullptr);
  // "Вторник" — the first letter is enough to tell the alphabets apart, and
  // char32_t comparison keeps the assertion free of encoding questions.
  EXPECT_EQ(grid->at(1, 0).character, U'В');
  EXPECT_EQ(grid->at(1, 1).character, U'т');
}

// The viewer reads the catalogue off the stage after the trigger, so a
// successful run has to leave it there.
TEST_F(TeletextSinkStage, Trigger_CachesTheDatasetForTheViewer) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextCataloguedPage page;
  page.magazine = 1;
  page.page_number = 0x00;
  page.times_seen = 4;
  page.first_seen_frame = 12;
  page.last_seen_frame = 900;
  deps_result.dataset.pages.push_back(page);
  deps_result.dataset.summary.packets_recovered = 84;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  EXPECT_TRUE(instance_->has_results());
  ASSERT_EQ(instance_->dataset().pages.size(), 1u);
  EXPECT_EQ(instance_->dataset().pages[0].magazine, 1);
  EXPECT_EQ(instance_->dataset().pages[0].times_seen, 4u);
  EXPECT_EQ(instance_->dataset().summary.packets_recovered, 84u);
  EXPECT_NE(instance_->get_trigger_status().find("; 1 page"), std::string::npos)
      << instance_->get_trigger_status();
}

// A failed run has no results to show, but what it did recover is still worth
// keeping: the pages are why the user triggered it.
TEST_F(TeletextSinkStage, Trigger_KeepsThePartialDatasetOnFailure) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = false;
  deps_result.message = "Cancelled after 10 of 100 frames";
  deps_result.dataset.pages.emplace_back();
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_FALSE(instance_->trigger(make_valid_input(),
                                  {{"output_path", std::string("out")}},
                                  mockObservationContext_));

  EXPECT_FALSE(instance_->has_results());
  EXPECT_EQ(instance_->dataset().pages.size(), 1u);
}

}  // namespace orc_unit_test
