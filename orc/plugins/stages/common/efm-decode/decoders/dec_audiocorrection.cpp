/*
 * File:        dec_audiocorrection.cpp
 * Purpose:     efm-decoder-audio - EFM Data24 to Audio decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dec_audiocorrection.h"

#include <fmt/format.h>

#include <cmath>
#include <utility>

#include "../efm-lib/efm_constants.h"

namespace {

// Section/frame geometry (98 frames of 12 mono samples = 6 stereo pairs each).
constexpr int kFramesPerSection = efm::kFramesPerSection;
constexpr int kSamplesPerChannelPerFrame = 6;
constexpr int kSamplesPerFrame = 12;  // 6 stereo pairs, interleaved L,R
constexpr int kSamplesPerChannel =
    kFramesPerSection * kSamplesPerChannelPerFrame;  // 588

// E-6 concealment strategy: bridge short bursts (up to this many consecutive
// flagged samples, per channel) by linear interpolation between the flanking
// clean samples; mute anything longer.  A single-sample gap reduces to the mean
// of its neighbours, matching the original conceal behaviour.
constexpr int kMaxInterpolationGap = 8;

// Length (per channel) of the fade applied at each end of a muted burst so that
// the transition to/from silence does not produce an audible click.
constexpr int kMuteRampSamples = 4;

// Per-channel sample latency of the CIRC de-interleaver. Subcode is carried in
// the F3 frame and is NOT de-interleaved, so a section's Q-channel time refers
// to data that emerges from the de-interleaver this many samples later. Region
// attribution compensates for the lag so that a burst is credited to the track
// a listener actually hears it in.
constexpr int kLatencySamples = efm::kDeinterleaveLatencySamples;  // 666

// Map a section's Q-channel metadata onto a disc region.
DiscRegion regionForMetadata(const SectionMetadata& metadata) {
  switch (metadata.sectionType().type()) {
    case SectionType::LeadIn:
      return DiscRegion::LeadIn;
    case SectionType::LeadOut:
      return DiscRegion::LeadOut;
    case SectionType::UserData:
    default:
      // IEC 60908-1999 §17.5.1: in the programme area INDEX 00 marks a pause
      // (the pre-gap preceding a track's audio); 01 and above are the running
      // index within the track proper.
      return metadata.isPause() ? DiscRegion::Pause : DiscRegion::Programme;
  }
}

}  // namespace

std::string discRegionName(DiscRegion region) {
  switch (region) {
    case DiscRegion::LeadIn:
      return "Lead-in";
    case DiscRegion::Pause:
      return "Pause (pre-gap)";
    case DiscRegion::Programme:
      return "Programme";
    case DiscRegion::LeadOut:
      return "Lead-out";
    case DiscRegion::OutsideProgrammeArea:
    default:
      return "Outside programme area";
  }
}

AudioCorrection::AudioCorrection()
    : m_nextOrdinal(0),
      m_historyBase(0),
      m_firstSectionCorrected(false),
      m_concealedSamplesCount(0),
      m_silencedSamplesCount(0),
      m_validSamplesCount(0),
      m_warmupSamplesCount(0),
      m_drainSamplesCount(0),
      m_gapFillerSamplesCount(0) {}

void AudioCorrection::pushSection(const AudioSection& audioSection) {
  // Add the data to the input buffer
  m_inputBuffer.push_back(audioSection);

  // Process the queue
  processQueue();
}

void AudioCorrection::pushSection(AudioSection&& audioSection) {
  // Move the data into the input buffer to avoid a deep copy.
  m_inputBuffer.push_back(std::move(audioSection));

  // Process the queue
  processQueue();
}

AudioSection AudioCorrection::popSection() {
  // Move the first item out of the output buffer to avoid a deep copy.
  AudioSection result = std::move(m_outputBuffer.front());
  m_outputBuffer.pop_front();
  return result;
}

bool AudioCorrection::isReady() const {
  // Return true if the output buffer is not empty
  return !m_outputBuffer.empty();
}

void AudioCorrection::processQueue() {
  // R-5: guard against an empty input buffer (defensive - the only caller
  // pushes before calling, but an empty front() would be undefined behaviour).
  if (m_inputBuffer.empty()) return;

  // Pop a section from the input buffer (move to avoid a deep copy)
  recordSectionMetadata(m_inputBuffer.front().metadata);
  m_correctionBuffer.push_back(std::move(m_inputBuffer.front()));
  m_correctionOrdinals.push_back(m_nextOrdinal++);
  m_inputBuffer.pop_front();

  // A section can be corrected once its following neighbour is available.  When
  // the buffer is full its layout is:
  //   [0] preceding (already corrected), [1] correcting, [2] following (raw)
  if (m_correctionBuffer.size() == 3) {
    // E-6 edge priming (start of stream): the very first section has no
    // preceding neighbour, so correct it now - using the second section as its
    // following context - before it is emitted.
    if (!m_firstSectionCorrected) {
      m_correctionBuffer[0] =
          correctSection(nullptr, m_correctionBuffer.at(0),
                         &m_correctionBuffer.at(1), m_correctionOrdinals.at(0));
      m_firstSectionCorrected = true;
    }

    m_correctionBuffer[1] =
        correctSection(&m_correctionBuffer.at(0), m_correctionBuffer.at(1),
                       &m_correctionBuffer.at(2), m_correctionOrdinals.at(1));

    // Emit the (now fully corrected) preceding section.
    m_outputBuffer.push_back(std::move(m_correctionBuffer.at(0)));
    m_correctionBuffer.erase(m_correctionBuffer.begin());
    m_correctionOrdinals.erase(m_correctionOrdinals.begin());
  }
}

void AudioCorrection::recordSectionMetadata(const SectionMetadata& metadata) {
  // The de-interleave latency reaches at most two sections back, so keep a
  // short window rather than the whole disc.
  constexpr size_t kHistoryDepth = 4;
  m_metadataHistory.push_back(metadata);
  while (m_metadataHistory.size() > kHistoryDepth) {
    m_metadataHistory.pop_front();
    ++m_historyBase;
  }
}

const SectionMetadata* AudioCorrection::discOriginOf(uint64_t ordinal,
                                                     int offset) const {
  // Position of this sample in the emitted per-channel stream, then the disc
  // position it actually came from once the de-interleave lag is removed.
  const uint64_t emitted = ordinal * static_cast<uint64_t>(kSamplesPerChannel) +
                           static_cast<uint64_t>(offset);
  if (emitted < static_cast<uint64_t>(kLatencySamples)) {
    // Predates the stream: this sample is de-interleaver warm-up filler and
    // corresponds to no disc position at all.
    return nullptr;
  }
  const uint64_t discPosition = emitted - kLatencySamples;
  const uint64_t discSection = discPosition / kSamplesPerChannel;
  if (discSection < m_historyBase) return nullptr;
  size_t index = static_cast<size_t>(discSection - m_historyBase);
  if (index >= m_metadataHistory.size()) return nullptr;

  // A section whose Q-channel CRC failed carries default metadata (UserData,
  // track 0, time 00:00:00), and F2SectionCorrection::flush() emits any such
  // sections still in its buffer at end of stream. Attributing samples with
  // that metadata would invent a track 0 at 00:00:00, so walk back to the
  // nearest section whose Q position was actually decoded. The Q timeline is
  // contiguous, so the preceding valid section still describes the right
  // region to within the length of the invalid run.
  while (!m_metadataHistory[index].isValid()) {
    if (index == 0) return nullptr;
    --index;
  }
  return &m_metadataHistory[index];
}

void AudioCorrection::appendChannelSamples(const AudioSection& section,
                                           std::vector<int16_t>& valLeft,
                                           std::vector<uint8_t>& errLeft,
                                           std::vector<uint8_t>& padLeft,
                                           std::vector<int16_t>& valRight,
                                           std::vector<uint8_t>& errRight,
                                           std::vector<uint8_t>& padRight) {
  for (int frameIndex = 0; frameIndex < kFramesPerSection; ++frameIndex) {
    const Audio& frame = section.frame(frameIndex);
    // Reference each vector once per frame (P-3: no per-frame copy or
    // per-sample allocation).  data() and errorData() return a shared
    // zero-filled 12-element vector for empty frames.
    const std::vector<int16_t>& data = frame.data();
    const std::vector<uint8_t>& errorData = frame.errorData();
    const std::vector<uint8_t>& paddedData = frame.paddedData();
    for (int sample = 0; sample < kSamplesPerChannelPerFrame; ++sample) {
      const int left = sample * 2;
      const int right = left + 1;
      valLeft.push_back(data[left]);
      errLeft.push_back(errorData[left]);
      padLeft.push_back(paddedData[left]);
      valRight.push_back(data[right]);
      errRight.push_back(errorData[right]);
      padRight.push_back(paddedData[right]);
    }
  }
}

void AudioCorrection::correctChannel(std::vector<int16_t>& val,
                                     std::vector<uint8_t>& err,
                                     const std::vector<uint8_t>& padded,
                                     std::vector<uint8_t>& concealed,
                                     int midStart, int midEnd,
                                     uint64_t ordinal) {
  const int total = static_cast<int>(val.size());

  int index = midStart;
  while (index < midEnd) {
    if (err[index] == 0) {
      // Valid sample - leave it untouched.
      ++m_validSamplesCount;
      ++index;
      continue;
    }

    // Extent of the flagged run inside the region we are allowed to write.
    int runStart = index;
    int runEnd = index;
    while (runEnd < midEnd && err[runEnd] != 0) ++runEnd;

    // Nearest clean anchor to the left (may lie in the preceding section).
    int leftAnchor = runStart - 1;
    while (leftAnchor >= 0 && err[leftAnchor] != 0) --leftAnchor;
    const bool haveLeft = leftAnchor >= 0;

    // Nearest clean anchor to the right (may lie in the following section); the
    // flagged run can continue past our writable region into that neighbour.
    int rightAnchor = runEnd;
    while (rightAnchor < total && err[rightAnchor] != 0) ++rightAnchor;
    const bool haveRight = rightAnchor < total;

    // Full flagged burst spans (leftAnchor, rightAnchor); its length decides
    // interpolate-vs-mute even when it straddles a section boundary.
    const int gapLength =
        (haveLeft && haveRight) ? rightAnchor - leftAnchor - 1 : total;

    if (haveLeft && haveRight && gapLength <= kMaxInterpolationGap) {
      // Bridge the gap with linear interpolation between the flanking clean
      // samples.
      const int32_t v0 = val[leftAnchor];
      const int32_t v1 = val[rightAnchor];
      const int span = rightAnchor - leftAnchor;
      for (int j = runStart; j < runEnd; ++j) {
        // Result stays between v0 and v1, so it cannot overflow int16.
        val[j] =
            static_cast<int16_t>(v0 + ((v1 - v0) * (j - leftAnchor)) / span);
        err[j] = 0;
        concealed[j] = 1;
        ++m_concealedSamplesCount;
      }
    } else {
      // Burst too long to bridge (or unbounded at a stream edge): mute it, with
      // a short fade to/from the flanking clean samples to avoid a click.  The
      // fade is positioned relative to the full burst so that a burst split
      // across a section boundary ramps consistently.
      const int burstStart = haveLeft ? leftAnchor + 1 : runStart;
      const int burstEnd = haveRight ? rightAnchor - 1 : runEnd - 1;
      for (int j = runStart; j < runEnd; ++j) {
        double sample = 0.0;
        if (haveLeft) {
          const int distance = j - burstStart;  // from the start of the burst
          if (distance < kMuteRampSamples) {
            const double weight = 1.0 - static_cast<double>(distance + 1) /
                                            (kMuteRampSamples + 1);
            sample += val[leftAnchor] * weight;
          }
        }
        if (haveRight) {
          const int distance = burstEnd - j;  // from the end of the burst
          if (distance < kMuteRampSamples) {
            const double weight = 1.0 - static_cast<double>(distance + 1) /
                                            (kMuteRampSamples + 1);
            sample += val[rightAnchor] * weight;
          }
        }
        val[j] = static_cast<int16_t>(std::lround(sample));
        err[j] = 1;
        concealed[j] = 0;
        ++m_silencedSamplesCount;
      }
    }

    index = runEnd;
  }

  // Attribution pass. Every sample of the corrected region is now in its final
  // state, so classify it once: first by cause (structural filler carries no
  // disc data and must never reach a quality figure), otherwise by the disc
  // region it belongs to once the de-interleave lag is removed.
  for (int j = midStart; j < midEnd; ++j) {
    const int offset = j - midStart;

    if (padded[j] != 0) {
      // Decoder-supplied filler, of three kinds. The first kLatencySamples of
      // the emitted stream are the de-interleaver warming up. Filler whose
      // originating section was fabricated to bridge a gap in the EFM (R-6)
      // sits mid-stream and holds the timeline together across it. Anything
      // else this late is the end-of-stream drain.
      const uint64_t emitted =
          ordinal * static_cast<uint64_t>(kSamplesPerChannel) +
          static_cast<uint64_t>(offset);
      if (emitted < static_cast<uint64_t>(kLatencySamples)) {
        ++m_warmupSamplesCount;
      } else {
        const SectionMetadata* origin = discOriginOf(ordinal, offset);
        const DiscRegion region = (origin != nullptr)
                                      ? regionForMetadata(*origin)
                                      : DiscRegion::OutsideProgrammeArea;
        if (origin != nullptr && origin->isGapFiller()) {
          // A gap in the middle of the disc, not a truncated capture. Charging
          // it to the drain would have the report claim the tail of the audio
          // was lost.
          ++m_gapFillerSamplesCount;
          ++m_gapFillerRegionSamples[static_cast<size_t>(region)];
        } else {
          ++m_drainSamplesCount;
          // Record where on the disc the lost tail falls, so the report can
          // tell a harmless lead-out drain apart from lost programme audio.
          ++m_drainRegionSamples[static_cast<size_t>(region)];
        }
      }
      continue;
    }

    const SectionMetadata* origin = discOriginOf(ordinal, offset);
    const DiscRegion region = (origin != nullptr)
                                  ? regionForMetadata(*origin)
                                  : DiscRegion::OutsideProgrammeArea;
    RegionLoss& loss = m_regionLoss[static_cast<size_t>(region)];
    ++loss.decoded;

    const bool wasConcealed = concealed[j] != 0;
    const bool wasSilenced = err[j] != 0;
    if (wasConcealed) ++loss.concealed;
    if (wasSilenced) ++loss.silenced;

    // Per-track detail, so a warning can name the track and timecode a
    // listener would hear the damage in.
    if (region == DiscRegion::Programme && origin != nullptr &&
        (wasConcealed || wasSilenced)) {
      TrackLoss& track = m_trackLosses[origin->trackNumber()];
      if (wasConcealed) ++track.concealed;
      if (wasSilenced) {
        ++track.silenced;
        const SectionTime when = origin->absoluteSectionTime();
        if (!track.haveSilenced) {
          track.firstSilenced = when;
          track.lastSilenced = when;
          track.haveSilenced = true;
        } else {
          if (when < track.firstSilenced) track.firstSilenced = when;
          if (when > track.lastSilenced) track.lastSilenced = when;
        }
      }
    }
  }
}

AudioSection AudioCorrection::correctSection(const AudioSection* preceding,
                                             const AudioSection& correcting,
                                             const AudioSection* following,
                                             uint64_t ordinal) {
  // Build contiguous per-channel sample streams spanning
  // [preceding | correcting | following].  Only the correcting section is
  // written back; the neighbours provide concealment anchors.
  std::vector<int16_t> valLeft, valRight;
  std::vector<uint8_t> errLeft, errRight;
  std::vector<uint8_t> padLeft, padRight;
  const int reserveSize = kSamplesPerChannel * 3;
  valLeft.reserve(reserveSize);
  valRight.reserve(reserveSize);
  errLeft.reserve(reserveSize);
  errRight.reserve(reserveSize);
  padLeft.reserve(reserveSize);
  padRight.reserve(reserveSize);

  if (preceding != nullptr) {
    appendChannelSamples(*preceding, valLeft, errLeft, padLeft, valRight,
                         errRight, padRight);
  }
  const int midStart = (preceding != nullptr) ? kSamplesPerChannel : 0;
  appendChannelSamples(correcting, valLeft, errLeft, padLeft, valRight,
                       errRight, padRight);
  const int midEnd = midStart + kSamplesPerChannel;
  if (following != nullptr) {
    appendChannelSamples(*following, valLeft, errLeft, padLeft, valRight,
                         errRight, padRight);
  }

  // Concealment flags for the middle region (sized to the full stream so it can
  // be indexed with the same offsets as the sample vectors).
  std::vector<uint8_t> concealedLeft(valLeft.size(), 0);
  std::vector<uint8_t> concealedRight(valRight.size(), 0);

  correctChannel(valLeft, errLeft, padLeft, concealedLeft, midStart, midEnd,
                 ordinal);
  correctChannel(valRight, errRight, padRight, concealedRight, midStart, midEnd,
                 ordinal);

  // Reassemble the corrected middle section from the per-channel streams.
  AudioSection corrected;
  for (int frameIndex = 0; frameIndex < kFramesPerSection; ++frameIndex) {
    std::vector<int16_t> data(kSamplesPerFrame);
    std::vector<uint8_t> errorData(kSamplesPerFrame);
    std::vector<uint8_t> concealedData(kSamplesPerFrame);
    std::vector<uint8_t> paddedData(kSamplesPerFrame);
    const int frameBase = midStart + frameIndex * kSamplesPerChannelPerFrame;
    for (int sample = 0; sample < kSamplesPerChannelPerFrame; ++sample) {
      const int ext = frameBase + sample;
      const int left = sample * 2;
      const int right = left + 1;
      data[left] = valLeft[ext];
      data[right] = valRight[ext];
      errorData[left] = errLeft[ext];
      errorData[right] = errRight[ext];
      concealedData[left] = concealedLeft[ext];
      concealedData[right] = concealedRight[ext];
      paddedData[left] = padLeft[ext];
      paddedData[right] = padRight[ext];
    }
    Audio frame;
    frame.setData(data);
    frame.setErrorData(errorData);
    frame.setConcealedData(concealedData);
    frame.setPaddedData(paddedData);
    corrected.pushFrame(frame);
  }

  corrected.metadata = correcting.metadata;
  return corrected;
}

void AudioCorrection::showStatistics() const {
  ORC_LOG_INFO("Audio correction statistics:");
  ORC_LOG_INFO(
      "  Total mono samples: {}",
      m_validSamplesCount + m_concealedSamplesCount + m_silencedSamplesCount);
  ORC_LOG_INFO("  Valid mono samples: {}", m_validSamplesCount);
  ORC_LOG_INFO("  Concealed mono samples: {}", m_concealedSamplesCount);
  ORC_LOG_INFO("  Silenced mono samples: {}", m_silencedSamplesCount);

  ORC_LOG_INFO("  Structural (no disc data):");
  ORC_LOG_INFO("    De-interleave warm-up samples: {}", m_warmupSamplesCount);
  ORC_LOG_INFO("    End-of-stream drain samples: {}", m_drainSamplesCount);

  ORC_LOG_INFO("  By disc region (samples carrying disc data):");
  for (size_t i = 0; i < static_cast<size_t>(DiscRegion::Count); ++i) {
    const RegionLoss& loss = m_regionLoss[i];
    ORC_LOG_INFO("    {}: {} decoded, {} concealed, {} silenced",
                 discRegionName(static_cast<DiscRegion>(i)), loss.decoded,
                 loss.concealed, loss.silenced);
  }
}

void AudioCorrection::flush() {
  // E-6 edge priming (end of stream): correct and emit every section still held
  // in the lookahead buffer.  The final section has no following neighbour, and
  // for a stream shorter than three sections the first was never corrected
  // either - both cases fall out of the null-neighbour handling.
  if (m_correctionBuffer.empty()) return;

  const int count = static_cast<int>(m_correctionBuffer.size());

  // If the first section has already been corrected then only the final section
  // remains uncorrected; otherwise the whole (short) buffer still needs it.
  const int firstUncorrected = m_firstSectionCorrected ? count - 1 : 0;

  for (int i = firstUncorrected; i < count; ++i) {
    const AudioSection* preceding =
        (i == 0) ? nullptr : &m_correctionBuffer.at(i - 1);
    const AudioSection* following =
        (i + 1 < count) ? &m_correctionBuffer.at(i + 1) : nullptr;
    m_correctionBuffer[i] =
        correctSection(preceding, m_correctionBuffer.at(i), following,
                       m_correctionOrdinals.at(i));
  }
  m_firstSectionCorrected = true;

  for (auto& section : m_correctionBuffer) {
    m_outputBuffer.push_back(std::move(section));
  }
  m_correctionBuffer.clear();
  m_correctionOrdinals.clear();
}
