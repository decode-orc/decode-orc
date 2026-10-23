/*
 * File:        nabts_sink_stage.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NABTS Sink Stage - recovers North American Basic Teletext
 *              data lines and exports the packet stream
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_STAGE_H
#define ORC_NABTS_SINK_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/streaming_capability.h>
#include <orc/stage/tooling/catalogue_results.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nabts_catalogue_view.h"
#include "nabts_sink_deps_interface.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

class IStageServices;

/**
 * @brief The receiver a page is drawn against until a reader picks another
 *
 * The 256 by 200 grid of X3.110 Table D1 item 10 is the resolution the
 * standard's own service reference model requires, and so what a set-top
 * decoder of the period put on screen; anything sharper is a later reading of
 * the same page rather than the one its author was working to.
 *
 * There is no parameter behind this. Which receiver a page is drawn for changes
 * nothing the recovery found — the records are read off the recording, and the
 * receiver decides only how they are drawn afterwards — so it is a way of
 * looking, offered on the viewer's own dropdown. Making it a parameter as well
 * would mean paying a full re-run of the recovery pass, which is what editing
 * any parameter costs, to change something that pass cannot see.
 */
inline constexpr NaplpsRenderMode kDefaultRenderMode =
    NaplpsRenderMode::kReference;

/**
 * @brief NABTS Sink Stage
 *
 * Extracts North American Basic Teletext (CEA-516, ITU-R BT.653 System C) data
 * lines from the VBI of a 525-line source and writes the recovered packets as a
 * flat, headerless packet stream in transmission coding, strictly temporally
 * ordered (frame → field → ascending line): `.t33` for the 33-byte data packet
 * of CEA-516 §3.1.
 *
 * The packets are also reassembled into data groups (§4) and teletext records
 * (§5) as the pass goes, and the records the range carried are catalogued for
 * the host to browse through ICatalogueResults.
 *
 * Separate from the Teletext Sink because the two services share nothing above
 * the packet — see docs-tech/nabts-support-design.md §2.
 *
 * This is a SINK stage - it has inputs but no outputs. All work happens in
 * trigger(); execute() only caches the input so the node can preview before it
 * has been triggered.
 */
class NabtsSinkStage : public DAGStage,
                       public ParameterizedStage,
                       public TriggerableStage,
                       public StageToolProvider,
                       public IStagePreviewCapability,
                       public ICatalogueResults,
                       public IStreamingCompatibility {
 public:
  explicit NabtsSinkStage(IStageServices* stage_services);
  ~NabtsSinkStage() override = default;

  /// Testing seam: inject a pre-built deps instance to substitute concrete
  /// dep creation in trigger().
  void set_deps_override(std::shared_ptr<INabtsSinkStageDeps> deps) {
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
  const NabtsAnalysisDataset& dataset() const { return dataset_; }

  // ICatalogueResults interface. Running every record's presentation code into
  // a display list costs real work and memory, and a run that only exports a
  // packet stream never needs it, so it is built on first ask and cached.
  // Called from the host's render worker, hence the lock.
  const CatalogueDataset& catalogue() const override {
    return catalogue_for(std::string());
  }

  // The same records drawn for a receiver the reader picked in the viewer
  // rather than the one the project's parameter names. Only the display lists
  // differ, so this rebuilds the catalogue and nothing above it: the records
  // themselves were recovered once and are not read again.
  const CatalogueDataset& catalogue(
      const std::string& view_option) const override {
    return catalogue_for(view_option);
  }

  // The same records read with or without the bytes they lost rebuilt, which
  // the reader switches between while looking. Only the interpretation
  // differs: the records themselves were recovered once and are not read
  // again, and what a run exports is untouched by this.
  const CatalogueDataset& catalogue(
      const std::string& view_option,
      const std::vector<std::string>& active_toggles) const override {
    const auto active = [&](const char* id) {
      return std::find(active_toggles.begin(), active_toggles.end(),
                       std::string(id)) != active_toggles.end();
    };
    return catalogue_for(view_option, active(kNabtsReconstructToggleId),
                         active(kNabtsReceiverPaceToggleId),
                         active(kNabtsTu2000xPaceToggleId));
  }

  // A page that links keys to its macros, played: presented as the catalogue
  // presents it under the reader's options, then each key pressed on it since
  // it was shown. Replayed from the recovered records every time, so the same
  // keys always give the same page and nothing is held between calls.
  CataloguePayload played(
      const std::string& item_id, const std::vector<std::string>& keys,
      const std::string& view_option,
      const std::vector<std::string>& active_toggles) const override {
    const auto active = [&](const char* id) {
      return std::find(active_toggles.begin(), active_toggles.end(),
                       std::string(id)) != active_toggles.end();
    };
    // The lock the catalogue is built under, so the dataset is not replaced by
    // a trigger while a page of it is being played.
    std::lock_guard<std::mutex> lock(catalogue_mutex_);
    return build_nabts_played_payload(
        dataset_, item_id, keys,
        naplps_render_mode_from_name(view_option, kDefaultRenderMode),
        active(kNabtsReconstructToggleId), active(kNabtsReceiverPaceToggleId),
        active(kNabtsTu2000xPaceToggleId));
  }

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  std::vector<StageToolDescriptor> get_stage_tools() const override {
    return {StageToolDescriptor{
        "nabts_analysis", "NABTS Records",
        "Decode the NABTS service and browse the records it carried.",
        StageToolKind::CatalogueBrowser, false, kCatalogueBrowserContractId}};
  }

  // IStreamingCompatibility interface. Whether "-" is actually usable also
  // depends on export_records/export_captions/write_report, which this
  // pre-flight check has no visibility into (see parse_config()/analyse());
  // that combination is refused with a clear error at trigger() time instead.
  bool supports_streaming_execution() const override { return true; }

 private:
  /// The catalogue as drawn for the receiver @p view_option names, or for the
  /// reference receiver where it names none — which an empty string and an
  /// unrecognised one both do. Built if what is held was built for another
  /// receiver. One is held rather than one per receiver: the host copies what
  /// it is given before it asks again, and a page of deposited pixels is a
  /// large thing to keep four copies of for a reader looking at one.
  const CatalogueDataset& catalogue_for(const std::string& view_option,
                                        bool reconstruct = false,
                                        bool receiver_pace = false,
                                        bool tu2000x_pace = false) const {
    std::lock_guard<std::mutex> lock(catalogue_mutex_);
    const NaplpsRenderMode mode =
        naplps_render_mode_from_name(view_option, kDefaultRenderMode);
    const NaplpsReceiverPace pace =
        naplps_receiver_pace_for(receiver_pace, tu2000x_pace);
    if (!catalogue_ || catalogue_mode_ != mode ||
        catalogue_reconstruct_ != reconstruct || catalogue_pace_ != pace) {
      // The catalogue held goes before the next is built, so the two are
      // never held at once.
      catalogue_.reset();
      catalogue_ = std::make_unique<CatalogueDataset>(build_nabts_catalogue(
          dataset_, mode, reconstruct, receiver_pace, tu2000x_pace));
      catalogue_mode_ = mode;
      catalogue_reconstruct_ = reconstruct;
      catalogue_pace_ = pace;
    } else {
      // The same pages under another pair of pace toggles that come to the
      // same receiver: only the boxes the reader set change.
      for (CatalogueViewToggle& toggle : catalogue_->schema.toggles) {
        if (toggle.id == kNabtsReceiverPaceToggleId) {
          toggle.active = receiver_pace;
        } else if (toggle.id == kNabtsTu2000xPaceToggleId) {
          toggle.active = tu2000x_pace;
        }
      }
    }
    return *catalogue_;
  }

  /// Drop the cached catalogue so the next reader rebuilds it from the dataset
  /// that has just replaced the one it was built from.
  void invalidate_catalogue() {
    std::lock_guard<std::mutex> lock(catalogue_mutex_);
    catalogue_.reset();
  }

  // Parses the parameter set into deps options, converting the 1-based UI line
  // window to the 0-based field lines the slicer uses. Throws
  // std::runtime_error on missing/invalid parameters.
  NabtsSinkOptions parse_config(
      const std::map<std::string, ParameterValue>& parameters) const;

  std::map<std::string, ParameterValue> parameters_;
  std::string trigger_status_{"Idle"};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool> is_processing_{false};
  std::atomic<bool> cancel_requested_{false};
  IStageServices* stage_services_{nullptr};
  std::shared_ptr<INabtsSinkStageDeps> deps_override_;

  // Cached results of the last trigger, resolved into the host-facing
  // catalogue on demand.
  NabtsAnalysisDataset dataset_;
  bool has_results_{false};
  // Built from dataset_ on first catalogue() call; cleared with it.
  mutable std::mutex catalogue_mutex_;
  mutable std::unique_ptr<CatalogueDataset> catalogue_;
  // The receiver catalogue_ was built for, which is the reference one unless a
  // reader asked the viewer for another.
  mutable NaplpsRenderMode catalogue_mode_{NaplpsRenderMode::kReference};
  // Whether catalogue_ was built with the records' lost bytes rebuilt. Part of
  // the cache key for the same reason the receiver is: both change the payloads
  // and neither changes the records.
  mutable bool catalogue_reconstruct_{false};
  // The receiver whose pace catalogue_ was built at, if any. Keyed like the
  // rest: it moves when each figure appears, not what the records hold.
  mutable NaplpsReceiverPace catalogue_pace_{NaplpsReceiverPace::kOff};
  mutable std::shared_ptr<const VideoFrameRepresentation> cached_input_;
};

}  // namespace orc

#endif  // ORC_NABTS_SINK_STAGE_H
