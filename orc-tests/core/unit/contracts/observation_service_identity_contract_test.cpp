/*
 * File:        observation_service_identity_contract_test.cpp
 * Module:      orc-tests/core/unit/contracts
 * Purpose:     Tie CoreObservationService identity to the observers it runs
 *
 * Guards against drift between the service's id scheme and the observation
 * schema each observer registers (the same schema aggregated by
 * dag_executor.cpp). For every observer the service advertises, its
 * ObserverInfo must match the concrete observer's version and
 * provided-observation keys, and the service id must be one of the namespaces
 * those keys use.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <biphase_observer.h>
#include <black_psnr_observer.h>
#include <burst_level_observer.h>
#include <closed_caption_observer.h>
#include <colour_frame_phase_observer.h>
#include <fm_code_observer.h>
#include <frame_quality_observer.h>
#include <gtest/gtest.h>
#include <observer.h>
#include <orc/stage/observation/observation_service_interface.h>
#include <white_flag_observer.h>
#include <white_snr_observer.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core_observation_service.h"

namespace orc {
namespace tests {
namespace {

// Independent restatement of the id -> concrete observer binding. If the
// service's registry changes without this table changing (or vice versa) the
// test fails, pinning the public id scheme to the concrete observers.
struct ExpectedObserver {
  std::string id;
  std::function<std::unique_ptr<Observer>()> make;
};

std::vector<ExpectedObserver> expected_observers() {
  return {
      {"white_snr", [] { return std::make_unique<WhiteSNRObserver>(); }},
      {"black_psnr", [] { return std::make_unique<BlackPSNRObserver>(); }},
      {"burst_level", [] { return std::make_unique<BurstLevelObserver>(); }},
      {"closed_caption",
       [] { return std::make_unique<ClosedCaptionObserver>(); }},
      {"biphase", [] { return std::make_unique<BiphaseObserver>(); }},
      {"colour_frame_phase",
       [] { return std::make_unique<ColourFramePhaseObserver>(); }},
      {"disc_quality", [] { return std::make_unique<FrameQualityObserver>(); }},
      {"fm_code", [] { return std::make_unique<FmCodeObserver>(); }},
      {"white_flag", [] { return std::make_unique<WhiteFlagObserver>(); }},
  };
}

const ObserverInfo* find_info(const std::vector<ObserverInfo>& infos,
                              const std::string& id) {
  for (const auto& info : infos) {
    if (info.id == id) {
      return &info;
    }
  }
  return nullptr;
}

TEST(ObservationServiceIdentityContract,
     ObserverInfoMatchesConcreteObserverOutput) {
  CoreObservationService service;
  const auto infos = service.available_observers();

  const auto expected = expected_observers();
  EXPECT_EQ(infos.size(), expected.size());

  for (const auto& exp : expected) {
    const ObserverInfo* info = find_info(infos, exp.id);
    ASSERT_NE(info, nullptr) << "service does not advertise id: " << exp.id;

    auto observer = exp.make();
    EXPECT_EQ(info->version, observer->observer_version()) << "id=" << exp.id;

    const auto keys = observer->get_provided_observations();
    ASSERT_EQ(info->provided_observations.size(), keys.size())
        << "id=" << exp.id;
    for (size_t i = 0; i < keys.size(); ++i) {
      EXPECT_EQ(info->provided_observations[i].namespace_, keys[i].namespace_)
          << "id=" << exp.id << " index=" << i;
      EXPECT_EQ(info->provided_observations[i].name, keys[i].name)
          << "id=" << exp.id << " index=" << i;
    }

    // The service id must be one of the namespaces the observer writes, so the
    // string identity cannot drift from the registered schema.
    std::set<std::string> namespaces;
    for (const auto& key : keys) {
      namespaces.insert(key.namespace_);
    }
    EXPECT_TRUE(namespaces.count(exp.id) == 1)
        << "id '" << exp.id << "' is not among the observer's namespaces";
  }
}

}  // namespace
}  // namespace tests
}  // namespace orc
