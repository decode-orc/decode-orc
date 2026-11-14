/*
 * File:        ffmpegpresetdialog.h
 * Module:      orc-view_core
 * Purpose:     Configuration dialog for FFmpeg video sink presets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef FFMPEGPRESETDIALOG_H
#define FFMPEGPRESETDIALOG_H

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <string>
#include <vector>

#include "configdialogbase.h"

/**
 * @brief Configuration dialog for Video Sink FFmpeg export presets
 *
 * This dialog provides a user-friendly interface for selecting video export
 * profiles without requiring users to understand codec details. Based on the
 * profiles from the legacy tbc-video-export tool.
 *
 * Features:
 * - Organized preset categories (Lossless, ProRes, Web, Broadcast, etc.)
 * - Hardware encoder detection and selection
 * - Auto-configuration of codec parameters
 * - Profile descriptions with use-case guidance
 *
 * The dialog translates preset selections into the output_mode/ffmpeg_format
 * and related parameters expected by VideoSinkStage. Applying a preset always
 * selects FFmpeg output mode.
 */
class FFmpegPresetDialog : public ConfigDialogBase {
  Q_OBJECT

 public:
  /// @p project_directory is the folder a browsed output file is stored
  /// relative to; empty stores it as picked.
  explicit FFmpegPresetDialog(const QString& project_directory = QString(),
                              QWidget* parent = nullptr);
  ~FFmpegPresetDialog() override = default;

  /// The hardware encoder types the stage can use ("vaapi", "nvenc", ...).
  /// Until this is called the dialog offers software encoding only.
  void setHardwareEncoders(std::vector<std::string> types);

 protected:
  void apply_configuration() override;
  void load_from_parameters(const orc::view::ToolParameterMap& params) override;

 private slots:
  void on_category_changed(int index);
  void on_preset_changed(int index);
  void on_hardware_encoder_changed(int index);
  void on_deinterlace_changed(Qt::CheckState state);

 private:
  void update_preset_list();
  void update_preset_description();
  // Enables "Weave still frames" only while deinterlacing is on and allowed.
  void update_weave_stills_enabled();
  void on_browse_filename_clicked();
  std::string get_file_extension_for_format(
      const std::string& format_string) const;

  struct PresetInfo {
    std::string format_string;   // e.g., "mov-prores"
    std::string name;            // Display name
    std::string description;     // Usage description
    std::string container;       // mp4, mkv, mov, mxf
    std::string codec;           // h264, hevc, prores, etc.
    bool supports_hardware;      // Can use hardware encoding
    bool supports_deinterlace;   // Web variant available
    int default_crf;             // Default quality
    std::string default_preset;  // Default encoder preset
    int default_bitrate;         // Default bitrate (0 = use CRF)
  };

  // UI Components
  QComboBox* category_combo_;
  QComboBox* preset_combo_;
  QLabel* description_label_;

  // Output filename group
  QLineEdit* filename_edit_;
  QPushButton* browse_btn_;

  // Hardware encoder group
  QGroupBox* hardware_group_;
  QComboBox* hardware_encoder_combo_;
  QLabel* hardware_status_label_;

  // Options group
  QCheckBox* deinterlace_checkbox_;
  QCheckBox* weave_stills_checkbox_;  // deinterlace_mode motion_only
  QComboBox* aspect_ratio_combo_;
  QLineEdit* video_filter_edit_;
  QCheckBox* embed_audio_checkbox_;
  QDoubleSpinBox* audio_gain_spinbox_;  // Enabled only when embedding audio
  QCheckBox* embed_captions_checkbox_;
  QCheckBox* embed_chapters_checkbox_;
  QCheckBox* embed_disc_metadata_checkbox_;
  QComboBox* disc_metadata_detail_combo_;  // Follows the disc metadata option

  // Advanced settings group
  QComboBox* quality_preset_combo_;
  QSpinBox* crf_spinbox_;
  QSpinBox* bitrate_spinbox_;

  // Hardware encoder types the stage reported
  std::vector<std::string> available_hw_encoders_;

  // Preset database
  std::vector<PresetInfo> all_presets_;
  std::vector<PresetInfo> current_category_presets_;

  // State tracking
  bool updating_ui_;           // Flag to prevent recursive updates
  QString project_directory_;  // Browsed paths are stored relative to it
};

#endif  // FFMPEGPRESETDIALOG_H
