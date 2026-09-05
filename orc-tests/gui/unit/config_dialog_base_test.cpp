/*
 * File:        config_dialog_base_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Rule-application widget tests for ConfigDialogBase subclasses
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <map>
#include <string>

#include "ffmpegpresetdialog.h"
#include "masklineconfigdialog.h"

namespace gui_unit_test {
namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-config-dialog-base-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = [] {
    auto* created_app = new QApplication(argc, argv);
    created_app->setQuitOnLastWindowClosed(false);
    return created_app;
  }();

  return *app;
}

QGroupBox* findGroupByTitle(QWidget& parent, const QString& title) {
  const auto groups = parent.findChildren<QGroupBox*>();
  for (auto* group : groups) {
    if (group != nullptr && group->title() == title) {
      return group;
    }
  }
  return nullptr;
}

QWidget* fieldWidgetByRowLabel(QGroupBox& group, const QString& row_label) {
  auto* layout = qobject_cast<QFormLayout*>(group.layout());
  if (layout == nullptr) {
    return nullptr;
  }

  for (int row = 0; row < layout->rowCount(); ++row) {
    auto* label_item = layout->itemAt(row, QFormLayout::LabelRole);
    auto* field_item = layout->itemAt(row, QFormLayout::FieldRole);
    if (label_item == nullptr || field_item == nullptr) {
      continue;
    }

    auto* label = qobject_cast<QLabel*>(label_item->widget());
    if (label != nullptr && label->text() == row_label) {
      return field_item->widget();
    }
  }

  return nullptr;
}

void clickOk(ConfigDialogBase& dialog) {
  auto* button_box = dialog.findChild<QDialogButtonBox*>();
  ASSERT_NE(button_box, nullptr);

  auto* ok_button = button_box->button(QDialogButtonBox::Ok);
  ASSERT_NE(ok_button, nullptr);

  ok_button->click();
  QCoreApplication::processEvents();
}

}  // namespace

TEST(ConfigDialogBaseTest, MaskLineDialog_AppliesRangeAndMaskLevelRules) {
  (void)ensureApplication();

  MaskLineConfigDialog dialog;

  auto* lines_group = findGroupByTitle(dialog, "Lines to Mask");
  auto* level_group = findGroupByTitle(dialog, "Mask Level");
  ASSERT_NE(lines_group, nullptr);
  ASSERT_NE(level_group, nullptr);

  auto* start_line = qobject_cast<QSpinBox*>(
      fieldWidgetByRowLabel(*lines_group, "Start Line:"));
  auto* end_line =
      qobject_cast<QSpinBox*>(fieldWidgetByRowLabel(*lines_group, "End Line:"));
  auto* level_preset_combo = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*level_group, "Level Preset:"));
  ASSERT_NE(start_line, nullptr);
  ASSERT_NE(end_line, nullptr);
  ASSERT_NE(level_preset_combo, nullptr);

  QPushButton* add_button = nullptr;
  for (auto* button : dialog.findChildren<QPushButton*>()) {
    if (button->text() == "Add Range") {
      add_button = button;
      break;
    }
  }
  ASSERT_NE(add_button, nullptr);

  // Broadcast line 20 (single line, field 1).
  start_line->setValue(20);
  end_line->setValue(20);
  add_button->click();

  level_preset_combo->setCurrentIndex(1);  // White (844)

  clickOk(dialog);

  const auto params = dialog.get_parameters();
  ASSERT_TRUE(params.find("lineSpec") != params.end());
  ASSERT_TRUE(params.find("maskSampleLevel") != params.end());
  ASSERT_TRUE(std::holds_alternative<std::string>(params.at("lineSpec")));
  ASSERT_TRUE(std::holds_alternative<int32_t>(params.at("maskSampleLevel")));

  // Broadcast line 20 → frame-flat 0-based line 19 in field 1, mirrored to
  // line 332 (313 + 19) in field 2 (PAL default: 313 lines in field 1).
  EXPECT_EQ(std::get<std::string>(params.at("lineSpec")), "19,332");
  EXPECT_EQ(std::get<int32_t>(params.at("maskSampleLevel")), 844);
}

TEST(ConfigDialogBaseTest, MaskLineDialog_AppliesCustomLevelAndRangeRule) {
  (void)ensureApplication();

  MaskLineConfigDialog dialog;

  auto* lines_group = findGroupByTitle(dialog, "Lines to Mask");
  auto* level_group = findGroupByTitle(dialog, "Mask Level");
  ASSERT_NE(lines_group, nullptr);
  ASSERT_NE(level_group, nullptr);

  auto* start_line = qobject_cast<QSpinBox*>(
      fieldWidgetByRowLabel(*lines_group, "Start Line:"));
  auto* end_line =
      qobject_cast<QSpinBox*>(fieldWidgetByRowLabel(*lines_group, "End Line:"));
  auto* level_preset_combo = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*level_group, "Level Preset:"));
  auto* custom_level = qobject_cast<QSpinBox*>(
      fieldWidgetByRowLabel(*level_group, "Custom (0–1023):"));
  ASSERT_NE(start_line, nullptr);
  ASSERT_NE(end_line, nullptr);
  ASSERT_NE(level_preset_combo, nullptr);
  ASSERT_NE(custom_level, nullptr);

  QPushButton* add_button = nullptr;
  for (auto* button : dialog.findChildren<QPushButton*>()) {
    if (button->text() == "Add Range") {
      add_button = button;
      break;
    }
  }
  ASSERT_NE(add_button, nullptr);

  // Broadcast lines 4–7 (field 1 range).
  start_line->setValue(4);
  end_line->setValue(7);
  add_button->click();

  // Custom spinbox is only editable when the "Custom" preset is selected.
  EXPECT_FALSE(custom_level->isEnabled());
  level_preset_combo->setCurrentIndex(2);  // Custom
  EXPECT_TRUE(custom_level->isEnabled());
  custom_level->setValue(512);

  clickOk(dialog);

  const auto params = dialog.get_parameters();
  ASSERT_TRUE(params.find("lineSpec") != params.end());
  ASSERT_TRUE(params.find("maskSampleLevel") != params.end());
  ASSERT_TRUE(std::holds_alternative<std::string>(params.at("lineSpec")));
  ASSERT_TRUE(std::holds_alternative<int32_t>(params.at("maskSampleLevel")));

  // Broadcast 4–7 → frame-flat 3-6 in field 1, mirrored to 316-319 in
  // field 2 (PAL default: 313 lines in field 1).
  EXPECT_EQ(std::get<std::string>(params.at("lineSpec")), "3-6,316-319");
  EXPECT_EQ(std::get<int32_t>(params.at("maskSampleLevel")), 512);
}

TEST(ConfigDialogBaseTest,
     FfmpegDialog_UpdatesFilenameExtensionWhenCategoryChanges) {
  (void)ensureApplication();

  FFmpegPresetDialog dialog;

  auto* category_group = findGroupByTitle(dialog, "Export Category");
  auto* filename_group = findGroupByTitle(dialog, "Output Filename");
  ASSERT_NE(category_group, nullptr);
  ASSERT_NE(filename_group, nullptr);

  auto* category_combo = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*category_group, "Category:"));
  auto* filename_container =
      fieldWidgetByRowLabel(*filename_group, "Filename:");
  ASSERT_NE(category_combo, nullptr);
  ASSERT_NE(filename_container, nullptr);

  auto* filename_edit = filename_container->findChild<QLineEdit*>();
  ASSERT_NE(filename_edit, nullptr);

  filename_edit->setText("capture.mp4");
  category_combo->setCurrentIndex(3);  // Broadcast (MXF)
  QCoreApplication::processEvents();

  EXPECT_TRUE(filename_edit->text().endsWith(".mxf"));
}

TEST(ConfigDialogBaseTest, FfmpegDialog_AppliesPresetAndOptionRules) {
  (void)ensureApplication();

  FFmpegPresetDialog dialog;

  auto* category_group = findGroupByTitle(dialog, "Export Category");
  auto* preset_group = findGroupByTitle(dialog, "Preset Selection");
  auto* filename_group = findGroupByTitle(dialog, "Output Filename");
  auto* options_group = findGroupByTitle(dialog, "Export Options");
  auto* advanced_group =
      findGroupByTitle(dialog, "Advanced Settings (Optional)");
  ASSERT_NE(category_group, nullptr);
  ASSERT_NE(preset_group, nullptr);
  ASSERT_NE(filename_group, nullptr);
  ASSERT_NE(options_group, nullptr);
  ASSERT_NE(advanced_group, nullptr);

  auto* category_combo = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*category_group, "Category:"));
  auto* preset_combo =
      qobject_cast<QComboBox*>(fieldWidgetByRowLabel(*preset_group, "Preset:"));
  auto* filename_container =
      fieldWidgetByRowLabel(*filename_group, "Filename:");
  auto* deinterlace = qobject_cast<QCheckBox*>(
      fieldWidgetByRowLabel(*options_group, "Deinterlace for web"));
  auto* embed_audio = qobject_cast<QCheckBox*>(
      fieldWidgetByRowLabel(*options_group, "Embed audio"));
  auto* embed_captions = qobject_cast<QCheckBox*>(
      fieldWidgetByRowLabel(*options_group, "Embed closed captions"));
  auto* quality = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*advanced_group, "Encoder Speed:"));
  auto* crf = qobject_cast<QSpinBox*>(
      fieldWidgetByRowLabel(*advanced_group, "Quality (CRF):"));
  auto* bitrate = qobject_cast<QSpinBox*>(
      fieldWidgetByRowLabel(*advanced_group, "Bitrate (Mbps):"));
  ASSERT_NE(category_combo, nullptr);
  ASSERT_NE(preset_combo, nullptr);
  ASSERT_NE(filename_container, nullptr);
  ASSERT_NE(deinterlace, nullptr);
  ASSERT_NE(embed_audio, nullptr);
  ASSERT_NE(embed_captions, nullptr);
  ASSERT_NE(quality, nullptr);
  ASSERT_NE(crf, nullptr);
  ASSERT_NE(bitrate, nullptr);

  auto* filename_edit = filename_container->findChild<QLineEdit*>();
  ASSERT_NE(filename_edit, nullptr);

  category_combo->setCurrentIndex(4);  // Universal (H.264)
  preset_combo->setCurrentIndex(0);    // H.264 (High Quality)
  QCoreApplication::processEvents();

  EXPECT_TRUE(deinterlace->isEnabled());

  deinterlace->setChecked(true);
  embed_audio->setChecked(true);
  embed_captions->setChecked(true);
  quality->setCurrentIndex(4);  // Very Slow
  crf->setValue(21);
  bitrate->setValue(8);
  filename_edit->setText("exports/out.mp4");

  clickOk(dialog);

  const auto params = dialog.get_parameters();

  ASSERT_TRUE(params.find("output_mode") != params.end());
  ASSERT_TRUE(params.find("ffmpeg_format") != params.end());
  ASSERT_TRUE(params.find("hardware_encoder") != params.end());
  ASSERT_TRUE(params.find("apply_deinterlace") != params.end());
  ASSERT_TRUE(params.find("encoder_preset") != params.end());
  ASSERT_TRUE(params.find("encoder_crf") != params.end());
  ASSERT_TRUE(params.find("encoder_bitrate") != params.end());
  ASSERT_TRUE(params.find("embed_audio") != params.end());
  ASSERT_TRUE(params.find("embed_closed_captions") != params.end());
  ASSERT_TRUE(params.find("output_path") != params.end());

  ASSERT_TRUE(std::holds_alternative<std::string>(params.at("output_mode")));
  ASSERT_TRUE(std::holds_alternative<std::string>(params.at("ffmpeg_format")));
  ASSERT_TRUE(
      std::holds_alternative<std::string>(params.at("hardware_encoder")));
  ASSERT_TRUE(std::holds_alternative<bool>(params.at("apply_deinterlace")));
  ASSERT_TRUE(std::holds_alternative<std::string>(params.at("encoder_preset")));
  ASSERT_TRUE(std::holds_alternative<int32_t>(params.at("encoder_crf")));
  ASSERT_TRUE(std::holds_alternative<int32_t>(params.at("encoder_bitrate")));
  ASSERT_TRUE(std::holds_alternative<bool>(params.at("embed_audio")));
  ASSERT_TRUE(std::holds_alternative<bool>(params.at("embed_closed_captions")));
  ASSERT_TRUE(std::holds_alternative<std::string>(params.at("output_path")));

  EXPECT_EQ(std::get<std::string>(params.at("output_mode")), "ffmpeg");
  EXPECT_EQ(std::get<std::string>(params.at("ffmpeg_format")), "mp4-h264");
  EXPECT_EQ(std::get<std::string>(params.at("hardware_encoder")), "none");
  EXPECT_TRUE(std::get<bool>(params.at("apply_deinterlace")));
  EXPECT_EQ(std::get<std::string>(params.at("encoder_preset")), "veryslow");
  EXPECT_EQ(std::get<int32_t>(params.at("encoder_crf")), 21);
  EXPECT_EQ(std::get<int32_t>(params.at("encoder_bitrate")), 8000000);
  EXPECT_TRUE(std::get<bool>(params.at("embed_audio")));
  EXPECT_TRUE(std::get<bool>(params.at("embed_closed_captions")));
  EXPECT_EQ(std::get<std::string>(params.at("output_path")), "exports/out.mp4");
}

TEST(ConfigDialogBaseTest, FfmpegDialog_Av1WebPresetDefaultsToCrf32) {
  (void)ensureApplication();

  FFmpegPresetDialog dialog;
  auto* category_group = findGroupByTitle(dialog, "Export Category");
  auto* preset_group = findGroupByTitle(dialog, "Preset Selection");
  auto* advanced_group =
      findGroupByTitle(dialog, "Advanced Settings (Optional)");
  ASSERT_NE(category_group, nullptr);
  ASSERT_NE(preset_group, nullptr);
  ASSERT_NE(advanced_group, nullptr);

  auto* category_combo = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*category_group, "Category:"));
  auto* preset_combo =
      qobject_cast<QComboBox*>(fieldWidgetByRowLabel(*preset_group, "Preset:"));
  ASSERT_NE(category_combo, nullptr);
  ASSERT_NE(preset_combo, nullptr);

  category_combo->setCurrentIndex(5);  // Modern (H.265/AV1)
  QCoreApplication::processEvents();
  const int av1_index = preset_combo->findText("AV1 (Web Delivery)");
  ASSERT_GE(av1_index, 0);
  preset_combo->setCurrentIndex(av1_index);
  QCoreApplication::processEvents();

  clickOk(dialog);
  const auto parameters = dialog.get_parameters();
  ASSERT_NE(parameters.find("encoder_crf"), parameters.end());
  EXPECT_EQ(std::get<int>(parameters.at("encoder_crf")), 32);
}

TEST(ConfigDialogBaseTest, FfmpegDialog_Vp415ePresetEmbedsDiscMetadata) {
  (void)ensureApplication();

  FFmpegPresetDialog dialog;
  auto* category_group = findGroupByTitle(dialog, "Export Category");
  auto* preset_group = findGroupByTitle(dialog, "Preset Selection");
  auto* options_group = findGroupByTitle(dialog, "Export Options");
  ASSERT_NE(category_group, nullptr);
  ASSERT_NE(preset_group, nullptr);
  ASSERT_NE(options_group, nullptr);

  auto* category_combo = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*category_group, "Category:"));
  auto* preset_combo =
      qobject_cast<QComboBox*>(fieldWidgetByRowLabel(*preset_group, "Preset:"));
  auto* embed_disc = qobject_cast<QCheckBox*>(
      fieldWidgetByRowLabel(*options_group, "Embed disc metadata"));
  auto* embed_chapters = qobject_cast<QCheckBox*>(
      fieldWidgetByRowLabel(*options_group, "Embed chapter metadata"));
  auto* detail_combo = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*options_group, "Disc metadata detail:"));
  ASSERT_NE(category_combo, nullptr);
  ASSERT_NE(preset_combo, nullptr);
  ASSERT_NE(embed_disc, nullptr);
  ASSERT_NE(embed_chapters, nullptr);
  ASSERT_NE(detail_combo, nullptr);

  category_combo->setCurrentIndex(0);  // Lossless / Archive
  preset_combo->setCurrentIndex(1);    // FFV1 for VP415e
  QCoreApplication::processEvents();

  // The preset exists to produce a self-contained file for a player emulator,
  // which needs the picture-number map to seek at all.
  EXPECT_TRUE(embed_disc->isChecked());
  EXPECT_TRUE(embed_chapters->isChecked());
  EXPECT_TRUE(detail_combo->isEnabled());
  EXPECT_EQ(detail_combo->currentIndex(), 0)
      << "the raw VBI dump is an archival choice, not an emulation one";

  clickOk(dialog);
  const auto params = dialog.get_parameters();
  ASSERT_TRUE(params.find("embed_disc_metadata") != params.end());
  ASSERT_TRUE(std::holds_alternative<bool>(params.at("embed_disc_metadata")));
  EXPECT_TRUE(std::get<bool>(params.at("embed_disc_metadata")));
  ASSERT_TRUE(params.find("disc_metadata_detail") != params.end());
  EXPECT_EQ(std::get<std::string>(params.at("disc_metadata_detail")), "map");
  EXPECT_EQ(std::get<std::string>(params.at("ffmpeg_format")),
            "mkv-ffv1-bt601");
}

TEST(ConfigDialogBaseTest, FfmpegDialog_DiscMetadataIsMatroskaOnly) {
  (void)ensureApplication();

  FFmpegPresetDialog dialog;
  auto* category_group = findGroupByTitle(dialog, "Export Category");
  auto* preset_group = findGroupByTitle(dialog, "Preset Selection");
  auto* options_group = findGroupByTitle(dialog, "Export Options");
  ASSERT_NE(category_group, nullptr);
  ASSERT_NE(preset_group, nullptr);
  ASSERT_NE(options_group, nullptr);

  auto* category_combo = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*category_group, "Category:"));
  auto* preset_combo =
      qobject_cast<QComboBox*>(fieldWidgetByRowLabel(*preset_group, "Preset:"));
  auto* embed_disc = qobject_cast<QCheckBox*>(
      fieldWidgetByRowLabel(*options_group, "Embed disc metadata"));
  ASSERT_NE(category_combo, nullptr);
  ASSERT_NE(preset_combo, nullptr);
  ASSERT_NE(embed_disc, nullptr);

  category_combo->setCurrentIndex(0);  // Lossless / Archive
  preset_combo->setCurrentIndex(1);    // FFV1 for VP415e (MKV)
  QCoreApplication::processEvents();
  ASSERT_TRUE(embed_disc->isChecked());

  // MP4 and MOV reject attachment streams outright, so the tick must not
  // survive a switch to a container that cannot carry the document.
  category_combo->setCurrentIndex(4);  // Universal (H.264) -> MP4
  preset_combo->setCurrentIndex(0);
  QCoreApplication::processEvents();

  EXPECT_FALSE(embed_disc->isEnabled());
  EXPECT_FALSE(embed_disc->isChecked());

  clickOk(dialog);
  const auto params = dialog.get_parameters();
  EXPECT_FALSE(std::get<bool>(params.at("embed_disc_metadata")));
}

TEST(ConfigDialogBaseTest, FfmpegDialog_RoundTripsAspectRatioAndVideoFilter) {
  (void)ensureApplication();

  FFmpegPresetDialog dialog;

  auto* options_group = findGroupByTitle(dialog, "Export Options");
  ASSERT_NE(options_group, nullptr);

  auto* aspect_combo = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*options_group, "Display aspect ratio:"));
  auto* filter_edit = qobject_cast<QLineEdit*>(
      fieldWidgetByRowLabel(*options_group, "Custom video filter:"));
  ASSERT_NE(aspect_combo, nullptr);
  ASSERT_NE(filter_edit, nullptr);

  // Defaults: auto aspect, no filter
  EXPECT_EQ(aspect_combo->currentIndex(), 0);
  EXPECT_TRUE(filter_edit->text().isEmpty());

  aspect_combo->setCurrentIndex(1);  // 4:3
  filter_edit->setText("  fieldmatch,decimate  ");

  clickOk(dialog);

  const auto params = dialog.get_parameters();
  ASSERT_TRUE(params.find("display_aspect_ratio") != params.end());
  ASSERT_TRUE(params.find("video_filter") != params.end());
  ASSERT_TRUE(
      std::holds_alternative<std::string>(params.at("display_aspect_ratio")));
  ASSERT_TRUE(std::holds_alternative<std::string>(params.at("video_filter")));
  EXPECT_EQ(std::get<std::string>(params.at("display_aspect_ratio")), "4:3");
  // Surrounding whitespace is trimmed on apply
  EXPECT_EQ(std::get<std::string>(params.at("video_filter")),
            "fieldmatch,decimate");

  // Loading parameters back must restore the UI state
  FFmpegPresetDialog reloaded;
  reloaded.set_parameters(
      {{"display_aspect_ratio", std::string("16:9")},
       {"video_filter", std::string("bwdif=mode=send_frame")}});

  auto* reloaded_options = findGroupByTitle(reloaded, "Export Options");
  ASSERT_NE(reloaded_options, nullptr);
  auto* reloaded_aspect = qobject_cast<QComboBox*>(
      fieldWidgetByRowLabel(*reloaded_options, "Display aspect ratio:"));
  auto* reloaded_filter = qobject_cast<QLineEdit*>(
      fieldWidgetByRowLabel(*reloaded_options, "Custom video filter:"));
  ASSERT_NE(reloaded_aspect, nullptr);
  ASSERT_NE(reloaded_filter, nullptr);
  EXPECT_EQ(reloaded_aspect->currentIndex(), 2);
  EXPECT_EQ(reloaded_filter->text(), "bwdif=mode=send_frame");
}

TEST(ConfigDialogBaseTest, FfmpegDialog_AudioGainFollowsEmbedAudioOption) {
  (void)ensureApplication();

  FFmpegPresetDialog dialog;

  auto* options_group = findGroupByTitle(dialog, "Export Options");
  ASSERT_NE(options_group, nullptr);

  auto* embed_audio = qobject_cast<QCheckBox*>(
      fieldWidgetByRowLabel(*options_group, "Embed audio"));
  auto* gain = qobject_cast<QDoubleSpinBox*>(
      fieldWidgetByRowLabel(*options_group, "Audio gain (dB):"));
  ASSERT_NE(embed_audio, nullptr);
  ASSERT_NE(gain, nullptr);

  // The gain control is only available when audio is embedded.
  EXPECT_FALSE(gain->isEnabled());
  embed_audio->setChecked(true);
  EXPECT_TRUE(gain->isEnabled());
  embed_audio->setChecked(false);
  EXPECT_FALSE(gain->isEnabled());

  // Apply and round-trip the value.
  embed_audio->setChecked(true);
  gain->setValue(6.0);
  clickOk(dialog);

  const auto params = dialog.get_parameters();
  ASSERT_TRUE(params.find("audio_gain_db") != params.end());
  ASSERT_TRUE(std::holds_alternative<double>(params.at("audio_gain_db")));
  EXPECT_EQ(std::get<double>(params.at("audio_gain_db")), 6.0);

  FFmpegPresetDialog reloaded;
  reloaded.set_parameters({{"embed_audio", true}, {"audio_gain_db", -3.0}});

  auto* reloaded_options = findGroupByTitle(reloaded, "Export Options");
  ASSERT_NE(reloaded_options, nullptr);
  auto* reloaded_gain = qobject_cast<QDoubleSpinBox*>(
      fieldWidgetByRowLabel(*reloaded_options, "Audio gain (dB):"));
  ASSERT_NE(reloaded_gain, nullptr);
  EXPECT_TRUE(reloaded_gain->isEnabled());
  EXPECT_EQ(reloaded_gain->value(), -3.0);
}

}  // namespace gui_unit_test
