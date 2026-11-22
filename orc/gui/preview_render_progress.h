/*
 * File:        preview_render_progress.h
 * Module:      orc-gui
 * Purpose:     Pure helper that turns the time a preview render has been
 *              running into the preview window's "Rendering..." text, with a
 *              predicted percentage learned from earlier renders of the same
 *              view (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>

namespace orc::gui {

// Nothing in the render path reports how far through a frame it is: the slow
// work happens inside the stages' frame pulls, not in the graph walk. What can
// be known is how long the same view took last time, and stepping through a
// slow stage costs much the same frame after frame. So the text is a
// prediction when there is history ("Rendering... ~40%") and an honest
// elapsed-seconds counter when there is none, or when the render has already
// outlived its prediction ("Rendering... 12 s").
//
// Times are milliseconds from any monotonic clock the caller owns; the class
// never reads a clock itself.
class PreviewRenderProgress {
 public:
  // Renders shorter than this never show any text.
  static constexpr std::int64_t kShowAfterMs = 2000;
  // Renders shorter than this are not learned from: they are almost all cache
  // hits, and would make the next real render look nearly done at once.
  static constexpr std::int64_t kLearnFromMs = 1000;
  // A prediction never claims to be finished before the frame arrives.
  static constexpr int kMaxPredictedPercent = 95;

  // A request has been sent. An empty key means "nothing to predict from"
  // (e.g. the query for a stage's available outputs), so only elapsed time is
  // shown. A begin while a request is already in flight (stage selection
  // followed by its first preview) keeps the wait that is already on screen
  // going rather than starting it again from zero.
  void begin(const std::string& key, std::int64_t now_ms) {
    if (!in_flight_) {
      wait_start_ms_ = now_ms;
    }
    in_flight_ = true;
    key_ = key;
    request_start_ms_ = now_ms;
  }

  // The request finished. A successful keyed render long enough to be real
  // work updates the expectation for its key.
  void finish(bool success, std::int64_t now_ms) {
    if (!in_flight_) {
      return;
    }
    const std::int64_t duration = now_ms - request_start_ms_;
    if (success && !key_.empty() && duration >= kLearnFromMs) {
      auto it = expected_ms_.find(key_);
      if (it == expected_ms_.end()) {
        expected_ms_.emplace(key_, static_cast<double>(duration));
      } else {
        // Halfway between the old figure and the new one: follows a stage
        // whose cost has changed (new parameters) within a couple of frames
        // without one odd frame throwing the next prediction out.
        it->second = 0.5 * it->second + 0.5 * static_cast<double>(duration);
      }
    }
    in_flight_ = false;
    key_.clear();
  }

  // The request ended without a frame (error, or the graph was replaced).
  void abandon() {
    in_flight_ = false;
    key_.clear();
  }

  bool inFlight() const { return in_flight_; }

  // Text for the preview window's title, or empty while there is nothing to
  // say (no render in flight, or one that has not yet been slow).
  std::string statusText(std::int64_t now_ms) const {
    if (!in_flight_) {
      return {};
    }
    const std::int64_t waited = now_ms - wait_start_ms_;
    if (waited < kShowAfterMs) {
      return {};
    }

    if (!key_.empty()) {
      const auto it = expected_ms_.find(key_);
      const double running = static_cast<double>(now_ms - request_start_ms_);
      if (it != expected_ms_.end() && running <= it->second) {
        const int percent =
            std::clamp(static_cast<int>(running * 100.0 / it->second), 1,
                       kMaxPredictedPercent);
        return "Rendering... ~" + std::to_string(percent) + "%";
      }
    }
    return "Rendering... " + std::to_string(waited / 1000) + " s";
  }

  // Forget what earlier renders cost (a different project, whose node ids
  // name different stages). Any render in flight carries on.
  void forgetPredictions() { expected_ms_.clear(); }

 private:
  bool in_flight_ = false;
  std::string key_;
  std::int64_t wait_start_ms_ = 0;
  std::int64_t request_start_ms_ = 0;
  std::map<std::string, double> expected_ms_;
};

}  // namespace orc::gui
