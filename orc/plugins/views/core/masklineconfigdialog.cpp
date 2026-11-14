/*
 * File:        masklineconfigdialog.cpp
 * Module:      orc-view_core
 * Purpose:     Configuration dialog for mask line stage (broadcast line entry)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "masklineconfigdialog.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <algorithm>
#include <sstream>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

MaskLineConfigDialog::MaskLineConfigDialog(QWidget* parent)
    : ConfigDialogBase("Mask Line Config", parent) {
  // ---- Lines to Mask group -----------------------------------------------
  auto* lines_group = create_group("Lines to Mask");
  auto* lines_layout = qobject_cast<QFormLayout*>(lines_group->layout());

  add_info_label(
      lines_layout,
      "Enter broadcast line numbers (PAL: 1–625, NTSC/PAL-M: 1–525). "
      "Field 1 = lines 1–313 (PAL) / 1–263 (NTSC/PAL-M). "
      "Each range is automatically applied to the equivalent lines in "
      "<b>both</b> fields.");

  start_line_spinbox_ =
      add_spinbox(lines_layout, "Start Line:", 1, 625, 1,
                  "First broadcast line to mask (1-based, full-frame)");
  end_line_spinbox_ = add_spinbox(
      lines_layout, "End Line:", 1, 625, 1,
      "Last broadcast line to mask (1-based, full-frame, must be ≥ start)");

  // Add / Remove / Clear buttons in a single row
  auto* buttons_container = new QWidget(this);
  auto* buttons_hbox = new QHBoxLayout(buttons_container);
  buttons_hbox->setContentsMargins(0, 0, 0, 0);
  add_button_ = new QPushButton("Add Range", this);
  auto* remove_button = new QPushButton("Remove Selected", this);
  auto* clear_button = new QPushButton("Clear All", this);
  buttons_hbox->addWidget(add_button_);
  buttons_hbox->addWidget(remove_button);
  buttons_hbox->addWidget(clear_button);
  buttons_hbox->addStretch();
  lines_layout->addRow(buttons_container);

  // Ranges list
  ranges_list_ = new QListWidget(this);
  ranges_list_->setMaximumHeight(120);
  ranges_list_->setToolTip(
      "Broadcast line ranges to mask. Lines are converted to frame-flat "
      "0-based indices on apply.");
  lines_layout->addRow("Ranges:", ranges_list_);

  // ---- Mask Level group --------------------------------------------------
  auto* level_group = create_group("Mask Level");
  auto* level_layout = qobject_cast<QFormLayout*>(level_group->layout());

  QStringList level_presets;
  level_presets << "Blanking (256)" << "White (844)" << "Custom";
  mask_level_preset_combo_ =
      add_combobox(level_layout, "Level Preset:", level_presets,
                   "10-bit sample level written to all masked lines");

  mask_level_spinbox_ =
      add_spinbox(level_layout, "Custom (0–1023):", 0, 1023, 256,
                  "Custom raw 10-bit sample level for masked pixels");
  mask_level_spinbox_->setEnabled(false);

  // ---- Signal connections ------------------------------------------------
  connect(add_button_, &QPushButton::clicked, this,
          &MaskLineConfigDialog::on_add_range);
  connect(remove_button, &QPushButton::clicked, this,
          &MaskLineConfigDialog::on_remove_selected);
  connect(clear_button, &QPushButton::clicked, this,
          &MaskLineConfigDialog::on_clear_all);
  connect(mask_level_preset_combo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &MaskLineConfigDialog::on_mask_level_preset_changed);
}

// ---------------------------------------------------------------------------
// Slot implementations
// ---------------------------------------------------------------------------

void MaskLineConfigDialog::on_add_range() {
  int start = start_line_spinbox_->value();
  int end = end_line_spinbox_->value();
  if (end < start) std::swap(start, end);

  const QString label = (start == end) ? QString::number(start)
                                       : QString("%1–%2").arg(start).arg(end);

  auto* item = new QListWidgetItem(label, ranges_list_);
  item->setData(Qt::UserRole, QVariantList{start, end});
}

void MaskLineConfigDialog::on_remove_selected() {
  delete ranges_list_->currentItem();
}

void MaskLineConfigDialog::on_clear_all() { ranges_list_->clear(); }

void MaskLineConfigDialog::on_mask_level_preset_changed(int index) {
  const bool is_custom = (index == 2);
  mask_level_spinbox_->setEnabled(is_custom);

  if (!is_custom) {
    int32_t blanking = 256, white = 844;
    resolve_video_levels(blanking, white);
    mask_level_spinbox_->setValue(index == 1 ? white : blanking);
  }
}

// ---------------------------------------------------------------------------
// apply_configuration / load_from_parameters
// ---------------------------------------------------------------------------

void MaskLineConfigDialog::apply_configuration() {
  set_parameter("lineSpec", build_line_spec_from_ui());

  int32_t blanking = 256, white = 844;
  resolve_video_levels(blanking, white);

  int32_t mask_level;
  const int preset_idx = mask_level_preset_combo_->currentIndex();
  if (preset_idx == 1) {
    mask_level = white;
  } else if (preset_idx == 2) {
    mask_level = mask_level_spinbox_->value();
  } else {
    mask_level = blanking;
  }
  set_parameter("maskSampleLevel", mask_level);
}

void MaskLineConfigDialog::load_from_parameters(
    const orc::view::ToolParameterMap& params) {
  ranges_list_->clear();

  if (const auto spec = text_value(params, "lineSpec")) {
    parse_line_spec_to_ui(*spec);
  }

  if (const auto level = int_value(params, "maskSampleLevel")) {
    int32_t blanking = 256, white = 844;
    resolve_video_levels(blanking, white);

    if (*level == blanking) {
      mask_level_preset_combo_->setCurrentIndex(0);
    } else if (*level == white) {
      mask_level_preset_combo_->setCurrentIndex(1);
    } else {
      mask_level_preset_combo_->setCurrentIndex(2);
      mask_level_spinbox_->setValue(*level);
    }
  } else {
    mask_level_preset_combo_->setCurrentIndex(0);
  }
}

// ---------------------------------------------------------------------------
// Public setters
// ---------------------------------------------------------------------------

void MaskLineConfigDialog::setVideoSignal(
    const std::optional<MaskLineSignal>& signal) {
  cached_video_params_ = signal;
  update_spinbox_limits();
  update_mask_level_labels();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

int MaskLineConfigDialog::field1_line_count() const {
  if (cached_video_params_.has_value()) {
    switch (cached_video_params_->system) {
      case orc::view::VideoSystem::PAL:
        return 313;
      case orc::view::VideoSystem::NTSC:
      case orc::view::VideoSystem::PAL_M:
        return 263;
      default:
        break;
    }
  }
  return 313;
}

// Returns the total number of lines in a full frame for the detected system.
// PAL: 625, NTSC/PAL-M: 525.
int MaskLineConfigDialog::max_frame_lines() const {
  if (cached_video_params_.has_value()) {
    switch (cached_video_params_->system) {
      case orc::view::VideoSystem::PAL:
        return 625;
      case orc::view::VideoSystem::NTSC:
      case orc::view::VideoSystem::PAL_M:
        return 525;
      default:
        break;
    }
  }
  return 625;
}

void MaskLineConfigDialog::update_spinbox_limits() {
  const int max_lines = max_frame_lines();
  start_line_spinbox_->setMaximum(max_lines);
  end_line_spinbox_->setMaximum(max_lines);
}

void MaskLineConfigDialog::update_mask_level_labels() {
  int32_t blanking = 256, white = 844;
  resolve_video_levels(blanking, white);
  mask_level_preset_combo_->setItemText(0,
                                        QString("Blanking (%1)").arg(blanking));
  mask_level_preset_combo_->setItemText(1, QString("White (%1)").arg(white));
}

void MaskLineConfigDialog::resolve_video_levels(int32_t& blanking,
                                                int32_t& white) const {
  blanking = 256;
  white = 844;
  if (cached_video_params_.has_value()) {
    const auto& vp = *cached_video_params_;
    if (vp.blanking_level >= 0 && vp.white_level > vp.blanking_level) {
      blanking = vp.blanking_level;
      white = vp.white_level;
    }
  }
}

// Converts the list-widget entries (full-frame broadcast 1-based) to a
// frame-flat 0-based lineSpec string covering both field 1 and field 2.
//
// PAL field layout in the CVBS flat buffer (0-based):
//   Field 1: lines 0..312  (broadcast 1..313)
//   Field 2: lines 313..624 (broadcast 314..625)
//
// For a broadcast range entirely in field 1 [S,E ≤ f1]:
//   Field 1 flat: S-1 .. E-1
//   Field 2 flat: f1+(S-1) .. f1+(E-1)
//
// For a broadcast range entirely in field 2 [S,E > f1]:
//   Field 2 flat: S-1 .. E-1
//   Field 1 flat (mirror): S-f1-1 .. E-f1-1
//
// Ranges spanning the boundary are split and each portion handled separately.
std::string MaskLineConfigDialog::build_line_spec_from_ui() const {
  const int f1 = field1_line_count();
  const int max_flat = max_frame_lines() - 1;
  std::string result;

  auto append_range = [&](int flat_start, int flat_end) {
    flat_end = std::min(flat_end, max_flat);
    if (flat_start < 0 || flat_start > flat_end) return;
    if (!result.empty()) result += ",";
    if (flat_start == flat_end) {
      result += std::to_string(flat_start);
    } else {
      result += std::to_string(flat_start) + "-" + std::to_string(flat_end);
    }
  };

  // Emit both fields for a sub-range in field 1 (1-based, 1..f1).
  auto emit_f1_range = [&](int s, int e) {
    append_range(s - 1, e - 1);
    const int f2_start = f1 + s - 1;
    if (f2_start <= max_flat) append_range(f2_start, f1 + e - 1);
  };

  // Emit both fields for a sub-range in field 2 (1-based full-frame,
  // f1+1..max).
  auto emit_f2_range = [&](int s, int e) {
    append_range(s - f1 - 1, e - f1 - 1);  // field 1 mirror
    append_range(s - 1, e - 1);            // field 2 flat
  };

  for (int i = 0; i < ranges_list_->count(); ++i) {
    const QVariantList data =
        ranges_list_->item(i)->data(Qt::UserRole).toList();
    const int bc_start = data[0].toInt();
    const int bc_end = data[1].toInt();

    if (bc_end <= f1) {
      emit_f1_range(bc_start, bc_end);
    } else if (bc_start > f1) {
      emit_f2_range(bc_start, bc_end);
    } else {
      // Split at the field boundary
      emit_f1_range(bc_start, f1);
      emit_f2_range(f1 + 1, bc_end);
    }
  }
  return result;
}

// Parses a frame-flat 0-based lineSpec and populates the list widget with
// field-relative broadcast 1-based line numbers.
//
// Only field 1 ranges (flat index < f1_lines) are loaded; field 2 ranges are
// implicit mirrors and are skipped to avoid duplicates.
// Legacy F:/S:/A: prefixed tokens are ignored.
void MaskLineConfigDialog::parse_line_spec_to_ui(const std::string& line_spec) {
  if (line_spec.empty()) return;

  const int f1 = field1_line_count();

  std::istringstream ss(line_spec);
  std::string token;
  while (std::getline(ss, token, ',')) {
    const size_t first = token.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    const size_t last = token.find_last_not_of(" \t");
    token = token.substr(first, last - first + 1);

    if (token.size() >= 2 && token[1] == ':') continue;

    int flat_start = -1, flat_end = -1;
    const size_t dash = token.find('-');
    if (dash != std::string::npos) {
      try {
        flat_start = std::stoi(token.substr(0, dash));
        flat_end = std::stoi(token.substr(dash + 1));
      } catch (...) {
        continue;
      }
    } else {
      try {
        flat_start = flat_end = std::stoi(token);
      } catch (...) {
        continue;
      }
    }

    if (flat_start < 0 || flat_end < flat_start) continue;

    // Skip field 2 ranges — they are regenerated automatically on apply.
    if (flat_start >= f1) continue;

    // Convert field 1 frame-flat → broadcast (add 1).
    const int bc_start = flat_start + 1;
    const int bc_end = std::min(flat_end, f1 - 1) + 1;

    const QString label = (bc_start == bc_end)
                              ? QString::number(bc_start)
                              : QString("%1–%2").arg(bc_start).arg(bc_end);

    auto* item = new QListWidgetItem(label, ranges_list_);
    item->setData(Qt::UserRole, QVariantList{bc_start, bc_end});
  }
}
