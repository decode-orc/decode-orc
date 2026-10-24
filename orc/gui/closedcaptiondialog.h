/*
 * File:        closedcaptiondialog.h
 * Module:      orc-gui
 * Purpose:     Closed caption preview dialog (observer dialog)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef CLOSEDCAPTIONDIALOG_H
#define CLOSEDCAPTIONDIALOG_H

#include <orc/support/eia608_service_demux.h>
#include <orc_closed_caption.h>

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QStatusBar>
#include <QString>
#include <QTableWidget>
#include <cstdint>
#include <vector>

#include "closed_caption_assembler.h"

/**
 * @brief Modeless preview of the closed captions the previewer has passed
 *
 * An observer dialog (owned by MainWindow, raised from the preview window's
 * Observers menu) that follows the frame previewer. It is stateful, unlike the
 * other observer dialogs: EIA-608 carries two bytes per frame and builds a
 * caption out of them over a second or more, so what is on screen at any one
 * frame is the result of a run of frames before it, not of that frame's own
 * data (see ClosedCaptionAssembler).
 *
 * What it shows is a transcript of the captions decoded so far, each with the
 * frame it appeared at, so a caption can be found again in the recording. The
 * transcript follows the previewer: the caption that would be on screen at the
 * preview frame is marked in it and scrolled to as that frame moves, which is
 * what tells a reader the captions really are keeping step with the picture.
 *
 * Status text lives in a status bar along the bottom edge so transient messages
 * never reflow the transcript, and the recovered byte pair of the current frame
 * is shown there too — it is the observation itself, and the only way to tell a
 * frame carrying nothing from one whose bytes did not decode.
 *
 * MainWindow drives it: setCurrentFrame() on frame changes, then issues one
 * requestClosedCaptionData() per frame reported by framesNeedingData() and
 * feeds the responses back through deliverFrameData().
 */
class ClosedCaptionDialog : public QDialog {
  Q_OBJECT

 public:
  explicit ClosedCaptionDialog(QWidget* parent = nullptr);
  ~ClosedCaptionDialog();

  /**
   * @brief Show a "reading" pending state while observation requests are in
   *        flight, cleared once every window frame has been delivered.
   */
  void showPending();

  /// Clear the display, the byte cache and the transcript (project closed)
  void clearContent();

  /// Drop cached data and the transcript (view node or DAG changed)
  void clearCache();

  /// Advance the trailing window to end at @p frame_index and re-render
  void setCurrentFrame(uint64_t frame_index);

  /// Next batch of frames in the current window still lacking caption data.
  /// Deliberately capped: call again as deliveries land (see
  /// ClosedCaptionAssembler::kMaxFramesPerRequest).
  std::vector<uint64_t> framesNeedingData() const {
    return assembler_.framesNeedingData();
  }

  /// First frame of the current trailing window
  uint64_t windowStartFrame() const { return assembler_.windowStartFrame(); }

  /// Last frame of the current trailing window (the previewer's frame)
  uint64_t currentFrame() const { return assembler_.currentFrame(); }

  /// Highest frame a delivery is still worth making for
  /// (see ClosedCaptionAssembler::retainedFrameLimit())
  uint64_t retainedFrameLimit() const {
    return assembler_.retainedFrameLimit();
  }

  /**
   * @brief Deliver a closedCaptionDataReady response for one frame
   *
   * @param available       False when the frame could not be observed
   * @param field1_id_value First field of the frame (FieldID::value())
   * @param field1          Caption data for the first field
   * @param field2          Caption data for the second field
   */
  void deliverFrameData(
      bool available, uint64_t field1_id_value,
      const orc::presenters::ClosedCaptionFieldDataView& field1,
      const orc::presenters::ClosedCaptionFieldDataView& field2);

  /// The caption that would be on screen at the current frame (test seam;
  /// empty when the screen is blank there)
  QString currentCaptionText() const;

  /// Captions currently listed, in table order (test seam)
  std::vector<QString> listedCaptions() const;

  /// Row of the transcript marked as the caption on screen, or -1 (test seam)
  int currentCaptionRow() const { return current_row_; }

  /// Caption-mode readout (test seam; empty when hidden)
  QString modeText() const;

  /// Recovered-bytes readout for the current frame (test seam)
  QString dataText() const;

  /// Caption status text shown at the left of the status bar (test seam)
  QString statusText() const;

  /// Service currently being decoded
  orc::EIA608Service service() const { return assembler_.service(); }

  /// Choose the service to decode, as the selector does (test seam)
  void selectService(orc::EIA608Service service);

 signals:
  /**
   * @brief The dialog needs the window read again
   *
   * Emitted when the service selection changes: everything decoded under the
   * old selection is discarded, so the frames of the window have to be
   * re-requested even though the previewer has not moved.
   */
  void dataRequestNeeded();

 private:
  /// Transcript table columns
  enum CaptionColumn {
    kColumnFrame = 0,    ///< frame the caption appeared at (1-based)
    kColumnCaption = 1,  ///< the caption text
    kColumnCount = 2,
  };

  void setupUI();
  /// Apply the selector's current choice to the assembler and ask for data
  void onServiceChanged();
  /// Refresh the readouts and the transcript for the current frame
  void renderCurrentFrame();
  /// Merge the transcript into the table when it has changed
  void refreshCaptionList();
  /// Mark the row of the caption on screen and scroll it into view
  void markCurrentCaption(const ClosedCaptionAssembler::ScreenChange* current);
  /// Update the pending-observation notice from the outstanding frame count
  void updatePendingStatus();

  /// Human-readable caption mode ("Pop-on", "Roll-up 2 rows", "Paint-on")
  static QString formatMode(const ClosedCaptionAssembler::ScreenChange& change);

  /// The frame's recovered byte pairs, e.g. "F1 94 2C" — or a note that it
  /// carried none. A byte that failed its parity check is marked. The fields
  /// are numbered as television fields: when @p fields_reversed the first
  /// stored field is field 2.
  static QString formatFrameData(const ClosedCaptionAssembler::FrameData* data,
                                 bool fields_reversed);

  ClosedCaptionAssembler assembler_;

  // Transcript revision the table was last built from; guards needless merges.
  uint64_t listed_revision_ = 0;
  bool list_populated_ = false;
  // Frames listed, in table order, so a merge can find its common prefix
  // without re-parsing the displayed numbers.
  std::vector<uint64_t> listed_frames_;
  // Row currently marked as the caption on screen (-1 when none).
  int current_row_ = -1;

  QTableWidget* captions_table_ = nullptr;
  QStatusBar* status_bar_ = nullptr;
  // "Caption shown from frame N" / "No caption on screen".
  QComboBox* service_selector_ = nullptr;
  QLabel* status_summary_ = nullptr;
  // "Pop-on" — the caption mode the stream last selected.
  QLabel* mode_label_ = nullptr;
  // "Data: 94 2C" — the byte pair recovered for the current frame.
  QLabel* data_label_ = nullptr;
  // Pending-state notice shown while async observation requests are in flight.
  QLabel* status_label_ = nullptr;

  // The caption on screen at the current frame; the transcript marker and the
  // status summary are both about this, and it is the dialog's test seam.
  ClosedCaptionAssembler::CaptionScreen current_screen_;
  bool has_current_screen_ = false;
};

#endif  // CLOSEDCAPTIONDIALOG_H
