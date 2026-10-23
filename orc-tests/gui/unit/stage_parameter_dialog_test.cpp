/*
 * File:        stage_parameter_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Widget tests for StageParameterDialog parameter editing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <map>
#include <string>
#include <vector>

#include "audio_channel_pair_notice.h"
#include "stage_parameter_context.h"
#include "stageparameterdialog.h"

namespace gui_unit_test {
namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-stage-parameter-dialog-test";
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

// The field widget a form row holds, exactly as the dialog placed it: the
// editor itself, or the container carrying it alongside a slider or a Browse
// button.
QWidget* fieldForDisplayName(StageParameterDialog& dialog,
                             const QString& display_name) {
  auto* form = dialog.findChild<QFormLayout*>();
  if (form == nullptr) {
    return nullptr;
  }

  const QString expected_label = display_name + ":";

  for (int row = 0; row < form->rowCount(); ++row) {
    auto* label_item = form->itemAt(row, QFormLayout::LabelRole);
    auto* field_item = form->itemAt(row, QFormLayout::FieldRole);
    if (label_item == nullptr || field_item == nullptr) {
      continue;
    }

    auto* label = qobject_cast<QLabel*>(label_item->widget());
    if (label != nullptr && label->text() == expected_label) {
      return field_item->widget();
    }
  }

  return nullptr;
}

// The control holding a parameter's value. A bounded numeric parameter puts
// its spin box in a row beside a slider, so the editor sits a level down from
// the field; every other type is its own field.
QWidget* widgetForDisplayName(StageParameterDialog& dialog,
                              const QString& display_name) {
  QWidget* field = fieldForDisplayName(dialog, display_name);
  if (field == nullptr) {
    return nullptr;
  }
  if (auto* spin = field->findChild<QAbstractSpinBox*>()) {
    return spin;
  }
  return field;
}

// The slider offered beside a parameter's editor, or nullptr when it has none.
QSlider* sliderForDisplayName(StageParameterDialog& dialog,
                              const QString& display_name) {
  QWidget* field = fieldForDisplayName(dialog, display_name);
  return field == nullptr ? nullptr : field->findChild<QSlider*>();
}

orc::ParameterDescriptor makeDescriptor(
    const std::string& name, const std::string& display_name,
    const orc::ParameterType type, const orc::ParameterValue& default_value,
    const std::optional<orc::ParameterValue>& min_value = std::nullopt,
    const std::optional<orc::ParameterValue>& max_value = std::nullopt,
    const std::vector<std::string>& allowed_strings = {}) {
  orc::ParameterDescriptor desc;
  desc.name = name;
  desc.display_name = display_name;
  desc.description = display_name + " description";
  desc.type = type;
  desc.constraints.default_value = default_value;
  desc.constraints.min_value = min_value;
  desc.constraints.max_value = max_value;
  desc.constraints.allowed_strings = allowed_strings;
  return desc;
}

}  // namespace

TEST(StageParameterDialogTest,
     Get_ValuesRoundTripsAllSupportedParameterEditorTypes) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(
      makeDescriptor("int_param", "Int Param", orc::ParameterType::INT32,
                     static_cast<int32_t>(0), static_cast<int32_t>(-50),
                     static_cast<int32_t>(50)));
  descriptors.push_back(
      makeDescriptor("uint_param", "UInt Param", orc::ParameterType::UINT32,
                     static_cast<uint32_t>(0), static_cast<uint32_t>(0),
                     static_cast<uint32_t>(100)));
  descriptors.push_back(makeDescriptor("double_param", "Double Param",
                                       orc::ParameterType::DOUBLE, 0.0, -10.0,
                                       10.0));
  descriptors.push_back(makeDescriptor("bool_param", "Bool Param",
                                       orc::ParameterType::BOOL, false));
  descriptors.push_back(makeDescriptor("string_param", "String Param",
                                       orc::ParameterType::STRING,
                                       std::string("initial")));
  descriptors.push_back(
      makeDescriptor("enum_param", "Enum Param", orc::ParameterType::STRING,
                     std::string("alpha"), std::nullopt, std::nullopt,
                     {"alpha", "beta", "gamma"}));

  std::map<std::string, orc::ParameterValue> current_values;
  current_values["int_param"] = static_cast<int32_t>(-12);
  current_values["uint_param"] = static_cast<uint32_t>(88);
  current_values["double_param"] = 3.25;
  current_values["bool_param"] = true;
  current_values["string_param"] = std::string("current");
  current_values["enum_param"] = std::string("beta");

  StageParameterDialog dialog("test-stage", "Test Stage",
                              "test stage description", descriptors,
                              current_values);

  auto* int_spin =
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Int Param"));
  auto* uint_spin =
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "UInt Param"));
  auto* double_spin = qobject_cast<QDoubleSpinBox*>(
      widgetForDisplayName(dialog, "Double Param"));
  auto* bool_check =
      qobject_cast<QCheckBox*>(widgetForDisplayName(dialog, "Bool Param"));
  auto* string_edit =
      qobject_cast<QLineEdit*>(widgetForDisplayName(dialog, "String Param"));
  auto* enum_combo =
      qobject_cast<QComboBox*>(widgetForDisplayName(dialog, "Enum Param"));

  ASSERT_NE(int_spin, nullptr);
  ASSERT_NE(uint_spin, nullptr);
  ASSERT_NE(double_spin, nullptr);
  ASSERT_NE(bool_check, nullptr);
  ASSERT_NE(string_edit, nullptr);
  ASSERT_NE(enum_combo, nullptr);

  int_spin->setValue(31);
  uint_spin->setValue(77);
  double_spin->setValue(-2.5);
  bool_check->setChecked(false);
  string_edit->setText("updated-value");
  enum_combo->setCurrentText("gamma");

  const auto values = dialog.get_values();

  ASSERT_TRUE(values.find("int_param") != values.end());
  ASSERT_TRUE(values.find("uint_param") != values.end());
  ASSERT_TRUE(values.find("double_param") != values.end());
  ASSERT_TRUE(values.find("bool_param") != values.end());
  ASSERT_TRUE(values.find("string_param") != values.end());
  ASSERT_TRUE(values.find("enum_param") != values.end());

  ASSERT_TRUE(std::holds_alternative<int32_t>(values.at("int_param")));
  ASSERT_TRUE(std::holds_alternative<uint32_t>(values.at("uint_param")));
  ASSERT_TRUE(std::holds_alternative<double>(values.at("double_param")));
  ASSERT_TRUE(std::holds_alternative<bool>(values.at("bool_param")));
  ASSERT_TRUE(std::holds_alternative<std::string>(values.at("string_param")));
  ASSERT_TRUE(std::holds_alternative<std::string>(values.at("enum_param")));

  EXPECT_EQ(std::get<int32_t>(values.at("int_param")), 31);
  EXPECT_EQ(std::get<uint32_t>(values.at("uint_param")), 77U);
  EXPECT_DOUBLE_EQ(std::get<double>(values.at("double_param")), -2.5);
  EXPECT_FALSE(std::get<bool>(values.at("bool_param")));
  EXPECT_EQ(std::get<std::string>(values.at("string_param")), "updated-value");
  EXPECT_EQ(std::get<std::string>(values.at("enum_param")), "gamma");
}

// The TBC sink's pair picker uses the same value/label combo entries, so the
// dialog shows "0: Analogue" while the project stores "0".
TEST(StageParameterDialogTest, Combo_TbcSinkAudioChannelPairShowsPairNames) {
  (void)ensureApplication();

  const char sep = StageParameterDialog::kComboValueLabelSeparator;
  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "audio_channel_pair", "Audio Channel Pair", orc::ParameterType::STRING,
      std::string("0"), std::nullopt, std::nullopt,
      {orc::gui::audioChannelPairComboEntry(0, "Analogue", sep),
       orc::gui::audioChannelPairComboEntry(1, "EFM digital audio", sep)}));

  std::map<std::string, orc::ParameterValue> current_values;
  current_values["audio_channel_pair"] = std::string("1");

  StageParameterDialog dialog("tbc_sink", "TBC Sink", "desc", descriptors,
                              current_values);

  auto* combo = qobject_cast<QComboBox*>(
      widgetForDisplayName(dialog, "Audio Channel Pair"));
  ASSERT_NE(combo, nullptr);

  EXPECT_EQ(combo->itemText(0).toStdString(), "0: Analogue");
  EXPECT_EQ(combo->itemText(1).toStdString(), "1: EFM digital audio");
  EXPECT_EQ(combo->currentText().toStdString(), "1: EFM digital audio");

  auto values = dialog.get_values();
  ASSERT_TRUE(
      std::holds_alternative<std::string>(values.at("audio_channel_pair")));
  EXPECT_EQ(std::get<std::string>(values.at("audio_channel_pair")), "1");
}

TEST(StageParameterDialogTest, Combo_ShowsLabelWhileStoringBareValue) {
  (void)ensureApplication();

  const char sep = StageParameterDialog::kComboValueLabelSeparator;
  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(
      makeDescriptor("channel_pair", "Channel pair", orc::ParameterType::STRING,
                     std::string("0"), std::nullopt, std::nullopt,
                     {std::string("0") + sep + "0 - Analogue Audio",
                      std::string("1") + sep + "1 - EFM digital audio"}));

  std::map<std::string, orc::ParameterValue> current_values;
  current_values["channel_pair"] = std::string("1");

  StageParameterDialog dialog("audio_channel_map", "Audio Channel Map", "desc",
                              descriptors, current_values);

  auto* combo =
      qobject_cast<QComboBox*>(widgetForDisplayName(dialog, "Channel pair"));
  ASSERT_NE(combo, nullptr);

  // Display carries the description; the stored value stays the bare index.
  EXPECT_EQ(combo->itemText(0).toStdString(), "0 - Analogue Audio");
  EXPECT_EQ(combo->itemText(1).toStdString(), "1 - EFM digital audio");
  EXPECT_EQ(combo->currentText().toStdString(), "1 - EFM digital audio");

  auto values = dialog.get_values();
  ASSERT_TRUE(std::holds_alternative<std::string>(values.at("channel_pair")));
  EXPECT_EQ(std::get<std::string>(values.at("channel_pair")), "1");

  // Selecting the first entry returns its bare value, not the label.
  combo->setCurrentIndex(0);
  values = dialog.get_values();
  EXPECT_EQ(std::get<std::string>(values.at("channel_pair")), "0");
}

TEST(StageParameterDialogTest,
     Numeric_EditorsRespectConfiguredBoundsAndClampStepping) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(
      makeDescriptor("int_param", "Int Param", orc::ParameterType::INT32,
                     static_cast<int32_t>(0), static_cast<int32_t>(-10),
                     static_cast<int32_t>(10)));
  descriptors.push_back(
      makeDescriptor("uint_param", "UInt Param", orc::ParameterType::UINT32,
                     static_cast<uint32_t>(0), static_cast<uint32_t>(1),
                     static_cast<uint32_t>(3)));
  descriptors.push_back(makeDescriptor("double_param", "Double Param",
                                       orc::ParameterType::DOUBLE, 0.0, -0.5,
                                       0.5));

  StageParameterDialog dialog("test-stage", "Test Stage",
                              "test stage description", descriptors, {});

  auto* int_spin =
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Int Param"));
  auto* uint_spin =
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "UInt Param"));
  auto* double_spin = qobject_cast<QDoubleSpinBox*>(
      widgetForDisplayName(dialog, "Double Param"));

  ASSERT_NE(int_spin, nullptr);
  ASSERT_NE(uint_spin, nullptr);
  ASSERT_NE(double_spin, nullptr);

  EXPECT_EQ(int_spin->minimum(), -10);
  EXPECT_EQ(int_spin->maximum(), 10);
  EXPECT_EQ(uint_spin->minimum(), 1);
  EXPECT_EQ(uint_spin->maximum(), 3);
  EXPECT_DOUBLE_EQ(double_spin->minimum(), -0.5);
  EXPECT_DOUBLE_EQ(double_spin->maximum(), 0.5);

  int_spin->setValue(999);
  EXPECT_EQ(int_spin->value(), 10);
  int_spin->setValue(-999);
  EXPECT_EQ(int_spin->value(), -10);

  uint_spin->setValue(0);
  EXPECT_EQ(uint_spin->value(), 1);
  uint_spin->setValue(999);
  EXPECT_EQ(uint_spin->value(), 3);

  double_spin->setValue(10.0);
  EXPECT_DOUBLE_EQ(double_spin->value(), 0.5);
  double_spin->setValue(-10.0);
  EXPECT_DOUBLE_EQ(double_spin->value(), -0.5);

  int_spin->setValue(9);
  int_spin->stepUp();
  EXPECT_EQ(int_spin->value(), 10);
  int_spin->stepUp();
  EXPECT_EQ(int_spin->value(), 10);

  uint_spin->setValue(2);
  uint_spin->stepUp();
  EXPECT_EQ(uint_spin->value(), 3);
  uint_spin->stepUp();
  EXPECT_EQ(uint_spin->value(), 3);

  double_spin->setValue(0.0);
  double_spin->stepUp();
  EXPECT_DOUBLE_EQ(double_spin->value(), 0.5);
  double_spin->stepDown();
  EXPECT_DOUBLE_EQ(double_spin->value(), -0.5);
  double_spin->stepDown();
  EXPECT_DOUBLE_EQ(double_spin->value(), -0.5);
}

TEST(StageParameterDialogTest, Av1RateControlsEnableForAv1Format) {
  (void)ensureApplication();

  auto ffmpeg_format =
      makeDescriptor("ffmpeg_format", "FFmpeg Format",
                     orc::ParameterType::STRING, std::string("mkv-ffv1"),
                     std::nullopt, std::nullopt, {"mkv-ffv1", "mp4-av1"});
  auto encoder_crf =
      makeDescriptor("encoder_crf", "Encoder CRF", orc::ParameterType::INT32,
                     static_cast<int32_t>(18), static_cast<int32_t>(0),
                     static_cast<int32_t>(51));
  encoder_crf.constraints.depends_on =
      orc::ParameterDependency{"ffmpeg_format", {"mp4-av1"}};
  auto encoder_bitrate =
      makeDescriptor("encoder_bitrate", "Encoder Bitrate",
                     orc::ParameterType::INT32, static_cast<int32_t>(0),
                     static_cast<int32_t>(0), static_cast<int32_t>(100000000));
  encoder_bitrate.constraints.depends_on =
      orc::ParameterDependency{"ffmpeg_format", {"mp4-av1"}};

  StageParameterDialog dialog("video_sink", "Video Sink", "",
                              {ffmpeg_format, encoder_crf, encoder_bitrate},
                              {});

  auto* format_combo =
      qobject_cast<QComboBox*>(widgetForDisplayName(dialog, "FFmpeg Format"));
  // A dependency hides the whole row, slider included, so the field is what
  // the state is read from.
  auto* crf = fieldForDisplayName(dialog, "Encoder CRF");
  auto* bitrate = fieldForDisplayName(dialog, "Encoder Bitrate");
  ASSERT_NE(format_combo, nullptr);
  ASSERT_NE(crf, nullptr);
  ASSERT_NE(bitrate, nullptr);
  ASSERT_NE(
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Encoder CRF")),
      nullptr);
  EXPECT_TRUE(crf->isHidden());
  EXPECT_TRUE(bitrate->isHidden());

  const int av1_index = format_combo->findText("mp4-av1");
  ASSERT_GE(av1_index, 0);
  format_combo->setCurrentIndex(av1_index);
  QCoreApplication::processEvents();

  EXPECT_FALSE(crf->isHidden());
  EXPECT_FALSE(bitrate->isHidden());
}

TEST(StageParameterDialogTest,
     FrameMapRanges_DisplayedOneBasedAndStoredZeroBased) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "ranges", "Frame Ranges", orc::ParameterType::STRING, std::string("")));

  std::map<std::string, orc::ParameterValue> current_values;
  current_values["ranges"] = std::string("0-10,PAD_5,20-30");

  StageParameterDialog dialog("frame_map", "Frame Map", "", descriptors,
                              current_values);

  auto* edit =
      qobject_cast<QLineEdit*>(widgetForDisplayName(dialog, "Frame Ranges"));
  ASSERT_NE(edit, nullptr);

  // Stored 0-based value is displayed 1-based (matching the preview)
  EXPECT_EQ(edit->text().toStdString(), "1-11,PAD_5,21-31");

  // A 1-based edit is stored 0-based
  edit->setText("1-101,201");
  const auto values = dialog.get_values();
  ASSERT_TRUE(values.find("ranges") != values.end());
  ASSERT_TRUE(std::holds_alternative<std::string>(values.at("ranges")));
  EXPECT_EQ(std::get<std::string>(values.at("ranges")), "0-100,200");
}

TEST(StageParameterDialogTest,
     DecoderSchedule_ShownForMultipleAndStoredZeroBased) {
  (void)ensureApplication();

  // The two decoder parameters as the video sink publishes them for PAL.
  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "decoder_type", "Decoder Type", orc::ParameterType::STRING,
      std::string("pal2d"), std::nullopt, std::nullopt,
      {"pal2d", "transform2d", "transform3d", "secam", "mono", "multiple"}));
  auto schedule = makeDescriptor("decoder_schedule", "Decoder Schedule",
                                 orc::ParameterType::STRING, std::string(""));
  schedule.constraints.depends_on =
      orc::ParameterDependency{"decoder_type", {"multiple"}};
  descriptors.push_back(schedule);

  std::map<std::string, orc::ParameterValue> current_values;
  current_values["decoder_type"] = std::string("pal2d");
  current_values["decoder_schedule"] = std::string("");

  StageParameterDialog dialog("video_sink", "Video Sink", "", descriptors,
                              current_values);

  auto* type =
      qobject_cast<QComboBox*>(widgetForDisplayName(dialog, "Decoder Type"));
  auto* edit = qobject_cast<QLineEdit*>(
      widgetForDisplayName(dialog, "Decoder Schedule"));
  ASSERT_NE(type, nullptr);
  ASSERT_NE(edit, nullptr);

  // A single decoder: no schedule to fill in, and none judged.
  EXPECT_TRUE(edit->isHidden());
  EXPECT_TRUE(dialog.collect_validation_errors().isEmpty());

  // "multiple" brings the schedule up, and an empty one is not accepted.
  type->setCurrentIndex(type->findData(QString("multiple")));
  EXPECT_FALSE(edit->isHidden());
  EXPECT_EQ(dialog.collect_validation_errors().size(), 1);

  // A decoder this project does not offer is named.
  edit->setText("transform3d, 1001-1500:ntsc2d");
  const QStringList errors = dialog.collect_validation_errors();
  ASSERT_EQ(errors.size(), 1);
  EXPECT_TRUE(errors.front().contains("ntsc2d"))
      << errors.front().toStdString();

  // Typed 1-based, stored 0-based.
  edit->setText("transform3d, 1001-1500:transform2d");
  EXPECT_TRUE(dialog.collect_validation_errors().isEmpty());
  const auto values = dialog.get_values();
  EXPECT_EQ(std::get<std::string>(values.at("decoder_type")), "multiple");
  EXPECT_EQ(std::get<std::string>(values.at("decoder_schedule")),
            "transform3d,1000-1499:transform2d");
}

TEST(StageParameterDialogTest, DecoderSchedule_StoredValueDisplayedOneBased) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(
      makeDescriptor("decoder_type", "Decoder Type", orc::ParameterType::STRING,
                     std::string("pal2d"), std::nullopt, std::nullopt,
                     {"pal2d", "transform2d", "transform3d", "multiple"}));
  auto schedule = makeDescriptor("decoder_schedule", "Decoder Schedule",
                                 orc::ParameterType::STRING, std::string(""));
  schedule.constraints.depends_on =
      orc::ParameterDependency{"decoder_type", {"multiple"}};
  descriptors.push_back(schedule);

  std::map<std::string, orc::ParameterValue> current_values;
  current_values["decoder_type"] = std::string("multiple");
  current_values["decoder_schedule"] =
      std::string("transform3d,0-99:transform2d");

  StageParameterDialog dialog("video_sink", "Video Sink", "", descriptors,
                              current_values);

  auto* edit = qobject_cast<QLineEdit*>(
      widgetForDisplayName(dialog, "Decoder Schedule"));
  ASSERT_NE(edit, nullptr);
  EXPECT_FALSE(edit->isHidden());
  EXPECT_EQ(edit->text().toStdString(), "transform3d,1-100:transform2d");
}

TEST(StageParameterDialogTest,
     DropoutMapSpec_FrameValuesConvertedLineValuesUntouched) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor("dropout_map", "Dropout Map",
                                       orc::ParameterType::STRING,
                                       std::string("[]")));

  std::map<std::string, orc::ParameterValue> current_values;
  current_values["dropout_map"] =
      std::string("[{frame:0,add:[{line:10,start:100,end:200}]}]");

  StageParameterDialog dialog("dropout_map", "Dropout Map", "", descriptors,
                              current_values);

  auto* edit =
      qobject_cast<QLineEdit*>(widgetForDisplayName(dialog, "Dropout Map"));
  ASSERT_NE(edit, nullptr);

  EXPECT_EQ(edit->text().toStdString(),
            "[{frame:1,add:[{line:10,start:100,end:200}]}]");

  edit->setText("[{frame:42,add:[{line:5,start:1,end:2}]}]");
  const auto values = dialog.get_values();
  ASSERT_TRUE(std::holds_alternative<std::string>(values.at("dropout_map")));
  EXPECT_EQ(std::get<std::string>(values.at("dropout_map")),
            "[{frame:41,add:[{line:5,start:1,end:2}]}]");
}

TEST(StageParameterDialogTest,
     MaskLineSpec_LegacyUnparseableValuePassesThroughUnchanged) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor("lineSpec", "Line Specification",
                                       orc::ParameterType::STRING,
                                       std::string("")));

  std::map<std::string, orc::ParameterValue> current_values;
  current_values["lineSpec"] = std::string("F:20");

  StageParameterDialog dialog("mask_line", "Mask Line", "", descriptors,
                              current_values);

  auto* edit = qobject_cast<QLineEdit*>(
      widgetForDisplayName(dialog, "Line Specification"));
  ASSERT_NE(edit, nullptr);

  // Legacy specs that cannot be converted are shown verbatim...
  EXPECT_EQ(edit->text().toStdString(), "F:20");

  // ...and saved back untouched when the user does not modify them.
  const auto values = dialog.get_values();
  ASSERT_TRUE(std::holds_alternative<std::string>(values.at("lineSpec")));
  EXPECT_EQ(std::get<std::string>(values.at("lineSpec")), "F:20");
}

TEST(StageParameterDialogTest,
     NonSpecStringParameters_AreNotConvertedForOtherStages) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "ranges", "Frame Ranges", orc::ParameterType::STRING, std::string("")));

  std::map<std::string, orc::ParameterValue> current_values;
  current_values["ranges"] = std::string("0-10");

  // Same parameter name on a different stage: no conversion
  StageParameterDialog dialog("some_other_stage", "Other Stage", "",
                              descriptors, current_values);

  auto* edit =
      qobject_cast<QLineEdit*>(widgetForDisplayName(dialog, "Frame Ranges"));
  ASSERT_NE(edit, nullptr);
  EXPECT_EQ(edit->text().toStdString(), "0-10");

  const auto values = dialog.get_values();
  EXPECT_EQ(std::get<std::string>(values.at("ranges")), "0-10");
}

TEST(StageParameterDialogTest,
     OpeningSize_KeepsEveryRowAtFullHeight_WhenTheFormIsTallerThanTheScreen) {
  (void)ensureApplication();

  // Far more parameters than fit on any screen: the form has to be scrolled,
  // and the rows must keep their natural height rather than being compressed
  // to make the whole list fit.
  std::vector<orc::ParameterDescriptor> descriptors;
  for (int i = 0; i < 80; ++i) {
    descriptors.push_back(makeDescriptor(
        "param_" + std::to_string(i), "Parameter " + std::to_string(i),
        orc::ParameterType::DOUBLE, 0.0, 0.0, 100.0));
  }

  StageParameterDialog dialog("test-stage", "Test Stage",
                              "A stage with a very long parameter list.",
                              descriptors, {});
  dialog.show();
  QCoreApplication::processEvents();

  ASSERT_NE(dialog.findChild<QScrollArea*>("parameter_scroll_area"), nullptr);

  auto* form = dialog.findChild<QFormLayout*>();
  ASSERT_NE(form, nullptr);
  for (int row = 0; row < form->rowCount(); ++row) {
    auto* field_item = form->itemAt(row, QFormLayout::FieldRole);
    ASSERT_NE(field_item, nullptr);
    auto* field = field_item->widget();
    ASSERT_NE(field, nullptr);
    EXPECT_GE(field->height(), field->sizeHint().height())
        << "row " << row << " was squashed below its preferred height";
  }

  // ...and the dialog itself still fits on the screen it opened on.
  const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
  EXPECT_LE(dialog.height(), available.height());
  EXPECT_LE(dialog.width(), available.width());
}

TEST(StageParameterDialogTest,
     OpeningSize_IsWideEnoughToReadAPath_WhenAParameterTakesAFilePath) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor("output_path", "Output File",
                                       orc::ParameterType::FILE_PATH,
                                       std::string("")));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});
  dialog.show();
  QCoreApplication::processEvents();

  // A working path is far longer than the 400px floor the dialog used to open
  // at, which left the field too narrow to read one in.
  const int minimum_width = dialog.fontMetrics().averageCharWidth() * 80;
  EXPECT_GE(dialog.width(), minimum_width);

  auto* edit = dialog.findChild<QLineEdit*>("file_path_edit");
  ASSERT_NE(edit, nullptr);
  EXPECT_GT(edit->width(), dialog.width() / 3);
}

// ---------------------------------------------------------------------------
// Live update (issue #295): parameters judged by eye are applied to the
// preview as they are changed, rather than only when Update is pressed.
// ---------------------------------------------------------------------------

namespace {

// Waiting for a request that is expected: generous, and costs nothing when
// the signal arrives, because the wait returns as soon as it does.
constexpr int kLiveUpdateWaitMs = 3000;

// Waiting to confirm no request is made: several times the dialog's settle
// window, which is as long as a "nothing happened" check needs to be.
constexpr int kLiveUpdateQuietMs = 1500;

QCheckBox* liveUpdateCheck(StageParameterDialog& dialog) {
  return dialog.findChild<QCheckBox*>("live_update_check");
}

std::vector<orc::ParameterDescriptor> makeLevelDescriptors() {
  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(
      makeDescriptor("blackLevel", "Black IRE", orc::ParameterType::INT32,
                     static_cast<int32_t>(0), static_cast<int32_t>(-1),
                     static_cast<int32_t>(60000)));
  descriptors.push_back(
      makeDescriptor("whiteLevel", "White IRE", orc::ParameterType::INT32,
                     static_cast<int32_t>(100), static_cast<int32_t>(-1),
                     static_cast<int32_t>(60000)));
  return descriptors;
}

}  // namespace

TEST(StageParameterDialogTest,
     LiveUpdate_IsOfferedAndUnticked_WhenDialogOpens) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "gain", "Gain", orc::ParameterType::DOUBLE, 1.0, 0.0, 4.0));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});

  auto* check = liveUpdateCheck(dialog);
  ASSERT_NE(check, nullptr);
  EXPECT_FALSE(check->isChecked());
  EXPECT_FALSE(dialog.is_live_update_enabled());
}

TEST(StageParameterDialogTest,
     LiveUpdate_RequestsAnUpdate_WhenAParameterIsChangedWithLiveUpdateOn) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "gain", "Gain", orc::ParameterType::DOUBLE, 1.0, 0.0, 4.0));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});
  QSignalSpy live(&dialog, &StageParameterDialog::live_update_requested);
  QSignalSpy manual(&dialog, &StageParameterDialog::update_requested);

  auto* check = liveUpdateCheck(dialog);
  ASSERT_NE(check, nullptr);
  check->setChecked(true);

  auto* spin =
      qobject_cast<QDoubleSpinBox*>(widgetForDisplayName(dialog, "Gain"));
  ASSERT_NE(spin, nullptr);
  spin->setValue(2.5);

  ASSERT_TRUE(live.wait(kLiveUpdateWaitMs));
  EXPECT_EQ(live.count(), 1);
  // The manual Update path is untouched: the caller can tell the two apart.
  EXPECT_EQ(manual.count(), 0);
  EXPECT_DOUBLE_EQ(std::get<double>(dialog.get_values().at("gain")), 2.5);
}

TEST(StageParameterDialogTest,
     LiveUpdate_CoalescesABurstOfEditsIntoASingleRequest) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "gain", "Gain", orc::ParameterType::DOUBLE, 1.0, 0.0, 4.0));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});
  QSignalSpy live(&dialog, &StageParameterDialog::live_update_requested);

  auto* check = liveUpdateCheck(dialog);
  ASSERT_NE(check, nullptr);
  check->setChecked(true);

  auto* spin =
      qobject_cast<QDoubleSpinBox*>(widgetForDisplayName(dialog, "Gain"));
  ASSERT_NE(spin, nullptr);
  // Stands in for holding a spin box arrow down or typing several digits.
  for (int step = 1; step <= 5; ++step) {
    spin->setValue(1.0 + 0.1 * step);
  }

  ASSERT_TRUE(live.wait(kLiveUpdateWaitMs));
  EXPECT_EQ(live.count(), 1);
  EXPECT_DOUBLE_EQ(std::get<double>(dialog.get_values().at("gain")), 1.5);
}

TEST(StageParameterDialogTest,
     LiveUpdate_RequestsNothing_WhenTheCheckboxIsUnticked) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "gain", "Gain", orc::ParameterType::DOUBLE, 1.0, 0.0, 4.0));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});
  QSignalSpy live(&dialog, &StageParameterDialog::live_update_requested);

  auto* spin =
      qobject_cast<QDoubleSpinBox*>(widgetForDisplayName(dialog, "Gain"));
  ASSERT_NE(spin, nullptr);
  spin->setValue(2.5);

  EXPECT_FALSE(live.wait(kLiveUpdateQuietMs));
  EXPECT_EQ(live.count(), 0);
}

TEST(StageParameterDialogTest,
     LiveUpdate_RequestsNothing_WhenTickedWithNothingChanged) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "gain", "Gain", orc::ParameterType::DOUBLE, 1.0, 0.0, 4.0));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});
  QSignalSpy live(&dialog, &StageParameterDialog::live_update_requested);

  auto* check = liveUpdateCheck(dialog);
  ASSERT_NE(check, nullptr);
  check->setChecked(true);

  // The preview already shows these values; re-rendering them is pure cost.
  EXPECT_FALSE(live.wait(kLiveUpdateQuietMs));
  EXPECT_EQ(live.count(), 0);
}

TEST(StageParameterDialogTest,
     LiveUpdate_RequestsNothing_WhenTheValuesFailCrossParameterValidation) {
  (void)ensureApplication();

  auto descriptors = makeLevelDescriptors();
  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});
  QSignalSpy live(&dialog, &StageParameterDialog::live_update_requested);

  auto* check = liveUpdateCheck(dialog);
  ASSERT_NE(check, nullptr);
  check->setChecked(true);

  auto* black =
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Black IRE"));
  auto* white =
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "White IRE"));
  ASSERT_NE(black, nullptr);
  ASSERT_NE(white, nullptr);

  // Black above white: a state an adjustment passes through, so it must be
  // left un-applied silently rather than raised in a message box.
  black->setValue(500);
  EXPECT_FALSE(live.wait(kLiveUpdateQuietMs));
  EXPECT_EQ(live.count(), 0);

  // Once the pair is consistent again, the edit is applied.
  white->setValue(900);
  ASSERT_TRUE(live.wait(kLiveUpdateWaitMs));
  EXPECT_EQ(live.count(), 1);
}

TEST(StageParameterDialogTest,
     LiveUpdate_RequestsNothing_WhenEditedBackToTheValuesLastApplied) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "gain", "Gain", orc::ParameterType::DOUBLE, 1.0, 0.0, 4.0));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});
  QSignalSpy live(&dialog, &StageParameterDialog::live_update_requested);

  auto* check = liveUpdateCheck(dialog);
  ASSERT_NE(check, nullptr);
  check->setChecked(true);

  auto* spin =
      qobject_cast<QDoubleSpinBox*>(widgetForDisplayName(dialog, "Gain"));
  ASSERT_NE(spin, nullptr);
  spin->setValue(2.5);
  ASSERT_TRUE(live.wait(kLiveUpdateWaitMs));
  ASSERT_EQ(live.count(), 1);

  // Overshoot and come back within one settle window: the preview already
  // shows 2.5.
  spin->setValue(3.0);
  spin->setValue(2.5);
  EXPECT_FALSE(live.wait(kLiveUpdateQuietMs));
  EXPECT_EQ(live.count(), 1);
}

TEST(StageParameterDialogTest,
     LiveUpdate_LeavesFilePathEditsToTheUpdateButton) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor("output_path", "Output File",
                                       orc::ParameterType::FILE_PATH,
                                       std::string("")));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});
  QSignalSpy live(&dialog, &StageParameterDialog::live_update_requested);

  auto* check = liveUpdateCheck(dialog);
  ASSERT_NE(check, nullptr);
  check->setChecked(true);
  // Let the settle window opened by the tick pass, so what follows is
  // measuring the path edit alone.
  ASSERT_FALSE(live.wait(kLiveUpdateQuietMs));

  auto* edit = dialog.findChild<QLineEdit*>("file_path_edit");
  ASSERT_NE(edit, nullptr);
  // A path is typed a character at a time; none of the partial states is
  // worth re-opening a source for, so typing one starts nothing.
  edit->setText("/tmp/partial-path");

  EXPECT_FALSE(live.wait(kLiveUpdateQuietMs));
  EXPECT_EQ(live.count(), 0);
}

// ---------------------------------------------------------------------------
// Sliders (issue #295): a parameter the stage has bounded can be swept by eye
// instead of typed, with the exact value still there to type when wanted.
// ---------------------------------------------------------------------------

TEST(StageParameterDialogTest,
     Slider_IsOfferedBesideTheEditor_WhenTheParameterIsBounded) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "bounded", "Bounded", orc::ParameterType::INT32, static_cast<int32_t>(0),
      static_cast<int32_t>(-50), static_cast<int32_t>(50)));
  descriptors.push_back(makeDescriptor("open_ended", "Open Ended",
                                       orc::ParameterType::INT32,
                                       static_cast<int32_t>(0)));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});

  auto* slider = sliderForDisplayName(dialog, "Bounded");
  ASSERT_NE(slider, nullptr);
  EXPECT_EQ(slider->minimum(), -50);
  EXPECT_EQ(slider->maximum(), 50);
  EXPECT_EQ(slider->value(), 0);
  // The exact value can still be typed: the editor is there either way.
  EXPECT_NE(qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Bounded")),
            nullptr);

  // Nothing to sweep between: a parameter with no bounds keeps its editor
  // alone.
  EXPECT_EQ(sliderForDisplayName(dialog, "Open Ended"), nullptr);
  EXPECT_NE(qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Open Ended")),
            nullptr);
}

TEST(StageParameterDialogTest, Slider_IsNotOffered_WhenTheRangeIsTooWideToAim) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  // Millions of values behind a few hundred pixels: a slider could not be
  // aimed, so this one is typed.
  descriptors.push_back(makeDescriptor(
      "bitrate", "Bitrate", orc::ParameterType::INT32, static_cast<int32_t>(0),
      static_cast<int32_t>(0), static_cast<int32_t>(100000000)));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});

  EXPECT_EQ(sliderForDisplayName(dialog, "Bitrate"), nullptr);
  EXPECT_NE(qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Bitrate")),
            nullptr);
}

TEST(StageParameterDialogTest, Slider_AndEditorCarryTheSameValueBothWays) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "level", "Level", orc::ParameterType::INT32, static_cast<int32_t>(0),
      static_cast<int32_t>(-50), static_cast<int32_t>(50)));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});

  auto* slider = sliderForDisplayName(dialog, "Level");
  auto* spin = qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Level"));
  ASSERT_NE(slider, nullptr);
  ASSERT_NE(spin, nullptr);

  // Swept: the value the dialog reports follows the slider.
  slider->setValue(25);
  EXPECT_EQ(spin->value(), 25);
  EXPECT_EQ(std::get<int32_t>(dialog.get_values().at("level")), 25);

  // Typed: the slider follows the editor.
  spin->setValue(-40);
  EXPECT_EQ(slider->value(), -40);
  EXPECT_EQ(std::get<int32_t>(dialog.get_values().at("level")), -40);
}

TEST(StageParameterDialogTest, Slider_SweepsTheWholeRangeOfADoubleParameter) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "gain", "Chroma Gain", orc::ParameterType::DOUBLE, 1.0, 0.0, 4.0));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});

  auto* slider = sliderForDisplayName(dialog, "Chroma Gain");
  auto* spin = qobject_cast<QDoubleSpinBox*>(
      widgetForDisplayName(dialog, "Chroma Gain"));
  ASSERT_NE(slider, nullptr);
  ASSERT_NE(spin, nullptr);

  // The slider counts steps across the range; the ends of its travel are the
  // ends of the range.
  slider->setValue(slider->minimum());
  EXPECT_DOUBLE_EQ(spin->value(), 0.0);
  slider->setValue(slider->maximum());
  EXPECT_DOUBLE_EQ(spin->value(), 4.0);

  slider->setValue(slider->maximum() / 2);
  EXPECT_DOUBLE_EQ(spin->value(), 2.0);
  EXPECT_DOUBLE_EQ(std::get<double>(dialog.get_values().at("gain")), 2.0);

  // And a typed value puts the slider where that value sits in the range.
  spin->setValue(3.0);
  EXPECT_EQ(slider->value(), slider->maximum() * 3 / 4);
}

TEST(StageParameterDialogTest,
     Slider_RequestsALiveUpdate_WhenSweptWithLiveUpdateOn) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "gain", "Chroma Gain", orc::ParameterType::DOUBLE, 1.0, 0.0, 4.0));

  StageParameterDialog dialog("test-stage", "Test Stage", "", descriptors, {});
  QSignalSpy live(&dialog, &StageParameterDialog::live_update_requested);

  auto* check = liveUpdateCheck(dialog);
  ASSERT_NE(check, nullptr);
  check->setChecked(true);

  auto* slider = sliderForDisplayName(dialog, "Chroma Gain");
  ASSERT_NE(slider, nullptr);
  // A drag arrives as a run of positions; the preview is rendered once, for
  // where the sweep came to rest.
  for (int position = 1; position <= 10; ++position) {
    slider->setValue(slider->maximum() * position / 20);
  }

  ASSERT_TRUE(live.wait(kLiveUpdateWaitMs));
  EXPECT_EQ(live.count(), 1);
  EXPECT_DOUBLE_EQ(std::get<double>(dialog.get_values().at("gain")), 2.0);
}

// ---------------------------------------------------------------------------
// Cross-parameter validation: the dialog must actually apply the rules, on
// the parameter names the stages really publish.
// ---------------------------------------------------------------------------

TEST(StageParameterDialogTest,
     Validation_NamesThePairThatIsTheWrongWayRound_ForVideoParameters) {
  (void)ensureApplication();

  // The Video Parameters surface as the stage publishes it (PAL geometry).
  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(
      makeDescriptor("activeVideoStart", "Active Video Start",
                     orc::ParameterType::INT32, static_cast<int32_t>(157),
                     static_cast<int32_t>(-1), static_cast<int32_t>(1134)));
  descriptors.push_back(
      makeDescriptor("activeVideoEnd", "Active Video End",
                     orc::ParameterType::INT32, static_cast<int32_t>(1105),
                     static_cast<int32_t>(-1), static_cast<int32_t>(1135)));
  descriptors.push_back(
      makeDescriptor("firstActiveFrameLine", "First Active Frame Line",
                     orc::ParameterType::INT32, static_cast<int32_t>(44),
                     static_cast<int32_t>(-1), static_cast<int32_t>(624)));
  descriptors.push_back(
      makeDescriptor("lastActiveFrameLine", "Last Active Frame Line",
                     orc::ParameterType::INT32, static_cast<int32_t>(620),
                     static_cast<int32_t>(-1), static_cast<int32_t>(625)));

  StageParameterDialog dialog("video_params", "Video Parameters", "",
                              descriptors, {});
  EXPECT_TRUE(dialog.collect_validation_errors().isEmpty());

  auto* first_line = qobject_cast<QSpinBox*>(
      widgetForDisplayName(dialog, "First Active Frame Line"));
  ASSERT_NE(first_line, nullptr);
  first_line->setValue(621);

  const QStringList errors = dialog.collect_validation_errors();
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors.front(),
            "First Active Frame Line must be less than Last Active Frame "
            "Line.");
}

TEST(StageParameterDialogTest,
     Validation_CannotBeReachedPastTheRangeTheStageDeclares) {
  (void)ensureApplication();

  // The editor cannot leave the declared range in the first place, so a
  // sample offset past the end of the line is not reachable by typing it or
  // by sweeping the slider to its end.
  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(
      makeDescriptor("activeVideoEnd", "Active Video End",
                     orc::ParameterType::INT32, static_cast<int32_t>(1105),
                     static_cast<int32_t>(-1), static_cast<int32_t>(1135)));

  StageParameterDialog dialog("video_params", "Video Parameters", "",
                              descriptors, {});

  auto* spin =
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Active Video End"));
  auto* slider = sliderForDisplayName(dialog, "Active Video End");
  ASSERT_NE(spin, nullptr);
  ASSERT_NE(slider, nullptr);

  spin->setValue(10000);
  EXPECT_EQ(spin->value(), 1135);

  slider->setValue(slider->maximum());
  EXPECT_EQ(std::get<int32_t>(dialog.get_values().at("activeVideoEnd")), 1135);
}

// ---------------------------------------------------------------------------
// Modeless editing: the window stays open while the project changes around it
// ---------------------------------------------------------------------------

TEST(StageParameterDialogTest, Title_NamesTheNodeTheDialogBelongsTo) {
  // Several editors can be open at once, so each says which node in the graph
  // it edits.
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor("threshold", "Threshold",
                                       orc::ParameterType::INT32,
                                       static_cast<int32_t>(100)));

  StageParameterDialog dialog("dropout_correct", "Dropout Correct", "",
                              descriptors, {});
  EXPECT_EQ(dialog.windowTitle(), "Dropout Correct Parameters");

  dialog.set_node_identity("First pressing", "4");
  EXPECT_EQ(dialog.windowTitle(),
            QString("Dropout Correct Parameters — First pressing (4)"));

  // A rename reaches the open window.
  dialog.set_node_identity("Second pressing", "4");
  EXPECT_EQ(dialog.windowTitle(),
            QString("Dropout Correct Parameters — Second pressing (4)"));
}

TEST(StageParameterDialogTest, Refresh_ValuesShowsWhatWasChangedElsewhere) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor("threshold", "Threshold",
                                       orc::ParameterType::INT32,
                                       static_cast<int32_t>(100)));

  StageParameterDialog dialog("dropout_correct", "Dropout Correct", "",
                              descriptors, {});
  dialog.set_node_identity("Correct", "4");
  EXPECT_FALSE(dialog.has_unapplied_edits());

  std::map<std::string, orc::ParameterValue> values;
  values["threshold"] = static_cast<int32_t>(250);
  EXPECT_TRUE(dialog.refresh_values(values));

  auto* spin =
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Threshold"));
  ASSERT_NE(spin, nullptr);
  EXPECT_EQ(spin->value(), 250);
  // The refreshed values are what the node holds, so they count as applied.
  EXPECT_FALSE(dialog.has_unapplied_edits());
  EXPECT_EQ(dialog.windowTitle(),
            QString("Dropout Correct Parameters — Correct (4)"));
}

TEST(StageParameterDialogTest, Refresh_ValuesIsDeclinedWhileEditsAreUnapplied) {
  // A refresh must never take the user's half-finished edit away from them.
  // It is refused instead, and the title says the node has moved on.
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor("threshold", "Threshold",
                                       orc::ParameterType::INT32,
                                       static_cast<int32_t>(100)));

  StageParameterDialog dialog("dropout_correct", "Dropout Correct", "",
                              descriptors, {});
  dialog.set_node_identity("Correct", "4");

  auto* spin =
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Threshold"));
  ASSERT_NE(spin, nullptr);
  spin->setValue(175);
  EXPECT_TRUE(dialog.has_unapplied_edits());

  std::map<std::string, orc::ParameterValue> values;
  values["threshold"] = static_cast<int32_t>(250);
  EXPECT_FALSE(dialog.refresh_values(values));

  EXPECT_EQ(spin->value(), 175);
  EXPECT_EQ(dialog.windowTitle(),
            QString("Dropout Correct Parameters — Correct (4) *"));
}

TEST(StageParameterDialogTest, Refresh_ContextNarrowsAnOpenDialogsChoices) {
  // The graph can be rewired under an open editor: an audio stage moved to a
  // source with different channel pairs has to offer the new ones.
  (void)ensureApplication();

  const char sep = StageParameterDialog::kComboValueLabelSeparator;
  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "channel_pair", "Channel Pair", orc::ParameterType::STRING,
      std::string("0"), std::nullopt, std::nullopt,
      {orc::gui::audioChannelPairComboEntry(0, "Analogue", sep)}));

  StageParameterDialog dialog("audio_align", "Audio Align", "Aligns audio.",
                              descriptors, {});

  auto* combo =
      qobject_cast<QComboBox*>(widgetForDisplayName(dialog, "Channel Pair"));
  ASSERT_NE(combo, nullptr);
  EXPECT_EQ(combo->count(), 1);

  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "audio_align";
  inputs.descriptors = descriptors;
  inputs.descriptors.front().constraints.allowed_strings = {"0", "1", "2"};
  inputs.stage_description = "Aligns audio.";
  inputs.input_audio_pair_names =
      std::vector<std::string>{"Analogue", "EFM digital audio"};
  dialog.refresh_context(orc::gui::buildStageParameterContext(inputs));

  // The form was rebuilt, so the combo has to be found again.
  combo =
      qobject_cast<QComboBox*>(widgetForDisplayName(dialog, "Channel Pair"));
  ASSERT_NE(combo, nullptr);
  EXPECT_EQ(combo->count(), 2);
  EXPECT_EQ(combo->itemText(1).toStdString(), "1: EFM digital audio");
}

TEST(StageParameterDialogTest, Refresh_ContextKeepsEditsInProgress) {
  // Rewiring the graph is no reason to throw away what the user has typed.
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor("threshold", "Threshold",
                                       orc::ParameterType::INT32,
                                       static_cast<int32_t>(100)));

  StageParameterDialog dialog("dropout_correct", "Dropout Correct", "",
                              descriptors, {});

  auto* spin =
      qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Threshold"));
  ASSERT_NE(spin, nullptr);
  spin->setValue(175);

  orc::gui::StageParameterContext context;
  context.descriptors = descriptors;
  context.current_values["threshold"] = static_cast<int32_t>(250);
  dialog.refresh_context(context);

  spin = qobject_cast<QSpinBox*>(widgetForDisplayName(dialog, "Threshold"));
  ASSERT_NE(spin, nullptr);
  EXPECT_EQ(spin->value(), 175);
  EXPECT_TRUE(dialog.has_unapplied_edits());
}

TEST(StageParameterDialogTest,
     Refresh_ContextDoesNotCountItsOwnChoiceAsAUsersEdit) {
  // Narrowing a dropdown can move a value the user never chose: the selection
  // they had may no longer be on offer. That is the refresh's doing, not an
  // edit of theirs, so the editor must not go on to report unapplied edits
  // and decline every refresh that follows.
  (void)ensureApplication();

  const char sep = StageParameterDialog::kComboValueLabelSeparator;
  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor(
      "channel_pair", "Channel Pair", orc::ParameterType::STRING,
      std::string("0"), std::nullopt, std::nullopt,
      {orc::gui::audioChannelPairComboEntry(0, "Analogue", sep),
       orc::gui::audioChannelPairComboEntry(1, "EFM digital audio", sep)}));

  std::map<std::string, orc::ParameterValue> current_values;
  current_values["channel_pair"] = std::string("1");

  StageParameterDialog dialog("audio_align", "Audio Align", "", descriptors,
                              current_values);
  EXPECT_FALSE(dialog.has_unapplied_edits());

  // The input now carries one pair, so the pair that was selected is gone.
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "audio_align";
  inputs.descriptors = descriptors;
  inputs.descriptors.front().constraints.allowed_strings = {"0", "1"};
  inputs.current_values = current_values;
  inputs.input_audio_pair_names = std::vector<std::string>{"Analogue"};
  dialog.refresh_context(orc::gui::buildStageParameterContext(inputs));

  EXPECT_FALSE(dialog.has_unapplied_edits());

  // And a later refresh is still accepted rather than declined.
  std::map<std::string, orc::ParameterValue> values;
  values["channel_pair"] = std::string("0");
  EXPECT_TRUE(dialog.refresh_values(values));
  EXPECT_FALSE(dialog.windowTitle().endsWith("*"));
}

TEST(StageParameterDialogTest, Refresh_ContextUpdatesTheHeaderAndResetButton) {
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor("blackLevel", "Black Level",
                                       orc::ParameterType::INT32,
                                       static_cast<int32_t>(-1)));

  StageParameterDialog dialog("video_params", "Video Parameters",
                              "Overrides source video parameters.", descriptors,
                              {});

  auto* description = dialog.findChild<QLabel*>("stage_description_label");
  ASSERT_NE(description, nullptr);
  EXPECT_EQ(description->text(), "Overrides source video parameters.");

  orc::gui::StageParameterContext context;
  context.descriptors = descriptors;
  context.stage_description =
      "Overrides source video parameters. Now with a "
      "source behind it.";
  context.current_values["blackLevel"] = static_cast<int32_t>(16384);
  context.reset_values = std::map<std::string, orc::ParameterValue>{
      {"blackLevel", static_cast<int32_t>(16384)}};
  dialog.refresh_context(context);

  EXPECT_EQ(description->text(),
            "Overrides source video parameters. Now with a source behind it.");
  EXPECT_EQ(std::get<int32_t>(dialog.get_values().at("blackLevel")), 16384);
}

TEST(StageParameterDialogTest, LiveUpdate_RenamesCancelToClose) {
  // With live update ticked the values are applied as they are made, so a
  // button offering to cancel them would be promising what it cannot do.
  (void)ensureApplication();

  std::vector<orc::ParameterDescriptor> descriptors;
  descriptors.push_back(makeDescriptor("threshold", "Threshold",
                                       orc::ParameterType::INT32,
                                       static_cast<int32_t>(100)));

  StageParameterDialog dialog("dropout_correct", "Dropout Correct", "",
                              descriptors, {});

  auto* live_update = dialog.findChild<QCheckBox*>("live_update_check");
  auto* buttons = dialog.findChild<QDialogButtonBox*>();
  ASSERT_NE(live_update, nullptr);
  ASSERT_NE(buttons, nullptr);
  auto* cancel = buttons->button(QDialogButtonBox::Cancel);
  ASSERT_NE(cancel, nullptr);

  EXPECT_EQ(cancel->text(), "Cancel");
  live_update->setChecked(true);
  EXPECT_EQ(cancel->text(), "Close");
  live_update->setChecked(false);
  EXPECT_EQ(cancel->text(), "Cancel");
}

}  // namespace gui_unit_test
