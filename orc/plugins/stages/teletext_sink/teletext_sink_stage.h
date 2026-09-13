/*
 * File:        teletext_sink_stage.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Teletext Sink Stage - recovers WST teletext, exports
 *              the packet stream and presents a page viewer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SINK_STAGE_H
#define ORC_TELETEXT_SINK_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/tooling/catalogue_results.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "teletext_catalogue_view.h"
#include "teletext_sink_deps_interface.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

class IStageServices;

/**
 * @brief Teletext Sink Stage
 *
 * Extracts World System Teletext (ITU-R BT.653 System B) data lines from the
 * VBI and writes the recovered packets as a flat, headerless packet stream in
 * transmission coding, strictly temporally ordered (frame → field → ascending
 * line): `.t42` for the 42-byte 625-line service (ETSI EN 300 706) and `.t34`
 * for the 34-byte 525-line one (BT.653 Table 1b).
 *
 * The same pass builds a catalogue of every page the range carried, which the
 * host reads through ICatalogueResults and presents as the stage's
 * batch-analysis tool.
 *
 * This is a SINK stage - it has inputs but no outputs. All work happens in
 * trigger(); execute() only caches the input. It is a sink rather than an
 * analysis sink because writing the packet stream is the product; the page
 * catalogue is a by-product of the same pass, offered through a batch-analysis
 * stage tool, which any stage may declare regardless of its node type.
 */
class TeletextSinkStage : public DAGStage,
                          public ParameterizedStage,
                          public TriggerableStage,
                          public StageToolProvider,
                          public IStagePreviewCapability,
                          public ICatalogueResults {
 public:
  explicit TeletextSinkStage(IStageServices* stage_services);
  ~TeletextSinkStage() override = default;

  /// Testing seam: inject a pre-built deps instance to substitute concrete
  /// dep creation in trigger().
  void set_deps_override(std::shared_ptr<ITeletextSinkStageDeps> deps) {
    deps_override_ = std::move(deps);
  }

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

  void cancel_trigger() override { cancel_requested_.store(true); }

  /// Whether the last trigger left a catalogue behind
  bool has_results() const override { return has_results_; }

  /// The raw catalogue the last trigger built. Plugin-private: the host reads
  /// the stage through ICatalogueResults and never sees this type. Kept public
  /// for the stage's own tests, which compile against these sources.
  const TeletextAnalysisDataset& dataset() const { return dataset_; }

  // ICatalogueResults interface. Resolving every sub-page into a drawable cell
  // grid costs real work and memory, and a run that only exports a packet
  // stream never needs it, so it is built on first ask and cached. Called from
  // the host's render worker, hence the lock.
  const CatalogueDataset& catalogue() const override {
    std::lock_guard<std::mutex> lock(catalogue_mutex_);
    if (!catalogue_) {
      catalogue_ = std::make_unique<CatalogueDataset>(
          build_teletext_catalogue(dataset_));
    }
    return *catalogue_;
  }

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  std::vector<StageToolDescriptor> get_stage_tools() const override {
    return {StageToolDescriptor{
        "teletext_analysis", "Teletext Pages",
        "Decode the teletext service and browse the pages it carried.",
        StageToolKind::CatalogueBrowser, false, kCatalogueBrowserContractId}};
  }

 private:
  /// Drop the cached catalogue so the next reader rebuilds it from the dataset
  /// that has just replaced the one it was built from.
  void invalidate_catalogue() {
    std::lock_guard<std::mutex> lock(catalogue_mutex_);
    catalogue_.reset();
  }

  // Parses the parameter set into deps options, converting the 1-based UI line
  // window to the 0-based field lines the slicer uses. Throws
  // std::runtime_error on missing/invalid parameters.
  TeletextSinkOptions parse_config(
      const std::map<std::string, ParameterValue>& parameters) const;

  std::map<std::string, ParameterValue> parameters_;
  std::string trigger_status_{"Idle"};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool> is_processing_{false};
  std::atomic<bool> cancel_requested_{false};
  IStageServices* stage_services_{nullptr};
  std::shared_ptr<ITeletextSinkStageDeps> deps_override_;

  // Cached results of the last trigger, resolved into the host-facing
  // catalogue on demand.
  TeletextAnalysisDataset dataset_;
  bool has_results_{false};
  // Built from dataset_ on first catalogue() call; cleared with it.
  mutable std::mutex catalogue_mutex_;
  mutable std::unique_ptr<CatalogueDataset> catalogue_;
  mutable std::shared_ptr<const VideoFrameRepresentation> cached_input_;
};

}  // namespace orc

#endif  // ORC_TELETEXT_SINK_STAGE_H
