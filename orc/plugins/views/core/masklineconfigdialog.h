/*
 * File:        masklineconfigdialog.h
 * Module:      orc-view_core
 * Purpose:     Configuration dialog for mask line stage (broadcast line entry)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef MASKLINECONFIGDIALOG_H
#define MASKLINECONFIGDIALOG_H

#include <orc/view/signal_units.h>

#include <QComboBox>
#include <QGroupBox>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <cstdint>
#include <optional>
#include <string>

#include "configdialogbase.h"

/// The video system and levels (10-bit samples) of what a Mask Line stage
/// masks, as its stage reports them. A level is -1 when unknown.
struct MaskLineSignal {
  orc::view::VideoSystem system = orc::view::VideoSystem::Unknown;
  int32_t blanking_level = -1;
  int32_t white_level = -1;
};

/**
 * @brief Configuration dialog for the mask line stage
 *
 * Lets the user specify lines to mask using broadcast interlaced line numbering
 * (PAL: 1–625, NTSC/PAL-M: 1–525).  Field 1 occupies the lower half of that
 * range and field 2 the upper half; entering lines from either or both halves
 * masks the corresponding frame-flat lines in the CVBS frame buffer.
 *
 * Line numbers are converted from 1-based broadcast to 0-based frame-flat on
 * apply: broadcast line N → frame-flat line N-1.
 */
class MaskLineConfigDialog : public ConfigDialogBase {
  Q_OBJECT

 public:
  explicit MaskLineConfigDialog(QWidget* parent = nullptr);
  ~MaskLineConfigDialog() override = default;

  // Sets the valid line-number range and mask-level presets for the input's
  // video system. Call before set_parameters(): the stored lines are read in
  // the system's numbering.
  void setVideoSignal(const std::optional<MaskLineSignal>& signal);

 protected:
  void apply_configuration() override;
  void load_from_parameters(const orc::view::ToolParameterMap& params) override;

 private slots:
  void on_add_range();
  void on_remove_selected();
  void on_clear_all();
  void on_mask_level_preset_changed(int index);

 private:
  int field1_line_count() const;
  int max_frame_lines() const;
  void update_spinbox_limits();
  void update_mask_level_labels();
  void resolve_video_levels(int32_t& blanking, int32_t& white) const;
  std::string build_line_spec_from_ui() const;
  void parse_line_spec_to_ui(const std::string& line_spec);

  // Line entry
  QSpinBox* start_line_spinbox_;
  QSpinBox* end_line_spinbox_;
  QPushButton* add_button_;
  QListWidget* ranges_list_;

  // Mask level
  QComboBox* mask_level_preset_combo_;
  QSpinBox* mask_level_spinbox_;

  std::optional<MaskLineSignal> cached_video_params_;
};

#endif  // MASKLINECONFIGDIALOG_H
