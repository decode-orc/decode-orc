/*
 * File:        core_observation_service_test.cpp
 * Module:      orc-tests/core/unit/observers
 * Purpose:     Unit tests for CoreObservationService (host observation service)
 *
 * Covers id lookup, unknown-id failure, available_observers() completeness,
 * and result delivery through a mocked IObservationContext. No filesystem,
 * network, or clock access — a mocked VideoFrameRepresentation drives the
 * observer deterministically.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "core_observation_service.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/observation/observation_service_interface.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../include/observation_context_interface_mock.h"
#include "../mocks/mock_video_frame_representation.h"

namespace orc {
namespace tests {
namespace {

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

// The complete set of observer ids the service must expose. Kept here as an
// independent restatement of the registry contract so a drift in either
// direction (added or dropped observer) fails this test.
const std::set<std::string> kExpectedObserverIds{
    "white_snr",      "black_psnr", "burst_level",
    "closed_caption", "biphase",    "colour_frame_phase",
    "disc_quality",   "fm_code",    "white_flag"};

TEST(CoreObservationService, AvailableObservers_ExposesEveryStandardObserver) {
  CoreObservationService service;
  const auto infos = service.available_observers();

  ASSERT_EQ(infos.size(), kExpectedObserverIds.size());

  std::set<std::string> seen_ids;
  for (const auto& info : infos) {
    EXPECT_FALSE(info.id.empty());
    EXPECT_FALSE(info.version.empty()) << "id=" << info.id;
    EXPECT_FALSE(info.provided_observations.empty()) << "id=" << info.id;
    EXPECT_TRUE(seen_ids.insert(info.id).second) << "duplicate id: " << info.id;
  }

  EXPECT_EQ(seen_ids, kExpectedObserverIds);
}

// Independent restatement of the statefulness contract (ObserverInfo::stateless
// / ObserverRegistryEntry::stateless). Only observers modelling a cross-frame
// stream are stateful; a drift in either direction fails this test. See the
// classification comment in core_observation_service.cpp.
const std::map<std::string, bool> kExpectedStateless{
    {"white_snr", true},    {"black_psnr", true},
    {"burst_level", true},  {"closed_caption", false},
    {"biphase", true},      {"colour_frame_phase", false},
    {"disc_quality", true}, {"fm_code", true},
    {"white_flag", true}};

TEST(CoreObservationService, AvailableObservers_ClassifiesStatefulness) {
  CoreObservationService service;
  const auto infos = service.available_observers();

  ASSERT_EQ(infos.size(), kExpectedStateless.size());
  for (const auto& info : infos) {
    const auto it = kExpectedStateless.find(info.id);
    ASSERT_NE(it, kExpectedStateless.end()) << "unclassified id: " << info.id;
    EXPECT_EQ(info.stateless, it->second) << "id=" << info.id;
  }
}

TEST(CoreObservationService, CreateObserver_ReturnsHandle_ForEveryKnownId) {
  CoreObservationService service;
  for (const auto& id : kExpectedObserverIds) {
    auto handle = service.create_observer(id);
    EXPECT_NE(handle, nullptr) << "id=" << id;
  }
}

TEST(CoreObservationService, CreateObserver_ReturnsNull_ForUnknownId) {
  CoreObservationService service;
  EXPECT_EQ(service.create_observer("does_not_exist"), nullptr);
  EXPECT_EQ(service.create_observer(""), nullptr);
}

TEST(CoreObservationService, RunObserver_ReturnsFalse_ForUnknownId) {
  CoreObservationService service;
  NiceMock<orc_unit_test::MockVideoFrameRepresentation> vfr;
  // A strict context: an unknown id must not touch the context at all.
  ::testing::StrictMock<orc_unit_test::MockObservationContext> context;

  EXPECT_FALSE(
      service.run_observer("does_not_exist", vfr, FrameID(0), context));
}

TEST(CoreObservationService, RunObserver_DeliversResults_ThroughContext) {
  CoreObservationService service;

  // FrameQualityObserver writes zeroed disc_quality metrics for the frame when
  // it carries no video parameters — a deterministic path that needs no real
  // sample data.
  NiceMock<orc_unit_test::MockVideoFrameRepresentation> vfr;
  ON_CALL(vfr, get_video_parameters())
      .WillByDefault(Return(std::optional<SourceParameters>{}));

  orc_unit_test::MockObservationContext context;
  // One frame-level result keyed by the first field (frame 0 -> field id 0).
  EXPECT_CALL(context, set(FieldID(0), "disc_quality", "quality_score", _))
      .Times(1);
  EXPECT_CALL(context, set(FieldID(0), "disc_quality", "dropout_count", _))
      .Times(1);
  EXPECT_CALL(context, set(FieldID(0), "disc_quality", "phase_valid", _))
      .Times(1);

  EXPECT_TRUE(service.run_observer("disc_quality", vfr, FrameID(0), context));
}

TEST(CoreObservationService, CreateObserver_HandleForwardsToObserver) {
  CoreObservationService service;
  auto handle = service.create_observer("disc_quality");
  ASSERT_NE(handle, nullptr);

  NiceMock<orc_unit_test::MockVideoFrameRepresentation> vfr;
  ON_CALL(vfr, get_video_parameters())
      .WillByDefault(Return(std::optional<SourceParameters>{}));

  orc_unit_test::MockObservationContext context;
  EXPECT_CALL(context, set(_, "disc_quality", _, _)).Times(3);

  handle->process_frame(vfr, FrameID(0), context);
}

// ---------------------------------------------------------------------------
// Applicability (standard_observer_applies / filter_applicable_observers)
// ---------------------------------------------------------------------------

SourceParameters params_for(VideoSystem system) {
  SourceParameters params;
  params.system = system;
  return params;
}

// Independent restatement of the applicability contract (Observer::applies_to
// per observer): fm_code/white_flag are NTSC-only, closed_caption is 525-line
// only (NTSC + PAL_M), everything else applies to every system. A drift in
// either direction fails this test.
const std::map<std::string, std::set<VideoSystem>> kExpectedApplicability{
    {"white_snr", {VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M}},
    {"black_psnr", {VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M}},
    {"burst_level", {VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M}},
    {"closed_caption", {VideoSystem::NTSC, VideoSystem::PAL_M}},
    {"biphase", {VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M}},
    {"colour_frame_phase",
     {VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M}},
    {"disc_quality", {VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M}},
    {"fm_code", {VideoSystem::NTSC}},
    {"white_flag", {VideoSystem::NTSC}},
};

TEST(StandardObserverApplies, MatchesApplicabilityContract) {
  for (const auto& [id, systems] : kExpectedApplicability) {
    for (const VideoSystem system :
         {VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M}) {
      EXPECT_EQ(standard_observer_applies(id, params_for(system)),
                systems.count(system) != 0)
          << "id=" << id << " system=" << static_cast<int>(system);
    }
  }
}

TEST(StandardObserverApplies, UnknownIdIsAlwaysApplicable) {
  // Observers injected by a test service must never be filtered.
  EXPECT_TRUE(standard_observer_applies("not_a_registry_id",
                                        params_for(VideoSystem::PAL)));
  EXPECT_TRUE(standard_observer_applies("", params_for(VideoSystem::NTSC)));
}

TEST(FilterApplicableObservers, DropsInapplicableObserversPerSystem) {
  CoreObservationService service;
  const auto all = service.available_observers();

  const auto id_set = [](const std::vector<ObserverInfo>& infos) {
    std::set<std::string> ids;
    for (const auto& info : infos) {
      ids.insert(info.id);
    }
    return ids;
  };

  auto pal_expected = kExpectedObserverIds;
  pal_expected.erase("fm_code");
  pal_expected.erase("white_flag");
  pal_expected.erase("closed_caption");
  EXPECT_EQ(
      id_set(filter_applicable_observers(all, params_for(VideoSystem::PAL))),
      pal_expected);

  const auto& ntsc_expected = kExpectedObserverIds;
  EXPECT_EQ(
      id_set(filter_applicable_observers(all, params_for(VideoSystem::NTSC))),
      ntsc_expected);
}

TEST(FilterApplicableObservers, FailsOpenWithoutVideoParameters) {
  CoreObservationService service;
  const auto all = service.available_observers();
  const auto filtered = filter_applicable_observers(all, std::nullopt);
  ASSERT_EQ(filtered.size(), all.size());
  for (size_t i = 0; i < all.size(); ++i) {
    EXPECT_EQ(filtered[i].id, all[i].id);
  }
}

TEST(FilterApplicableObservers, KeepsUnknownObserverIds) {
  ObserverInfo custom;
  custom.id = "test_only_observer";
  custom.version = "1.0.0";
  const auto filtered =
      filter_applicable_observers({custom}, params_for(VideoSystem::NTSC));
  ASSERT_EQ(filtered.size(), 1u);
  EXPECT_EQ(filtered[0].id, "test_only_observer");
}

}  // namespace
}  // namespace tests
}  // namespace orc
