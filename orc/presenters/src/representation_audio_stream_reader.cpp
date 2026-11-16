/*
 * File:        representation_audio_stream_reader.cpp
 * Module:      orc-presenters
 * Purpose:     Frame → float sample reads over a resolved representation's
 *              audio channel pair
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "representation_audio_stream_reader.h"

#include <orc/stage/audio/audio_sample_feed.h>

#include <algorithm>

namespace orc::presenters {

namespace {

// Frame containing absolute stereo-pair stream position |pos|. PAL is a plain
// constant stride; NTSC/PAL-M inverts the cumulative round(n × 8008 / 5)
// mapping by estimate-then-correct, so the 1602/1601 cadence stays exact at
// every position (SMPTE 272M-1994 §14.3 Table 1). Same arithmetic as
// audio_align's frame_containing_pair().
uint64_t frame_containing_pair(uint64_t pos, orc::VideoSystem system) {
  switch (system) {
    case orc::VideoSystem::PAL:
    case orc::VideoSystem::SECAM:
      // ITU-R BT.1700 Annex 1 Part B (625-line PAL, and SECAM on the same
      // raster): 1920 pairs per frame.
      return pos / 1920u;
    case orc::VideoSystem::NTSC:
    case orc::VideoSystem::PAL_M: {
      uint64_t n = pos * 5u / 8008u;
      while (orc::audio_pair_offset(n + 1, system) <= pos) ++n;
      while (n > 0 && orc::audio_pair_offset(n, system) > pos) --n;
      return n;
    }
    case orc::VideoSystem::Unknown:
      break;
  }
  // VideoSystem::Unknown has no defined audio layout.
  return 0;
}

// Lower-case spelling of an AudioOrigin for the view layer, so the GUI/CLI need
// no knowledge of the SDK enum.
const char* audio_origin_name(orc::AudioOrigin origin) {
  switch (origin) {
    case orc::AudioOrigin::ANALOGUE:
      return "analogue";
    case orc::AudioOrigin::HIFI:
      return "hifi";
    case orc::AudioOrigin::EFM:
      return "efm";
    case orc::AudioOrigin::IMPORTED:
      return "imported";
    case orc::AudioOrigin::DERIVED:
      return "derived";
    case orc::AudioOrigin::UNKNOWN:
    default:
      return "unknown";
  }
}

// The representation's video system, or Unknown when it reports no parameters.
orc::VideoSystem system_of(
    const orc::VideoFrameRepresentation& representation) {
  const auto params = representation.get_video_parameters();
  return params ? params->system : orc::VideoSystem::Unknown;
}

// True when the system fixes a usable audio cadence. audio_pairs_in_frame()
// returns 0 for VideoSystem::Unknown, which is exactly the "no addressable
// audio" condition.
bool cadence_is_defined(orc::VideoSystem system) {
  return orc::audio_pairs_in_frame(0, system) > 0;
}

}  // namespace

std::vector<orc::AudioPairView> enumerate_audio_channel_pairs(
    const orc::VideoFrameRepresentation& representation) {
  std::vector<orc::AudioPairView> pairs;
  if (!cadence_is_defined(system_of(representation))) {
    return pairs;
  }

  const size_t count = representation.audio_channel_pair_count();
  pairs.reserve(count);
  for (size_t p = 0; p < count; ++p) {
    const auto descriptor = representation.get_audio_channel_pair_descriptor(p);
    orc::AudioPairView view;
    view.index = p;
    view.name = descriptor ? descriptor->name : std::string();
    view.origin = audio_origin_name(descriptor ? descriptor->origin
                                               : orc::AudioOrigin::UNKNOWN);
    pairs.push_back(std::move(view));
  }
  return pairs;
}

std::shared_ptr<IAudioStreamReader> make_audio_stream_reader(
    std::shared_ptr<const orc::VideoFrameRepresentation> representation,
    size_t pair) {
  if (!representation) {
    return nullptr;
  }
  if (pair >= representation->audio_channel_pair_count()) {
    return nullptr;
  }
  const orc::VideoSystem system = system_of(*representation);
  if (!cadence_is_defined(system)) {
    return nullptr;
  }
  return std::make_shared<RepresentationAudioStreamReader>(
      std::move(representation), pair, system);
}

RepresentationAudioStreamReader::RepresentationAudioStreamReader(
    std::shared_ptr<const orc::VideoFrameRepresentation> representation,
    size_t pair, orc::VideoSystem system)
    : representation_(std::move(representation)),
      pair_(pair),
      system_(system),
      range_(representation_ ? representation_->frame_range()
                             : orc::FrameIDRange{0, 0}) {
  // An empty representation must not present frame 0 as readable.
  if (!representation_) {
    range_ = orc::FrameIDRange{1, 0};
  }
}

void RepresentationAudioStreamReader::prime(
    const AudioPrimeProgressCallback& progress) {
  if (!representation_) {
    return;
  }
  // AudioPrimeProgressCallback is signature-compatible with the SDK's
  // AudioDecodeProgressFn, so this forwards straight to the producer.
  representation_->prime_audio_decode(progress);
}

std::vector<float> RepresentationAudioStreamReader::readFrames(
    orc::FrameID first_frame, uint64_t frame_count) {
  std::vector<float> out;
  if (!representation_ || frame_count == 0 || range_.empty()) {
    return out;
  }

  // Clamp the requested run to the readable range.
  const orc::FrameID first = std::max(first_frame, range_.first);
  if (first > range_.last) {
    return out;
  }
  const uint64_t requested_end = first_frame + frame_count;
  if (requested_end <= first) {
    return out;
  }
  const orc::FrameID last = std::min(requested_end - 1, range_.last);

  // Size the output from the cadence, not from what the producer returns: a
  // short or missing block becomes silence rather than a shortened buffer, so
  // the audio clock stays aligned with the frame timeline.
  const uint64_t total_pairs = orc::audio_pair_offset(last + 1, system_) -
                               orc::audio_pair_offset(first, system_);
  out.assign(static_cast<size_t>(total_pairs) * 2, 0.0f);

  size_t write = 0;
  for (orc::FrameID frame = first; frame <= last; ++frame) {
    const size_t expected_samples =
        static_cast<size_t>(orc::audio_pairs_in_frame(frame, system_)) * 2;
    const std::vector<int32_t> carrier =
        representation_->get_audio_samples(pair_, frame);
    // A representation may legitimately return nothing for a frame (e.g. a
    // placeholder pair); the pre-zeroed span then plays as silence. Anything
    // longer than the cadence allows is truncated for the same reason.
    const size_t convert = std::min(carrier.size(), expected_samples);
    for (size_t s = 0; s < convert; ++s) {
      out[write + s] = orc::audio_carrier_to_float(carrier[s]);
    }
    write += expected_samples;
  }

  return out;
}

uint64_t RepresentationAudioStreamReader::frameForPairPosition(
    uint64_t pair_position) const {
  return frame_containing_pair(pair_position, system_);
}

uint64_t RepresentationAudioStreamReader::pairPositionForFrame(
    orc::FrameID frame) const {
  return orc::audio_pair_offset(frame, system_);
}

orc::FrameIDRange RepresentationAudioStreamReader::frameRange() const {
  return range_;
}

}  // namespace orc::presenters
