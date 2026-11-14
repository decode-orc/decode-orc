/*
 * File:        config_session_controller.cpp
 * Module:      orc-view_core
 * Purpose:     Connect a stage's configuration dialog to its tool session
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "config_session_controller.h"

#include <QMessageBox>
#include <string>
#include <utility>

#include "../../stages/common/config-tools/stage_config_wire.h"
#include "configdialogbase.h"
#include "ffmpegpresetdialog.h"
#include "masklineconfigdialog.h"

namespace wire = orc::config_wire;

namespace {

orc::view::VideoSystem view_system(wire::System system) {
  switch (system) {
    case wire::System::PAL:
      return orc::view::VideoSystem::PAL;
    case wire::System::NTSC:
      return orc::view::VideoSystem::NTSC;
    case wire::System::PAL_M:
      return orc::view::VideoSystem::PAL_M;
    default:
      return orc::view::VideoSystem::Unknown;
  }
}

}  // namespace

ConfigSessionController::ConfigSessionController(
    ConfigDialogBase* dialog,
    std::shared_ptr<orc::view::IStageToolSession> session, QString apply_label)
    : QObject(dialog),
      dialog_(dialog),
      session_(std::move(session)),
      apply_label_(std::move(apply_label)) {
  dialog->set_submit_handler([this]() { submit(); });
}

void ConfigSessionController::loadStoredParameters() {
  if (dialog_) {
    dialog_->set_parameters(session_->parameters());
  }
}

void ConfigSessionController::submit() {
  if (!dialog_) return;
  if (!session_->is_open()) {
    dialog_->reject();
    return;
  }
  dialog_->set_ok_enabled(false);
  QPointer<ConfigSessionController> self(this);
  // On success the rebuild that follows closes the dialog, and this reply
  // is dropped with the session; it only arrives when nothing was written.
  session_->apply_parameters(
      dialog_->get_parameters(), apply_label_.toStdString(),
      [self](orc::view::ToolReply reply) {
        if (!self || !self->dialog_) return;
        if (reply.ok) {
          self->dialog_->accept();
          return;
        }
        self->dialog_->set_ok_enabled(true);
        QMessageBox::warning(self->dialog_, "Configuration Error",
                             QString("The configuration was not applied: %1")
                                 .arg(QString::fromStdString(reply.error)));
      });
}

// --- Mask Line --------------------------------------------------------------

MaskLineSessionController::MaskLineSessionController(
    MaskLineConfigDialog* dialog,
    std::shared_ptr<orc::view::IStageToolSession> session)
    : ConfigSessionController(dialog, std::move(session),
                              "Applied mask line configuration"),
      mask_dialog_(dialog) {
  dialog->set_ok_enabled(false);
  QPointer<MaskLineSessionController> self(this);
  job_ = this->session().run_job(
      wire::kSignalVerb, {}, {}, [self](orc::view::ToolReply reply) {
        if (!self || !self->mask_dialog_) return;
        wire::Signal signal;
        std::string why;
        if (reply.ok && wire::decode(reply.data, &signal, &why)) {
          self->mask_dialog_->setVideoSignal(
              MaskLineSignal{view_system(signal.system), signal.blanking_level,
                             signal.white_level});
        }
        // Without the signal (no input yet, say) the dialog numbers lines
        // as PAL, as it always has.
        self->showStored();
      });
  if (!job_) {
    showStored();
  }
}

void MaskLineSessionController::showStored() {
  loadStoredParameters();
  if (mask_dialog_) mask_dialog_->set_ok_enabled(true);
}

// --- FFmpeg preset ----------------------------------------------------------

FfmpegPresetSessionController::FfmpegPresetSessionController(
    FFmpegPresetDialog* dialog,
    std::shared_ptr<orc::view::IStageToolSession> session)
    : ConfigSessionController(dialog, std::move(session),
                              "Applied FFmpeg export preset"),
      ffmpeg_dialog_(dialog) {
  loadStoredParameters();
  QPointer<FfmpegPresetSessionController> self(this);
  this->session().request(
      wire::kHardwareEncodersVerb, {}, [self](orc::view::ToolReply reply) {
        if (!self || !self->ffmpeg_dialog_ || !reply.ok) return;
        wire::HardwareEncoders encoders;
        std::string why;
        if (wire::decode(reply.data, &encoders, &why)) {
          self->ffmpeg_dialog_->setHardwareEncoders(std::move(encoders.types));
        }
      });
}
