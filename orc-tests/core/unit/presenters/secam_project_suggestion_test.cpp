/*
 * File:        secam_project_suggestion_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     The SECAM stage filter, and the suggestion to switch a PAL
 *              project that treats its signal as SECAM to the SECAM
 *              format
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/presenters/include/secam_project_suggestion.h"

#include <gtest/gtest.h>
#include <orc/stage/node_type.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../../../orc/core/include/project.h"
#include "../../../../orc/presenters/include/project_presenter.h"

namespace orc_unit_test {
namespace {

using orc::presenters::findSecamProjectSuggestion;
using orc::presenters::ProjectPresenter;
using orc::presenters::StageInfo;
using orc::presenters::VideoFormat;

bool has_stage(const std::vector<StageInfo>& stages, const std::string& name) {
  return std::any_of(stages.begin(), stages.end(),
                     [&](const StageInfo& s) { return s.name == name; });
}

struct NodeSpec {
  int id;
  std::string stage;
  std::string node_type;
  // decoder_type / decoder_schedule for a video_sink node; empty for none.
  std::string decoder_type;
  std::string decoder_schedule;
  // format for a vbi_source node; empty for none.
  std::string vbi_format = {};
};

std::string string_param(const std::string& key, const std::string& value) {
  return "        " + key +
         ":\n"
         "          type: string\n"
         "          value: \"" +
         value + "\"\n";
}

orc::Project make_project(const std::string& format,
                          const std::vector<NodeSpec>& nodes) {
  std::string yaml =
      "project:\n"
      "  name: secam-suggestion-test\n"
      "  version: \"2.0\"\n"
      "  video_format: " +
      format +
      "\n"
      "  source_format: Composite\n"
      "  amplitude_unit: mV\n"
      "dag:\n"
      "  nodes:\n";
  for (const auto& n : nodes) {
    yaml += "    - id: " + std::to_string(n.id) + "\n";
    yaml += "      stage: " + n.stage + "\n";
    yaml += "      node_type: " + n.node_type + "\n";
    if (!n.decoder_type.empty() || !n.decoder_schedule.empty() ||
        !n.vbi_format.empty()) {
      yaml += "      parameters:\n";
      if (!n.vbi_format.empty()) {
        yaml += string_param("format", n.vbi_format);
      }
      if (!n.decoder_type.empty()) {
        yaml += string_param("decoder_type", n.decoder_type);
      }
      if (!n.decoder_schedule.empty()) {
        yaml += string_param("decoder_schedule", n.decoder_schedule);
      }
    }
  }
  yaml += "  edges: []\n";
  return orc::project_io::load_project_from_yaml(yaml, "/virtual/test.orcprj");
}

ProjectPresenter wrap(orc::Project& project) {
  return ProjectPresenter(static_cast<void*>(&project));
}

}  // namespace

// ── Stage filter ─────────────────────────────────────────────────────────

// A SECAM project offers the stages that run on any system, and not the PAL
// colour stages: PAL_ONLY means PAL colour, not "625 lines".
TEST(SecamStageFilterTest, OffersAllFormatStagesButNotPalOnly) {
  const auto secam = ProjectPresenter::getAvailableStages(VideoFormat::SECAM);
  const auto pal = ProjectPresenter::getAvailableStages(VideoFormat::PAL);

  EXPECT_TRUE(has_stage(secam, "tbc_source"));
  EXPECT_TRUE(has_stage(secam, "video_sink"));
  EXPECT_TRUE(has_stage(secam, "dropout_correct"));

  for (const char* pal_only :
       {"PAL_CVBS_Source", "fm_beat_cancel", "channel_equalise"}) {
    EXPECT_TRUE(has_stage(pal, pal_only)) << pal_only;
    EXPECT_FALSE(has_stage(secam, pal_only)) << pal_only;
  }
  EXPECT_FALSE(has_stage(secam, "NTSC_CVBS_Source"));
  EXPECT_FALSE(has_stage(secam, "PAL_M_CVBS_Source"));

  // The SECAM CVBS Source is offered in a SECAM project only.
  EXPECT_TRUE(has_stage(secam, "SECAM_CVBS_Source"));
  EXPECT_FALSE(has_stage(pal, "SECAM_CVBS_Source"));
}

// The LaserDisc stages are NOT_SECAM: no LaserDisc carries SECAM, so a SECAM
// project does not offer them, and the other systems still do.
TEST(SecamStageFilterTest, LaserDiscStagesAreNotOfferedForSecam) {
  const auto secam = ProjectPresenter::getAvailableStages(VideoFormat::SECAM);
  for (const auto format :
       {VideoFormat::PAL, VideoFormat::NTSC, VideoFormat::PAL_M}) {
    const auto others = ProjectPresenter::getAvailableStages(format);
    for (const char* laserdisc : {"AC3RFSink", "EFMSink", "RawEFMSink",
                                  "daphne_vbi_sink", "efm_audio_decode"}) {
      EXPECT_TRUE(has_stage(others, laserdisc)) << laserdisc;
      EXPECT_FALSE(has_stage(secam, laserdisc)) << laserdisc;
    }
  }
  EXPECT_FALSE(
      orc::is_stage_compatible_with_format("EFMSink", orc::VideoSystem::SECAM));
  EXPECT_TRUE(
      orc::is_stage_compatible_with_format("EFMSink", orc::VideoSystem::PAL));
}

// ── Suggestion ───────────────────────────────────────────────────────────

TEST(SecamProjectSuggestionTest, PalProjectDecodingSecam_CanSwitch) {
  auto project = make_project("PAL", {{1, "tbc_source", "SOURCE", "", ""},
                                      {2, "video_sink", "SINK", "secam", ""}});
  auto presenter = wrap(project);

  const auto suggestion = findSecamProjectSuggestion(presenter);
  ASSERT_TRUE(suggestion.has_value());
  EXPECT_TRUE(suggestion->canSwitch());
  ASSERT_EQ(suggestion->secam_nodes.size(), 1u);
  EXPECT_NE(suggestion->secam_nodes[0].find("node 2"), std::string::npos);

  const std::string message = suggestion->message();
  EXPECT_NE(message.find("Switch this project to SECAM?"), std::string::npos)
      << message;

  // The CLI cannot ask, so it says how to switch instead.
  const std::string cli = suggestion->cliMessage();
  EXPECT_EQ(cli.find('?'), std::string::npos) << cli;
  EXPECT_NE(cli.find("video_format: SECAM"), std::string::npos) << cli;
}

// Nothing is switched by finding a suggestion: the format stays PAL and the
// project is not marked modified until the user agrees.
TEST(SecamProjectSuggestionTest, FindingDoesNotConvert) {
  auto project = make_project("PAL", {{1, "tbc_source", "SOURCE", "", ""},
                                      {2, "video_sink", "SINK", "secam", ""}});
  auto presenter = wrap(project);

  ASSERT_TRUE(findSecamProjectSuggestion(presenter).has_value());
  EXPECT_EQ(presenter.getVideoFormat(), VideoFormat::PAL);
  EXPECT_FALSE(presenter.isModified());
}

TEST(SecamProjectSuggestionTest, PalOnlyStage_BlocksTheSwitch) {
  auto project =
      make_project("PAL", {{1, "PAL_CVBS_Source", "SOURCE", "", ""},
                           {2, "channel_equalise", "TRANSFORM", "", ""},
                           {3, "video_sink", "SINK", "secam", ""}});
  auto presenter = wrap(project);

  const auto suggestion = findSecamProjectSuggestion(presenter);
  ASSERT_TRUE(suggestion.has_value());
  EXPECT_FALSE(suggestion->canSwitch());
  ASSERT_EQ(suggestion->blocking_nodes.size(), 2u);
  EXPECT_NE(suggestion->blocking_nodes[0].find("node 1"), std::string::npos);
  EXPECT_NE(suggestion->blocking_nodes[1].find("node 2"), std::string::npos);

  const std::string message = suggestion->message();
  EXPECT_EQ(message.find("Switch this project"), std::string::npos) << message;
  EXPECT_NE(message.find("cannot be switched"), std::string::npos) << message;
}

TEST(SecamProjectSuggestionTest, ScheduleNamingSecam_IsDetected) {
  auto project = make_project(
      "PAL", {{1, "tbc_source", "SOURCE", "", ""},
              {2, "video_sink", "SINK", "multiple", "0-99:pal2d, held:secam"}});
  auto presenter = wrap(project);

  EXPECT_TRUE(findSecamProjectSuggestion(presenter).has_value());
}

// vbi_source's SECAM capture format in a PAL project is a SECAM capture too.
TEST(SecamProjectSuggestionTest, VbiSourceOnSecamFormat_IsDetected) {
  auto project =
      make_project("PAL", {{1, "vbi_source", "SOURCE", "", "",
                            "bt8x8 card dump, 8-bit (WST, SECAM source)"}});
  auto presenter = wrap(project);

  const auto suggestion = findSecamProjectSuggestion(presenter);
  ASSERT_TRUE(suggestion.has_value());
  EXPECT_TRUE(suggestion->canSwitch());
  ASSERT_EQ(suggestion->secam_nodes.size(), 1u);
  EXPECT_NE(suggestion->secam_nodes[0].find("node 1"), std::string::npos);
}

TEST(SecamProjectSuggestionTest, VbiSourceOnPalFormat_NoSuggestion) {
  auto project = make_project("PAL", {{1, "vbi_source", "SOURCE", "", "",
                                       "bt8x8 card dump, 8-bit (WST)"}});
  auto presenter = wrap(project);

  EXPECT_FALSE(findSecamProjectSuggestion(presenter).has_value());
}

// A schedule left behind when decoder_type is a single decoder is not read
// by the sink, so it does not make the project look like SECAM.
TEST(SecamProjectSuggestionTest, StaleSchedule_IsIgnored) {
  auto project = make_project(
      "PAL", {{1, "tbc_source", "SOURCE", "", ""},
              {2, "video_sink", "SINK", "transform2d", "0-99:secam"}});
  auto presenter = wrap(project);

  EXPECT_FALSE(findSecamProjectSuggestion(presenter).has_value());
}

TEST(SecamProjectSuggestionTest, PalDecoder_NoSuggestion) {
  auto project =
      make_project("PAL", {{1, "tbc_source", "SOURCE", "", ""},
                           {2, "video_sink", "SINK", "transform2d", ""}});
  auto presenter = wrap(project);

  EXPECT_FALSE(findSecamProjectSuggestion(presenter).has_value());
}

// Only a PAL project is asked: a SECAM project already is one.
TEST(SecamProjectSuggestionTest, SecamProject_NoSuggestion) {
  auto project =
      make_project("SECAM", {{1, "tbc_source", "SOURCE", "", ""},
                             {2, "video_sink", "SINK", "secam", ""}});
  auto presenter = wrap(project);

  EXPECT_FALSE(findSecamProjectSuggestion(presenter).has_value());
}

}  // namespace orc_unit_test
