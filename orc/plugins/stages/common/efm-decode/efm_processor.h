/*
 * File:        efm_processor.h
 * Purpose:     efm-decoder - Unified EFM decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef EFM_PROCESSOR_H
#define EFM_PROCESSOR_H

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bounded_queue.h"
#include "decoders.h"
#include "section.h"

// General pipeline decoders
#include "dec_channeltof3frame.h"
#include "dec_f2sectioncorrection.h"
#include "dec_f3frametof2section.h"
#include "dec_tvaluestochannel.h"

// D24 pipeline decoders
#include "dec_f1sectiontodata24section.h"
#include "dec_f2sectiontof1section.h"

// Audio pipeline decoders
#include "dec_audiocorrection.h"
#include "dec_audiodeemphasis.h"
#include "dec_data24toaudio.h"

// Data pipeline decoders
#include "dec_data24torawsector.h"
#include "dec_rawsectortosector.h"
#include "dec_sectorcorrection.h"

// Audio output writers
#include "writer_raw.h"
#include "writer_wav.h"
#include "writer_wav_metadata.h"

// Data output writers
#include "writer_sector.h"
#include "writer_sector_metadata.h"

class EfmProcessor {
 public:
  EfmProcessor();
  ~EfmProcessor();

  EfmProcessor(const EfmProcessor&) = delete;
  EfmProcessor& operator=(const EfmProcessor&) = delete;

  // Streaming API: feed t-values field-by-field without a temporary buffer.
  // Call beginStream() once, then pushChunk() for each field's samples,
  // then finishStream() to flush and finalise output.
  //
  // The chunk carries PACKED EFM bytes exactly as they appear in an .efm file:
  // the t-value in the low nibble and the producer's doubt in the high nibble
  // (CVBS File Format Specification, EFM extension format). A caller with no
  // confidence information simply passes plain t-values, which is the same
  // stream with an all-zero doubt nibble.
  bool beginStream(const std::string& outputFilename, int64_t totalTValues = 0);
  void pushChunk(const std::vector<uint8_t>& chunk);
  bool finishStream();

  // When finishStream() returns false, this holds a human-readable explanation
  // of why decoding did not succeed (empty on success).
  const std::string& lastError() const { return m_lastError; }

  // Mode selection
  void setAudioMode(bool audioMode);  // true = audio (default), false = data

  // EFM options
  void setNoTimecodes(bool noTimecodes);

  // Issue #307: the smallest producer doubt (0 trusted - 15 distrusted) that
  // makes an otherwise-clean symbol a C1/C2 erasure candidate. Every .efm byte
  // carries the producer's doubt in its high nibble; the CIRC uses it to seed
  // erasures for the symbols that demodulate cleanly but are wrong anyway.
  // 0 disables doubt-derived erasures (bit-exact legacy behaviour).
  void setDoubtErasureThreshold(uint8_t threshold);

  // Audio options
  void setAudacityLabels(bool audacityLabels);
  void setNoAudioConcealment(bool noAudioConcealment);
  // Q-8: when true, the 50/15 us pre-emphasis CONTROL flag is ignored and no
  // de-emphasis is applied (audio is emitted exactly as decoded). Default
  // false: pre-emphasised sections are de-emphasised during decode.
  void setIgnorePreemphasis(bool ignorePreemphasis);
  void setZeroPad(bool zeroPad);
  void setNoWavHeader(bool noWavHeader);

  // Audio/video sync alignment (issue #231). When enabled, the head of the
  // decoded audio stream is aligned with the EFM input timeline (for a
  // LaserDisc capture, the video timeline): the CIRC de-interleave warm-up
  // (kDeinterleaveLatencySamples pairs of filler at the stream head) is
  // removed and the input consumed during decoder sync acquisition
  // (discarded T-values, pre-sync F3 frames, settle/lead-in sections) is
  // restored as silence. Audio mode only; ignored when zero-pad is active
  // (zero-pad anchors the stream to disc absolute time 00:00:00 instead).
  void setAudioSyncOffset(bool audioSyncOffset);
  // Additional user slip in 44.1 kHz stereo pairs applied on top of the
  // computed alignment (positive delays the audio relative to the input
  // timeline, negative advances it). Applied even when the computed
  // alignment is disabled.
  void setAudioSyncSlipPairs(int64_t slipPairs);
  // The net head adjustment applied to the audio output, in 44.1 kHz stereo
  // pairs (positive = silence prepended, negative = pairs dropped). Valid
  // after finishStream(); 0 when no adjustment was applied.
  int64_t appliedAudioSyncOffsetPairs() const {
    return m_appliedAudioSyncOffsetPairs;
  }

  // Data options
  void setOutputMetadata(bool outputMetadata);

  // Report options
  void setReportOutput(bool reportOutput);
  // Overrides the report file path. When empty (the default) the report is
  // written to the decode output filename with a ".txt" suffix appended;
  // when set, the report is written to exactly this path. Needed by callers
  // that decode to a scratch file but want the report at a user-chosen path.
  void setReportFilename(const std::string& reportFilename);

  // Statistics output
  void showAllStatistics() const;

 private:
  // -----------------------------------------------------------------------
  // Configuration flags
  // -----------------------------------------------------------------------
  bool m_audioMode;  // true = audio path, false = data path
  bool m_noTimecodes;
  uint8_t m_doubtErasureThreshold;
  bool m_audacityLabels;
  bool m_noAudioConcealment;
  bool m_ignorePreemphasis;
  bool m_zeroPad;
  bool m_noWavHeader;
  bool m_outputMetadata;
  bool m_reportOutput;
  std::string m_reportFilename;  // empty = derive from m_outputFilename

  // Audio/video sync alignment (issue #231); see setAudioSyncOffset().
  bool m_audioSyncOffset{false};
  int64_t m_audioSyncSlipPairs{0};
  // Input consumed by the front end before its first output section, in
  // 44.1 kHz stereo pairs. Snapshotted on the FRONT-END thread the moment the
  // first F2 section becomes ready (the head counters are frozen by then);
  // the queue hand-off orders it before any back-end read.
  bool m_headSkipCaptured{false};
  int64_t m_headSkipPairs{0};
  // Set once by the back end when the head adjustment has been decided and
  // handed to the active audio writer.
  bool m_audioSyncApplied{false};
  int64_t m_appliedAudioSyncOffsetPairs{0};

  // -----------------------------------------------------------------------
  // Pipeline decoder instances
  // -----------------------------------------------------------------------

  // General pipeline (EFM → F2 section)
  TvaluesToChannel m_tValuesToChannel;
  ChannelToF3Frame m_channelToF3;
  F3FrameToF2Section m_f3FrameToF2Section;
  F2SectionCorrection m_f2SectionCorrection;

  // D24 pipeline (F2 section → Data24 section)
  F2SectionToF1Section m_f2SectionToF1Section;
  F1SectionToData24Section m_f1SectionToData24Section;

  // Audio pipeline (Data24 section → audio)
  Data24ToAudio m_data24ToAudio;
  AudioCorrection m_audioCorrection;
  AudioDeemphasis m_audioDeemphasis;

  // Data pipeline (Data24 section → ECMA-130 sectors)
  Data24ToRawSector m_data24ToRawSector;
  RawSectorToSector m_rawSectorToSector;
  SectorCorrection m_sectorCorrection;

  // -----------------------------------------------------------------------
  // I/O instances
  // -----------------------------------------------------------------------
  WriterWav m_writerWav;
  WriterRaw m_writerRaw;
  WriterWavMetadata m_writerWavMetadata;
  WriterSector m_writerSector;
  WriterSectorMetadata m_writerSectorMetadata;

  // -----------------------------------------------------------------------
  // Pipeline statistics
  // -----------------------------------------------------------------------
  struct AllPipelineStatistics {
    // General pipeline timing (µs)
    int64_t channelToF3Time{0};
    int64_t f3ToF2Time{0};
    int64_t f2CorrectionTime{0};

    // D24 pipeline timing (µs)
    int64_t f2ToF1Time{0};
    int64_t f1ToData24Time{0};

    // Audio pipeline timing (µs)
    int64_t data24ToAudioTime{0};
    int64_t audioCorrectionTime{0};

    // Data pipeline timing (µs)
    int64_t data24ToRawSectorTime{0};
    int64_t rawSectorToSectorTime{0};
  } m_pipelineStats;

  // Overall wall-clock start time
  std::chrono::high_resolution_clock::time_point m_startTime;

  // -----------------------------------------------------------------------
  // Streaming-API state (populated by beginStream / used by
  // pushChunk+finishStream)
  // -----------------------------------------------------------------------
  std::string m_outputFilename;
  bool m_zeroPadApplied{false};
  int64_t m_totalTValues{0};
  int64_t m_processedTValues{0};
  int m_lastProgress{0};

  // Explanation set when finishStream() fails (see lastError()).
  std::string m_lastError;

  // -----------------------------------------------------------------------
  // P-9: two-stage split.
  //
  // The pipeline is divided at the F2-section boundary. The bit-level FRONT END
  // (t-values -> Channel -> F3 -> F2 -> F2SectionCorrection) runs on the
  // caller's thread inside pushChunk(); each completed F2 section is handed to
  // the section-level BACK END (F2 -> F1 -> Data24 -> audio/data -> writers),
  // which runs on m_backEndThread and consumes sections from m_sectionQueue.
  //
  // Thread-safety: the two halves share no decoder or writer objects, so the
  // only cross-thread state is m_sectionQueue (a synchronised hand-off) and the
  // m_backEndError marshalling below. The pipeline-timing fields updated by the
  // front end are disjoint from those updated by the back end, and all
  // statistics are read only in finishStream() after the back-end thread has
  // joined. The shared Reed-Solomon codecs used by the back end are const-safe
  // for concurrent decode() (see reedsolomon.cpp / rspc.cpp, P-12).
  // -----------------------------------------------------------------------

  // Bounded hand-off buffer between the front and back end. ~256 F2 sections
  // (~3.4 s of audio, ~2.5 MB) is enough to keep both halves busy while
  // bounding memory if one side stalls.
  static constexpr std::size_t kSectionQueueCapacity = 256;
  std::unique_ptr<BoundedQueue<F2Section>> m_sectionQueue;
  std::thread m_backEndThread;
  // Set by the back-end thread if it throws; re-raised on the caller's thread
  // in finishStream() so the efm::EfmDecodeError stage-boundary contract (R-1)
  // is preserved rather than the exception escaping the worker thread.
  std::exception_ptr m_backEndError;

  // Front-end producer: advance the bit-level stages and enqueue every ready F2
  // section. Returns false if the back end has aborted (stop feeding).
  bool drainFrontEnd();
  // Back-end thread entry point: consume F2 sections, then flush the tail.
  void backEndLoop();

  // Per-iteration pipeline drain helpers
  void drainBackEnd(bool& zeroPadApplied);
  void drainAudioPipeline();
  void drainDataPipeline();

  // Audio/video sync alignment helpers (issue #231).
  void captureHeadSkip();       // front-end thread, before first hand-off
  void applyAudioSyncOffset();  // back-end thread, before first audio write

  void showGeneralPipelineStatistics() const;
  void showD24PipelineStatistics() const;
  void showAudioPipelineStatistics() const;
  void showDataPipelineStatistics() const;

  // Curated decode-report sections (see showAllStatistics).
  void showReportHeader() const;  // banner + file/mode/result
  void showSummary() const;       // Part A - at-a-glance decode quality
  void showDiscContents() const;  // Part B - Q-channel / track table
  void showQuality() const;       // Part C - signal & error-correction quality
  // Part C - the decoder's own de-interleave warm-up / drain boundaries and the
  // window of the stream they leave recoverable. Informational, not a defect.
  void showDecodeBoundaries() const;
};

#endif  // EFM_PROCESSOR_H
