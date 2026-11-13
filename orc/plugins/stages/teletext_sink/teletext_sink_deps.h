/*
 * File:        teletext_sink_deps.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     TeletextSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SINK_DEPS_H
#define ORC_TELETEXT_SINK_DEPS_H

#include <orc/stage/triggerable_stage.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "teletext_packet_spill.h"
#include "teletext_scan_state.h"
#include "teletext_sink_deps_interface.h"
#include "teletext_squash_stats.h"
#include "vbi-services/teletext_recovery_stats.h"

namespace orc {

class IStageServices;
class IFileWriterUint8;

/**
 * @brief Runs the analysis pass: frame loop, packet writer, page catalogue.
 *
 * One linear pass over the frame range slices every candidate VBI line with
 * TeletextFrameSlicer, writes the recovered packets as the service's flat
 * stream (.t42 on 625 lines, .t34 on 525), and feeds the same packets to a
 * page decoder whose snapshots build the catalogue the stage tool displays.
 *
 * Nothing is read from or written to the observation store: the stage owns its
 * decoding end to end, which is what lets its tuning parameters mean anything.
 */
class TeletextSinkDeps : public ITeletextSinkStageDeps {
 public:
  // Where a squashing run keeps its packet stream until the rewrite pass.
  using PacketSpillFactory =
      std::function<std::unique_ptr<TeletextPacketSpill>()>;

  // |stage_services| may be null (direct in-process construction in tests);
  // the run then fails for want of a writer factory. |make_spill| defaults to
  // a scratch file (make_teletext_packet_spill); unit tests pass an in-memory
  // one.
  explicit TeletextSinkDeps(IStageServices* stage_services,
                            PacketSpillFactory make_spill = {})
      : stage_services_(stage_services), make_spill_(std::move(make_spill)) {}

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested) override;

  TeletextSinkResult analyse(const VideoFrameRepresentation* representation,
                             const TeletextSinkOptions& options) override;

 private:
  // Assemble the run's diagnostic report: what was exported, how recovery
  // went, and what combining repeated rows changed. Always built — it costs a
  // string, and a run that recovered little is exactly when it is worth
  // reading.
  static std::string build_report(const TeletextSinkOptions& options,
                                  const TeletextSinkResult& result,
                                  uint64_t total_frames,
                                  const TeletextRecoveryStats& stats,
                                  const TeletextSquashStats& squash_stats,
                                  const TeletextScanState& scan_state);

  // Write |result.report| to <output>.txt when the option asks for it,
  // stamping result.report_path on success. A failure here is logged and
  // otherwise ignored: the packet stream is the product, the report is a note
  // about it.
  void write_report(const TeletextSinkOptions& options,
                    TeletextSinkResult& result) const;

  IStageServices* stage_services_{nullptr};
  PacketSpillFactory make_spill_;
  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* cancel_requested_{nullptr};
};

}  // namespace orc

#endif  // ORC_TELETEXT_SINK_DEPS_H
