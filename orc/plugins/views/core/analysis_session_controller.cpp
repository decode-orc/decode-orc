/*
 * File:        analysis_session_controller.cpp
 * Module:      orc-view_core
 * Purpose:     Feed an analysis sink's graph window from its tool session
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "analysis_session_controller.h"

#include <limits>
#include <string>
#include <utility>

#include "../../stages/common/analysis-results/analysis_series_wire.h"
#include "analysis_display_series.h"
#include "analysisdialogbase.h"
#include "burstlevelanalysisdialog.h"
#include "dropoutanalysisdialog.h"
#include "snranalysisdialog.h"

namespace wire = orc::analysis_wire;
namespace series = orc::analysis_view;

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

AnalysisSessionController::AnalysisSessionController(
    AnalysisDialogBase* dialog,
    std::shared_ptr<orc::view::IStageToolSession> session)
    : QObject(dialog), session_(std::move(session)) {
  QPointer<AnalysisSessionController> self(this);

  // A click on the graph moves the main preview there.
  connect(dialog, &AnalysisDialogBase::frameActivated, this,
          [this](int32_t frame_number) {
            if (frame_number >= 1 && session_->is_open()) {
              session_->show_frame(static_cast<uint64_t>(frame_number - 1));
            }
          });

  session_->set_preview_frame_handler([self](uint64_t frame_index) {
    if (!self) return;
    self->preview_frame_ = frame_index;
    self->moveFrameMarker(self->currentFrame());
  });

  // Each trigger leaves new results behind.
  session_->set_results_changed_handler([self]() {
    if (self) self->requestResults();
  });
}

void AnalysisSessionController::requestResults() {
  QPointer<AnalysisSessionController> self(this);
  session_->request(wire::kResultsVerb, {}, [self](orc::view::ToolReply reply) {
    if (!self) return;
    if (!reply.ok) {
      self->showNoResults(QString::fromStdString(
          reply.error.empty() ? "The stage sent no results" : reply.error));
      return;
    }
    QString error;
    if (!self->showResults(reply.data, &error)) {
      self->showNoResults(error);
    }
  });
}

int32_t AnalysisSessionController::currentFrame() const {
  if (!preview_frame_) {
    return 1;
  }
  const uint64_t frame = *preview_frame_ + 1;
  return frame > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())
             ? std::numeric_limits<int32_t>::max()
             : static_cast<int32_t>(frame);
}

// --- Dropout ----------------------------------------------------------------

DropoutSessionController::DropoutSessionController(
    DropoutAnalysisDialog* dialog,
    std::shared_ptr<orc::view::IStageToolSession> session)
    : AnalysisSessionController(dialog, std::move(session)), dialog_(dialog) {
  requestResults();
}

bool DropoutSessionController::showResults(const std::vector<uint8_t>& data,
                                           QString* error) {
  wire::DropoutResults results;
  std::string why;
  if (!wire::decode(data, &results, &why)) {
    *error = QString::fromStdString(why);
    return false;
  }
  if (!dialog_) return true;
  const auto display =
      series::make_dropout_series(results.frames, results.total_frames);
  if (display.points.empty() || display.total_frames == 0) {
    dialog_->showNoDataMessage(
        "No dropout analysis data available.\n\n"
        "Make sure dropout detection is enabled in the pipeline.");
    return true;
  }
  dialog_->startUpdate(display.total_frames, display.decimated);
  for (const auto& point : display.points) {
    if (point.bucket.has_data) {
      dialog_->addDataPoint(point.bucket.frame_label,
                            static_cast<double>(point.dropout_length_samples),
                            point.bucket.frame_start, point.bucket.frame_end,
                            point.dropout_count);
    }
  }
  dialog_->finishUpdate(currentFrame());
  return true;
}

void DropoutSessionController::showNoResults(const QString& reason) {
  if (dialog_) dialog_->showNoDataMessage(reason);
}

void DropoutSessionController::moveFrameMarker(int32_t frame_number) {
  if (dialog_) dialog_->updateFrameMarker(frame_number);
}

// --- SNR --------------------------------------------------------------------

SnrSessionController::SnrSessionController(
    SNRAnalysisDialog* dialog,
    std::shared_ptr<orc::view::IStageToolSession> session)
    : AnalysisSessionController(dialog, std::move(session)), dialog_(dialog) {
  requestResults();
}

bool SnrSessionController::showResults(const std::vector<uint8_t>& data,
                                       QString* error) {
  wire::SnrResults results;
  std::string why;
  if (!wire::decode(data, &results, &why)) {
    *error = QString::fromStdString(why);
    return false;
  }
  if (!dialog_) return true;
  const auto display =
      series::make_snr_series(results.frames, results.total_frames);
  if (display.points.empty() || display.total_frames == 0) {
    dialog_->showNoDataMessage(
        "No SNR analysis data available.\n\n"
        "Make sure VITS (Vertical Interval Test Signal) is present in the "
        "source.");
    return true;
  }
  dialog_->startUpdate(display.total_frames, display.decimated);
  for (const auto& point : display.points) {
    if (point.bucket.has_data) {
      const double white = point.has_white_snr
                               ? point.white_snr
                               : std::numeric_limits<double>::quiet_NaN();
      const double black = point.has_black_psnr
                               ? point.black_psnr
                               : std::numeric_limits<double>::quiet_NaN();
      dialog_->addDataPoint(point.bucket.frame_label, white, black,
                            point.bucket.frame_start, point.bucket.frame_end);
    }
  }
  dialog_->finishUpdate(currentFrame());
  return true;
}

void SnrSessionController::showNoResults(const QString& reason) {
  if (dialog_) dialog_->showNoDataMessage(reason);
}

void SnrSessionController::moveFrameMarker(int32_t frame_number) {
  if (dialog_) dialog_->updateFrameMarker(frame_number);
}

// --- Burst level ------------------------------------------------------------

BurstLevelSessionController::BurstLevelSessionController(
    BurstLevelAnalysisDialog* dialog,
    std::shared_ptr<orc::view::IStageToolSession> session)
    : AnalysisSessionController(dialog, std::move(session)), dialog_(dialog) {
  dialog->setAmplitudeUnit(this->session().amplitude_unit());
  QPointer<BurstLevelSessionController> self(this);
  this->session().set_amplitude_unit_handler(
      [self](orc::view::AmplitudeUnit unit) {
        if (self && self->dialog_) self->dialog_->setAmplitudeUnit(unit);
      });
  requestResults();
}

bool BurstLevelSessionController::showResults(const std::vector<uint8_t>& data,
                                              QString* error) {
  wire::BurstLevelResults results;
  std::string why;
  if (!wire::decode(data, &results, &why)) {
    *error = QString::fromStdString(why);
    return false;
  }
  if (!dialog_) return true;
  const auto display =
      series::make_burst_level_series(results.frames, results.total_frames);
  if (display.points.empty() || display.total_frames == 0) {
    dialog_->showNoDataMessage(
        "No burst level data available.\n\n"
        "Color burst detection may have failed.");
    return true;
  }
  dialog_->setVideoLevels(view_system(results.levels.system),
                          results.levels.blanking_level,
                          results.levels.white_level);
  dialog_->startUpdate(display.total_frames, display.decimated);
  for (const auto& point : display.points) {
    if (point.bucket.has_data) {
      dialog_->addDataPoint(point.bucket.frame_label, point.median_burst_10bit,
                            point.bucket.frame_start, point.bucket.frame_end);
    }
  }
  dialog_->finishUpdate(currentFrame());
  return true;
}

void BurstLevelSessionController::showNoResults(const QString& reason) {
  if (dialog_) dialog_->showNoDataMessage(reason);
}

void BurstLevelSessionController::moveFrameMarker(int32_t frame_number) {
  if (dialog_) dialog_->updateFrameMarker(frame_number);
}
