/*
 * File:        stageparameterdialog.cpp
 * Module:      orc-gui
 * Purpose:     Stage parameter dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "stageparameterdialog.h"

#include <frame_numbering.h>
#include <orc/support/pipe_io.h>

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QStringList>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <utility>

#include "logging.h"
#include "parameter_value_rules.h"

namespace {

// Screen size assumed when the dialog cannot work out which screen it will
// open on (never expected in practice; keeps the opening size sane if it
// happens).
constexpr int kFallbackScreenWidth = 1024;
constexpr int kFallbackScreenHeight = 768;

// How long the dialog waits for editing to settle before a live update is
// applied. Long enough that typing a three-digit value or holding a spin box
// arrow produces one preview render rather than one per step, short enough
// that a deliberate adjustment still feels immediate.
constexpr int kLiveUpdateSettleMs = 400;

// Number of positions a slider offers for a floating-point parameter. The
// editor beside it still holds the exact value, so this only sets how fine a
// sweep of the range is; a thousand steps is finer than the slider is wide on
// any screen.
constexpr int kDoubleSliderSteps = 1000;

// Widest integer range still worth sweeping. Past this a slider pixel covers
// so many values that it cannot be aimed, and the parameter is one to type
// rather than sweep.
constexpr int64_t kMaxSliderIntegerRange = 100000;

// How much of the range a page step (clicking the groove, Page Up/Down)
// covers: one twentieth, so the whole range is a score of clicks away.
constexpr int kSliderPageDivisions = 20;

// Narrowest a slider may be drawn. Below this there is not enough travel for
// the slider to be worth having.
constexpr int kSliderMinimumWidth = 140;

// Opening width, in average character widths, for a stage that has a file path
// or free-form string parameter. Wide enough for a working path to be readable
// without the user having to resize the dialog first.
constexpr int kTextEntryWidthChars = 88;

// Splits an allowed_strings entry "value\x1flabel" into (value, label). With no
// separator the whole entry serves as both the stored value and the label.
std::pair<QString, QString> split_combo_item(const std::string& entry) {
  const auto pos = entry.find(StageParameterDialog::kComboValueLabelSeparator);
  if (pos == std::string::npos) {
    const QString both = QString::fromStdString(entry);
    return {both, both};
  }
  return {QString::fromStdString(entry.substr(0, pos)),
          QString::fromStdString(entry.substr(pos + 1))};
}

// Selects the combo entry whose stored data equals |value|; falls back to
// matching the visible text (for combos built without value/label separation).
void select_combo_value(QComboBox* combo, const QString& value) {
  const int idx = combo->findData(value);
  if (idx >= 0) {
    combo->setCurrentIndex(idx);
  } else {
    combo->setCurrentText(value);
  }
}

}  // namespace

StageParameterDialog::StageParameterDialog(
    const std::string& stage_name, const std::string& display_name,
    const std::string& stage_description,
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::map<std::string, orc::ParameterValue>& current_values,
    const QString& project_path,
    const std::optional<std::map<std::string, orc::ParameterValue>>&
        reset_values,
    QWidget* parent)
    : QDialog(parent),
      stage_name_(stage_name),
      descriptors_(descriptors),
      project_path_(project_path),
      reset_values_(reset_values),
      display_name_(QString::fromStdString(display_name)) {
  update_window_title();
  setMinimumWidth(400);

  auto* main_layout = new QVBoxLayout(this);

  // The description and the parameter form live inside a scroll area: a stage
  // with a long parameter list asks for more height than Qt is willing to give
  // a dialog, and without somewhere for the overflow to go the layout takes
  // the shortfall out of every row at once (see the opening-size block below).
  auto* content = new QWidget();
  auto* content_layout = new QVBoxLayout(content);
  content_layout->setContentsMargins(0, 0, 0, 0);
  // The gap under the description comes from the label's own bottom margin
  // rather than from spacing here. A word-wrapped label reports its height
  // for the width it is given, and with spacing in this layout the box
  // layout's height-for-width pass ends up reserving that spacing twice —
  // leaving the form a few pixels short of what its rows asked for, which it
  // takes out of the rows. The form keeps the spacing the style asks for.
  const int style_layout_spacing = content_layout->spacing();
  content_layout->setSpacing(0);

  // Stage description label (shown at the top when non-empty). Built whether
  // or not there is anything to say yet: a refresh can give the stage a note
  // it did not open with, and an empty label takes no room.
  description_label_ = new QLabel();
  description_label_->setObjectName("stage_description_label");
  description_label_->setWordWrap(true);
  description_label_->setStyleSheet(
      "color: palette(window-text); font-style: italic;");
  // Carries the gap to the form as well as its own, since the layout it
  // sits in has no spacing of its own (see above).
  description_label_->setContentsMargins(0, 0, 0, 6 + style_layout_spacing);
  content_layout->addWidget(description_label_);
  set_stage_description(stage_description);

  // Form layout for parameters
  form_layout_ = new QFormLayout();
  form_layout_->setSpacing(style_layout_spacing);
  content_layout->addLayout(form_layout_);
  content_layout->addStretch();

  scroll_area_ = new QScrollArea();
  scroll_area_->setObjectName("parameter_scroll_area");
  scroll_area_->setWidgetResizable(true);
  scroll_area_->setFrameShape(QFrame::NoFrame);
  // Without AdjustToContents a scroll area reports a fixed placeholder hint,
  // which would open every dialog at the same arbitrary height.
  scroll_area_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
  scroll_area_->setWidget(content);
  main_layout->addWidget(scroll_area_, 1);

  // Reset button uses metadata values when provided, otherwise descriptor
  // defaults.
  const bool has_custom_reset_values =
      reset_values_.has_value() && !reset_values_->empty();
  reset_button_ =
      new QPushButton(has_custom_reset_values ? "Reset to Metadata Values"
                                              : "Reset to Defaults");
  connect(reset_button_, &QPushButton::clicked, this,
          &StageParameterDialog::on_reset_defaults);

  // Dialog buttons
  button_box_ =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  auto* update_button =
      button_box_->addButton("Update", QDialogButtonBox::ApplyRole);
  connect(button_box_, &QDialogButtonBox::accepted, this,
          &StageParameterDialog::on_validate_and_accept);
  connect(button_box_, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(update_button, &QPushButton::clicked, this,
          &StageParameterDialog::on_validate_and_update);

  // Live update: apply edits to the preview as they are made, so a value that
  // is judged by eye (black level, chroma gain) can be found by adjusting and
  // looking rather than by typing a number and pressing Update. Editing a
  // file path never starts one — a path is typed a character at a time and
  // none of the partial states is worth re-opening a source for — though a
  // path already edited is carried along by the next update, like every other
  // value in the form.
  live_update_check_ = new QCheckBox("Live update");
  live_update_check_->setObjectName("live_update_check");
  live_update_check_->setToolTip(
      "Apply parameter changes to the preview as they are made.\n"
      "Editing a file path does not start one: use Update or OK.");
  // Off on every open: whether a live apply is affordable depends on the
  // stage and the source, so the cheap state is the one to start in.
  connect(live_update_check_, &QCheckBox::toggled, this,
          &StageParameterDialog::on_live_update_toggled);

  live_update_timer_ = new QTimer(this);
  live_update_timer_->setSingleShot(true);
  live_update_timer_->setInterval(kLiveUpdateSettleMs);
  connect(live_update_timer_, &QTimer::timeout, this,
          &StageParameterDialog::on_live_update_timeout);

  // Built last: the form's widgets report their edits to this dialog, and
  // build_ui() disables the reset button for a stage with no parameters, so
  // the buttons and the live-update timer must already exist.
  build_ui(current_values);

  // The button row is a widget rather than a bare layout so that the opening
  // size can ask it how tall it really is (see apply_opening_size()).
  button_row_ = new QWidget();
  auto* button_layout = new QHBoxLayout(button_row_);
  button_layout->setContentsMargins(0, 0, 0, 0);
  button_layout->addWidget(reset_button_);
  button_layout->addWidget(live_update_check_);
  button_layout->addStretch();
  button_layout->addWidget(button_box_);

  main_layout->addWidget(button_row_);

  // The values the dialog opened with count as already applied: ticking live
  // update without having changed anything must not re-render the preview.
  last_applied_values_ = get_values();
}

void StageParameterDialog::set_stage_description(
    const std::string& description) {
  description_label_->setText(QString::fromStdString(description));
  description_label_->setVisible(!description.empty());
}

void StageParameterDialog::update_window_title() {
  QString title = QString("%1 Parameters").arg(display_name_);
  if (!node_label_.isEmpty()) {
    title += QString(" — %1").arg(node_label_);
  }
  if (!node_id_text_.isEmpty()) {
    title += QString(" (%1)").arg(node_id_text_);
  }
  if (refresh_declined_) {
    // The node has moved on from what is on screen; the user's own edits are
    // still here and still theirs to apply or abandon.
    title += " *";
  }
  setWindowTitle(title);
}

void StageParameterDialog::set_node_identity(const QString& node_label,
                                             const QString& node_id) {
  node_label_ = node_label;
  node_id_text_ = node_id;
  update_window_title();
}

bool StageParameterDialog::has_unapplied_edits() const {
  if (!last_applied_values_.has_value()) {
    return false;
  }
  return *last_applied_values_ != get_values();
}

bool StageParameterDialog::refresh_values(
    const std::map<std::string, orc::ParameterValue>& values) {
  if (has_unapplied_edits()) {
    refresh_declined_ = true;
    update_window_title();
    return false;
  }

  for (const auto& desc : descriptors_) {
    auto it = values.find(desc.name);
    if (it != values.end()) {
      set_widget_value(desc.name, it->second);
    }
  }

  update_dependencies();
  last_applied_values_ = get_values();
  refresh_declined_ = false;
  update_window_title();
  return true;
}

void StageParameterDialog::refresh_context(
    const orc::gui::StageParameterContext& context) {
  // Edits in progress survive a context refresh: the graph changing under the
  // window is no reason to throw away what the user has typed into it. The
  // node's own values are the starting point, with anything the user has
  // changed laid over the top.
  const bool had_unapplied_edits = has_unapplied_edits();
  auto values = context.current_values;
  if (had_unapplied_edits) {
    for (const auto& [name, value] : get_values()) {
      values[name] = value;
    }
  }

  const bool descriptors_changed =
      !orc::gui::descriptorsMatch(descriptors_, context.descriptors);

  set_stage_description(context.stage_description);
  reset_values_ = context.reset_values;
  const bool has_custom_reset_values =
      reset_values_.has_value() && !reset_values_->empty();
  reset_button_->setText(has_custom_reset_values ? "Reset to Metadata Values"
                                                 : "Reset to Defaults");

  if (descriptors_changed) {
    descriptors_ = context.descriptors;
    rebuild_form(values);
  } else {
    for (const auto& desc : descriptors_) {
      auto it = values.find(desc.name);
      if (it != values.end()) {
        set_widget_value(desc.name, it->second);
      }
    }
    update_dependencies();
  }

  // An editor that was showing the node's values still is, whatever the new
  // context made of them — a narrowed dropdown can move a value the user did
  // not choose, and that is not an edit of theirs to hold against them. One
  // that was holding edits keeps holding them, and they stay unapplied.
  if (!had_unapplied_edits) {
    last_applied_values_ = get_values();
  }
  refresh_declined_ = false;
  update_window_title();
}

void StageParameterDialog::rebuild_form(
    const std::map<std::string, orc::ParameterValue>& current_values) {
  // removeRow() deletes the widgets it takes out, which is what is wanted
  // here: every row is about to be built again from the new descriptors.
  while (form_layout_->rowCount() > 0) {
    form_layout_->removeRow(0);
  }
  parameter_widgets_.clear();
  spec_display_baseline_.clear();
  reset_button_->setEnabled(true);

  build_ui(current_values);
}

void StageParameterDialog::showEvent(QShowEvent* event) {
  // Sized on first show rather than in the constructor: the widgets are
  // polished by then, so the form reports the height it will really occupy.
  // This runs before the window is mapped, so nothing is seen to resize.
  if (!opening_size_applied_) {
    opening_size_applied_ = true;
    apply_opening_size();
    apply_opening_position();
  }
  QDialog::showEvent(event);
}

void StageParameterDialog::set_opening_position(const QPoint& top_left) {
  opening_position_ = top_left;
}

void StageParameterDialog::apply_opening_position() {
  if (!opening_position_.has_value()) {
    return;
  }

  const QScreen* dialog_screen =
      parentWidget() != nullptr ? parentWidget()->screen() : screen();
  const QRect available =
      dialog_screen != nullptr
          ? dialog_screen->availableGeometry()
          : QRect(0, 0, kFallbackScreenWidth, kFallbackScreenHeight);

  // The size is settled by now, so the clamp can keep the whole window on
  // screen rather than only its corner.
  const QSize window_size = size();
  const int x = std::clamp(
      opening_position_->x(), available.left(),
      std::max(available.left(), available.right() - window_size.width()));
  const int y = std::clamp(
      opening_position_->y(), available.top(),
      std::max(available.top(), available.bottom() - window_size.height()));
  move(x, y);
}

void StageParameterDialog::apply_opening_size() {
  // Left to itself, Qt opens a dialog at its layout hint capped at two thirds
  // of the screen, and that hint is only as wide as the widest widget asks
  // for. Both defaults hurt here: the cap squashes long parameter lists (a
  // sink with thirty parameters loses a pixel or two from every row), and the
  // hint leaves file-path fields far too narrow to read a path in. Open at the
  // size the form actually wants instead, widened for text entry and bounded
  // by the screen rather than by two thirds of it.
  const QScreen* dialog_screen =
      parentWidget() != nullptr ? parentWidget()->screen() : screen();
  const QRect available =
      dialog_screen != nullptr
          ? dialog_screen->availableGeometry()
          : QRect(0, 0, kFallbackScreenWidth, kFallbackScreenHeight);

  int width = sizeHint().width();
  const bool has_text_entry =
      std::any_of(descriptors_.begin(), descriptors_.end(),
                  [](const orc::ParameterDescriptor& desc) {
                    return desc.type == orc::ParameterType::FILE_PATH ||
                           (desc.type == orc::ParameterType::STRING &&
                            desc.constraints.allowed_strings.empty());
                  });
  if (has_text_entry) {
    width = std::max(width,
                     fontMetrics().averageCharWidth() * kTextEntryWidthChars);
  }
  width = std::min(width, available.width() * 9 / 10);

  // Height comes from the scrolled content rather than from the dialog's own
  // hint: a scroll area caps the height it asks for at 24 text lines, which
  // would open every long parameter list part-scrolled for no reason. Ask the
  // content how tall it is at the width the dialog will actually open at, so
  // the word-wrapped description contributes the lines it really needs.
  const QMargins margins = layout()->contentsMargins();
  const int content_width = width - margins.left() - margins.right();
  const int max_height = available.height() * 9 / 10;

  auto* content_layout = scroll_area_->widget()->layout();
  const int content_height =
      content_layout->hasHeightForWidth()
          ? content_layout->totalHeightForWidth(content_width)
          : content_layout->sizeHint().height();

  // The button row sits outside the scroll area and is always fully shown, so
  // its full height comes off the content's. Asking the row itself keeps this
  // right whatever it holds.
  const int chrome = margins.top() + margins.bottom() + layout()->spacing() +
                     button_row_->sizeHint().height();

  int height = content_height + chrome;
  if (height > max_height) {
    // The content will scroll, so the vertical scroll bar takes width from it.
    height = max_height;
    const int bar = scroll_area_->verticalScrollBar()->sizeHint().width();
    width = std::min(width + bar, available.width() * 9 / 10);
  }

  resize(width, height);
}

void StageParameterDialog::build_ui(
    const std::map<std::string, orc::ParameterValue>& current_values) {
  for (const auto& desc : descriptors_) {
    // |widget| is the field the form row holds, |editor| the control holding
    // the value; they differ where the row also carries a slider or a Browse
    // button.
    QWidget* widget = nullptr;
    QWidget* editor = nullptr;

    // A numeric parameter the stage has bounded on both sides can be swept
    // with a slider. One left open has no travel to offer.
    const bool bounded = desc.constraints.min_value.has_value() &&
                         desc.constraints.max_value.has_value();

    // Get current value or default
    orc::ParameterValue value;
    auto it = current_values.find(desc.name);
    if (it != current_values.end()) {
      value = it->second;
    } else if (desc.constraints.default_value.has_value()) {
      value = *desc.constraints.default_value;
    } else {
      // No default - use type-specific default
      switch (desc.type) {
        case orc::ParameterType::INT32:
          value = static_cast<int32_t>(0);
          break;
        case orc::ParameterType::UINT32:
          value = static_cast<uint32_t>(0);
          break;
        case orc::ParameterType::DOUBLE:
          value = 0.0;
          break;
        case orc::ParameterType::BOOL:
          value = false;
          break;
        case orc::ParameterType::STRING:
        case orc::ParameterType::FILE_PATH:
          value = std::string("");
          break;
      }
    }

    // Create appropriate widget based on type
    switch (desc.type) {
      case orc::ParameterType::INT32: {
        auto* spin = new QSpinBox();
        if (desc.constraints.min_value.has_value()) {
          spin->setMinimum(std::get<int32_t>(*desc.constraints.min_value));
        } else {
          spin->setMinimum(std::numeric_limits<int32_t>::min());
        }
        if (desc.constraints.max_value.has_value()) {
          spin->setMaximum(std::get<int32_t>(*desc.constraints.max_value));
        } else {
          spin->setMaximum(std::numeric_limits<int32_t>::max());
        }
        spin->setValue(std::get<int32_t>(value));

        editor = spin;
        widget = bounded ? with_slider(spin) : spin;
        break;
      }

      case orc::ParameterType::UINT32: {
        auto* spin = new QSpinBox();
        if (desc.constraints.min_value.has_value()) {
          spin->setMinimum(static_cast<int>(
              std::get<uint32_t>(*desc.constraints.min_value)));
        } else {
          spin->setMinimum(0);
        }
        if (desc.constraints.max_value.has_value()) {
          spin->setMaximum(static_cast<int>(
              std::get<uint32_t>(*desc.constraints.max_value)));
        } else {
          spin->setMaximum(std::numeric_limits<int>::max());
        }
        spin->setValue(static_cast<int>(std::get<uint32_t>(value)));
        editor = spin;
        widget = bounded ? with_slider(spin) : spin;
        break;
      }

      case orc::ParameterType::DOUBLE: {
        auto* spin = new QDoubleSpinBox();
        spin->setDecimals(4);
        // An unbounded default range makes QDoubleSpinBox size itself to the
        // width of numeric_limits::max() rendered in full (~300 digits), which
        // blows the dialog past the screen. Fall back to a large but finite
        // range when a descriptor leaves the bounds unset.
        constexpr double kDefaultDoubleBound = 1e12;
        if (desc.constraints.min_value.has_value()) {
          spin->setMinimum(std::get<double>(*desc.constraints.min_value));
        } else {
          spin->setMinimum(-kDefaultDoubleBound);
        }
        if (desc.constraints.max_value.has_value()) {
          spin->setMaximum(std::get<double>(*desc.constraints.max_value));
        } else {
          spin->setMaximum(kDefaultDoubleBound);
        }
        spin->setValue(std::get<double>(value));
        editor = spin;
        widget = bounded ? with_slider(spin) : spin;
        break;
      }

      case orc::ParameterType::BOOL: {
        auto* check = new QCheckBox();
        check->setChecked(std::get<bool>(value));
        editor = check;
        widget = check;
        break;
      }

      case orc::ParameterType::STRING: {
        if (!desc.constraints.allowed_strings.empty()) {
          // Use combo box for constrained strings. Each entry may carry a
          // display label distinct from its stored value (see
          // kComboValueLabelSeparator).
          auto* combo = new QComboBox();
          for (const auto& allowed : desc.constraints.allowed_strings) {
            const auto [item_value, item_label] = split_combo_item(allowed);
            combo->addItem(item_label, item_value);
          }
          select_combo_value(
              combo, QString::fromStdString(std::get<std::string>(value)));
          editor = combo;
          widget = combo;
        } else {
          // Use line edit for free-form strings. Indexed spec parameters are
          // stored 0-based but presented 1-based.
          auto* edit = new QLineEdit();
          const std::string display_text =
              to_display_spec(desc.name, std::get<std::string>(value));
          edit->setText(QString::fromStdString(display_text));
          if (orc::indexed_spec_kind(stage_name_, desc.name) !=
              orc::IndexedSpecKind::kNone) {
            spec_display_baseline_[desc.name] = display_text;
          }
          editor = edit;
          widget = edit;
        }
        break;
      }

      case orc::ParameterType::FILE_PATH: {
        // File path with browse button
        auto* container = new QWidget();
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);

        auto* edit = new QLineEdit();
        edit->setText(QString::fromStdString(std::get<std::string>(value)));
        edit->setObjectName("file_path_edit");

        auto* browse_btn = new QPushButton("Browse...");
        browse_btn->setObjectName("browse_button");

        // Capture stage_name and param name for determining dialog type
        std::string stage_name_copy = stage_name_;
        std::string param_name = desc.name;
        std::string display_name = desc.display_name;
        std::string file_ext_hint = desc.file_extension_hint;

        // Determine if this is an output path (save dialog) or input path (open
        // dialog). An explicit descriptor flag takes precedence; otherwise fall
        // back to a name heuristic (sink stages, "output" in the name).
        bool is_output = desc.output_path ||
                         (stage_name_copy.find("sink") != std::string::npos) ||
                         (param_name.find("output") != std::string::npos) ||
                         (display_name.find("Output") != std::string::npos);

        // Connect browse button to file dialog
        connect(
            browse_btn, &QPushButton::clicked,
            [this, edit, stage_name_copy, display_name, file_ext_hint,
             is_output]() {
              QSettings settings("orc-project", "orc-gui");
              QString settings_key =
                  QString("lastSourceDirectory/%1")
                      .arg(QString::fromStdString(stage_name_copy));

              // Get last directory for this source type
              QString last_dir =
                  settings.value(settings_key, QDir::homePath()).toString();

              // Use current path's directory if it exists, otherwise use
              // last_dir
              QString start_dir = last_dir;
              if (!edit->text().isEmpty()) {
                QFileInfo info(edit->text());
                if (info.exists() && info.dir().exists()) {
                  start_dir = info.dir().absolutePath();
                } else if (!edit->text().isEmpty()) {
                  // Path doesn't exist yet (output file) - use its directory if
                  // valid
                  QFileInfo parent_info(info.absolutePath());
                  if (parent_info.exists() && parent_info.isDir()) {
                    start_dir = parent_info.absolutePath();
                  }
                }
              }

              // Build file filter based on extension hint
              QString filter = "All Files (*)";
              QString dialog_title =
                  is_output ? "Select Output File" : "Select Input File";

              if (!file_ext_hint.empty()) {
                QString ext = QString::fromStdString(file_ext_hint);
                // Handle multiple extensions separated by | (e.g., ".rgb|.mp4")
                QStringList extensions = ext.split('|');
                QString ext_patterns;
                QString ext_names;

                for (const QString& e : extensions) {
                  QString trimmed = e.trimmed();
                  if (trimmed.isEmpty()) continue;
                  if (!ext_patterns.isEmpty()) {
                    ext_patterns += " ";
                    ext_names += "/";
                  }
                  ext_patterns += "*" + trimmed;
                  // The name drops the leading dot of every extension, not
                  // just the first: a hint listing several would otherwise
                  // read "VBI/.FLAC/.U16".
                  QString name = trimmed;
                  if (name.startsWith('.')) name.remove(0, 1);
                  ext_names += name.toUpper();
                }

                filter =
                    ext_names + " Files (" + ext_patterns + ");;All Files (*)";
                dialog_title = is_output
                                   ? "Select Output " + ext_names + " File"
                                   : "Select " + ext_names + " File";
              }

              QString file;
              if (is_output) {
                file = QFileDialog::getSaveFileName(this, dialog_title,
                                                    start_dir, filter);
              } else {
                file = QFileDialog::getOpenFileName(this, dialog_title,
                                                    start_dir, filter);
              }

              if (!file.isEmpty()) {
                // Convert to relative path if we have a project path
                QString path_to_store = file;
                if (!project_path_.isEmpty()) {
                  QDir project_dir(QFileInfo(project_path_).absolutePath());
                  path_to_store = project_dir.relativeFilePath(file);
                }
                edit->setText(path_to_store);
                // Save directory for this source type
                settings.setValue(settings_key, QFileInfo(file).absolutePath());
              }
            });

        // Special handling for input_path: auto-populate pcm_path and efm_path
        if (param_name == "input_path") {
          connect(edit, &QLineEdit::textChanged, [this, edit]() {
            QString tbc_path = edit->text();
            if (tbc_path.isEmpty()) return;

            // Get base path (remove .tbc extension if present)
            QString base_path = tbc_path;
            if (base_path.endsWith(".tbc", Qt::CaseInsensitive)) {
              base_path = base_path.left(base_path.length() - 4);
            }

            // Check for .pcm file
            auto pcm_it = parameter_widgets_.find("pcm_path");
            if (pcm_it != parameter_widgets_.end()) {
              QWidget* pcm_container = pcm_it->second.widget;
              QLineEdit* pcm_edit =
                  pcm_container->findChild<QLineEdit*>("file_path_edit");
              if (pcm_edit && pcm_edit->text().isEmpty()) {
                QString pcm_path = base_path + ".pcm";
                if (QFileInfo::exists(pcm_path)) {
                  pcm_edit->setText(pcm_path);
                }
              }
            }

            // Check for .efm file
            auto efm_it = parameter_widgets_.find("efm_path");
            if (efm_it != parameter_widgets_.end()) {
              QWidget* efm_container = efm_it->second.widget;
              QLineEdit* efm_edit =
                  efm_container->findChild<QLineEdit*>("file_path_edit");
              if (efm_edit && efm_edit->text().isEmpty()) {
                QString efm_path = base_path + ".efm";
                if (QFileInfo::exists(efm_path)) {
                  efm_edit->setText(efm_path);
                }
              }
            }

            // Check for .ac3sym file
            auto ac3rf_it = parameter_widgets_.find("ac3rf_path");
            if (ac3rf_it != parameter_widgets_.end()) {
              QWidget* ac3rf_container = ac3rf_it->second.widget;
              QLineEdit* ac3rf_edit =
                  ac3rf_container->findChild<QLineEdit*>("file_path_edit");
              if (ac3rf_edit && ac3rf_edit->text().isEmpty()) {
                QString ac3rf_path = base_path + ".ac3sym";
                if (QFileInfo::exists(ac3rf_path)) {
                  ac3rf_edit->setText(ac3rf_path);
                }
              }
            }
          });
        }

        // Special handling for YC source stages: auto-populate y_path/c_path
        // and pcm_path/efm_path
        if (param_name == "y_path" || param_name == "c_path") {
          connect(
              edit, &QLineEdit::textChanged,
              [this, edit, param_name, stage_name_copy]() {
                QString current_path = edit->text();
                if (current_path.isEmpty()) return;

                // Determine the extension pair and strip it to get the base
                // path. tbc_source uses .tbcy/.tbcc; CVBS source uses
                // .cvbsy/.cvbsc.
                QString base_path = current_path;
                QString y_ext, c_ext;
                if (base_path.endsWith(".tbcy", Qt::CaseInsensitive) ||
                    base_path.endsWith(".tbcc", Qt::CaseInsensitive)) {
                  base_path = base_path.left(base_path.length() - 5);
                  y_ext = ".tbcy";
                  c_ext = ".tbcc";
                } else if (base_path.endsWith(".cvbsy", Qt::CaseInsensitive) ||
                           base_path.endsWith(".cvbsc", Qt::CaseInsensitive)) {
                  base_path = base_path.left(base_path.length() - 6);
                  y_ext = ".cvbsy";
                  c_ext = ".cvbsc";
                } else {
                  y_ext = ".tbcy";
                  c_ext = ".tbcc";
                }

                // Always sync the complementary YC file to match the new base
                // name
                if (param_name == "y_path") {
                  // Changing y_path: keep c_path in sync with the same base
                  // name
                  auto c_it = parameter_widgets_.find("c_path");
                  if (c_it != parameter_widgets_.end()) {
                    QWidget* c_container = c_it->second.widget;
                    QLineEdit* c_edit =
                        c_container->findChild<QLineEdit*>("file_path_edit");
                    if (c_edit) {
                      QSignalBlocker blocker(c_edit);
                      c_edit->setText(base_path + c_ext);
                    }
                  }
                } else if (param_name == "c_path") {
                  // Changing c_path: keep y_path in sync with the same base
                  // name
                  auto y_it = parameter_widgets_.find("y_path");
                  if (y_it != parameter_widgets_.end()) {
                    QWidget* y_container = y_it->second.widget;
                    QLineEdit* y_edit =
                        y_container->findChild<QLineEdit*>("file_path_edit");
                    if (y_edit) {
                      QSignalBlocker blocker(y_edit);
                      y_edit->setText(base_path + y_ext);
                    }
                  }
                }

                // Auto-populate pcm_path if not already set
                auto pcm_it = parameter_widgets_.find("pcm_path");
                if (pcm_it != parameter_widgets_.end()) {
                  QWidget* pcm_container = pcm_it->second.widget;
                  QLineEdit* pcm_edit =
                      pcm_container->findChild<QLineEdit*>("file_path_edit");
                  if (pcm_edit && pcm_edit->text().isEmpty()) {
                    QString pcm_path = base_path + ".pcm";
                    if (QFileInfo::exists(pcm_path)) {
                      pcm_edit->setText(pcm_path);
                    }
                  }
                }

                // Auto-populate efm_path if not already set
                auto efm_it = parameter_widgets_.find("efm_path");
                if (efm_it != parameter_widgets_.end()) {
                  QWidget* efm_container = efm_it->second.widget;
                  QLineEdit* efm_edit =
                      efm_container->findChild<QLineEdit*>("file_path_edit");
                  if (efm_edit && efm_edit->text().isEmpty()) {
                    QString efm_path = base_path + ".efm";
                    if (QFileInfo::exists(efm_path)) {
                      efm_edit->setText(efm_path);
                    }
                  }
                }

                // Auto-populate ac3rf_path if not already set
                auto ac3rf_it = parameter_widgets_.find("ac3rf_path");
                if (ac3rf_it != parameter_widgets_.end()) {
                  QWidget* ac3rf_container = ac3rf_it->second.widget;
                  QLineEdit* ac3rf_edit =
                      ac3rf_container->findChild<QLineEdit*>("file_path_edit");
                  if (ac3rf_edit && ac3rf_edit->text().isEmpty()) {
                    QString ac3rf_path = base_path + ".ac3sym";
                    if (QFileInfo::exists(ac3rf_path)) {
                      ac3rf_edit->setText(ac3rf_path);
                    }
                  }
                }

                // Auto-populate db_path if not already set
                auto db_it = parameter_widgets_.find("db_path");
                if (db_it != parameter_widgets_.end()) {
                  QWidget* db_container = db_it->second.widget;
                  QLineEdit* db_edit =
                      db_container->findChild<QLineEdit*>("file_path_edit");
                  if (db_edit && db_edit->text().isEmpty()) {
                    QString db_path = base_path + ".tbc.db";
                    if (QFileInfo::exists(db_path)) {
                      db_edit->setText(db_path);
                    }
                  }
                }
              });
        }

        layout->addWidget(edit, 1);  // Line edit takes most space
        layout->addWidget(browse_btn);

        editor = container;
        widget = container;
        break;
      }
    }

    if (widget) {
      // Create label with description as tooltip
      auto* label = new QLabel(QString::fromStdString(desc.display_name) + ":");
      label->setToolTip(QString::fromStdString(desc.description));
      widget->setToolTip(QString::fromStdString(desc.description));

      form_layout_->addRow(label, widget);
      parameter_widgets_[desc.name] =
          ParameterWidget{desc.type, widget, editor, label};

      // Connect change signals: dependent widgets are refreshed, and a live
      // update is scheduled when the user has asked for one. FILE_PATH is
      // absent by design (see the live update checkbox).
      switch (desc.type) {
        case orc::ParameterType::STRING:
          if (auto* combo = qobject_cast<QComboBox*>(editor)) {
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, &StageParameterDialog::on_parameter_changed);
          } else if (auto* edit = qobject_cast<QLineEdit*>(editor)) {
            connect(edit, &QLineEdit::textChanged, this,
                    &StageParameterDialog::on_parameter_changed);
          }
          break;
        case orc::ParameterType::INT32:
        case orc::ParameterType::UINT32:
          connect(static_cast<QSpinBox*>(editor),
                  QOverload<int>::of(&QSpinBox::valueChanged), this,
                  &StageParameterDialog::on_parameter_changed);
          break;
        case orc::ParameterType::DOUBLE:
          connect(static_cast<QDoubleSpinBox*>(editor),
                  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                  &StageParameterDialog::on_parameter_changed);
          break;
        case orc::ParameterType::BOOL:
          // Use stateChanged (Qt 6.0+) for compatibility with older Qt versions
          // checkStateChanged is only available in Qt 6.7+
          QT_WARNING_PUSH
          QT_WARNING_DISABLE_DEPRECATED
          connect(static_cast<QCheckBox*>(editor), &QCheckBox::stateChanged,
                  this, &StageParameterDialog::on_parameter_changed);
          QT_WARNING_POP
          break;
        default:
          break;
      }
    }
  }

  // Initial dependency update
  update_dependencies();

  // If no parameters, show message
  if (descriptors_.empty()) {
    form_layout_->addRow(
        new QLabel("This stage has no configurable parameters."));
    reset_button_->setEnabled(false);
  }
}

QWidget* StageParameterDialog::with_slider(QSpinBox* spin) {
  const int64_t range = static_cast<int64_t>(spin->maximum()) - spin->minimum();
  if (range <= 0 || range > kMaxSliderIntegerRange) {
    return spin;
  }

  auto* container = new QWidget();
  auto* layout = new QHBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);

  auto* slider = new QSlider(Qt::Horizontal);
  slider->setObjectName("parameter_slider");
  slider->setRange(spin->minimum(), spin->maximum());
  slider->setValue(spin->value());
  slider->setSingleStep(spin->singleStep());
  slider->setPageStep(
      std::max<int>(1, static_cast<int>(range / kSliderPageDivisions)));
  slider->setMinimumWidth(kSliderMinimumWidth);

  connect(slider, &QSlider::valueChanged, this, [this, spin](int value) {
    slider_sync_in_progress_ = true;
    spin->setValue(value);
    slider_sync_in_progress_ = false;
  });
  connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this, slider](int value) {
            if (slider_sync_in_progress_) return;
            const QSignalBlocker blocker(slider);
            slider->setValue(value);
          });

  layout->addWidget(slider, 1);
  layout->addWidget(spin);
  return container;
}

QWidget* StageParameterDialog::with_slider(QDoubleSpinBox* spin) {
  const double span = spin->maximum() - spin->minimum();
  if (span <= 0.0) {
    return spin;
  }

  auto* container = new QWidget();
  auto* layout = new QHBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);

  // The slider counts steps across the range rather than carrying the value:
  // QSlider is integer-only, and the editor beside it keeps the exact figure.
  const double minimum = spin->minimum();
  auto position_of = [minimum, span](double value) {
    const double fraction = (value - minimum) / span;
    return static_cast<int>(std::lround(fraction * kDoubleSliderSteps));
  };
  auto value_at = [minimum, span](int position) {
    return minimum + span * position / kDoubleSliderSteps;
  };

  auto* slider = new QSlider(Qt::Horizontal);
  slider->setObjectName("parameter_slider");
  slider->setRange(0, kDoubleSliderSteps);
  slider->setValue(position_of(spin->value()));
  slider->setPageStep(kDoubleSliderSteps / kSliderPageDivisions);
  slider->setMinimumWidth(kSliderMinimumWidth);

  connect(slider, &QSlider::valueChanged, this,
          [this, spin, value_at](int position) {
            slider_sync_in_progress_ = true;
            spin->setValue(value_at(position));
            slider_sync_in_progress_ = false;
          });
  connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
          [this, slider, position_of](double value) {
            if (slider_sync_in_progress_) return;
            const QSignalBlocker blocker(slider);
            slider->setValue(position_of(value));
          });

  layout->addWidget(slider, 1);
  layout->addWidget(spin);
  return container;
}

void StageParameterDialog::set_widget_value(const std::string& param_name,
                                            const orc::ParameterValue& value) {
  auto it = parameter_widgets_.find(param_name);
  if (it == parameter_widgets_.end()) return;

  const auto& pw = it->second;

  switch (pw.type) {
    case orc::ParameterType::INT32:
      static_cast<QSpinBox*>(pw.editor)->setValue(std::get<int32_t>(value));
      break;
    case orc::ParameterType::UINT32:
      static_cast<QSpinBox*>(pw.editor)->setValue(
          static_cast<int>(std::get<uint32_t>(value)));
      break;
    case orc::ParameterType::DOUBLE:
      static_cast<QDoubleSpinBox*>(pw.editor)->setValue(
          std::get<double>(value));
      break;
    case orc::ParameterType::BOOL:
      static_cast<QCheckBox*>(pw.editor)->setChecked(std::get<bool>(value));
      break;
    case orc::ParameterType::STRING:
      if (auto* combo = qobject_cast<QComboBox*>(pw.editor)) {
        select_combo_value(
            combo, QString::fromStdString(std::get<std::string>(value)));
      } else if (auto* edit = qobject_cast<QLineEdit*>(pw.editor)) {
        const std::string display_text =
            to_display_spec(param_name, std::get<std::string>(value));
        edit->setText(QString::fromStdString(display_text));
        if (orc::indexed_spec_kind(stage_name_, param_name) !=
            orc::IndexedSpecKind::kNone) {
          spec_display_baseline_[param_name] = display_text;
        }
      }
      break;
    case orc::ParameterType::FILE_PATH: {
      // For FILE_PATH, the editor is a container with a QLineEdit inside
      auto* edit = pw.editor->findChild<QLineEdit*>("file_path_edit");
      if (edit) {
        edit->setText(QString::fromStdString(std::get<std::string>(value)));
      }
      break;
    }
  }
}

orc::ParameterValue StageParameterDialog::get_widget_value(
    const std::string& param_name) const {
  auto it = parameter_widgets_.find(param_name);
  if (it == parameter_widgets_.end()) {
    return static_cast<int32_t>(0);  // Should never happen
  }

  const auto& pw = it->second;

  switch (pw.type) {
    case orc::ParameterType::INT32:
      return static_cast<int32_t>(static_cast<QSpinBox*>(pw.editor)->value());
    case orc::ParameterType::UINT32:
      return static_cast<uint32_t>(static_cast<QSpinBox*>(pw.editor)->value());
    case orc::ParameterType::DOUBLE:
      return static_cast<QDoubleSpinBox*>(pw.editor)->value();
    case orc::ParameterType::BOOL:
      return static_cast<QCheckBox*>(pw.editor)->isChecked();
    case orc::ParameterType::STRING:
      if (auto* combo = qobject_cast<QComboBox*>(pw.editor)) {
        const QVariant data = combo->currentData();
        return data.isValid() ? data.toString().toStdString()
                              : combo->currentText().toStdString();
      } else if (auto* edit = qobject_cast<QLineEdit*>(pw.editor)) {
        return from_display_spec(param_name, edit->text().toStdString());
      }
      break;
    case orc::ParameterType::FILE_PATH: {
      // For FILE_PATH, the editor is a container with a QLineEdit inside
      auto* edit = pw.editor->findChild<QLineEdit*>("file_path_edit");
      if (edit) {
        return edit->text().toStdString();
      }
      break;
    }
  }

  return static_cast<int32_t>(0);  // Should never happen
}

std::string StageParameterDialog::to_display_spec(
    const std::string& param_name, const std::string& stored_value) const {
  const auto kind = orc::indexed_spec_kind(stage_name_, param_name);
  return orc::indexed_spec_to_presentation(kind, stored_value);
}

std::string StageParameterDialog::from_display_spec(
    const std::string& param_name, const std::string& display_value) const {
  const auto kind = orc::indexed_spec_kind(stage_name_, param_name);
  auto stored = orc::indexed_spec_from_presentation(kind, display_value);
  return stored ? *stored : display_value;
}

void StageParameterDialog::on_reset_defaults() {
  for (const auto& desc : descriptors_) {
    bool value_applied = false;

    if (reset_values_.has_value()) {
      auto reset_it = reset_values_->find(desc.name);
      if (reset_it != reset_values_->end()) {
        set_widget_value(desc.name, reset_it->second);
        value_applied = true;
      }
    }

    if (!value_applied && desc.constraints.default_value.has_value()) {
      set_widget_value(desc.name, *desc.constraints.default_value);
    }
  }
}

bool StageParameterDialog::validate_values() {
  // Helper: resolve a potentially-relative path to absolute using the project
  // directory
  auto resolve_path = [this](const QString& path) -> QString {
    if (path.isEmpty() || project_path_.isEmpty()) return path;
    if (QFileInfo(path).isAbsolute()) return path;
    return QDir(QFileInfo(project_path_).absolutePath()).filePath(path);
  };

  // Helper: verify the SQLite metadata (.tbc.db) file exists; accept a legacy
  // JSON file with an info log, or warn generically if nothing is found.
  auto check_db_file = [this](const QString& db_path) -> bool {
    if (db_path.isEmpty()) return true;
    if (QFileInfo::exists(db_path)) return true;

    if (db_path.endsWith(".db", Qt::CaseInsensitive)) {
      QString json_path = db_path.left(db_path.length() - 3) + ".json";
      if (QFileInfo::exists(json_path)) {
        ORC_LOG_INFO(
            "TBC source '{}' has legacy JSON metadata; consider re-decoding "
            "with a current version of ld-decode/vhs-decode",
            QFileInfo(json_path).fileName().toStdString());
        return true;
      }
    }
    QMessageBox::warning(this, "Missing Metadata File",
                         QString("Metadata file not found:\n%1\n\nRe-run the "
                                 "decoder to generate a .tbc.db metadata file.")
                             .arg(db_path));
    return false;
  };

  // tbc_source derives db_path from input_path at runtime (input_path + ".db").
  // CVBS source stages also use input_path but do not require a .tbc.db
  // sidecar.
  const bool requires_derived_db_metadata = (stage_name_ == "tbc_source");

  if (requires_derived_db_metadata) {
    auto input_it = parameter_widgets_.find("input_path");
    if (input_it != parameter_widgets_.end()) {
      auto* edit =
          input_it->second.widget->findChild<QLineEdit*>("file_path_edit");
      if (edit && !edit->text().isEmpty()) {
        if (!check_db_file(resolve_path(edit->text()) + ".db")) return false;
      }
    }
  }

  // YC source stages: db_path is an explicit parameter
  auto db_it = parameter_widgets_.find("db_path");
  if (db_it != parameter_widgets_.end()) {
    auto* edit = db_it->second.widget->findChild<QLineEdit*>("file_path_edit");
    if (edit && !edit->text().isEmpty()) {
      if (!check_db_file(resolve_path(edit->text()))) return false;
    }
  }

  const QStringList validation_errors = collect_validation_errors();
  if (!validation_errors.isEmpty()) {
    QMessageBox::warning(this, "Invalid Parameters",
                         validation_errors.join("\n"));
    return false;
  }

  return true;
}

QStringList StageParameterDialog::collect_validation_errors() const {
  QStringList validation_errors;

  // The "-" stdio convention and live network stream URLs (see
  // orc/support/pipe_io.h) are CLI-only: validatePipeExecution(), which
  // checks IStreamingCompatibility for a non-seekable destination, is only
  // ever called from the CLI path — a GUI-triggered pipeline (preview,
  // re-trigger, the background observation pool) has no equivalent guard, so
  // neither value is safe to accept here even though a network URL, unlike
  // "-", isn't tied to the GUI process's own stdin/stdout. Rejected
  // generically for every FILE_PATH parameter of every stage, so it can
  // never reach a stage or be saved into a project file from this dialog.
  for (const auto& desc : descriptors_) {
    if (desc.type != orc::ParameterType::FILE_PATH) continue;
    const orc::ParameterValue value = get_widget_value(desc.name);
    if (!std::holds_alternative<std::string>(value)) continue;
    const auto& str_value = std::get<std::string>(value);
    if (str_value == orc::pipe_io::kStdioPathToken) {
      validation_errors << QString(
                               "%1: \"-\" (stdin/stdout) is reserved for "
                               "command-line use and cannot be set here.")
                               .arg(QString::fromStdString(desc.display_name));
    } else if (orc::pipe_io::is_network_stream_url(str_value)) {
      validation_errors << QString(
                               "%1: a network stream URL is reserved for "
                               "command-line use and cannot be set here.")
                               .arg(QString::fromStdString(desc.display_name));
    }
  }

  // Indexed spec parameters (frame/line ranges) are entered 1-based in the
  // UI; verify they convert cleanly to the stored 0-based form.
  for (const auto& desc : descriptors_) {
    const auto kind = orc::indexed_spec_kind(stage_name_, desc.name);
    if (kind == orc::IndexedSpecKind::kNone) continue;

    auto widget_it = parameter_widgets_.find(desc.name);
    if (widget_it == parameter_widgets_.end()) continue;
    auto* edit = qobject_cast<QLineEdit*>(widget_it->second.widget);
    if (edit == nullptr) continue;

    const std::string display_value = edit->text().toStdString();

    // Unmodified values (including unrecognised legacy specs shown verbatim)
    // pass through to the stage untouched.
    auto baseline_it = spec_display_baseline_.find(desc.name);
    if (baseline_it != spec_display_baseline_.end() &&
        baseline_it->second == display_value) {
      continue;
    }

    if (!orc::indexed_spec_from_presentation(kind, display_value)) {
      const QString example = (kind == orc::IndexedSpecKind::kDropoutMapSpec)
                                  ? "'[{frame:1,add:[{line:22,start:100,"
                                    "end:200}]}]'"
                                  : "'1-11,21-31'";
      validation_errors << QString(
                               "%1: invalid specification. Frame and line "
                               "numbers are 1-based (matching the preview); "
                               "for example %2.")
                               .arg(QString::fromStdString(desc.display_name))
                               .arg(example);
    }
  }

  // Relations between parameters, which no single descriptor can state.
  for (const auto& error : orc::gui::crossParameterErrors(get_values())) {
    validation_errors << QString::fromStdString(error);
  }

  return validation_errors;
}

void StageParameterDialog::on_validate_and_accept() {
  live_update_timer_->stop();
  if (validate_values()) {
    accept();
  }
}

void StageParameterDialog::on_validate_and_update() {
  live_update_timer_->stop();
  if (validate_values()) {
    last_applied_values_ = get_values();
    emit update_requested();
  }
}

bool StageParameterDialog::is_live_update_enabled() const {
  return live_update_check_ != nullptr && live_update_check_->isChecked();
}

void StageParameterDialog::on_parameter_changed() {
  update_dependencies();

  if (is_live_update_enabled()) {
    // Restart rather than let a pending shot through: a burst of edits is one
    // adjustment, and only the value it settles on is worth rendering.
    live_update_timer_->start();
  }
}

void StageParameterDialog::on_live_update_toggled(bool enabled) {
  // With live update ticked the edits have already been applied, so a button
  // offering to cancel them would be promising something it cannot do: the
  // window can only be closed, and what has been applied stays applied.
  if (auto* cancel_button = button_box_->button(QDialogButtonBox::Cancel)) {
    cancel_button->setText(enabled ? "Close" : "Cancel");
  }

  if (enabled) {
    // Ticking the box mid-edit shows the effect of what is already entered,
    // rather than waiting for the next keystroke to bring the preview level.
    live_update_timer_->start();
  } else {
    live_update_timer_->stop();
  }
}

void StageParameterDialog::on_live_update_timeout() {
  if (!is_live_update_enabled()) {
    return;
  }

  // A half-finished edit is a normal transient state here, so an invalid set
  // of values is simply not applied — no message box interrupts the user, and
  // the next edit gets its own chance.
  if (!collect_validation_errors().isEmpty()) {
    return;
  }

  auto values = get_values();
  if (last_applied_values_.has_value() && *last_applied_values_ == values) {
    return;  // Edited back to what is already applied; nothing to re-render.
  }

  last_applied_values_ = std::move(values);
  emit live_update_requested();
}

void StageParameterDialog::update_dependencies() {
  // Get current values of all parameters
  std::map<std::string, orc::ParameterValue> current_values;
  std::map<std::string, const orc::ParameterDescriptor*> descriptors_by_name;
  for (const auto& desc : descriptors_) {
    current_values[desc.name] = get_widget_value(desc.name);
    descriptors_by_name[desc.name] = &desc;
  }

  // A parameter is active only when its own dependency is met AND the
  // parameter it depends on is itself active (so e.g. encoder options keyed
  // on ffmpeg_format deactivate together with ffmpeg_format when
  // output_mode is "raw"). The visited set guards against dependency cycles.
  std::function<bool(const orc::ParameterDescriptor&, std::set<std::string>&)>
      is_active = [&](const orc::ParameterDescriptor& desc,
                      std::set<std::string>& visited) -> bool {
    if (!desc.constraints.depends_on.has_value()) {
      return true;
    }
    if (!visited.insert(desc.name).second) {
      return true;  // Cycle: treat as active rather than hiding everything
    }

    const auto& dep = *desc.constraints.depends_on;
    auto it = current_values.find(dep.parameter_name);
    if (it == current_values.end()) {
      return false;
    }

    std::string current_val = orc::parameter_util::value_to_string(it->second);
    bool satisfied =
        std::find(dep.required_values.begin(), dep.required_values.end(),
                  current_val) != dep.required_values.end();
    if (!satisfied) {
      return false;
    }

    auto parent_it = descriptors_by_name.find(dep.parameter_name);
    if (parent_it == descriptors_by_name.end()) {
      return true;
    }
    return is_active(*parent_it->second, visited);
  };

  // Check each parameter's dependencies
  for (const auto& desc : descriptors_) {
    if (!desc.constraints.depends_on.has_value()) {
      continue;  // No dependency, always enabled
    }

    const auto& dep = *desc.constraints.depends_on;
    std::set<std::string> visited;
    const bool should_enable = is_active(desc, visited);

    // Show/hide or enable/disable the widget and its label row
    auto widget_it = parameter_widgets_.find(desc.name);
    if (widget_it != parameter_widgets_.end()) {
      if (dep.hide_when_disabled) {
        widget_it->second.widget->setVisible(should_enable);
        if (widget_it->second.label) {
          widget_it->second.label->setVisible(should_enable);
        }
      } else {
        widget_it->second.widget->setEnabled(should_enable);
        if (widget_it->second.label) {
          widget_it->second.label->setEnabled(should_enable);
        }
      }
    }
  }
}

std::map<std::string, orc::ParameterValue> StageParameterDialog::get_values()
    const {
  std::map<std::string, orc::ParameterValue> values;

  for (const auto& desc : descriptors_) {
    values[desc.name] = get_widget_value(desc.name);
  }

  return values;
}
