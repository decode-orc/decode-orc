/*
 * File:        cc_sink_stage.h
 * Module:      orc-core
 * Purpose:     Closed Caption Sink Stage - exports one EIA-608 service, or
 *              every one in a pass
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_CORE_CC_SINK_STAGE_H
#define ORC_CORE_CC_SINK_STAGE_H

#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <memory>
#include <string>

#include "cc_sink_stage_deps_interface.h"

namespace orc {

/**
 * @brief Closed Caption Sink Stage
 *
 * Extracts one EIA-608 service from line 21 of a 525-line recording and
 * exports it as Scenarist SCC, SubRip, plain text or HTML.
 *
 * This is a SINK stage - it has inputs but no outputs.
 *
 * Line 21 carries two bytes per field, and those two bytes are shared by four
 * services: CC1 and CC2 (captions) and TEXT1 and TEXT2 (pages of text such as
 * schedules or scores) on the first field, CC3, CC4, TEXT3 and TEXT4 on the
 * second, which also carries the Extended Data Service. Which of them a byte
 * pair belongs to is set by the control codes that precede it, so an export
 * that does not demultiplex them interleaves whichever the recording used — a
 * caption spliced through a page of listings. The `service` parameter selects
 * one; everything else is left out, and counted in the run's report.
 *
 * SCC Format:
 * - Header: "Scenarist_SCC V1.0"
 * - Timestamps in format HH:MM:SS:FF (non-drop frame)
 * - Hex byte pairs (e.g., "1441" for bytes 0x14 and 0x41)
 * - The selected service's byte pairs exactly as transmitted, control-code
 *   duplicates and channel bits included
 *
 * SubRip / Plain Text / HTML:
 * - The decoded caption stream, as timed text
 * - Screen rows are kept as separate lines and keep their indent, so a text
 *   service's columns still line up (HTML renders them in a monospaced grid)
 *
 * Parameters:
 * - output_path: Output file path (.scc, .srt, .txt or .html)
 * - service: Which service to export (CC1-CC4, TEXT1-TEXT4, XDS), or All:
 *   every one in a single pass, into the folder output_path names
 * - format: Export format
 * - write_report: Write the line 21 report beside the output
 */
class CCSinkStage : public DAGStage,
                    public ParameterizedStage,
                    public TriggerableStage {
 public:
  CCSinkStage();
  ~CCSinkStage() override = default;

  // DAGStage interface
  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override;

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 0; }  // Sink has no outputs

  // ParameterizedStage interface
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // TriggerableStage interface
  bool trigger(const std::vector<ArtifactPtr>& inputs,
               const std::map<std::string, ParameterValue>& parameters,
               IObservationContext& observation_context) override;

  std::string get_trigger_status() const override;

  void set_progress_callback(TriggerProgressCallback callback) override {
    progress_callback_ = callback;
  }

  bool is_trigger_in_progress() const override { return is_processing_.load(); }

  void set_deps_override(std::shared_ptr<ICCSinkStageDeps> deps) {
    deps_override_ = deps;
  }

 private:
  CCExportOptions parse_config(
      const std::map<std::string, ParameterValue>& parameters) const;

  // Member variables
  std::map<std::string, ParameterValue> parameters_;
  TriggerProgressCallback progress_callback_;
  std::atomic<bool> is_processing_{false};
  std::atomic<bool> cancel_requested_{false};
  std::shared_ptr<ICCSinkStageDeps> deps_override_;
};

}  // namespace orc

#endif  // ORC_CORE_CC_SINK_STAGE_H
