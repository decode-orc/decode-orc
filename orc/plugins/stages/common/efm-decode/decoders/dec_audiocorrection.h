/*
 * File:        dec_audiocorrection.h
 * Purpose:     efm-decoder-audio - EFM Data24 to Audio decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DEC_AUDIOCORRECTION_H
#define DEC_AUDIOCORRECTION_H

#include <array>
#include <cstdint>
#include <map>
#include <string>

#include "decoders.h"
#include "section.h"

// Where on the disc a decoded sample falls, per the Q-channel (IEC 60908-1999
// §17.5.1). Lead-in and lead-out are digital silence by construction, so losses
// there carry no listenable content; only Programme losses affect playback.
enum class DiscRegion : uint8_t {
  LeadIn = 0,
  Pause,      // Track pre-gap, Q-channel INDEX 00
  Programme,  // Q-channel INDEX 01 and above
  LeadOut,
  OutsideProgrammeArea,  // No Q position could be established for the sample
  Count
};

std::string discRegionName(DiscRegion region);

// Sample tallies for one disc region. decoded counts every sample attributed to
// the region, so concealed and silenced are subsets of it.
struct RegionLoss {
  uint64_t decoded{0};
  uint64_t concealed{0};
  uint64_t silenced{0};
};

// Genuine losses within one programme track, with the absolute time span over
// which muting occurred so the report can name where a listener will hear it.
struct TrackLoss {
  uint64_t concealed{0};
  uint64_t silenced{0};
  SectionTime firstSilenced;
  SectionTime lastSilenced;
  bool haveSilenced{false};
};

class AudioCorrection : public Decoder {
 public:
  AudioCorrection();
  void pushSection(const AudioSection& audioSection);
  void pushSection(AudioSection&& audioSection);
  AudioSection popSection();
  bool isReady() const;
  void flush();

  void showStatistics() const;

  // Accessors for the curated decode report. These are whole-stream totals and
  // include the structural (warm-up / drain) samples; use the per-region and
  // per-cause breakdowns below to separate decoder artefacts from disc defects.
  uint64_t concealedSamples() const { return m_concealedSamplesCount; }
  uint64_t silencedSamples() const { return m_silencedSamplesCount; }
  uint64_t validSamples() const { return m_validSamplesCount; }

  // Breakdown by disc region. Only samples that carry disc data are counted
  // here; structural filler is excluded and reported by cause instead.
  const RegionLoss& regionLoss(DiscRegion region) const {
    return m_regionLoss[static_cast<size_t>(region)];
  }

  // Breakdown by cause, for samples the CIRC chain could not populate from the
  // disc. Warm-up filler precedes the stream, so it displaces nothing: the
  // de-interleaver simply delays its first genuine output by this much.
  uint64_t warmupSamples() const { return m_warmupSamplesCount; }
  // Drain filler is different: at end of stream the newest frames are still
  // spread across the delay lines and cannot be assembled from complete
  // codewords, so the tail of the disc really is unrecoverable. On a normal
  // disc it falls in the lead-out and costs nothing; on a capture that stops
  // inside the programme area it is genuine lost audio, which is why it is
  // also broken down by region.
  uint64_t drainSamples() const { return m_drainSamplesCount; }
  uint64_t drainSamplesIn(DiscRegion region) const {
    return m_drainRegionSamples[static_cast<size_t>(region)];
  }

  // R-6: filler F2SectionCorrection inserted mid-stream to bridge a gap in the
  // EFM. It is padded like the warm-up and drain, but it is neither: it stands
  // in for disc time that was not recovered, at the right place in the
  // timeline, so the audio stays in sync across the gap. Counted apart from
  // the drain so a mid-stream gap is not reported as a truncated capture.
  uint64_t gapFillerSamples() const { return m_gapFillerSamplesCount; }
  uint64_t gapFillerSamplesIn(DiscRegion region) const {
    return m_gapFillerRegionSamples[static_cast<size_t>(region)];
  }

  // Genuine per-track losses, keyed by Q-channel track number.
  const std::map<uint8_t, TrackLoss>& trackLosses() const {
    return m_trackLosses;
  }

 private:
  void processQueue();

  // Correct one section using its (optional) neighbours for concealment
  // context.  Either neighbour may be null at a stream edge (E-6 edge priming),
  // in which case bursts touching that edge are muted rather than interpolated.
  // ordinal is the section's position in the emitted stream, used to map its
  // samples back onto the disc timeline (see correctChannel).
  AudioSection correctSection(const AudioSection* preceding,
                              const AudioSection& correcting,
                              const AudioSection* following, uint64_t ordinal);

  // Flatten a section's 98 frames into contiguous per-channel sample streams so
  // that error bursts crossing a section boundary can be bridged from clean
  // samples in the neighbouring sections.
  static void appendChannelSamples(const AudioSection& section,
                                   std::vector<int16_t>& valLeft,
                                   std::vector<uint8_t>& errLeft,
                                   std::vector<uint8_t>& padLeft,
                                   std::vector<int16_t>& valRight,
                                   std::vector<uint8_t>& errRight,
                                   std::vector<uint8_t>& padRight);

  // Conceal/mute one channel's flagged samples in the region [midStart,
  // midEnd).
  void correctChannel(std::vector<int16_t>& val, std::vector<uint8_t>& err,
                      const std::vector<uint8_t>& padded,
                      std::vector<uint8_t>& concealed, int midStart, int midEnd,
                      uint64_t ordinal);

  // Attribute one sample of the section at `ordinal`, `offset` samples (per
  // channel) into it, and add it to the region tallies. Returns the metadata of
  // the disc section the sample actually came from, or nullptr if the sample
  // predates the stream (it lies inside the de-interleave warm-up).
  const SectionMetadata* discOriginOf(uint64_t ordinal, int offset) const;

  // Record the metadata of a section as it enters the correction buffer, so
  // that the de-interleave latency can later be resolved against it.
  void recordSectionMetadata(const SectionMetadata& metadata);

  std::deque<AudioSection> m_inputBuffer;
  std::deque<AudioSection> m_outputBuffer;

  std::vector<AudioSection> m_correctionBuffer;
  // Emission ordinals of the sections held in m_correctionBuffer, kept parallel
  // to it.
  std::vector<uint64_t> m_correctionOrdinals;
  uint64_t m_nextOrdinal;

  // Metadata of recently seen sections, indexed by ordinal - m_historyBase.
  // The de-interleave latency reaches at most two sections back, so a short
  // history is sufficient.
  std::deque<SectionMetadata> m_metadataHistory;
  uint64_t m_historyBase;

  // E-6 edge priming: has the first section (which has no preceding neighbour)
  // been corrected yet?  Controls the flush-time correction of the stream
  // edges.
  bool m_firstSectionCorrected;

  // Statistics (P-10: 64-bit so sample counters do not wrap on long captures).
  uint64_t m_concealedSamplesCount;
  uint64_t m_silencedSamplesCount;
  uint64_t m_validSamplesCount;

  uint64_t m_warmupSamplesCount;
  uint64_t m_drainSamplesCount;
  uint64_t m_gapFillerSamplesCount;

  std::array<RegionLoss, static_cast<size_t>(DiscRegion::Count)> m_regionLoss;
  std::array<uint64_t, static_cast<size_t>(DiscRegion::Count)>
      m_drainRegionSamples{};
  std::array<uint64_t, static_cast<size_t>(DiscRegion::Count)>
      m_gapFillerRegionSamples{};
  std::map<uint8_t, TrackLoss> m_trackLosses;
};

#endif  // DEC_AUDIOCORRECTION_H
