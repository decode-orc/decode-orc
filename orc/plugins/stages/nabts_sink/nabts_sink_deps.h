/*
 * File:        nabts_sink_deps.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Concrete NabtsSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_DEPS_H
#define ORC_NABTS_SINK_DEPS_H

#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nabts_data_group.h"
#include "nabts_record.h"
#include "nabts_record_catalogue.h"
#include "nabts_scan_state.h"
#include "nabts_sink_deps_interface.h"

namespace orc {

class IStageServices;
class TeletextRecoveryStats;

/**
 * @brief The caption service as a SubRip document
 *
 * The exported half of the pair the stage publishes: the same cue list that
 * becomes the caption track of the catalogue (nabts_catalogue_view.h) written
 * out as a file. Both take their reading of the service from
 * nabts_caption_cues(), which is what stops the file and the screen disagreeing
 * now that the host consumes the cues as data rather than deriving them itself.
 * Free and exposed so a test can hold the two renderings against each other.
 */
std::string nabts_caption_srt(const std::vector<NabtsCaptionCue>& cues);

/**
 * @brief The name a reference recording is known by
 *
 * What a reconstructed page says it was rebuilt from: the packet stream's file
 * name, or — for a stream a run wrote as out.t33 — the folder it is in, which
 * is what names the recording there. @p stream_file_name is the stream's file
 * name ("out.t33"), whether the stream itself or the records exported beside
 * it are read.
 */
std::string nabts_reference_source(const std::string& folder_name,
                                   const std::string& stream_file_name);

/**
 * @brief A record file export_records wrote, as a record to rebuild from
 *
 * export_records names each file <stream>.<key>.rec — the packet stream's
 * file name, then nabts_record_key() — and fills it with the record's data as
 * the run's vote made it, a byte that never arrived standing as the 0x00 of a
 * lost packet's filler. The record is named for the stream it was exported
 * beside (nabts_reference_source()) and keyed by the file name. It carries no
 * presence mask: a 0x00 byte is read as one that never arrived, which is how
 * NabtsRecordLibrary and NaplpsDialectModel read every reference, and no byte
 * that passed its parity is ever 0x00 (CEA-516 §3.3). Empty when
 * @p file_name is not one export_records writes, or @p data is empty.
 */
std::optional<NabtsReferenceRecord> nabts_reference_record_file(
    const std::string& folder_name, const std::string& file_name,
    std::vector<uint8_t> data);

/**
 * @brief The NABTS sink's real work: one pass over the frame range
 *
 * Recovers the System C data lines of every frame of the input, writes the
 * recovered packets as a flat 33-byte packet stream, and reports how the
 * recovery went.
 *
 * Thread safety: analyse() slices on several threads internally but is itself
 * single-threaded from the caller's point of view — one call at a time per
 * instance.
 */
class NabtsSinkDeps : public INabtsSinkStageDeps {
 public:
  explicit NabtsSinkDeps(IStageServices* stage_services)
      : stage_services_(stage_services) {}

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested) override;

  NabtsSinkResult analyse(const VideoFrameRepresentation* representation,
                          const NabtsSinkOptions& options) override;

 private:
  // The run's diagnostic report: what was exported and how the recovery went.
  std::string build_report(const NabtsSinkOptions& options,
                           const NabtsSinkResult& result, uint64_t total_frames,
                           const TeletextRecoveryStats& stats,
                           const NabtsScanState& scan_state,
                           const NabtsGroupStats& group_stats,
                           const NabtsRecordStats& record_stats) const;

  // Write every catalogued record's data beside the packet stream, one file
  // apiece. Never fails the export.
  void write_records(const NabtsSinkOptions& options,
                     NabtsSinkResult& result) const;

  // Write the caption service as a SubRip document beside the packet stream.
  // Never fails the export.
  void write_captions(const NabtsSinkOptions& options,
                      NabtsSinkResult& result) const;

  // Write |result.report| beside the packet stream. Never fails the export.
  void write_report(const NabtsSinkOptions& options,
                    NabtsSinkResult& result) const;

  // Write |size| bytes from |data| to |path| through the host's file writer,
  // buffered |buffer_bytes| at a time. False, warning that the |what| could
  // not be written, where the file could not be opened.
  bool write_file(const std::string& path, const uint8_t* data, size_t size,
                  size_t buffer_bytes, const char* what) const;

  // Read the reference recordings options.reference_records names — packet
  // streams, record files export_records wrote, or folders of either — into
  // |result.dataset.references|. Never fails the export.
  void load_references(const NabtsSinkOptions& options,
                       NabtsSinkResult& result) const;

  IStageServices* stage_services_{nullptr};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* cancel_requested_{nullptr};
};

}  // namespace orc

#endif  // ORC_NABTS_SINK_DEPS_H
