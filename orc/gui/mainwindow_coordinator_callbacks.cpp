/*
 * File:        mainwindow_coordinator_callbacks.cpp
 * Module:      orc-gui
 * Purpose:     RenderCoordinator callback implementations for MainWindow
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <QMessageBox>
#include <QProgressDialog>
#include <QStatusBar>
#include <algorithm>
#include <limits>
#include <optional>

#include "burstlevelanalysisdialog.h"
#include "cataloguedialog.h"
#include "closedcaptiondialog.h"
#include "dropoutanalysisdialog.h"
#include "fieldpreviewwidget.h"
#include "frame_profiler.h"
#include "logging.h"
#include "mainwindow.h"
#include "ntscobserverdialog.h"
#include "observation_status_formatter.h"
#include "presenters/include/render_presenter.h"
#include "presenters/include/vbi_view_models.h"
#include "previewdialog.h"
#include "snranalysisdialog.h"
#include "vbidialog.h"
#include "videoparameterobserverdialog.h"

// Coordinator response slot implementations

void MainWindow::onPreviewReady(uint64_t request_id,
                                PreviewRenderDeliveryPtr delivery) {
  // Ignore stale responses
  if (request_id != pending_preview_request_id_) {
    ORC_LOG_DEBUG("Ignoring stale preview response (id {} != {})", request_id,
                  pending_preview_request_id_);
    return;
  }
  if (!delivery) {
    return;
  }
  const orc::PreviewRenderResult& result = delivery->result;

  orc::gui::FrameProfiler::instance().markPreviewReady();
  // The worker's own breakdown of the wait that just ended. Attributing it is
  // what says whether a playing preview is bounded by re-executing the graph,
  // by filling the observation store a second time, or by neither.
  orc::gui::FrameProfiler::instance().addStage(
      orc::gui::FrameStage::kDagExecution, delivery->cost.dag_execution_us);
  orc::gui::FrameProfiler::instance().addStage(
      orc::gui::FrameStage::kObservationFill,
      delivery->cost.observation_fill_us);

  ORC_LOG_DEBUG("onPreviewReady: request_id={}, success={}", request_id,
                result.success);

  if (result.success) {
    if (!delivery->frame_image.isNull()) {
      // Already expanded on the worker for the GPU surface.
      preview_dialog_->previewWidget()->setImage(delivery->frame_image,
                                                 result.image.dropout_regions);
    } else {
      // Use public API image directly - no conversion needed
      preview_dialog_->previewWidget()->setImage(result.image);
    }
  } else {
    preview_dialog_->previewWidget()->clearImage();
    statusBar()->showMessage(
        QString("Render ERROR at stage %1: %2")
            .arg(QString::fromStdString(current_view_node_id_.to_string()))
            .arg(QString::fromStdString(result.error_message)),
        5000);
  }

  // Get the index we just rendered.
  const int rendered_index = pending_render_index_;

  endPreviewRenderInFlight();

  // Refresh vectorscope after every completed render so it stays in sync
  // with the preview as the user steps through frames — not only when
  // navigation has settled.
  preview_dialog_->setSharedPreviewCoordinate(buildCurrentPreviewCoordinate());
  applyDeliveredScopes(delivery->scopes);

  // The frame is on screen and every consumer has been served: close the
  // profiler's account of it. The follow-up render below opens a new frame.
  orc::gui::FrameProfiler::instance().endFrame();

  // If the user navigated while we were rendering, the dialog's current
  // index will already differ from what we just rendered — issue a follow-up.
  if (preview_dialog_->currentIndex() != rendered_index) {
    ORC_LOG_DEBUG(
        "Render queue clear - re-rendering for latest position: {} (just "
        "rendered {})",
        preview_dialog_->currentIndex(), rendered_index);
    updateAllPreviewComponents();
    return;
  }
}

void MainWindow::onVBIDataReady(uint64_t request_id,
                                orc::presenters::VBIFieldInfoView info) {
  // A frame's reading needs both of its fields, and only a frame newer than
  // the one already shown is worth showing. Testing against the fields
  // currently being asked about instead stopped this dialogue updating at all
  // while the vectorscope was open - see ResponsePairGate.
  auto reading = vbi_gate_.deliver(request_id, std::move(info));
  if (!reading) {
    return;
  }

  ORC_LOG_DEBUG("onVBIDataReady: request_id={}", request_id);

  // Delivered whether or not the dialog is currently visible, so that when it
  // is shown it already has the latest data.
  if (!vbi_dialog_ || !vbi_dialog_->isVisible()) {
    return;
  }

  if (reading->has_second) {
    vbi_dialog_->updateVBIInfoFrame(reading->first, reading->second);
  } else {
    vbi_dialog_->updateVBIInfo(reading->first);
  }
}

void MainWindow::onClosedCaptionDataReady(
    uint64_t request_id, bool available, qulonglong field1_id_value,
    orc::presenters::ClosedCaptionFieldDataView field1,
    qulonglong /*field2_id_value*/,
    orc::presenters::ClosedCaptionFieldDataView field2) {
  const auto pending = pending_closed_caption_requests_.find(request_id);
  if (pending == pending_closed_caption_requests_.end()) {
    return;  // stale / superseded response
  }
  pending_closed_caption_requests_.erase(pending);

  if (!closed_caption_dialog_) {
    return;
  }

  // Feed the dialog's caption cache whether or not it is currently visible, so
  // that when it is shown again the window is already warm.
  closed_caption_dialog_->deliverFrameData(
      available, static_cast<uint64_t>(field1_id_value), field1, field2);

  // The dialog hands out the frames it needs a batch at a time, so a delivery
  // is what draws down the next of them; without this the window would only
  // ever fill one batch per frame change.
  issueClosedCaptionRequests();
}

void MainWindow::onObservationDataReady(
    uint64_t request_id, bool available, qulonglong field_id_value,
    orc::presenters::VideoParameterObservationView video_params,
    orc::presenters::NtscFieldObservationsView ntsc) {
  FieldObservation field;
  field.field_id =
      orc::FieldID(static_cast<orc::FieldID::value_type>(field_id_value));
  field.available = available;
  field.video_params = std::move(video_params);
  field.ntsc = std::move(ntsc);

  // As in onVBIDataReady: both fields of a frame, newest completed frame wins.
  auto reading = observation_gate_.deliver(request_id, std::move(field));
  if (!reading) {
    return;
  }

  ORC_LOG_DEBUG("onObservationDataReady: request_id={}", request_id);

  const bool vp_visible = video_parameter_observer_dialog_ &&
                          video_parameter_observer_dialog_->isVisible();
  const bool ntsc_visible =
      ntsc_observer_dialog_ && ntsc_observer_dialog_->isVisible();

  // Every field the reading covers has to have been observed; a frame with
  // half its observations is not a frame the dialogues can show.
  const bool complete = reading->first.available &&
                        (!reading->has_second || reading->second.available);

  if (!complete) {
    if (vp_visible) {
      video_parameter_observer_dialog_->clearObservations();
    }
    if (ntsc_visible) {
      ntsc_observer_dialog_->clearObservations();
    }
    return;
  }

  if (reading->has_second) {
    if (vp_visible) {
      video_parameter_observer_dialog_->updateObservationsForFrame(
          reading->first.field_id, reading->first.video_params,
          reading->second.field_id, reading->second.video_params);
    }
    if (ntsc_visible) {
      ntsc_observer_dialog_->updateObservationsForFrame(
          reading->first.field_id, reading->first.ntsc,
          reading->second.field_id, reading->second.ntsc);
    }
    return;
  }

  if (vp_visible) {
    video_parameter_observer_dialog_->updateObservations(
        reading->first.field_id, reading->first.video_params);
  }
  if (ntsc_visible) {
    ntsc_observer_dialog_->updateObservations(reading->first.field_id,
                                              reading->first.ntsc);
  }
}

void MainWindow::onObservationProgress(bool active, int percent_complete,
                                       bool computing,
                                       qulonglong /*outstanding_nodes*/,
                                       bool sweep_paused) {
  const std::string message = orc::gui::formatObservationStatus(
      active, percent_complete, computing, sweep_paused);
  if (message.empty()) {
    statusBar()->clearMessage();
  } else {
    statusBar()->showMessage(QString::fromStdString(message));
  }
}

void MainWindow::onExecutionProgress(int node_id_value, qulonglong current,
                                     qulonglong total) {
  // Only the project-load modal consumes these; ordinary stage selection has
  // its own "Rendering..." feedback on the preview window.
  if (!project_load_in_progress_) {
    return;
  }

  // Name the node the way the rest of the UI does: user label first, stage name
  // otherwise, node id as a last resort.
  const orc::NodeID node_id(node_id_value);
  QString label = QString::fromStdString(node_id.to_string());
  const auto nodes = project_.presenter()->getNodes();
  for (const auto& node : nodes) {
    if (node.node_id != node_id) {
      continue;
    }
    if (!node.label.empty()) {
      label = QString::fromStdString(node.label);
    } else if (!node.stage_name.empty()) {
      label = QString::fromStdString(node.stage_name);
    }
    break;
  }

  ORC_LOG_DEBUG("Project load: executing '{}' ({} of {})", label.toStdString(),
                current, total);

  project_load_stage_label_ = label;
  project_load_current_ = current;
  project_load_total_ = total;
  updateProjectLoadProgressLabel();
}

void MainWindow::onObservationsInvalidated(QVector<int> changed_node_ids) {
  // If the node currently being viewed had its observations invalidated,
  // re-issue the async request so the open dialogs refresh with new data.
  if (!current_view_node_id_.is_valid()) {
    return;
  }
  for (int node_value : changed_node_ids) {
    if (node_value == current_view_node_id_.value()) {
      refreshObserverDialogs();
      break;
    }
  }
}

void MainWindow::onAvailableOutputsReady(
    uint64_t request_id, std::vector<orc::PreviewOutputInfo> outputs) {
  if (request_id != pending_outputs_request_id_) {
    return;
  }

  ORC_LOG_DEBUG("onAvailableOutputsReady: request_id={}, count={}", request_id,
                outputs.size());

  // The worker has finished everything a project load waits on (renderer
  // rebuild plus the DAG execution behind this query), so retire the modal
  // before the preview dialog is shown below.
  endProjectLoadProgress();

  available_outputs_ = std::move(outputs);

  // Try to preserve current option_id AND output_type across node switches
  bool found_match = false;
  for (const auto& output : available_outputs_) {
    if (output.option_id == current_option_id_ &&
        output.type == current_output_type_) {
      found_match = true;
      ORC_LOG_DEBUG("Preserved option_id '{}' and output_type={}",
                    current_option_id_, static_cast<int>(current_output_type_));
      break;
    }
  }
  // Fallback: preserve option_id only if exact type match is unavailable
  if (!found_match) {
    for (const auto& output : available_outputs_) {
      if (output.option_id == current_option_id_) {
        current_output_type_ = output.type;
        found_match = true;
        ORC_LOG_DEBUG("Preserved option_id '{}' with fallback output_type={}",
                      current_option_id_,
                      static_cast<int>(current_output_type_));
        break;
      }
    }
  }
  // Fallback: preserve output type if possible, even if option_id changes
  if (!found_match) {
    for (const auto& output : available_outputs_) {
      if (output.type == current_output_type_) {
        current_option_id_ = output.option_id;
        found_match = true;
        ORC_LOG_DEBUG("Preserved output_type {} with fallback option_id='{}'",
                      static_cast<int>(current_output_type_),
                      current_option_id_);
        break;
      }
    }
  }

  // If current option not available, try to find a sensible default
  if (!found_match && !available_outputs_.empty()) {
    // Prefer "interlaced_clamped" if available, otherwise use first output
    bool found_frame = false;
    for (const auto& output : available_outputs_) {
      if (output.option_id == "interlaced_clamped") {
        current_output_type_ = output.type;
        current_option_id_ = output.option_id;
        found_frame = true;
        break;
      }
    }
    if (!found_frame) {
      current_output_type_ = available_outputs_[0].type;
      current_option_id_ = available_outputs_[0].option_id;
    }
  }

  // Check if we should show preview dialog
  bool is_real_node = current_view_node_id_.is_valid();
  bool has_valid_content = false;
  for (const auto& output : available_outputs_) {
    if (output.is_available) {
      has_valid_content = true;
      break;
    }
  }

  bool auto_show_enabled =
      auto_show_preview_action_ && auto_show_preview_action_->isChecked();

  // Enable the Show Preview menu action whenever there's valid content
  if (is_real_node && has_valid_content) {
    show_preview_action_->setEnabled(true);
  }

  // Auto-show the preview dialog only if the setting is enabled
  if (!preview_dialog_->isVisible() && is_real_node && has_valid_content &&
      auto_show_enabled) {
    preview_dialog_->show();
  }

  // Update preview dialog to show current node
  // Get node label from project (prefer label, fallback to stage_name)
  const auto nodes = project_.presenter()->getNodes();
  auto node_it = std::find_if(nodes.begin(), nodes.end(),
                              [this](const orc::presenters::NodeInfo& n) {
                                return n.node_id == current_view_node_id_;
                              });
  QString node_label;
  if (node_it != nodes.end()) {
    if (!node_it->label.empty()) {
      node_label = QString::fromStdString(node_it->label);
    } else if (!node_it->stage_name.empty()) {
      node_label = QString::fromStdString(node_it->stage_name);
    } else {
      node_label = QString::fromStdString(current_view_node_id_.to_string());
    }
  } else {
    node_label = QString::fromStdString(current_view_node_id_.to_string());
  }
  preview_dialog_->setCurrentNode(
      node_label, QString::fromStdString(current_view_node_id_.to_string()));
  preview_dialog_->setCurrentNodeId(current_view_node_id_);

  // Update status bar to show which stage is being viewed
  QString node_display =
      QString::fromStdString(current_view_node_id_.to_string());
  statusBar()->showMessage(
      QString("Viewing output from stage: %1").arg(node_display), 5000);

  // Update UI controls
  updatePreviewModeCombo();
  refreshViewerControls();
  refreshPreviewViewAvailability();
  updateUIState();

  // If line scope is visible, refresh it after outputs and controls are updated
  refreshLineScopeForCurrentStage();

  // Update dropouts button state based on current output's availability
  // Find the current output info to check if dropouts are available
  bool dropouts_available = false;
  for (const auto& output : available_outputs_) {
    if (output.option_id == current_option_id_ &&
        output.type == current_output_type_) {
      dropouts_available = output.dropouts_available;
      break;
    }
  }

  // Update dropouts button - disable and turn off if not available
  if (preview_dialog_ && preview_dialog_->dropoutsButton()) {
    if (!dropouts_available) {
      // Disable and turn off dropouts for stages where they're not available
      // (e.g., chroma decoder)
      preview_dialog_->dropoutsButton()->setEnabled(false);
      preview_dialog_->dropoutsButton()->setChecked(false);
      render_coordinator_->setShowDropouts(false);
    } else {
      // Re-enable dropouts button for stages that support it
      preview_dialog_->dropoutsButton()->setEnabled(true);
    }
  }

  // Request initial preview
  updatePreview();
}

void MainWindow::onAudioChannelPairsReady(
    uint64_t request_id, std::vector<orc::AudioPairView> pairs) {
  if (request_id != pending_audio_pairs_request_id_) {
    return;  // Superseded by a later node selection
  }
  pending_audio_pairs_request_id_ = 0;

  ORC_LOG_DEBUG("onAudioChannelPairsReady: request_id={}, pairs={}", request_id,
                pairs.size());

  // An empty list is the normal answer for most projects: the selector then
  // shows a disabled "Mute/None" and playback keeps its video-only path.
  preview_dialog_->setAudioChannelPairs(pairs);
}

void MainWindow::onAudioStreamReaderReady(
    uint64_t request_id,
    std::shared_ptr<orc::presenters::IAudioStreamReader> reader) {
  if (request_id != pending_audio_reader_request_id_) {
    return;  // Superseded by a later pair selection
  }
  pending_audio_reader_request_id_ = 0;

  ORC_LOG_DEBUG("onAudioStreamReaderReady: request_id={}, usable={}",
                request_id, reader != nullptr);

  // A null reader means the pair turned out to be unplayable; the dialogue
  // falls back to timer-paced video-only playback.
  preview_dialog_->setAudioStreamReader(std::move(reader));
}

void MainWindow::onTriggerProgress(size_t current, size_t total,
                                   QString message) {
  // Ignore progress updates if we're not waiting for a trigger
  // This prevents race conditions where progress arrives after completion
  if (pending_trigger_request_id_ == 0) {
    return;
  }

  // Check validity - don't make a local copy of QPointer as it won't track
  // deletion We must check the member variable directly each time to ensure
  // Qt's tracking works
  if (!trigger_progress_dialog_ || total == 0) {
    return;
  }

  int percentage = static_cast<int>((current * 100) / total);

  // Prevent setting value to 100 which would trigger reset() - let
  // onTriggerComplete handle cleanup QProgressDialog calls reset() internally
  // when value reaches maximum, which can cause crashes if the dialog is being
  // deleted or is in an invalid state
  if (percentage >= 100) {
    percentage = 99;  // Cap at 99% to prevent automatic reset()
  }

  // Re-check member variable directly before each call in case dialog was
  // deleted
  if (trigger_progress_dialog_) {
    trigger_progress_dialog_->setValue(percentage);
  }
  if (trigger_progress_dialog_) {
    trigger_progress_dialog_->setLabelText(message);
  }
}

void MainWindow::onTriggerComplete(uint64_t request_id, bool success,
                                   QString status) {
  if (request_id != pending_trigger_request_id_) {
    return;
  }

  ORC_LOG_DEBUG("onTriggerComplete: success={}, status={}", success,
                status.toStdString());

  // CRITICAL: Clear pending request ID FIRST to stop any racing progress
  // updates This ensures onTriggerProgress will ignore any queued signals
  pending_trigger_request_id_ = 0;

  // Close and delete progress dialog safely
  if (trigger_progress_dialog_) {
    // Disconnect and block all signals from the dialog to prevent any callbacks
    // during destruction
    disconnect(trigger_progress_dialog_, nullptr, this, nullptr);
    trigger_progress_dialog_->blockSignals(true);
    // Reset to 0 before deletion to prevent any internal reset() call
    trigger_progress_dialog_->setValue(0);
    trigger_progress_dialog_->hide();
    trigger_progress_dialog_
        ->deleteLater();  // Use deleteLater for safe asynchronous deletion
                          // QPointer will be nulled when dialog is deleted
  }

  // If trigger was successful, automatically create dialog and request analysis
  // data for display.
  //
  // IMPORTANT: createAndShowAnalysisDialog must be deferred to the next event
  // loop iteration via QueuedConnection. QProgressDialog::setValue() calls
  // QCoreApplication::processEvents() internally; if triggerComplete arrives
  // during that processEvents (i.e. we are on the call stack of the trigger
  // dialog's setValue), running createAndShowAnalysisDialog synchronously
  // causes a second modal dialog's setValue to trigger another processEvents,
  // during which the DeferredDelete for the trigger dialog fires and destroys
  // it while its setValue stack frame is still live. The dangling d-pointer
  // then causes a null-dereference crash in QProgressBar::maximum().
  if (success && pending_trigger_node_id_.is_valid()) {
    const auto nodes = project_.presenter()->getNodes();
    auto node_it = std::find_if(nodes.begin(), nodes.end(),
                                [this](const orc::presenters::NodeInfo& n) {
                                  return n.node_id == pending_trigger_node_id_;
                                });

    if (node_it != nodes.end()) {
      orc::NodeID analysis_node_id = pending_trigger_node_id_;
      std::string analysis_stage_name = node_it->stage_name;
      QMetaObject::invokeMethod(
          this,
          [this, analysis_node_id, analysis_stage_name]() {
            createAndShowAnalysisDialog(analysis_node_id, analysis_stage_name);
          },
          Qt::QueuedConnection);
    }
  }

  // Show result
  if (success) {
    statusBar()->showMessage(status, 5000);
  } else {
    QMessageBox::warning(this, "Trigger Failed", status);
  }

  // Clear trigger state
  pending_trigger_node_id_ = orc::NodeID();
}

std::optional<orc::NodeID> MainWindow::takePendingResultsRead(
    uint64_t request_id) {
  const auto take =
      [request_id](std::unordered_map<uint64_t, orc::NodeID>& pending)
      -> std::optional<orc::NodeID> {
    auto it = pending.find(request_id);
    if (it == pending.end()) {
      return std::nullopt;
    }
    const orc::NodeID node_id = it->second;
    pending.erase(it);
    return node_id;
  };

  if (const auto node_id = take(pending_dropout_requests_)) return node_id;
  if (const auto node_id = take(pending_snr_requests_)) return node_id;
  if (const auto node_id = take(pending_burst_level_requests_)) return node_id;
  if (const auto node_id = take(pending_catalogue_requests_)) return node_id;
  return std::nullopt;
}

void MainWindow::onResultsNotAvailable(uint64_t request_id) {
  const auto node_id = takePendingResultsRead(request_id);
  if (!node_id) {
    return;  // stale response, or not a results read
  }

  ORC_LOG_DEBUG("Results read {} for node '{}' found nothing to read",
                request_id, node_id->to_string());

  // Not an error: the node simply has not been triggered since the DAG was
  // last built, which is also what a parameter edit leaves behind.
  const QString advice =
      tr("This stage has not been triggered yet, so there are no results to "
         "show.\n\nRight-click the stage and choose Trigger Stage to "
         "produce them.");

  auto dialog_it = catalogue_dialogs_.find(*node_id);
  if (dialog_it != catalogue_dialogs_.end() && dialog_it->second) {
    dialog_it->second->showError(advice);
  }

  // Deferred for the reason spelled out in onTriggerComplete: a modal opened
  // on this stack could nest an event loop under a live setValue() frame.
  QMetaObject::invokeMethod(
      this,
      [this, advice]() {
        QMessageBox::information(this, tr("Nothing to Show"), advice);
      },
      Qt::QueuedConnection);
}

bool MainWindow::reportFailedResultsRead(uint64_t request_id,
                                         const QString& message) {
  const auto node_id = takePendingResultsRead(request_id);
  if (!node_id) {
    return false;
  }

  ORC_LOG_ERROR("Results read {} for node '{}' failed: {}", request_id,
                node_id->to_string(), message.toStdString());

  // The catalogue viewer opens empty and pending; without this it would sit on
  // "Decoding..." for a read that has already failed.
  auto dialog_it = catalogue_dialogs_.find(*node_id);
  if (dialog_it != catalogue_dialogs_.end() && dialog_it->second) {
    dialog_it->second->showError(message);
  }

  // The reader asked for this and is waiting on it, so say so where they are
  // looking rather than in a status-bar message they may never see. Deferred
  // for the reason spelled out in onTriggerComplete.
  QMetaObject::invokeMethod(
      this,
      [this, message]() {
        QMessageBox::warning(this, tr("Analysis Failed"), message);
      },
      Qt::QueuedConnection);
  return true;
}

void MainWindow::onCoordinatorError(uint64_t request_id, QString message) {
  // Check if this is a line sample request error
  if (request_id == pending_line_sample_request_id_) {
    pending_line_sample_request_id_ = 0;

    // Line sample errors are expected for sink stages - log at DEBUG
    ORC_LOG_DEBUG(
        "Coordinator line sample error (request {}): {} (expected for sink "
        "stages)",
        request_id, message.toStdString());

    // Show empty line scope with appropriate message
    if (preview_dialog_ && preview_dialog_->isLineScopeVisible()) {
      ORC_LOG_DEBUG(
          "Line samples not available for this stage, showing empty line "
          "scope");

      QString node_id_str =
          QString::fromStdString(current_view_node_id_.to_string());

      // Calculate stage index (1-based) from the current node
      int stage_index = 1;
      const auto nodes = project_.presenter()->getNodes();
      for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].node_id == current_view_node_id_) {
          stage_index = static_cast<int>(i) + 1;  // Convert to 1-based
          break;
        }
      }

      // Show empty line scope (no samples) - this will display "No data
      // available for this line"
      preview_dialog_->showLineScope(node_id_str, stage_index, 0, 0, 0,
                                     std::vector<int16_t>(),  // Empty samples
                                     std::nullopt, 0, 0, 0,
                                     current_output_type_);
    }

    // Show brief message in status bar
    statusBar()->showMessage(QString("Line data not available for this stage"),
                             3000);
    return;
  }

  // For other errors, log at ERROR level
  ORC_LOG_ERROR("Coordinator error (request {}): {}", request_id,
                message.toStdString());

  // Clear any in-flight preview render state. A preview/available-outputs
  // request that resolves via the error signal (e.g. a null render presenter
  // because a stage plugin is missing) would otherwise leave the "Rendering..."
  // title/timer armed forever — this is the root of issue #209's stuck preview.
  // endPreviewRenderInFlight() was previously only reachable from
  // onPreviewReady.
  endPreviewRenderInFlight();

  // Same reasoning for the project-load modal: a failed available-outputs
  // request resolves through this signal, and the modal has no cancel button,
  // so leaving it up would wedge the application.
  endProjectLoadProgress();

  // A results read that failed outright (rather than simply finding nothing)
  // resolves through this signal, and its viewer is sitting open and empty
  // waiting for data that is not coming.
  if (reportFailedResultsRead(request_id, message)) {
    return;
  }

  // Show error in status bar for other errors
  statusBar()->showMessage(QString("Error: %1").arg(message), 5000);
}

void MainWindow::onDropoutDataReady(
    uint64_t request_id, orc::presenters::DropoutDisplaySeries series) {
  // Find which node this request was for
  auto req_it = pending_dropout_requests_.find(request_id);
  if (req_it == pending_dropout_requests_.end()) {
    ORC_LOG_DEBUG(
        "Ignoring stale dropout data response (unknown request_id {})",
        request_id);
    return;
  }

  orc::NodeID node_id = req_it->second;
  pending_dropout_requests_.erase(req_it);

  const int32_t total_frames = series.total_frames;

  ORC_LOG_DEBUG("onDropoutDataReady for node '{}': {} points, total={}",
                node_id.to_string(), series.points.size(), total_frames);

  // Find the dialog for this stage
  auto dialog_it = dropout_analysis_dialogs_.find(node_id);
  if (dialog_it == dropout_analysis_dialogs_.end() || !dialog_it->second ||
      !dialog_it->second->isVisible()) {
    return;
  }

  auto* dialog = dialog_it->second;

  // If no data available, show message
  if (series.points.empty() || total_frames == 0) {
    dialog->showNoDataMessage(
        "No dropout analysis data available.\n\n"
        "Make sure dropout detection is enabled in the pipeline.");
    return;
  }

  // Start update cycle
  dialog->startUpdate(total_frames, series.decimated);

  // Add all display points (buckets)
  for (const auto& point : series.points) {
    if (point.bucket.has_data) {
      dialog->addDataPoint(point.bucket.frame_label,
                           static_cast<double>(point.dropout_length_samples),
                           point.bucket.frame_start, point.bucket.frame_end,
                           point.dropout_count);
    }
  }

  // Finish update with current frame marker
  int32_t current_frame = 1;  // Default to first frame
  if (preview_dialog_ && preview_dialog_->previewSlider()) {
    current_frame =
        static_cast<int32_t>(preview_dialog_->previewSlider()->value()) + 1;
  }

  dialog->finishUpdate(current_frame);

  // Bring the graph window to the front now that it has data
  dialog->raise();
  dialog->activateWindow();
}

void MainWindow::onSNRDataReady(uint64_t request_id,
                                orc::presenters::SNRDisplaySeries series) {
  // Find which node this request was for
  auto req_it = pending_snr_requests_.find(request_id);
  if (req_it == pending_snr_requests_.end()) {
    ORC_LOG_DEBUG("Ignoring stale SNR data response (unknown request_id {})",
                  request_id);
    return;
  }

  orc::NodeID node_id = req_it->second;
  pending_snr_requests_.erase(req_it);

  const int32_t total_frames = series.total_frames;

  ORC_LOG_DEBUG("onSNRDataReady for node '{}': {} points, total={}",
                node_id.to_string(), series.points.size(), total_frames);

  // Find the dialog for this stage
  auto dialog_it = snr_analysis_dialogs_.find(node_id);
  if (dialog_it == snr_analysis_dialogs_.end() || !dialog_it->second ||
      !dialog_it->second->isVisible()) {
    return;
  }

  auto* dialog = dialog_it->second;

  // If no data available, show message
  if (series.points.empty() || total_frames == 0) {
    dialog->showNoDataMessage(
        "No SNR analysis data available.\n\n"
        "Make sure VITS (Vertical Interval Test Signal) is present in the "
        "source.");
    return;
  }

  // Start update cycle
  dialog->startUpdate(total_frames, series.decimated);

  // Add all display points (buckets)
  for (const auto& point : series.points) {
    if (point.bucket.has_data) {
      double white_snr = point.has_white_snr
                             ? point.white_snr
                             : std::numeric_limits<double>::quiet_NaN();
      double black_psnr = point.has_black_psnr
                              ? point.black_psnr
                              : std::numeric_limits<double>::quiet_NaN();
      dialog->addDataPoint(point.bucket.frame_label, white_snr, black_psnr,
                           point.bucket.frame_start, point.bucket.frame_end);
    }
  }

  // Finish update with current frame marker
  int32_t current_frame = 1;  // Default to first frame
  if (preview_dialog_ && preview_dialog_->previewSlider()) {
    current_frame =
        static_cast<int32_t>(preview_dialog_->previewSlider()->value()) + 1;
  }

  dialog->finishUpdate(current_frame);

  // Bring the graph window to the front now that it has data
  dialog->raise();
  dialog->activateWindow();
}

void MainWindow::onBurstLevelDataReady(
    uint64_t request_id, orc::presenters::BurstLevelDisplaySeries series) {
  // Find which node this request was for
  auto req_it = pending_burst_level_requests_.find(request_id);
  if (req_it == pending_burst_level_requests_.end()) {
    ORC_LOG_DEBUG(
        "Ignoring stale burst level data response (unknown request_id {})",
        request_id);
    return;
  }

  orc::NodeID node_id = req_it->second;
  pending_burst_level_requests_.erase(req_it);

  const int32_t total_frames = series.total_frames;

  ORC_LOG_DEBUG("onBurstLevelDataReady for node '{}': {} points, total={}",
                node_id.to_string(), series.points.size(), total_frames);

  // Find the dialog for this stage
  auto dialog_it = burst_level_analysis_dialogs_.find(node_id);
  if (dialog_it == burst_level_analysis_dialogs_.end() || !dialog_it->second ||
      !dialog_it->second->isVisible()) {
    return;
  }

  auto* dialog = dialog_it->second;

  // If no data available, show message
  if (series.points.empty() || total_frames == 0) {
    dialog->showNoDataMessage(
        "No burst level data available.\n\n"
        "Color burst detection may have failed.");
    return;
  }

  // Fetch video parameters for amplitude unit conversion.
  std::optional<orc::presenters::VideoParametersView> video_params;
  auto* core_project = project_.presenter()->getCoreProjectHandle();
  if (core_project) {
    orc::presenters::RenderPresenter render_presenter(core_project);
    // Throwaway helper presenter: parameter reads only — no sidecar,
    // scheduler, or sweeps (this runs on the GUI thread).
    render_presenter.setBackgroundObservationEnabled(false);
    render_presenter.setDAG(project_.getDAG());
    auto vp = render_presenter.getVideoParameters(node_id);
    if (vp.has_value()) {
      video_params = orc::presenters::toVideoParametersView(*vp);
    }
  }

  // Start update cycle
  dialog->startUpdate(total_frames, series.decimated);

  // Add all display points (buckets)
  bool first = true;
  for (const auto& point : series.points) {
    if (point.bucket.has_data) {
      dialog->addDataPoint(point.bucket.frame_label, point.median_burst_10bit,
                           point.bucket.frame_start, point.bucket.frame_end,
                           first ? video_params : std::nullopt);
      first = false;
    }
  }

  // Finish update with current frame marker
  int32_t current_frame = 1;
  if (preview_dialog_ && preview_dialog_->previewSlider()) {
    current_frame =
        static_cast<int32_t>(preview_dialog_->previewSlider()->value()) + 1;
  }

  dialog->finishUpdate(current_frame);

  // Bring the graph window to the front now that it has data
  dialog->raise();
  dialog->activateWindow();
}

void MainWindow::onCatalogueDataReady(uint64_t request_id,
                                      orc::CatalogueDataset data) {
  // Find which node this request was for
  auto req_it = pending_catalogue_requests_.find(request_id);
  if (req_it == pending_catalogue_requests_.end()) {
    ORC_LOG_DEBUG("Ignoring stale catalogue response (unknown request_id {})",
                  request_id);
    return;
  }

  orc::NodeID node_id = req_it->second;
  pending_catalogue_requests_.erase(req_it);

  ORC_LOG_DEBUG("onCatalogueDataReady for node '{}': {} items",
                node_id.to_string(), data.items.size());

  // Find the viewer for this node
  auto dialog_it = catalogue_dialogs_.find(node_id);
  if (dialog_it == catalogue_dialogs_.end() || !dialog_it->second) {
    return;
  }

  auto* dialog = dialog_it->second;
  dialog->setCatalogue(data);

  // Bring the viewer to the front now that it has data
  dialog->raise();
  dialog->activateWindow();
}
