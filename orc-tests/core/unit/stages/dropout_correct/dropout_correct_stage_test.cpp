/*
 * File:        dropout_correct_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for DropoutCorrectStage (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/dropout_correct/dropout_correct_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cmath>

#include "../../include/video_frame_representation_artifact_mock.h"

using ::testing::NiceMock;
using ::testing::Return;

namespace orc_unit_test {
namespace {

const orc::ParameterDescriptor* find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  auto it = std::find_if(
      descriptors.begin(), descriptors.end(),
      [&](const orc::ParameterDescriptor& d) { return d.name == name; });
  return it == descriptors.end() ? nullptr : &(*it);
}

}  // namespace

TEST(DropoutCorrectStageTest, RequiredInputCount_IsOne) {
  orc::DropoutCorrectStage stage;
  EXPECT_EQ(stage.required_input_count(), 1u);
}

TEST(DropoutCorrectStageTest, OutputCount_IsOne) {
  orc::DropoutCorrectStage stage;
  EXPECT_EQ(stage.output_count(), 1u);
}

TEST(DropoutCorrectStageTest, NodeTypeInfo_HasExpectedMetadata) {
  orc::DropoutCorrectStage stage;
  auto info = stage.get_node_type_info();
  EXPECT_EQ(info.type, orc::NodeType::TRANSFORM);
  EXPECT_EQ(info.stage_name, "dropout_correct");
  EXPECT_EQ(info.compatible_formats, orc::VideoFormatCompatibility::ALL);
}

TEST(DropoutCorrectStageTest, Descriptor_DefaultsMatchRuntimeDefaults) {
  orc::DropoutCorrectStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto params = stage.get_parameters();

  const auto* overcorrect =
      find_descriptor(descriptors, "overcorrect_extension");
  const auto* match_phase = find_descriptor(descriptors, "match_chroma_phase");
  const auto* highlight = find_descriptor(descriptors, "highlight_corrections");

  ASSERT_NE(overcorrect, nullptr);
  ASSERT_NE(match_phase, nullptr);
  ASSERT_NE(highlight, nullptr);

  ASSERT_TRUE(overcorrect->constraints.default_value.has_value());
  ASSERT_TRUE(match_phase->constraints.default_value.has_value());
  ASSERT_TRUE(highlight->constraints.default_value.has_value());

  EXPECT_EQ(std::get<uint32_t>(*overcorrect->constraints.default_value),
            std::get<uint32_t>(params.at("overcorrect_extension")));
  EXPECT_EQ(std::get<bool>(*match_phase->constraints.default_value),
            std::get<bool>(params.at("match_chroma_phase")));
  EXPECT_EQ(std::get<bool>(*highlight->constraints.default_value),
            std::get<bool>(params.at("highlight_corrections")));
}

TEST(DropoutCorrectStageTest, SetParameters_AcceptsValidValues) {
  orc::DropoutCorrectStage stage;

  const bool result =
      stage.set_parameters({{"overcorrect_extension", static_cast<uint32_t>(8)},
                            {"match_chroma_phase", false},
                            {"highlight_corrections", true}});

  EXPECT_TRUE(result);

  const auto params = stage.get_parameters();
  EXPECT_EQ(std::get<uint32_t>(params.at("overcorrect_extension")), 8u);
  EXPECT_FALSE(std::get<bool>(params.at("match_chroma_phase")));
  EXPECT_TRUE(std::get<bool>(params.at("highlight_corrections")));
}

TEST(DropoutCorrectStageTest, SetParameters_RejectsOutOfRangeOvercorrect) {
  orc::DropoutCorrectStage stage;
  EXPECT_FALSE(stage.set_parameters(
      {{"overcorrect_extension", static_cast<uint32_t>(49)}}));
}

TEST(DropoutCorrectStageTest, SetParameters_RejectsUnknownParameter) {
  orc::DropoutCorrectStage stage;
  EXPECT_FALSE(stage.set_parameters({{"unknown", true}}));
}

TEST(DropoutCorrectStageTest, Execute_ThrowsWhenNoInputProvided) {
  orc::DropoutCorrectStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({}, {}, ctx), orc::DAGExecutionError);
}

TEST(DropoutCorrectStageTest, Execute_ThrowsWhenInputIsWrongType) {
  struct FakeArt : public orc::Artifact {
    FakeArt() : Artifact(orc::ArtifactID("x"), orc::Provenance{}) {}
    std::string type_name() const override { return "x"; }
  };
  orc::DropoutCorrectStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({std::make_shared<FakeArt>()}, {}, ctx),
               orc::DAGExecutionError);
}

TEST(DropoutCorrectStageTest, Execute_ReturnsVFrameRWhenInputIsValid) {
  orc::DropoutCorrectStage stage;
  orc::ObservationContext ctx;
  auto source =
      std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  ON_CALL(*source, type_name()).WillByDefault(Return("test_vfr"));
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, 9u}));
  ON_CALL(*source, frame_count()).WillByDefault(Return(10u));

  const auto outputs = stage.execute({source}, {}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  auto vfr =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
  EXPECT_NE(vfr, nullptr);
  // Output wraps the input so it must be a different object
  EXPECT_NE(outputs[0].get(), source.get());
}

TEST(DropoutCorrectStageTest, RunsToLineDropouts_SplitsAcrossLines) {
  // A run of 10 samples starting at sample 5 on line 0 (SPL=10)
  // Line 0: samples 5–9 (5 samples), line 1: samples 0–4 (5 samples)
  std::vector<orc::DropoutRun> runs = {
      {0u, 5u, 10u, 0u}  // frame_id=0, start=5, count=10
  };
  auto lds = orc::DropoutCorrectStage::runs_to_line_dropouts(
      runs, orc::VideoSystem::NTSC, 10);
  ASSERT_EQ(lds.size(), 2u);
  EXPECT_EQ(lds[0].line, 0u);
  EXPECT_EQ(lds[0].start_sample, 5u);
  EXPECT_EQ(lds[0].end_sample, 9u);
  EXPECT_EQ(lds[1].line, 1u);
  EXPECT_EQ(lds[1].start_sample, 0u);
  EXPECT_EQ(lds[1].end_sample, 4u);
}

TEST(DropoutCorrectStageTest, RunsToLineDropouts_WholeLineDropout) {
  std::vector<orc::DropoutRun> runs = {
      {0u, 0u, 910u, 0u}  // exactly one NTSC line
  };
  auto lds = orc::DropoutCorrectStage::runs_to_line_dropouts(
      runs, orc::VideoSystem::NTSC, 910);
  ASSERT_EQ(lds.size(), 1u);
  EXPECT_EQ(lds[0].line, 0u);
  EXPECT_EQ(lds[0].start_sample, 0u);
  EXPECT_EQ(lds[0].end_sample, 909u);
}

TEST(DropoutCorrectStageTest, RunsToLineDropouts_EmptyRunsReturnsEmpty) {
  auto lds = orc::DropoutCorrectStage::runs_to_line_dropouts(
      {}, orc::VideoSystem::NTSC, 910);
  EXPECT_TRUE(lds.empty());
}

// ---------------------------------------------------------------------------
// End-to-end composite correction
// ---------------------------------------------------------------------------
namespace {
class FakeCompositeSource : public orc::VideoFrameRepresentation,
                            public orc::Artifact {
 public:
  static constexpr size_t kSpl = 910;
  static constexpr size_t kHeight = 525;
  static constexpr size_t kDoLine = 100;
  static constexpr size_t kDoStart = 200;
  static constexpr size_t kDoEnd = 210;

  FakeCompositeSource()
      : orc::Artifact(orc::ArtifactID("fake"), orc::Provenance{}) {
    buffer_.assign(kSpl * kHeight, int16_t{100});
    for (size_t s = kDoStart; s <= kDoEnd; ++s) {
      buffer_[kDoLine * kSpl + s] = 999;  // marker "dropout" value
    }
  }
  std::string type_name() const override { return "fake_composite"; }
  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0u, 1u};
  }
  size_t frame_count() const override { return 1; }
  bool has_frame(orc::FrameID id) const override { return id == 0; }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (id != 0) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = 0;
    d.system = orc::VideoSystem::NTSC;
    d.height = kHeight;
    d.samples_total = buffer_.size();
    d.samples_per_line_nominal = kSpl;
    return d;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    return id == 0 ? buffer_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return id == 0 ? buffer_ : std::vector<sample_type>{};
  }
  std::vector<orc::DropoutRun> get_dropout_hints(
      orc::FrameID id) const override {
    if (id != 0) return {};
    orc::DropoutRun r;
    r.frame_id = 0;
    r.sample_start = kDoLine * kSpl + kDoStart;
    r.sample_count = static_cast<uint32_t>(kDoEnd - kDoStart + 1);
    r.severity = 100;
    return {r};
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters p;
    p.system = orc::VideoSystem::NTSC;
    p.frame_width_nominal = static_cast<int32_t>(kSpl);
    p.frame_height = static_cast<int32_t>(kHeight);
    p.white_level = 800;
    p.black_level = 240;
    p.blanking_level = 240;
    p.sync_tip_level = 16;
    p.peak_level = 1019;
    p.active_video_start = 134;
    p.active_video_end = 753;
    p.first_active_frame_line = 40;
    p.last_active_frame_line = 480;
    return p;
  }

 private:
  std::vector<int16_t> buffer_;
};
}  // namespace

// Regression: find_replacement_line must select a replacement line even though
// the candidate distance always exceeds the zero-initialised default, so a
// composite dropout is replaced with neighbouring-line data rather than left
// untouched.
TEST(DropoutCorrectStageTest, CompositeCorrection_ReplacesDropoutPixels) {
  orc::DropoutCorrectStage stage;
  orc::ObservationContext ctx;
  auto source = std::make_shared<FakeCompositeSource>();

  const auto outputs = stage.execute({source}, {}, ctx);
  ASSERT_EQ(outputs.size(), 1u);
  auto corrected =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
  ASSERT_NE(corrected, nullptr);

  const int16_t* line = corrected->get_line(0, FakeCompositeSource::kDoLine);
  ASSERT_NE(line, nullptr);

  // Before correction the dropout samples were 999; neighbours are 100.
  for (size_t s = FakeCompositeSource::kDoStart;
       s <= FakeCompositeSource::kDoEnd; ++s) {
    EXPECT_EQ(line[s], 100) << "sample " << s << " not corrected";
  }
}

// The whole-frame accessors (get_frame / get_frame_copy) must also return the
// corrected samples — the chroma decoder reads the frame buffer this way, so a
// correction visible only through get_line() would leave it decoding the
// uncorrected dropout as a black line.
TEST(DropoutCorrectStageTest, CompositeCorrection_VisibleThroughGetFrame) {
  orc::DropoutCorrectStage stage;
  orc::ObservationContext ctx;
  auto source = std::make_shared<FakeCompositeSource>();

  const auto outputs = stage.execute({source}, {}, ctx);
  ASSERT_EQ(outputs.size(), 1u);
  auto corrected =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
  ASSERT_NE(corrected, nullptr);

  const int16_t* frame = corrected->get_frame(0);
  ASSERT_NE(frame, nullptr);
  const std::vector<int16_t> frame_copy = corrected->get_frame_copy(0);

  const size_t base = FakeCompositeSource::kDoLine * FakeCompositeSource::kSpl;
  for (size_t s = FakeCompositeSource::kDoStart;
       s <= FakeCompositeSource::kDoEnd; ++s) {
    EXPECT_EQ(frame[base + s], 100) << "get_frame sample " << s;
    EXPECT_EQ(frame_copy[base + s], 100) << "get_frame_copy sample " << s;
  }
}

// ---------------------------------------------------------------------------
// Replacement-line selection: phase (field) preference and dropout overlap
// ---------------------------------------------------------------------------
// Every line is filled with its own line index as the sample value, so the
// corrected sample value identifies which source line was copied.  Dropout
// lines have their dropout region overwritten with a sentinel so that copying a
// damaged line is detectable.
namespace {
constexpr int16_t kDropSentinel = 999;

class FakeIndexedSource : public orc::VideoFrameRepresentation,
                          public orc::Artifact {
 public:
  static constexpr size_t kField1 = 263;   // kNtscField1Lines
  static constexpr size_t kDoStart = 300;  // active-video region
  static constexpr size_t kDoEnd = 310;

  explicit FakeIndexedSource(std::vector<uint32_t> dropout_lines,
                             orc::VideoSystem system = orc::VideoSystem::NTSC,
                             int32_t last_active_frame_line = 480)
      : orc::Artifact(orc::ArtifactID("fake"), orc::Provenance{}),
        system_(system),
        last_active_frame_line_(last_active_frame_line),
        spl_(system == orc::VideoSystem::PAL     ? 1135
             : system == orc::VideoSystem::PAL_M ? 909
                                                 : 910),
        height_(system == orc::VideoSystem::PAL ? 625 : 525),
        dropout_lines_(std::move(dropout_lines)) {
    buffer_.resize(orc::frame_line_sample_offset(system_, spl_, height_));
    for (size_t line = 0; line < height_; ++line) {
      const size_t base = orc::frame_line_sample_offset(system_, spl_, line);
      for (size_t s = 0; s < spl_; ++s) {
        buffer_[base + s] = static_cast<int16_t>(line);
      }
    }
    for (uint32_t line : dropout_lines_) {
      const size_t base = orc::frame_line_sample_offset(system_, spl_, line);
      for (size_t s = kDoStart; s <= kDoEnd; ++s) {
        buffer_[base + s] = kDropSentinel;
      }
    }
  }
  std::string type_name() const override { return "fake_indexed"; }
  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0u, 1u};
  }
  size_t frame_count() const override { return 1; }
  bool has_frame(orc::FrameID id) const override { return id == 0; }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (id != 0) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = 0;
    d.system = system_;
    d.height = height_;
    d.samples_total = buffer_.size();
    d.samples_per_line_nominal = spl_;
    return d;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    return id == 0 ? buffer_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return id == 0 ? buffer_ : std::vector<sample_type>{};
  }
  std::vector<orc::DropoutRun> get_dropout_hints(
      orc::FrameID id) const override {
    std::vector<orc::DropoutRun> runs;
    if (id != 0) return runs;
    for (uint32_t line : dropout_lines_) {
      orc::DropoutRun r;
      r.frame_id = 0;
      r.sample_start =
          orc::frame_line_sample_offset(system_, spl_, line) + kDoStart;
      r.sample_count = static_cast<uint32_t>(kDoEnd - kDoStart + 1);
      r.severity = 100;
      runs.push_back(r);
    }
    return runs;
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters p;
    p.system = system_;
    p.frame_width_nominal = static_cast<int32_t>(spl_);
    p.frame_height = static_cast<int32_t>(height_);
    p.white_level = 800;
    p.black_level = 240;
    p.blanking_level = 240;
    p.sync_tip_level = 16;
    p.peak_level = 1019;
    if (system_ == orc::VideoSystem::PAL) {
      p.active_video_start = orc::kPalActiveVideoStart;
      p.active_video_end = orc::kPalActiveVideoEnd;
      p.first_active_frame_line = orc::kPalFirstActiveFrameLine;
      p.last_active_frame_line = orc::kPalLastActiveFrameLine;
    } else {
      p.active_video_start = 134;
      p.active_video_end = 753;
      p.first_active_frame_line = 40;
      p.last_active_frame_line = last_active_frame_line_;
    }
    return p;
  }

 private:
  orc::VideoSystem system_;
  int32_t last_active_frame_line_;
  size_t spl_;
  size_t height_;
  std::vector<int16_t> buffer_;
  std::vector<uint32_t> dropout_lines_;
};

int16_t corrected_sample_at(const std::shared_ptr<FakeIndexedSource>& source,
                            uint32_t line,
                            const std::string& method = "line_copy") {
  orc::DropoutCorrectStage stage;
  orc::ObservationContext ctx;
  const auto outputs = stage.execute({source}, {{"method", method}}, ctx);
  EXPECT_EQ(outputs.size(), 1u);
  auto corrected =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
  EXPECT_NE(corrected, nullptr);
  const int16_t* row = corrected->get_line(0, line);
  EXPECT_NE(row, nullptr);
  return row[(FakeIndexedSource::kDoStart + FakeIndexedSource::kDoEnd) / 2];
}
}  // namespace

// A field-1 target must be repaired from another field-1 (intrafield) line so
// the colour subcarrier phase is preserved, not from the spatially-nearer
// field-2 line.
TEST(DropoutCorrectStageTest, PrefersIntrafieldReplacement) {
  auto source = std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{100});
  const int16_t v = corrected_sample_at(source, 100);
  EXPECT_NE(v, kDropSentinel);
  EXPECT_LT(v, static_cast<int16_t>(FakeIndexedSource::kField1))
      << "replacement came from field 2 (line " << v << ") — wrong phase";
}

// A replacement line that itself carries a dropout over the same samples must
// be skipped in favour of a clean line.
TEST(DropoutCorrectStageTest, SkipsReplacementLinesWithOverlappingDropout) {
  // Damage the target and both nearest phase-matched candidates (NTSC step 2 →
  // 98/102); the search must skip them to the next in-phase lines (96/104).
  auto source =
      std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{98, 100, 102});
  const int16_t v = corrected_sample_at(source, 100);
  EXPECT_NE(v, kDropSentinel) << "copied samples from a dropout line";
  EXPECT_TRUE(v == 96 || v == 104)
      << "expected a clean in-phase line (96/104), got " << v;
}

// last_active_frame_line is exclusive: 480 leaves field 1 with lines 20..239,
// so line 240 is past the picture and must not be borrowed even though it is
// the nearest in-phase line.
TEST(DropoutCorrectStageTest, NeverBorrowsLinePastExclusiveActiveEnd) {
  // Target 238 with 236 damaged: up reaches 234 (distance 4), down would reach
  // 240 (distance 2) were the bound treated as inclusive.
  auto source =
      std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{236, 238});
  const int16_t v = corrected_sample_at(source, 238);
  EXPECT_EQ(v, 234) << "borrowed line " << v << " from outside the picture";
}

// PAL_M has 525-line geometry but PAL's colour: only lines 4 apart are in
// subcarrier and V-switch phase, so the 2-line NTSC step pastes chroma half
// a cycle out.
TEST(DropoutCorrectStageTest, PalMUsesFourLineStep) {
  auto source = std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{100},
                                                    orc::VideoSystem::PAL_M);
  const int16_t v = corrected_sample_at(source, 100);
  EXPECT_TRUE(v == 96 || v == 104)
      << "expected an in-phase line (96/104), got " << v;
}

// PAL line 23 (flat line 22) is a half-line inside the nominal active range;
// it carries blanking, so repairing the line four below it must use the next
// in-phase line down instead (regression: black blocks pasted onto frame line
// 53 of a Domesday capture).
TEST(DropoutCorrectStageTest, PalSkipsField1HalfLineAsReplacement) {
  auto source = std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{26},
                                                    orc::VideoSystem::PAL);
  const int16_t v = corrected_sample_at(source, 26);
  EXPECT_EQ(v, 30) << "replacement came from line " << v;
}

// PAL line 623 (flat line 313 + 309) is the half-line at the bottom of field 2.
TEST(DropoutCorrectStageTest, PalSkipsField2HalfLineAsReplacement) {
  // Target 618 with 614 damaged: down would reach the half-line 622 (distance
  // 4) ahead of the clean 610 (distance 8).
  auto source = std::make_shared<FakeIndexedSource>(
      std::vector<uint32_t>{614, 618}, orc::VideoSystem::PAL);
  const int16_t v = corrected_sample_at(source, 618);
  EXPECT_EQ(v, 610) << "replacement came from line " << v;
}

// NTSC line 21 (flat line 20) carries closed captions inside the nominal active
// range; repairing the line two below it must use the next in-phase line down.
TEST(DropoutCorrectStageTest, NtscSkipsField1CaptionLineAsReplacement) {
  auto source = std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{22});
  const int16_t v = corrected_sample_at(source, 22);
  EXPECT_EQ(v, 24) << "replacement came from line " << v;
}

// NTSC line 284 (flat line 263 + 20) is field 2's caption line, blank after the
// run-in when idle (regression: black pasted onto field 2 line 286 of a CC
// test disc).
TEST(DropoutCorrectStageTest, NtscSkipsField2CaptionLineAsReplacement) {
  auto source =
      std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{263 + 22});
  const int16_t v = corrected_sample_at(source, 263 + 22);
  EXPECT_EQ(v, 263 + 24) << "replacement came from line " << v;
}

// NTSC line 263 (flat line 262) is a half-line.  ld-decode's own active range
// (last_active_frame_line 525) reaches it, so it must be excluded by line.
TEST(DropoutCorrectStageTest, NtscSkipsHalfLine263AsReplacement) {
  // Target 260 with 258 damaged: down would reach the half-line 262 (distance
  // 2) ahead of the clean 256 (distance 4).
  auto source = std::make_shared<FakeIndexedSource>(
      std::vector<uint32_t>{258, 260}, orc::VideoSystem::NTSC, 525);
  const int16_t v = corrected_sample_at(source, 260);
  EXPECT_EQ(v, 256) << "replacement came from line " << v;
}

// Lines that carry data are left as they are: copying picture over a picture
// number or VITC destroys what a decoder could still read
// (regression: Stacker low-confidence marks on NTSC lines 17 and 18 filled,
// and the frame lost its CAV picture number).
TEST(DropoutCorrectStageTest, LeavesNtscField1VbiLineUncorrected) {
  for (const std::string method : {"line_copy", "interpolate"}) {
    auto source =
        std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{17});
    EXPECT_EQ(corrected_sample_at(source, 17, method), kDropSentinel) << method;
  }
}

TEST(DropoutCorrectStageTest, LeavesNtscField2VbiLineUncorrected) {
  auto source =
      std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{263 + 16});
  EXPECT_EQ(corrected_sample_at(source, 263 + 16), kDropSentinel);
}

// Lines 21 and 284 are inside the active picture, and carry picture on a
// disc without captions.
TEST(DropoutCorrectStageTest, StillCorrectsNtscCaptionLines) {
  auto source =
      std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{20, 263 + 20});
  EXPECT_NE(corrected_sample_at(source, 20), kDropSentinel);
  EXPECT_NE(corrected_sample_at(source, 263 + 20), kDropSentinel);
}

TEST(DropoutCorrectStageTest, LeavesPalVbiLineUncorrected) {
  auto source = std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{18},
                                                    orc::VideoSystem::PAL);
  EXPECT_EQ(corrected_sample_at(source, 18), kDropSentinel);
}

// ---------------------------------------------------------------------------
// method = interpolate
// ---------------------------------------------------------------------------

TEST(DropoutCorrectStageTest, Method_DefaultsToInterpolate) {
  orc::DropoutCorrectStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto* method = find_descriptor(descriptors, "method");
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(method->constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*method->constraints.default_value),
            "interpolate");
  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("method")),
            "interpolate");
  EXPECT_THAT(method->constraints.allowed_strings,
              ::testing::ElementsAre("line_copy", "interpolate"));
}

TEST(DropoutCorrectStageTest, Method_AcceptsInterpolateAndRejectsOthers) {
  orc::DropoutCorrectStage stage;
  EXPECT_TRUE(stage.set_parameters({{"method", std::string("line_copy")}}));
  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("method")),
            "line_copy");
  EXPECT_TRUE(stage.set_parameters({{"method", std::string("interpolate")}}));
  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("method")),
            "interpolate");
  EXPECT_FALSE(stage.set_parameters({{"method", std::string("blend")}}));
  EXPECT_FALSE(stage.set_parameters({{"method", true}}));
}

namespace {

// The value at the middle of the dropout of |line| after correction with
// |method|.
int16_t corrected_with(const std::shared_ptr<orc::Artifact>& source,
                       const std::string& method, uint32_t line,
                       size_t sample) {
  orc::DropoutCorrectStage stage;
  orc::ObservationContext ctx;
  EXPECT_TRUE(stage.set_parameters({{"method", method}}));
  const auto outputs = stage.execute({source}, {{"method", method}}, ctx);
  EXPECT_EQ(outputs.size(), 1u);
  auto corrected =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
  EXPECT_NE(corrected, nullptr);
  return corrected->get_line(0, line)[sample];
}

int16_t interpolated_at(const std::shared_ptr<FakeIndexedSource>& source,
                        uint32_t line) {
  return corrected_with(
      source, "interpolate", line,
      (FakeIndexedSource::kDoStart + FakeIndexedSource::kDoEnd) / 2);
}

}  // namespace

// A source without a burst is interpolated as it is: each line holds its own
// index, so the lines either side of line 100 give 100 back.
TEST(DropoutCorrectStageTest, Interpolate_MonochromeTakesTheLinesEitherSide) {
  auto source = std::make_shared<FakeIndexedSource>(std::vector<uint32_t>{100});
  EXPECT_EQ(interpolated_at(source, 100), 100);
}

// Damaged neighbours are skipped and the clean lines either side weighted by
// distance: with lines 98-101 damaged, line 99 takes 97 and 102 at 3:2 and
// line 100 takes them at 2:3, each giving its own index back.
TEST(DropoutCorrectStageTest, Interpolate_WeightsCleanLinesByDistance) {
  auto source = std::make_shared<FakeIndexedSource>(
      std::vector<uint32_t>{98, 99, 100, 101});
  EXPECT_EQ(interpolated_at(source, 99), 99);
  EXPECT_EQ(interpolated_at(source, 100), 100);
}

// With every line within reach damaged on both sides, the dropout is copied
// from a line as line_copy would.
TEST(DropoutCorrectStageTest, Interpolate_FallsBackToLineCopyBeyondReach) {
  std::vector<uint32_t> damaged;
  for (uint32_t l = 95; l <= 105; ++l) damaged.push_back(l);
  auto source = std::make_shared<FakeIndexedSource>(damaged);
  const int16_t v = interpolated_at(source, 100);
  EXPECT_TRUE(v == 94 || v == 106) << "got " << v;
}

namespace {

// A colour source: every line of each field carries the same colour on a
// luma ramp that rises down the field, with the system's subcarrier phase
// advancing line by line, PAL's and PAL-M's V-switch, and a burst. Dropout
// runs cover samples [do_start, do_end] of each line in |dropout_lines|,
// whose samples there are left as the truth (the correction is checked
// against them, not whether it changed them).
class FakeEncodedSource : public orc::VideoFrameRepresentation,
                          public orc::Artifact {
 public:
  static constexpr double kU = 45.0;
  static constexpr double kV = -35.0;

  FakeEncodedSource(orc::VideoSystem system,
                    std::vector<uint32_t> dropout_lines, size_t do_start,
                    size_t do_end)
      : orc::Artifact(orc::ArtifactID("fake"), orc::Provenance{}),
        system_(system),
        spl_(system == orc::VideoSystem::PAL     ? 1135
             : system == orc::VideoSystem::PAL_M ? 909
                                                 : 910),
        height_(system == orc::VideoSystem::PAL ? 625 : 525),
        field1_(system == orc::VideoSystem::PAL ? 313 : 263),
        do_start_(do_start),
        do_end_(do_end),
        dropout_lines_(std::move(dropout_lines)) {
    // Cycles of subcarrier a line advances, less whole cycles.
    const double step = system == orc::VideoSystem::PAL     ? 0.7516
                        : system == orc::VideoSystem::PAL_M ? 0.25
                                                            : 0.5;
    const bool pal_colour = system != orc::VideoSystem::NTSC;
    const auto [burst_start, burst_end] = orc::colour_burst_range(system);
    buffer_.resize(orc::frame_line_sample_offset(system_, spl_, height_));
    for (size_t line = 0; line < height_; ++line) {
      const size_t base = orc::frame_line_sample_offset(system_, spl_, line);
      const double phase = 2.0 * M_PI * step * static_cast<double>(line);
      const double v_switch = (pal_colour && (line % 2) != 0) ? -1.0 : 1.0;
      const double y =
          300.0 +
          3.0 * static_cast<double>(line < field1_ ? line : line - field1_);
      for (size_t n = 0; n < spl_; ++n) {
        const double theta = phase + (static_cast<double>(n) * M_PI / 2.0);
        double sample = 256.0;
        if (static_cast<int32_t>(n) >= burst_start &&
            static_cast<int32_t>(n) < burst_end) {
          sample += (-60.0 * std::sin(theta)) +
                    (pal_colour ? v_switch * 60.0 * std::cos(theta) : 0.0);
        } else if (n >= 150 && n < spl_ - 40) {
          sample =
              y + (kU * std::sin(theta)) + (v_switch * kV * std::cos(theta));
        }
        buffer_[base + n] = static_cast<int16_t>(std::lround(sample));
      }
    }
  }
  std::string type_name() const override { return "fake_encoded"; }
  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0u, 1u};
  }
  size_t frame_count() const override { return 1; }
  bool has_frame(orc::FrameID id) const override { return id == 0; }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (id != 0) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = 0;
    d.system = system_;
    d.height = height_;
    d.samples_total = buffer_.size();
    d.samples_per_line_nominal = spl_;
    return d;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    return id == 0 ? buffer_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return id == 0 ? buffer_ : std::vector<sample_type>{};
  }
  std::vector<orc::DropoutRun> get_dropout_hints(
      orc::FrameID id) const override {
    std::vector<orc::DropoutRun> runs;
    if (id != 0) return runs;
    for (uint32_t line : dropout_lines_) {
      orc::DropoutRun r;
      r.frame_id = 0;
      r.sample_start =
          orc::frame_line_sample_offset(system_, spl_, line) + do_start_;
      r.sample_count = static_cast<uint32_t>(do_end_ - do_start_ + 1);
      r.severity = 100;
      runs.push_back(r);
    }
    return runs;
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters p;
    p.system = system_;
    p.frame_width_nominal = static_cast<int32_t>(spl_);
    p.frame_height = static_cast<int32_t>(height_);
    p.blanking_level = 256;
    p.black_level = 256;
    p.white_level = 844;
    p.sync_tip_level = 4;
    p.peak_level = 1019;
    p.active_video_start = 160;
    p.active_video_end = static_cast<int32_t>(spl_) - 60;
    p.first_active_frame_line = 44;
    p.last_active_frame_line = static_cast<int32_t>(height_) - 6;
    return p;
  }
  int16_t truth(uint32_t line, size_t n) const {
    return buffer_[orc::frame_line_sample_offset(system_, spl_, line) + n];
  }

 private:
  orc::VideoSystem system_;
  size_t spl_;
  size_t height_;
  size_t field1_;
  size_t do_start_;
  size_t do_end_;
  std::vector<int16_t> buffer_;
  std::vector<uint32_t> dropout_lines_;
};

// The largest error against the truth over |line|'s samples [first, last]
// after correction with |method|.
int max_error(const std::shared_ptr<FakeEncodedSource>& source,
              const std::string& method, uint32_t line, size_t first,
              size_t last) {
  orc::DropoutCorrectStage stage;
  orc::ObservationContext ctx;
  const std::map<std::string, orc::ParameterValue> params{{"method", method}};
  const auto outputs = stage.execute({source}, params, ctx);
  EXPECT_EQ(outputs.size(), 1u);
  auto corrected =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
  EXPECT_NE(corrected, nullptr);
  const int16_t* row = corrected->get_line(0, line);
  int worst = 0;
  for (size_t n = first; n <= last; ++n) {
    worst = std::max(worst, std::abs(row[n] - source->truth(line, n)));
  }
  return worst;
}

class DropoutInterpolateSystemTest
    : public ::testing::TestWithParam<orc::VideoSystem> {};

}  // namespace

// A colour on a vertical luma ramp: interpolation from the lines either side
// rebuilds the luma and moves the chroma to the damaged line's phase, within
// rounding, where line copy brings the ramp from a phase-matched line two or
// four lines away.
TEST_P(DropoutInterpolateSystemTest, RebuildsRampAndColour) {
  const orc::VideoSystem system = GetParam();
  constexpr uint32_t kLine = 100;
  auto source = std::make_shared<FakeEncodedSource>(
      system, std::vector<uint32_t>{kLine}, 400, 440);
  EXPECT_LE(max_error(source, "interpolate", kLine, 400, 440), 2);
  EXPECT_GE(max_error(source, "line_copy", kLine, 400, 440), 5);
}

// A dropout that covers the line's burst too: the damaged line's phase comes
// from the nearest line in phase with it, and the picture is still rebuilt.
// On PAL that line is four away, where the subcarrier sits 2.3 degrees off
// (PAL's 1135.0064 samples a line), a code more of chroma error.
TEST_P(DropoutInterpolateSystemTest, RebuildsWhenTheBurstIsDamaged) {
  const orc::VideoSystem system = GetParam();
  constexpr uint32_t kLine = 120;
  auto source = std::make_shared<FakeEncodedSource>(
      system, std::vector<uint32_t>{kLine}, 60, 500);
  EXPECT_LE(max_error(source, "interpolate", kLine, 200, 500),
            system == orc::VideoSystem::PAL ? 3 : 2);
}

// Two damaged lines in a row in the second field: each is rebuilt from the
// clean lines beyond them, one and two lines away.
TEST_P(DropoutInterpolateSystemTest, BridgesTwoDamagedLines) {
  const orc::VideoSystem system = GetParam();
  const uint32_t field2 = system == orc::VideoSystem::PAL ? 313 : 263;
  auto source = std::make_shared<FakeEncodedSource>(
      system, std::vector<uint32_t>{field2 + 80, field2 + 81}, 300, 340);
  EXPECT_LE(max_error(source, "interpolate", field2 + 80, 300, 340), 2);
  EXPECT_LE(max_error(source, "interpolate", field2 + 81, 300, 340), 2);
}

INSTANTIATE_TEST_SUITE_P(
    Systems, DropoutInterpolateSystemTest,
    ::testing::Values(orc::VideoSystem::PAL, orc::VideoSystem::NTSC,
                      orc::VideoSystem::PAL_M),
    [](const ::testing::TestParamInfo<orc::VideoSystem>& info) {
      switch (info.param) {
        case orc::VideoSystem::PAL:
          return std::string("PAL");
        case orc::VideoSystem::NTSC:
          return std::string("NTSC");
        default:
          return std::string("PALM");
      }
    });

}  // namespace orc_unit_test
