/*
 * File:        vectorscope_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Offscreen tests for the vectorscope dialog's acquisition
 *              controls and their round-trip into a preview coordinate
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview/vectorscope_dialog.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QGroupBox>
#include <QLabel>
#include <QRadioButton>
#include <QSignalSpy>

namespace gui_unit_test {
namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-vectorscope-dialog-test";
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

// The dialog owns its controls privately; drive them the way a user would, by
// name, so the test exercises the same wiring the UI does.
QRadioButton* findRadio(QWidget& parent, const QString& text) {
  for (QRadioButton* button : parent.findChildren<QRadioButton*>()) {
    if (button->text() == text) return button;
  }
  return nullptr;
}

QCheckBox* findCheckBox(QWidget& parent, const QString& text) {
  for (QCheckBox* box : parent.findChildren<QCheckBox*>()) {
    if (box->text() == text) return box;
  }
  return nullptr;
}

QSpinBox* startLineSpinBox(QWidget& parent) {
  return parent.findChild<QSpinBox*>("vectorscope_start_line");
}

QSpinBox* endLineSpinBox(QWidget& parent) {
  return parent.findChild<QSpinBox*>("vectorscope_end_line");
}

QGroupBox* findGroup(QWidget& parent, const QString& title) {
  for (QGroupBox* box : parent.findChildren<QGroupBox*>()) {
    if (box->title() == title) return box;
  }
  return nullptr;
}

// Select an explicit line range the way the field view asks for one.
void selectLineRange(VectorscopeDialog& dialog, int start, int end) {
  QRadioButton* selected = findRadio(dialog, "Selected line(s)");
  ASSERT_NE(selected, nullptr);
  selected->click();

  QSpinBox* start_box = startLineSpinBox(dialog);
  QSpinBox* end_box = endLineSpinBox(dialog);
  ASSERT_NE(start_box, nullptr);
  ASSERT_NE(end_box, nullptr);

  start_box->setValue(start);
  end_box->setValue(end);
  QCoreApplication::processEvents();
}

// A frame of composite samples on the named interlaced frame lines.
orc::VectorscopeData compositeFrame(orc::VideoSystem system,
                                    const std::vector<uint16_t>& lines,
                                    uint32_t first_line, uint32_t last_line) {
  orc::VectorscopeData data;
  data.system = system;
  data.cvbs_white = 844;
  data.cvbs_blanking = 256;
  data.acquisition_mode = orc::VectorscopeAcquisitionMode::CompositeCarrier;
  data.sample_window = orc::VectorscopeSampleWindow::WholeLine;
  data.first_line = first_line;
  data.last_line = last_line;
  data.sample_stride = 1;
  data.field_number = 0;
  data.width = 2;
  data.height = static_cast<uint32_t>(lines.size());

  for (uint16_t line : lines) {
    const uint8_t field_id = static_cast<uint8_t>(line & 1U);
    data.samples.emplace_back(-5000.0, 5000.0, field_id,
                              orc::VectorscopeSampleClass::Picture,
                              orc::VectorscopeLinePhase::VPositive, line);
    data.samples.emplace_back(-4000.0, 4000.0, field_id,
                              orc::VectorscopeSampleClass::Picture,
                              orc::VectorscopeLinePhase::VPositive, line);
  }

  return data;
}

}  // namespace

TEST(VectorscopeDialogTest, CanShowAndClose) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();
  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();
  EXPECT_FALSE(dialog.isVisible());
}

TEST(VectorscopeDialogTest, DefaultsToTheDecodedAcquisition) {
  (void)ensureApplication();

  VectorscopeDialog dialog;

  // A colour-domain stage is the decoded plot's home, and that is the state
  // the dialog starts in until told otherwise.
  EXPECT_EQ(dialog.acquisitionMode(),
            orc::VectorscopeAcquisitionMode::DecodedComponent);
  EXPECT_EQ(dialog.sampleWindow(), orc::VectorscopeSampleWindow::WholeLine);
  EXPECT_TRUE(dialog.isActiveAreaOnly());
  EXPECT_FALSE(dialog.isLineRangeExplicit());
  EXPECT_EQ(dialog.firstLine(), 0u);
  EXPECT_EQ(dialog.lastLine(), 0u);
}

TEST(VectorscopeDialogTest, FieldViewExtents_AreMutuallyExclusive) {
  (void)ensureApplication();

  VectorscopeDialog dialog;

  QRadioButton* active_field = findRadio(dialog, "Active field");
  QRadioButton* whole_field = findRadio(dialog, "Whole field");
  QRadioButton* selected_lines = findRadio(dialog, "Selected line(s)");
  ASSERT_NE(active_field, nullptr);
  ASSERT_NE(whole_field, nullptr);
  ASSERT_NE(selected_lines, nullptr);

  // The three extents describe the same thing — how much of the frame is
  // acquired — so naming one must unname the others.
  EXPECT_TRUE(active_field->isChecked());

  whole_field->click();
  QCoreApplication::processEvents();
  EXPECT_FALSE(active_field->isChecked());
  EXPECT_FALSE(selected_lines->isChecked());
  EXPECT_FALSE(dialog.isActiveAreaOnly());
  EXPECT_FALSE(dialog.isLineRangeExplicit());

  selected_lines->click();
  QCoreApplication::processEvents();
  EXPECT_FALSE(active_field->isChecked());
  EXPECT_FALSE(whole_field->isChecked());
  EXPECT_FALSE(dialog.isActiveAreaOnly());
  EXPECT_TRUE(dialog.isLineRangeExplicit());
}

TEST(VectorscopeDialogTest, LineSpinBoxes_OnlyRespondToASelectedRange) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  QSpinBox* start_box = startLineSpinBox(dialog);
  QSpinBox* end_box = endLineSpinBox(dialog);
  QCheckBox* end_enabled = findCheckBox(dialog, "End line:");
  ASSERT_NE(start_box, nullptr);
  ASSERT_NE(end_box, nullptr);
  ASSERT_NE(end_enabled, nullptr);

  // Nothing to name while the whole field, or its active picture, is plotted.
  EXPECT_FALSE(start_box->isEnabled());
  EXPECT_FALSE(end_box->isEnabled());
  EXPECT_FALSE(end_enabled->isEnabled());

  findRadio(dialog, "Selected line(s)")->click();
  QCoreApplication::processEvents();
  EXPECT_TRUE(start_box->isEnabled());
  EXPECT_TRUE(end_box->isEnabled());
  EXPECT_TRUE(end_enabled->isEnabled());

  // An unticked end line leaves the start line standing on its own, so the
  // second spin box has nothing to say.
  end_enabled->setChecked(false);
  QCoreApplication::processEvents();
  EXPECT_TRUE(start_box->isEnabled());
  EXPECT_FALSE(end_box->isEnabled());

  dialog.close();
  QCoreApplication::processEvents();
}

TEST(VectorscopeDialogTest, SamplingControls_RoundTripIntoACoordinate) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  QCoreApplication::processEvents();

  QRadioButton* burst_only = findRadio(dialog, "Burst only");
  ASSERT_NE(burst_only, nullptr);
  burst_only->click();
  QCoreApplication::processEvents();

  EXPECT_EQ(dialog.sampleWindow(), orc::VectorscopeSampleWindow::BurstOnly);

  // The acquisition itself is not part of the request: it follows the data
  // type the stage produces, which the registry resolves.
  orc::PreviewCoordinate coordinate;
  dialog.applyAcquisitionTo(coordinate);
  EXPECT_EQ(coordinate.vectorscope_window,
            orc::VectorscopeSampleWindow::BurstOnly);
  // The whole active field is still plotted, so the range is left open-ended.
  EXPECT_TRUE(coordinate.vectorscope_active_area_only);
  EXPECT_EQ(coordinate.vectorscope_first_line, 0u);
  EXPECT_EQ(coordinate.vectorscope_last_line, 0u);
}

TEST(VectorscopeDialogTest, FieldView_IsOfferedForBothAcquisitions) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  QGroupBox* line_view = findGroup(dialog, "Line View");
  QGroupBox* field_view = findGroup(dialog, "Field View");
  QGroupBox* measurements = findGroup(dialog, "Measurements");
  ASSERT_NE(line_view, nullptr);
  ASSERT_NE(field_view, nullptr);
  ASSERT_NE(measurements, nullptr);

  // The field view governs which lines of the frame either scope plots, so it
  // is offered on both — that is what lets one be pointed at the same lines as
  // the other.  The line view picks a region of the line and only a composite
  // acquisition has one; the burst readouts likewise.
  EXPECT_TRUE(field_view->isVisible());
  EXPECT_FALSE(line_view->isVisible());
  EXPECT_FALSE(measurements->isVisible());

  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  QCoreApplication::processEvents();

  EXPECT_TRUE(field_view->isVisible());
  EXPECT_TRUE(line_view->isVisible());
  EXPECT_TRUE(measurements->isVisible());

  dialog.close();
  QCoreApplication::processEvents();
}

TEST(VectorscopeDialogTest, DecodedPlot_CarriesTheLineRangeIntoACoordinate) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  ASSERT_EQ(dialog.acquisitionMode(),
            orc::VectorscopeAcquisitionMode::DecodedComponent);

  selectLineRange(dialog, 41, 60);

  orc::PreviewCoordinate coordinate;
  dialog.applyAcquisitionTo(coordinate);
  EXPECT_EQ(coordinate.vectorscope_first_line, 40u);
  EXPECT_EQ(coordinate.vectorscope_last_line, 59u);
  // A named range is taken literally: intersecting it with the active picture
  // would silently drop the lines outside it.
  EXPECT_FALSE(coordinate.vectorscope_active_area_only);
}

TEST(VectorscopeDialogTest, LineRange_IsReportedZeroBased) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);

  selectLineRange(dialog, 101, 140);

  // Line numbers are 1-based in the UI and 0-based in the contract.
  EXPECT_EQ(dialog.firstLine(), 100u);
  EXPECT_EQ(dialog.lastLine(), 139u);
}

TEST(VectorscopeDialogTest, LineRange_IsNormalisedWhenInverted) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);

  selectLineRange(dialog, 200, 150);

  EXPECT_EQ(dialog.firstLine(), 149u);
  EXPECT_EQ(dialog.lastLine(), 199u);
}

TEST(VectorscopeDialogTest, UntickedEndLine_SelectsTheStartLineAlone) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);

  selectLineRange(dialog, 120, 200);

  QCheckBox* end_enabled = findCheckBox(dialog, "End line:");
  ASSERT_NE(end_enabled, nullptr);
  end_enabled->setChecked(false);
  QCoreApplication::processEvents();

  EXPECT_EQ(dialog.firstLine(), 119u);
  EXPECT_EQ(dialog.lastLine(), 119u);
}

TEST(VectorscopeDialogTest, SingleFieldPlot_OnlyOffersThatFieldsLines) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  selectLineRange(dialog, 100, 200);

  QSpinBox* start_box = startLineSpinBox(dialog);
  QSpinBox* end_box = endLineSpinBox(dialog);
  ASSERT_NE(start_box, nullptr);
  ASSERT_NE(end_box, nullptr);

  // Consecutive frame lines alternate fields, so a first-field plot can only
  // be pointed at the odd-numbered ones.  A value the parity cannot express
  // moves to the line below it.
  findRadio(dialog, "First Field Only")->click();
  QCoreApplication::processEvents();
  EXPECT_EQ(start_box->minimum(), 1);
  EXPECT_EQ(start_box->singleStep(), 2);
  EXPECT_EQ(start_box->value() % 2, 1);
  EXPECT_EQ(start_box->value(), 99);
  EXPECT_EQ(end_box->value(), 199);

  findRadio(dialog, "Second Field Only")->click();
  QCoreApplication::processEvents();
  EXPECT_EQ(start_box->minimum(), 2);
  EXPECT_EQ(start_box->singleStep(), 2);
  EXPECT_EQ(start_box->value() % 2, 0);
  EXPECT_EQ(start_box->value(), 98);
  EXPECT_EQ(end_box->value(), 198);

  findRadio(dialog, "All Fields")->click();
  QCoreApplication::processEvents();
  EXPECT_EQ(start_box->minimum(), 1);
  EXPECT_EQ(start_box->singleStep(), 1);
}

TEST(VectorscopeDialogTest, LineLimits_FollowTheSystemBeingPlotted) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  QSpinBox* start_box = startLineSpinBox(dialog);
  ASSERT_NE(start_box, nullptr);

  // Until a frame has been plotted the largest supported frame is assumed;
  // SMPTE 170M-2004 §11.3 puts an NTSC frame at 525 lines.
  EXPECT_EQ(start_box->maximum(), 625);

  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  dialog.updateVectorscope(
      compositeFrame(orc::VideoSystem::NTSC, {100, 101}, 0, 524));
  QCoreApplication::processEvents();

  EXPECT_EQ(start_box->maximum(), 525);

  dialog.close();
  QCoreApplication::processEvents();
}

TEST(VectorscopeDialogTest, SingleLineSelection_PlotsThatLineAlone) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.show();
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  QCoreApplication::processEvents();

  selectLineRange(dialog, 1, 1);
  QCheckBox* end_enabled = findCheckBox(dialog, "End line:");
  ASSERT_NE(end_enabled, nullptr);
  end_enabled->setChecked(false);
  QCoreApplication::processEvents();

  // Frame line 1 alone is the one selection the contract cannot state — a
  // zero last line means "to the last line of the frame" — so the acquisition
  // comes back holding the whole frame and the dialog narrows it.
  EXPECT_EQ(dialog.firstLine(), 0u);
  EXPECT_EQ(dialog.lastLine(), 0u);

  dialog.updateVectorscope(
      compositeFrame(orc::VideoSystem::PAL, {0, 1, 2, 3}, 0, 624));
  QCoreApplication::processEvents();

  QLabel* info = dialog.findChild<QLabel*>("vectorscope_info");
  ASSERT_NE(info, nullptr);
  EXPECT_TRUE(info->text().contains("lines 1-1")) << info->text().toStdString();
  EXPECT_TRUE(info->text().contains("2 samples")) << info->text().toStdString();

  dialog.close();
  QCoreApplication::processEvents();
}

TEST(VectorscopeDialogTest, ChangingAcquisitionRequestsFreshData) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  QSignalSpy spy(&dialog, &VectorscopeDialog::dataRefreshRequested);

  // Selecting a signal-domain stage switches the acquisition.  The two are
  // different data sets, so it has to re-ask rather than re-plot what is
  // already on screen.
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  QCoreApplication::processEvents();
  EXPECT_EQ(dialog.acquisitionMode(),
            orc::VectorscopeAcquisitionMode::CompositeCarrier);
  EXPECT_EQ(spy.count(), 1);

  // Re-stating the same acquisition is not a change and must not re-acquire.
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  QCoreApplication::processEvents();
  EXPECT_EQ(spy.count(), 1);

  QRadioButton* active_line = findRadio(dialog, "Active line");
  ASSERT_NE(active_line, nullptr);
  active_line->click();
  QCoreApplication::processEvents();
  EXPECT_EQ(spy.count(), 2);
}

TEST(VectorscopeDialogTest, DecodedPlot_ReAcquiresWhenTheLineRangeMoves) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  ASSERT_EQ(dialog.acquisitionMode(),
            orc::VectorscopeAcquisitionMode::DecodedComponent);

  QSignalSpy spy(&dialog, &VectorscopeDialog::dataRefreshRequested);

  // The field view narrows what either acquisition takes off the frame, so
  // both have to be re-asked for it.
  findRadio(dialog, "Selected line(s)")->click();
  QCoreApplication::processEvents();
  EXPECT_EQ(spy.count(), 1);

  startLineSpinBox(dialog)->setValue(60);
  QCoreApplication::processEvents();
  EXPECT_EQ(spy.count(), 2);
}

TEST(VectorscopeDialogTest, RendersACompositeAcquisitionWithoutCrashing) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  orc::VectorscopeData data;
  data.system = orc::VideoSystem::PAL;
  data.cvbs_white = 844;
  data.cvbs_blanking = 256;
  data.acquisition_mode = orc::VectorscopeAcquisitionMode::CompositeCarrier;
  data.sample_window = orc::VectorscopeSampleWindow::WholeLine;
  data.first_line = 0;
  data.last_line = 624;
  data.sample_stride = 2;
  data.field_number = 3;
  data.measurements.valid = true;
  data.measurements.burst_amplitude_ire = 21.4;
  data.measurements.burst_amplitude_percent = 99.9;
  data.measurements.burst_phase_jitter_degrees = 0.4;
  data.measurements.burst_line_count = 576;
  data.measurements.chroma_to_burst_ratio = 1.5;

  data.samples.emplace_back(-5000.0, 5000.0, 0,
                            orc::VectorscopeSampleClass::Burst,
                            orc::VectorscopeLinePhase::VPositive, 100);
  data.samples.emplace_back(-5000.0, -5000.0, 0,
                            orc::VectorscopeSampleClass::Burst,
                            orc::VectorscopeLinePhase::VNegative, 101);
  data.samples.emplace_back(-3600.0, 15100.0, 0,
                            orc::VectorscopeSampleClass::Picture,
                            orc::VectorscopeLinePhase::VPositive, 100);

  dialog.updateVectorscope(data);
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();
}

// A composite acquisition of SECAM says why it has nothing to plot, and does
// not plot data it is handed. The decoded acquisition plots as usual.
TEST(VectorscopeDialogTest, SecamComposite_SaysNotApplicable) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  auto* info = dialog.findChild<QLabel*>("vectorscope_info");
  ASSERT_NE(info, nullptr);

  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  QSignalSpy refresh(&dialog, &VectorscopeDialog::dataRefreshRequested);
  dialog.setVideoSystem(orc::VideoSystem::SECAM);
  EXPECT_EQ(refresh.count(), 1);
  ASSERT_NE(dialog.notApplicableReason(), nullptr);
  EXPECT_TRUE(info->text().startsWith("Not applicable to SECAM"))
      << info->text().toStdString();

  orc::VectorscopeData data;
  data.system = orc::VideoSystem::SECAM;
  data.acquisition_mode = orc::VectorscopeAcquisitionMode::CompositeCarrier;
  orc::UVSample sample{};
  sample.u = 1000.0;
  sample.v = 2000.0;
  data.samples.push_back(sample);
  dialog.updateVectorscope(data);
  EXPECT_TRUE(info->text().startsWith("Not applicable to SECAM"))
      << info->text().toStdString();

  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::DecodedComponent);
  EXPECT_EQ(dialog.notApplicableReason(), nullptr);
  EXPECT_EQ(info->text(), "No data");

  dialog.setVideoSystem(orc::VideoSystem::PAL);
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  EXPECT_EQ(dialog.notApplicableReason(), nullptr);

  dialog.close();
  QCoreApplication::processEvents();
}

}  // namespace gui_unit_test
