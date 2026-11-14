/*
 * File:        cc_sink_stage_deps.h
 * Module:      orc-core
 * Purpose:     CCSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CC_SINK_STAGE_DEPS_H
#define ORC_CORE_CC_SINK_STAGE_DEPS_H

#include <orc/stage/observation/observation_service_interface.h>
#include <orc/support/eia608_decoder.h>
#include <orc/support/eia608_service_demux.h>
#include <orc/support/logging.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "cc_line21_report.h"
#include "cc_sink_stage_deps_interface.h"
#include "cc_xds_decoder.h"

namespace orc {
class CCSinkStageDeps : public ICCSinkStageDeps {
 public:
  // observation_service may be null (e.g. an older host, or direct in-process
  // construction in tests); export_cc() then falls back to whatever closed
  // caption observations are already present in the context (none on an older
  // host), producing an empty file rather than crashing.
  explicit CCSinkStageDeps(IObservationService* observation_service = nullptr)
      : observation_service_(observation_service) {}

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested) override;

  CCExportResult export_cc(VideoFrameRepresentation* representation,
                           IObservationContext& observation_context,
                           CCExportOptions options) override;

 private:
  /// One recovered byte pair, with where in the stream it came from
  struct CaptionPair {
    /// The television field it was carried on: 0 for field 1 (CC1, CC2,
    /// TEXT1, TEXT2), 1 for field 2 (CC3, CC4, TEXT3, TEXT4, XDS)
    int field_in_frame = 0;
    /// Field index across the whole run, as the observation is keyed
    uint64_t field_index = 0;
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    /// The pair is the Extended Data Service's, and no caption or text
    /// service's (see CCXdsDecoder)
    bool xds = false;
  };

  /**
   * @brief Drive the closed_caption observer over the run and hand back every
   *        byte pair taken from line 21 of either field, in transmission
   *        order
   *
   * The pairs taken are EIA608Line21Run's: the observer found line 21's
   * waveform on the line, the bytes' parity held (a character whose partner
   * failed is taken alone), the pair is one EIA-608 defines for its field, and
   * its field carries line 21 data around it. The run is read whole before
   * any pair is handed on, because which stored field is television field 1
   * is decided from all of it: a capture that starts on field 2 has its fields
   * read the other way round. Every pair also goes to the run's report and
   * field 2's to the XDS decoder.
   *
   * Shared by all the writers: they differ in what they do with the pairs,
   * not in how the pairs are found. Returns false when the export was
   * cancelled part-way.
   */
  bool for_each_caption_pair(const VideoFrameRepresentation* representation,
                             IObservationContext& observation_context,
                             IObserverHandle* cc_observer,
                             const std::function<void(const CaptionPair&)>& fn);

  bool export_scc(const VideoFrameRepresentation* representation,
                  const CCExportOptions& options, VideoFormat format,
                  IObservationContext& observation_context,
                  IObserverHandle* cc_observer, int32_t& cc_frames_exported);

  /// The second field's XDS packets as a listing: each packet whose value
  /// differs from the last of its type, with when it arrived.
  bool export_xds(const VideoFrameRepresentation* representation,
                  const CCExportOptions& options, VideoFormat format,
                  IObservationContext& observation_context,
                  IObserverHandle* cc_observer, int32_t& cc_frames_exported);

  /// Plain text, SubRip and HTML: all three are the decoded caption stream,
  /// written out differently.
  bool export_decoded(const VideoFrameRepresentation* representation,
                      const CCExportOptions& options, VideoFormat format,
                      IObservationContext& observation_context,
                      IObserverHandle* cc_observer,
                      int32_t& cc_frames_exported);

  /// Every service in one pass: each of the eight caption and text services
  /// decoded as export_decoded() decodes it, and the XDS listed as
  /// export_xds() lists it, into the folder options.output_path names. A
  /// service that carried nothing writes no file.
  bool export_all(const VideoFrameRepresentation* representation,
                  const CCExportOptions& options, VideoFormat format,
                  IObservationContext& observation_context,
                  IObserverHandle* cc_observer, int32_t& cc_frames_exported);

  /// A service's decoded cues in |export_format| (SubRip, HTML or plain
  /// text), as export_decoded() writes them.
  void write_cues(std::ostream& out, const std::vector<CaptionCue>& cues,
                  CCExportFormat export_format, EIA608Service service,
                  VideoFormat format) const;

  std::string generate_timestamp(int32_t field_index, VideoFormat format) const;
  uint8_t apply_odd_parity(uint8_t byte) const;
  bool is_control_code(uint8_t byte) const;

  class SpdlogLoggerAdapter {
   public:
    template <typename... Args>
    void trace(const char* fmt, Args&&... args) const {
      ORC_LOG_TRACE(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(const char* fmt, Args&&... args) const {
      ORC_LOG_DEBUG(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(const char* fmt, Args&&... args) const {
      ORC_LOG_INFO(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(const char* fmt, Args&&... args) const {
      ORC_LOG_WARN(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(const char* fmt, Args&&... args) const {
      ORC_LOG_ERROR(fmt, std::forward<Args>(args)...);
    }
  };

  IObservationService* observation_service_{nullptr};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* cancel_requested_{nullptr};
  EIA608Decoder eia608_decoder_;
  // The XDS decoder and the run's report: reset for each export.
  CCXdsDecoder xds_decoder_;
  CCLine21Report report_;
  std::function<void(const CCXdsPacket&)> xds_listener_;
  SpdlogLoggerAdapter logger_;
};
}  // namespace orc

#endif  // ORC_CORE_CC_SINK_STAGE_DEPS_H
