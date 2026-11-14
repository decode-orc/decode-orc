/*
 * File:        core_view.cpp
 * Module:      orc-view_core
 * Purpose:     Entry points of the view binary for Decode-Orc's own stage
 *              tools, loaded by orc-gui like any plugin's view binary
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

// Like any view binary, this reaches its stages only through the
// IStageToolSession each window is opened with: no host header is included
// here, and only bytes cross
// (orc/plugins/stages/common/analysis-results/analysis_series_wire.h is the
// whole of what the analysis sinks and their windows share, and
// orc/plugins/stages/common/config-tools/stage_config_wire.h the whole of
// what Mask Line and the video sink share with their dialogs; the Dropout
// Editor shares only the parameter's text format,
// orc/plugins/stages/common/dropout-map/dropout_map_text.h, and the Frame Map
// Range Finder its contract id and parameter names,
// orc/plugins/stages/frame_map/frame_map_range_contract.h).

#include <orc/view/orc_view_registration.h>

#include <QDialog>
#include <QString>

#include "../../stages/common/analysis-results/analysis_series_wire.h"
#include "../../stages/common/config-tools/stage_config_wire.h"
#include "../../stages/common/dropout-map/dropout_map_text.h"
#include "../../stages/frame_map/frame_map_range_contract.h"
#include "analysis_session_controller.h"
#include "burstlevelanalysisdialog.h"
#include "config_session_controller.h"
#include "dropout_editor_dialog.h"
#include "dropoutanalysisdialog.h"
#include "ffmpegpresetdialog.h"
#include "frame_map_range_window.h"
#include "masklineconfigdialog.h"
#include "snranalysisdialog.h"

namespace {

// Each window is titled for its node and parented to the main window; its
// controller is parented to it, so it goes when the window does.
template <typename Dialog, typename Controller>
QWidget* make_analysis_view(const orc::view::ViewOpenRequest& request) {
  if (!request.session) {
    return nullptr;
  }
  auto* dialog = new Dialog(request.parent);
  dialog->setWindowTitle(QString::fromStdString(request.session->title()));
  new Controller(dialog, request.session);
  return dialog;
}

// A configuration dialog blocks the rest of the application while it is
// open, as it did before it moved here: what it shows are the node's stored
// values, which an edit elsewhere would change under it.
QWidget* make_mask_line_view(const orc::view::ViewOpenRequest& request) {
  if (!request.session) {
    return nullptr;
  }
  auto* dialog = new MaskLineConfigDialog(request.parent);
  dialog->setWindowTitle(QString::fromStdString(request.session->title()));
  dialog->setWindowModality(Qt::ApplicationModal);
  new MaskLineSessionController(dialog, request.session);
  return dialog;
}

QWidget* make_ffmpeg_preset_view(const orc::view::ViewOpenRequest& request) {
  if (!request.session) {
    return nullptr;
  }
  auto* dialog = new FFmpegPresetDialog(
      QString::fromStdString(request.session->project_directory()),
      request.parent);
  dialog->setWindowTitle(QString::fromStdString(request.session->title()));
  dialog->setWindowModality(Qt::ApplicationModal);
  new FfmpegPresetSessionController(dialog, request.session);
  return dialog;
}

// The Dropout Editor is a window of its own beside the main one, open while
// the user works elsewhere; it stays open across graph rebuilds.
QWidget* make_dropout_editor_view(const orc::view::ViewOpenRequest& request) {
  if (!request.session) {
    return nullptr;
  }
  auto* dialog = new DropoutEditorDialog(request.session, request.parent);
  dialog->setWindowTitle(QString::fromStdString(request.session->title()));
  return dialog;
}

// The Frame Map Range Finder is a StageToolKind::Analysis tool: the host's
// analysis window, with the start and end offered as time codes too.
QWidget* make_frame_map_range_view(const orc::view::ViewOpenRequest& request) {
  if (!request.session) {
    return nullptr;
  }
  return new FrameMapRangeWindow(request.session, request.parent);
}

constexpr orc::view::ViewPluginDescriptor kCoreViewDescriptor =
    ORC_VIEW_PLUGIN_DESCRIPTOR("decode-orc.views.core", "1.0.0");

}  // namespace

ORC_DEFINE_VIEW_PLUGIN(
    kCoreViewDescriptor,
    {orc::analysis_wire::kDropoutContractId,
     &make_analysis_view<DropoutAnalysisDialog, DropoutSessionController>},
    {orc::analysis_wire::kSnrContractId,
     &make_analysis_view<SNRAnalysisDialog, SnrSessionController>},
    {orc::analysis_wire::kBurstLevelContractId,
     &make_analysis_view<BurstLevelAnalysisDialog,
                         BurstLevelSessionController>},
    {orc::config_wire::kMaskLineContractId, &make_mask_line_view},
    {orc::config_wire::kFfmpegPresetContractId, &make_ffmpeg_preset_view},
    {orc::dropout_map_text::kDropoutEditorContractId,
     &make_dropout_editor_view},
    {orc::frame_map_range_contract::kContractId, &make_frame_map_range_view})
