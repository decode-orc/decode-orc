/*
 * File:        fake_audio_output.h
 * Module:      orc-tests/gui/unit
 * Purpose:     Scripted, device-free IAudioOutput for playback controller tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "audio_output.h"

namespace orc::gui::test {

/**
 * @brief Simulated audio device: a fixed-depth queue the test drains by hand.
 *
 * Models exactly what the controller relies on — bounded free space, partial
 * accepts, and a clock that only advances as queued audio is consumed — with
 * no real device, no timers and no wall clock anywhere.
 */
class FakeAudioOutput final : public IAudioOutput {
 public:
  // Default depth is the 200 ms the Qt implementation asks for at 48 kHz.
  explicit FakeAudioOutput(size_t capacity_pairs = 9600)
      : capacity_pairs_(capacity_pairs) {}

  bool start(uint32_t sample_rate_hz, uint32_t channels) override {
    ++start_calls_;
    requested_sample_rate_hz_ = sample_rate_hz;
    requested_channels_ = channels;
    if (!start_succeeds_) {
      return false;
    }
    started_ = true;
    queued_pairs_ = 0;
    played_pairs_ = 0;
    written_.clear();
    return true;
  }

  void stop() override {
    if (started_) {
      ++stop_calls_;
    }
    started_ = false;
    queued_pairs_ = 0;
  }

  size_t stereoPairsFree() const override {
    return started_ ? capacity_pairs_ - queued_pairs_ : 0;
  }

  size_t write(const float* interleaved, size_t stereo_pairs) override {
    if (!started_ || interleaved == nullptr) {
      return 0;
    }
    // Spelled out because size_t and uint64_t are distinct types where
    // size_t is unsigned long and uint64_t unsigned long long (macOS), which
    // leaves std::min nothing to deduce.
    const size_t accepted = static_cast<size_t>(
        std::min<uint64_t>(stereo_pairs, capacity_pairs_ - queued_pairs_));
    written_.insert(written_.end(), interleaved, interleaved + accepted * 2);
    queued_pairs_ += accepted;
    return accepted;
  }

  uint64_t playedMicroseconds() const override {
    if (requested_sample_rate_hz_ == 0) {
      return 0;
    }
    // The earliest microsecond at which played_pairs_ have finished, i.e. the
    // ceiling. Rounding down instead would make the caller's microseconds →
    // pairs conversion report one pair fewer than has actually played at pair
    // counts that are not a whole number of microseconds.
    return (played_pairs_ * 1000000ULL + requested_sample_rate_hz_ - 1) /
           requested_sample_rate_hz_;
  }

  void setVolume(double linear) override {
    volume_ = linear;
    volume_history_.push_back(linear);
  }

  // ---- Test drivers -------------------------------------------------------

  /// Consume up to @p stereo_pairs of queued audio, advancing the clock.
  void playPairs(uint64_t stereo_pairs) {
    const uint64_t consumed = std::min<uint64_t>(stereo_pairs, queued_pairs_);
    queued_pairs_ -= consumed;
    played_pairs_ += consumed;
  }

  /// Consume everything queued — what a device does when nobody feeds it.
  void playAllQueued() { playPairs(queued_pairs_); }

  /// Make the next start() fail, as a machine with no usable device would.
  void setStartSucceeds(bool succeeds) { start_succeeds_ = succeeds; }

  // ---- Observations -------------------------------------------------------

  bool isStarted() const { return started_; }
  int startCalls() const { return start_calls_; }
  int stopCalls() const { return stop_calls_; }
  uint32_t requestedSampleRateHz() const { return requested_sample_rate_hz_; }
  uint32_t requestedChannels() const { return requested_channels_; }
  uint64_t queuedPairs() const { return queued_pairs_; }
  uint64_t playedPairs() const { return played_pairs_; }
  double volume() const { return volume_; }
  const std::vector<double>& volumeHistory() const { return volume_history_; }
  /// Every sample handed to the device since the last start(), interleaved.
  const std::vector<float>& written() const { return written_; }
  uint64_t writtenPairs() const { return written_.size() / 2; }

 private:
  size_t capacity_pairs_;
  bool started_ = false;
  bool start_succeeds_ = true;
  int start_calls_ = 0;
  int stop_calls_ = 0;
  uint32_t requested_sample_rate_hz_ = 0;
  uint32_t requested_channels_ = 0;
  uint64_t queued_pairs_ = 0;
  uint64_t played_pairs_ = 0;
  double volume_ = 1.0;
  std::vector<double> volume_history_;
  std::vector<float> written_;
};

}  // namespace orc::gui::test
