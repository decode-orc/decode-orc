/*
 * File:        core_observation_service.cpp
 * Module:      orc-core
 * Purpose:     Host implementation of the plugin-facing IObservationService
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "core_observation_service.h"

#include <biphase_observer.h>
#include <black_psnr_observer.h>
#include <burst_level_observer.h>
#include <closed_caption_observer.h>
#include <colour_frame_phase_observer.h>
#include <fm_code_observer.h>
#include <frame_quality_observer.h>
#include <observer.h>
#include <orc/stage/video_frame_representation.h>
#include <white_flag_observer.h>
#include <white_snr_observer.h>

#include <array>
#include <utility>

namespace orc {

namespace {

// Factory signature for a single standard observer.
using ObserverFactory = std::unique_ptr<Observer> (*)();

// Registry entry binding a stable id to its observer factory.
struct ObserverRegistryEntry {
  const char* id;
  ObserverFactory factory;
  // True when the observer's output for a frame depends only on that frame's
  // data (surfaced as ObserverInfo::stateless). False for observers that model
  // a cross-frame stream and must be driven over a work item's frames in
  // ascending order. See ObserverInfo::stateless for the scheduler contract.
  bool stateless;
};

template <typename T>
std::unique_ptr<Observer> make_observer() {
  return std::make_unique<T>();
}

// Single source of truth for observer identity. The id strings are the stable
// public contract (they reuse each observer's primary observation namespace)
// and must never be renamed. The enumeration order fixes the order of
// available_observers().
//
// Statefulness classification (the `stateless` column): every standard observer
// computes each frame's measurement purely from that frame's samples, with two
// exceptions that model cross-frame streams and are therefore marked stateful
// so the background scheduler observes their frames in ascending order:
//   - closed_caption: EIA-608 line-21 bytes form a caption stream whose control
//     codes and pop-on/paint-on modes span successive frames.
//   - colour_frame_phase: the 4-/8-field colour-sequence phase is only
//     meaningful as an ordered progression across frames.
constexpr std::array<ObserverRegistryEntry, 9> kObserverRegistry{{
    {"white_snr", &make_observer<WhiteSNRObserver>, /*stateless=*/true},
    {"black_psnr", &make_observer<BlackPSNRObserver>, /*stateless=*/true},
    {"burst_level", &make_observer<BurstLevelObserver>, /*stateless=*/true},
    {"closed_caption", &make_observer<ClosedCaptionObserver>,
     /*stateless=*/false},
    {"biphase", &make_observer<BiphaseObserver>, /*stateless=*/true},
    {"colour_frame_phase", &make_observer<ColourFramePhaseObserver>,
     /*stateless=*/false},
    {"disc_quality", &make_observer<FrameQualityObserver>, /*stateless=*/true},
    {"fm_code", &make_observer<FmCodeObserver>, /*stateless=*/true},
    {"white_flag", &make_observer<WhiteFlagObserver>, /*stateless=*/true},
}};

// Look up a factory by id. Returns nullptr for an unknown id (no throw).
ObserverFactory find_factory(const std::string& observer_id) {
  for (const auto& entry : kObserverRegistry) {
    if (observer_id == entry.id) {
      return entry.factory;
    }
  }
  return nullptr;
}

// Handle wrapping a host-allocated observer instance. Per-caller state (e.g.
// ClosedCaptionObserver's cross-field pairing) lives in the owned observer.
class CoreObserverHandle final : public IObserverHandle {
 public:
  explicit CoreObserverHandle(std::unique_ptr<Observer> observer)
      : observer_(std::move(observer)) {}

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override {
    observer_->process_frame(representation, frame_id, context);
  }

 private:
  std::unique_ptr<Observer> observer_;
};

}  // namespace

std::vector<ObserverInfo> CoreObservationService::available_observers() const {
  std::vector<ObserverInfo> infos;
  infos.reserve(kObserverRegistry.size());
  for (const auto& entry : kObserverRegistry) {
    auto observer = entry.factory();
    infos.push_back(ObserverInfo{entry.id, observer->observer_version(),
                                 observer->get_provided_observations(),
                                 entry.stateless});
  }
  return infos;
}

std::unique_ptr<IObserverHandle> CoreObservationService::create_observer(
    const std::string& observer_id) const {
  ObserverFactory factory = find_factory(observer_id);
  if (!factory) {
    return nullptr;
  }
  return std::make_unique<CoreObserverHandle>(factory());
}

bool CoreObservationService::run_observer(
    const std::string& observer_id,
    const VideoFrameRepresentation& representation, FrameID frame_id,
    IObservationContext& context) const {
  ObserverFactory factory = find_factory(observer_id);
  if (!factory) {
    return false;
  }
  auto observer = factory();
  observer->process_frame(representation, frame_id, context);
  return true;
}

bool standard_observer_applies(const std::string& observer_id,
                               const SourceParameters& params) {
  // One shared instance per registry entry, created on first use (magic
  // statics make the initialisation thread-safe). applies_to() is const, pure
  // and thread-safe by contract, so sharing is safe; process_frame() is never
  // called on these instances.
  static const auto& instances = *[] {
    auto* arr =
        new std::array<std::unique_ptr<Observer>, kObserverRegistry.size()>();
    for (std::size_t i = 0; i < kObserverRegistry.size(); ++i) {
      (*arr)[i] = kObserverRegistry[i].factory();
    }
    return arr;
  }();
  for (std::size_t i = 0; i < kObserverRegistry.size(); ++i) {
    if (observer_id == kObserverRegistry[i].id) {
      return instances[i]->applies_to(params);
    }
  }
  // Unknown (e.g. test-injected) observers are never filtered.
  return true;
}

std::vector<ObserverInfo> filter_applicable_observers(
    const std::vector<ObserverInfo>& observers,
    const std::optional<SourceParameters>& params) {
  if (!params.has_value()) {
    return observers;  // fail open — see the header contract
  }
  std::vector<ObserverInfo> applicable;
  applicable.reserve(observers.size());
  for (const auto& observer : observers) {
    if (standard_observer_applies(observer.id, *params)) {
      applicable.push_back(observer);
    }
  }
  return applicable;
}

}  // namespace orc
