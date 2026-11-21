/*
 * File:        configdialogbase.h
 * Module:      orc-view_core
 * Purpose:     Base class for simple rule-based configuration dialogs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef CONFIGDIALOGBASE_H
#define CONFIGDIALOGBASE_H

#include <orc/view/stage_tool_session.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

/**
 * @brief Base class for simple rule-based configuration dialogs
 *
 * This class provides a framework for creating user-friendly configuration
 * dialogs that map UI controls (checkboxes, combo boxes, etc.) to stage
 * parameters. Unlike AnalysisDialogBase (for data visualization) and
 * StageParameterDialog (for generic parameter editing), this is designed
 * for simple, intuitive configuration through rules-based mapping.
 *
 * Key features:
 * - High-level UI controls (checkboxes, dropdowns) that map to parameters
 * - Rules that can set multiple parameters from a single UI control
 * - Preset/template support for common configurations
 * - Clear, user-friendly labels and descriptions
 * - No need for users to understand raw parameter values
 *
 * Example use cases:
 * - "Mask NTSC subtitles" checkbox -> sets lineSpec="F:21", maskSampleLevel=256
 * - "VBI removal mode" dropdown -> sets different line ranges per selection
 * - "Field selection" radio buttons -> sets field-related parameters
 *
 * Derived classes should:
 * 1. Build their UI in the constructor using helper methods
 * 2. Implement apply_configuration() to map UI state to parameters
 * 3. Optionally implement load_from_parameters() to initialize UI from params
 *
 * Values are text, as the tool session reads and writes them ("true", "12",
 * "0.5"): the node's stored parameters come in through set_parameters() and
 * what OK configures goes out through get_parameters(), which holds only the
 * values apply_configuration() set.
 */
class ConfigDialogBase : public QDialog {
  Q_OBJECT

 public:
  /**
   * @brief Constructor
   * @param title Dialog window title
   * @param parent Parent widget
   */
  explicit ConfigDialogBase(const QString& title, QWidget* parent = nullptr);
  virtual ~ConfigDialogBase() = default;

  /// What OK last configured: the values apply_configuration() set, and
  /// nothing else.
  orc::view::ToolParameterMap get_parameters() const;

  /// Show the node's stored values (as text) in the controls.
  void set_parameters(const orc::view::ToolParameterMap& params);

  /// What OK does once apply_configuration() has run. Without one the dialog
  /// accepts at once; with one, the handler decides (it writes the values to
  /// the node and accepts when that worked).
  void set_submit_handler(std::function<void()> handler);

  /// Enable or disable OK, e.g. while the dialog waits for its stage.
  void set_ok_enabled(bool enabled);

 protected:
  /**
   * @brief Apply current UI state to parameter map
   *
   * Derived classes must implement this to translate UI controls
   * into parameter values. This is called when the user clicks OK/Apply.
   */
  virtual void apply_configuration() = 0;

  /**
   * @brief Load parameter values into UI controls
   *
   * Derived classes should implement this to reverse the apply_configuration()
   * process - setting UI controls based on parameter values.
   *
   * @param params Parameter values to load into UI
   */
  virtual void load_from_parameters(const orc::view::ToolParameterMap& params);

  /**
   * @brief Create a group box for organizing related controls
   * @param title Group box title
   * @return New group box with form layout ready for controls
   */
  QGroupBox* create_group(const QString& title);

  /**
   * @brief Add a checkbox control
   * @param layout Layout to add to
   * @param label Checkbox label text
   * @param tooltip Optional tooltip text
   * @return Created checkbox widget
   */
  QCheckBox* add_checkbox(QFormLayout* layout, const QString& label,
                          const QString& tooltip = QString());

  /**
   * @brief Add a combo box (dropdown) control
   * @param layout Layout to add to
   * @param label Label text for the control
   * @param items List of items for the dropdown
   * @param tooltip Optional tooltip text
   * @return Created combo box widget
   */
  QComboBox* add_combobox(QFormLayout* layout, const QString& label,
                          const QStringList& items,
                          const QString& tooltip = QString());

  /**
   * @brief Add an integer spin box control
   * @param layout Layout to add to
   * @param label Label text for the control
   * @param min Minimum value
   * @param max Maximum value
   * @param default_value Default value
   * @param tooltip Optional tooltip text
   * @return Created spin box widget
   */
  QSpinBox* add_spinbox(QFormLayout* layout, const QString& label, int min,
                        int max, int default_value,
                        const QString& tooltip = QString());

  /**
   * @brief Add a double spin box control
   * @param layout Layout to add to
   * @param label Label text for the control
   * @param min Minimum value
   * @param max Maximum value
   * @param default_value Default value
   * @param decimals Number of decimal places
   * @param tooltip Optional tooltip text
   * @return Created double spin box widget
   */
  QDoubleSpinBox* add_double_spinbox(QFormLayout* layout, const QString& label,
                                     double min, double max,
                                     double default_value, int decimals = 2,
                                     const QString& tooltip = QString());

  /**
   * @brief Add an informational label
   * @param layout Layout to add to
   * @param text Label text (can include HTML formatting)
   */
  void add_info_label(QFormLayout* layout, const QString& text);

  /// Set a value OK writes to the node.
  void set_parameter(const std::string& name, const std::string& value);
  void set_parameter(const std::string& name, const char* value);
  void set_parameter(const std::string& name, bool value);
  void set_parameter(const std::string& name, int32_t value);
  void set_parameter(const std::string& name, double value);

  /// Read one of the node's stored values, if it is there and reads as the
  /// type.
  static std::optional<std::string> text_value(
      const orc::view::ToolParameterMap& params, const std::string& name);
  static std::optional<bool> bool_value(
      const orc::view::ToolParameterMap& params, const std::string& name);
  static std::optional<int32_t> int_value(
      const orc::view::ToolParameterMap& params, const std::string& name);
  static std::optional<double> double_value(
      const orc::view::ToolParameterMap& params, const std::string& name);

  // Main layouts
  QVBoxLayout* main_layout_;
  QFormLayout* form_layout_;
  QDialogButtonBox* button_box_;

 private slots:
  void on_accept();
  void on_reset();

 private:
  virtual void reset_to_defaults();

  // What apply_configuration() set.
  orc::view::ToolParameterMap parameters_;
  std::function<void()> submit_handler_;
};

#endif  // CONFIGDIALOGBASE_H
