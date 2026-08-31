/*
 * File:        catalogue_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 offscreen tests for the generic catalogue browser
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QRect>
#include <QSize>
#include <QTableWidget>
#include <QToolButton>
#include <cmath>

#include "catalogue_flash_clock.h"
#include "cataloguecellgridwidget.h"
#include "cataloguedialog.h"
#include "cataloguedisplaylistwidget.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-catalogue-dialog-test";
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

// A small character-cell payload: |text| written across the top row of a
// |rows| x |columns| grid in white on black.
orc::CatalogueCellGrid makeGrid(const std::string& text, int rows = 4,
                                int columns = 8) {
  orc::CatalogueCellGrid grid;
  grid.rows = rows;
  grid.columns = columns;
  grid.cell_aspect_width = 12;
  grid.cell_aspect_height = 20;
  grid.palette = {orc::CatalogueColour{0, 0, 0},
                  orc::CatalogueColour{255, 255, 255}};
  grid.cells.resize(static_cast<size_t>(rows) * static_cast<size_t>(columns));
  for (auto& cell : grid.cells) {
    cell.foreground = 1;
    cell.background = 0;
  }
  for (size_t i = 0; i < text.size() && i < static_cast<size_t>(columns); ++i) {
    grid.cells[i].character = static_cast<char32_t>(text[i]);
  }
  grid.row_status.resize(static_cast<size_t>(rows));
  return grid;
}

// A catalogue of |count| pages, each carrying one variant, in the shape the
// teletext sink produces: a find box, a variant stepper and a damage toggle.
orc::CatalogueDataset makePagedCatalogue(int count) {
  orc::CatalogueDataset data;
  data.schema.columns = {
      orc::CatalogueColumn{"page", "Page", false},
      orc::CatalogueColumn{"seen", "Seen", true},
  };
  data.schema.item_noun = "Page";
  data.schema.variant_noun = "Sub-page";
  data.schema.find_label = "Page:";
  data.schema.find_placeholder = "e.g. 100";
  data.schema.highlight_label = "Show data errors";
  data.schema.empty_message = "No pages were recovered";

  for (int i = 0; i < count; ++i) {
    const std::string id = "10" + std::to_string(i);

    orc::CatalogueItem page;
    page.id = id;
    page.find_key = id;
    page.values = {id, std::to_string(i + 1)};
    data.items.push_back(std::move(page));
    data.payloads.emplace_back();  // the page itself draws nothing

    orc::CatalogueItem variant;
    variant.id = id + "/0000";
    variant.parent_id = id;
    variant.variant_label = "0000";
    data.items.push_back(std::move(variant));

    orc::CataloguePayload payload;
    payload.kind = orc::CataloguePayload::Kind::kCellGrid;
    payload.grid = makeGrid(id);
    payload.headline =
        "Page " + id + " seen " + std::to_string(i + 1) + " times";
    payload.condition = "Complete (1 row)";
    data.payloads.push_back(std::move(payload));
  }

  data.summary.headline = "12 packets recovered";
  return data;
}

// A flat catalogue with no variants, in the shape the NABTS sink produces.
orc::CatalogueDataset makeFlatCatalogue() {
  orc::CatalogueDataset data;
  data.schema.columns = {orc::CatalogueColumn{"address", "Address", false}};
  data.schema.item_noun = "Record";

  orc::CatalogueItem record;
  record.id = "000/1A4 v0";
  record.find_key = record.id;
  record.values = {record.id};
  data.items.push_back(record);

  orc::CataloguePayload drawn;
  drawn.kind = orc::CataloguePayload::Kind::kDisplayList;
  drawn.display_list.aspect_height = 0.78125;
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.origin = orc::CataloguePoint{0.1, 0.1};
  op.size = orc::CatalogueSize{0.5, 0.5};
  op.filled = true;
  op.colour = orc::CatalogueColour{255, 255, 255};
  drawn.display_list.ops.push_back(op);
  drawn.companion_text = "HELLO NABTS";
  drawn.headline = "Record 000/1A4 seen 3 times";
  data.payloads.push_back(std::move(drawn));

  orc::CatalogueItem listing;
  listing.id = "000/1A5 v0";
  listing.find_key = listing.id;
  listing.values = {listing.id};
  data.items.push_back(listing);

  orc::CataloguePayload text;
  text.kind = orc::CataloguePayload::Kind::kText;
  text.document.text = "2/0  [control]  reset";
  text.headline = "Record 000/1A5 seen 1 time";
  data.payloads.push_back(std::move(text));

  orc::CatalogueItem cues;
  cues.id = "captions";
  cues.values = {"Caption track"};
  data.items.push_back(cues);

  orc::CataloguePayload table;
  table.kind = orc::CataloguePayload::Kind::kTable;
  table.table.columns = {orc::CatalogueColumn{"frames", "Frames", true},
                         orc::CatalogueColumn{"text", "Caption", false}};
  table.table.rows = {{"1-30", "FIRST CUE"}, {"31-60", "SECOND CUE"}};
  table.headline = "2 captions on A00/000";
  data.payloads.push_back(std::move(table));

  data.summary.notices.push_back("2 captions on A00/000");
  return data;
}

QImage renderWidget(QWidget& widget, int width, int height) {
  widget.resize(width, height);
  QImage image(width, height, QImage::Format_RGB32);
  image.fill(Qt::black);
  widget.render(&image);
  return image;
}

// Whole pixels per character rectangle, scaled up far enough that the
// widget's own minimum size cannot clamp the resize. At the grid's exact
// aspect, so the aspect lock leaves no letterbox and the page rect is the
// whole image.
int gridScale(const orc::CatalogueCellGrid& grid) {
  int scale = 1;
  while (grid.columns * grid.cell_aspect_width * scale < 512 ||
         grid.rows * grid.cell_aspect_height * scale < 512) {
    ++scale;
  }
  return scale;
}

QImage renderGrid(const orc::CatalogueCellGrid& grid, bool flash_lit = true) {
  CatalogueCellGridWidget widget;
  widget.setGrid(grid);
  // The phase is set rather than waited for: a wall-clock cycle would make
  // this test slow and flaky, and the widget paints whichever phase it holds.
  widget.setFlashLit(flash_lit);
  const int scale = gridScale(grid);
  return renderWidget(widget, grid.columns * grid.cell_aspect_width * scale,
                      grid.rows * grid.cell_aspect_height * scale);
}

/// Whether anything at all was drawn inside the character rectangle at
/// (row, column), against the black screen the grid sits on.
bool cellHasInk(const QImage& image, const orc::CatalogueCellGrid& grid,
                int row, int column) {
  const int scale = gridScale(grid);
  const int cell_w = grid.cell_aspect_width * scale;
  const int cell_h = grid.cell_aspect_height * scale;
  for (int y = row * cell_h; y < (row + 1) * cell_h; ++y) {
    for (int x = column * cell_w; x < (column + 1) * cell_w; ++x) {
      if (image.pixelColor(x, y) != QColor(Qt::black)) {
        return true;
      }
    }
  }
  return false;
}

/// Colour at the centre of the character rectangle at (row, column).
QColor cellCentre(const QImage& image, const orc::CatalogueCellGrid& grid,
                  int row, int column) {
  const int scale = gridScale(grid);
  const int cell_w = grid.cell_aspect_width * scale;
  const int cell_h = grid.cell_aspect_height * scale;
  return image.pixelColor(column * cell_w + cell_w / 2,
                          row * cell_h + cell_h / 2);
}

}  // namespace

TEST(CatalogueDialogTest, CanShowAndClose) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  QApplication::processEvents();
  EXPECT_TRUE(dialog.isVisible());
  dialog.close();
  QApplication::processEvents();
  EXPECT_FALSE(dialog.isVisible());
}

// A dataset whose payload vector does not line up with its items is a plugin
// bug; the dialogue says so rather than indexing past the end.
TEST(CatalogueDialogTest, InconsistentDatasetIsRefused) {
  ensureApplication();
  CatalogueDialog dialog;

  orc::CatalogueDataset broken;
  broken.items.emplace_back();
  dialog.setCatalogue(broken);

  EXPECT_TRUE(dialog.listedItems().empty());
  EXPECT_TRUE(dialog.headlineText().contains("inconsistent"));
}

// A trigger that fails leaves the viewer open on a pending state it must be
// told to abandon, or it reads as a decode still running.
TEST(CatalogueDialogTest, ErrorReplacesThePendingState) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.showPending();

  dialog.showError("The report file needs an output file");

  EXPECT_TRUE(dialog.listedItems().empty());
  EXPECT_EQ(dialog.headlineText(), "The report file needs an output file");
}

// A failure after a good run replaces that run rather than leaving stale pages
// on display under an error message.
TEST(CatalogueDialogTest, ErrorClearsAPreviousCatalogue) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(3));
  ASSERT_EQ(dialog.listedItems().size(), 3u);

  dialog.showError("Trigger failed");

  EXPECT_TRUE(dialog.listedItems().empty());
  EXPECT_EQ(dialog.currentItemIndex(), -1);
  EXPECT_EQ(dialog.headlineText(), "Trigger failed");
}

TEST(CatalogueDialogTest, ItemsAreTabulatedInDatasetOrder) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(3));

  // Only top-level items are listed; the variants live under the payload.
  const auto listed = dialog.listedItems();
  ASSERT_EQ(listed.size(), 3u);
  EXPECT_EQ(listed[0], "100");
  EXPECT_EQ(listed[1], "101");
  EXPECT_EQ(listed[2], "102");

  EXPECT_EQ(dialog.listedValue("101", 1), "2");
}

// Selecting a parent shows its first variant, which is what a page with
// sub-pages means.
TEST(CatalogueDialogTest, SelectingAnItemShowsItsPayload) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(3));

  dialog.selectItem(1);
  EXPECT_EQ(dialog.currentItemIndex(), 1);
  ASSERT_NE(dialog.currentGrid(), nullptr);
  EXPECT_EQ(dialog.headlineText(), "Page 101 seen 2 times");
  EXPECT_EQ(dialog.conditionText(), "Complete (1 row)");
}

TEST(CatalogueDialogTest, FindBoxSelectsByKey) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(3));

  dialog.setFindText("102");
  EXPECT_EQ(dialog.currentItemIndex(), 2);
  EXPECT_EQ(dialog.headlineText(), "Page 102 seen 3 times");

  // Case and surrounding space are the host's business, not the plugin's.
  dialog.setFindText("  100  ");
  EXPECT_EQ(dialog.currentItemIndex(), 0);

  dialog.close();
}

// A key the catalogue does not carry is not an error — the service simply did
// not send it — so the dialogue says which one was asked for.
TEST(CatalogueDialogTest, FindBoxReportsAKeyThatWasNotCarried) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(2));

  dialog.setFindText("777");
  EXPECT_EQ(dialog.currentItemIndex(), -1);
  EXPECT_EQ(dialog.currentGrid(), nullptr);
  EXPECT_TRUE(dialog.headlineText().contains("777"));

  dialog.close();
}

TEST(CatalogueDialogTest, ItemNavigationWrapsAtBothEnds) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(3));

  dialog.selectItem(0);
  dialog.showPreviousItem();
  EXPECT_EQ(dialog.currentItemIndex(), 2);
  dialog.showNextItem();
  EXPECT_EQ(dialog.currentItemIndex(), 0);
}

// --- Variants -------------------------------------------------------------

namespace {

// One page carrying |count| variants, as a multi-page set does.
orc::CatalogueDataset makeVariantCatalogue(int count) {
  orc::CatalogueDataset data;
  data.schema.columns = {orc::CatalogueColumn{"page", "Page", false}};
  data.schema.item_noun = "Page";
  data.schema.variant_noun = "Sub-page";

  orc::CatalogueItem page;
  page.id = "100";
  page.find_key = "100";
  page.values = {"100"};
  data.items.push_back(std::move(page));
  data.payloads.emplace_back();

  for (int i = 0; i < count; ++i) {
    const std::string label = "000" + std::to_string(i + 1);
    orc::CatalogueItem variant;
    variant.id = "100/" + label;
    variant.parent_id = "100";
    variant.variant_label = label;
    data.items.push_back(std::move(variant));

    orc::CataloguePayload payload;
    payload.kind = orc::CataloguePayload::Kind::kCellGrid;
    payload.grid = makeGrid(label);
    payload.headline = "Page 100 sub-page " + label;
    data.payloads.push_back(std::move(payload));
  }
  return data;
}

}  // namespace

TEST(CatalogueDialogTest, VariantStepperReportsThePositionInTheSequence) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeVariantCatalogue(3));

  dialog.selectItem(0);
  EXPECT_EQ(dialog.variantCount(), 3);
  EXPECT_EQ(dialog.variantIndex(), 0);
  EXPECT_EQ(dialog.variantText(), "Sub-page 1 of 3 (0001)");
  EXPECT_EQ(dialog.headlineText(), "Page 100 sub-page 0001");

  dialog.close();
}

TEST(CatalogueDialogTest, VariantNavigationWrapsAtBothEnds) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeVariantCatalogue(3));
  dialog.selectItem(0);

  dialog.showPreviousVariant();
  EXPECT_EQ(dialog.variantIndex(), 2);
  EXPECT_EQ(dialog.headlineText(), "Page 100 sub-page 0003");

  dialog.showNextVariant();
  EXPECT_EQ(dialog.variantIndex(), 0);

  dialog.close();
}

// A single variant is said rather than hidden, so "one of them" is
// distinguishable from a control that has not been noticed.
TEST(CatalogueDialogTest, SingleVariantSaysSoAndDisablesTheControls) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeVariantCatalogue(1));
  dialog.selectItem(0);

  EXPECT_EQ(dialog.variantCount(), 1);
  EXPECT_EQ(dialog.variantText(), "No sub-pages");

  dialog.close();
}

namespace {

// The shape the NABTS sink produces: a chain page whose members are variants,
// listed alongside a record that carries its own payload and has none.
orc::CatalogueDataset makeMixedVariantCatalogue() {
  orc::CatalogueDataset data = makeVariantCatalogue(2);

  orc::CatalogueItem standalone;
  standalone.id = "000/1A4 v0";
  standalone.find_key = standalone.id;
  standalone.values = {standalone.id};
  data.items.push_back(std::move(standalone));

  orc::CataloguePayload payload;
  payload.kind = orc::CataloguePayload::Kind::kCellGrid;
  payload.grid = makeGrid("1A4");
  payload.headline = "Record 000/1A4 seen 3 times";
  data.payloads.push_back(std::move(payload));
  return data;
}

}  // namespace

// An item with nothing to step to keeps the stepper standing and disabled,
// rather than removing it from the row: a control that came and went as the
// reader moved down the list would read as an interface glitch.
TEST(CatalogueDialogTest, ItemWithoutVariantsKeepsTheStepperInPlace) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeMixedVariantCatalogue());

  dialog.selectItem(0);
  EXPECT_EQ(dialog.variantText(), "Sub-page 1 of 2 (0001)");

  dialog.selectItem(1);
  EXPECT_EQ(dialog.variantCount(), 0);
  EXPECT_EQ(dialog.variantIndex(), 0);
  EXPECT_EQ(dialog.variantText(), "No sub-pages");
  EXPECT_EQ(dialog.headlineText(), "Record 000/1A4 seen 3 times");

  const auto* previous =
      dialog.findChild<QToolButton*>("cataloguePrevVariantButton");
  const auto* next =
      dialog.findChild<QToolButton*>("catalogueNextVariantButton");
  ASSERT_NE(previous, nullptr);
  ASSERT_NE(next, nullptr);
  EXPECT_FALSE(previous->isEnabled());
  EXPECT_FALSE(next->isEnabled());

  // Stepping a stepper with nowhere to go leaves the display where it was.
  dialog.showNextVariant();
  EXPECT_EQ(dialog.headlineText(), "Record 000/1A4 seen 3 times");

  dialog.close();
}

// Moving to a different item starts at the top of its sequence.
TEST(CatalogueDialogTest, ChangingItemResetsToTheFirstVariant) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(2));

  dialog.selectItem(0);
  dialog.selectItem(1);
  EXPECT_EQ(dialog.variantIndex(), 0);

  dialog.close();
}

// A schema with no variant noun hides the stepper entirely, which is what a
// flat catalogue wants.
TEST(CatalogueDialogTest, FlatCatalogueHasNoVariantStepper) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());
  dialog.selectItem(0);

  EXPECT_EQ(dialog.variantText(), QString());
  EXPECT_EQ(dialog.findText(), QString());  // no find box either

  dialog.close();
}

// --- Payload kinds --------------------------------------------------------

TEST(CatalogueDialogTest, DisplayListPayloadShowsItsCompanionText) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());

  dialog.selectItem(0);
  ASSERT_NE(dialog.currentDisplayList(), nullptr);
  EXPECT_EQ(dialog.currentDisplayList()->ops.size(), 1u);
  EXPECT_EQ(dialog.currentText(), "HELLO NABTS");
  EXPECT_EQ(dialog.currentGrid(), nullptr);

  dialog.close();
}

// The text beside a drawing is worth having and worth being able to put away:
// two panes side by side leave neither enough room on a page-sized window.
TEST(CatalogueDialogTest, TheTextPaneCanBePutAway) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());
  dialog.selectItem(0);

  auto* check = dialog.findChild<QCheckBox*>("catalogueShowTextCheck");
  ASSERT_NE(check, nullptr);
  EXPECT_TRUE(check->isVisibleTo(&dialog));
  // Away to begin with, the drawing having the whole pane until it is asked
  // for — but the text is there, not thrown away, and nothing is asked of the
  // stage to show it.
  EXPECT_FALSE(check->isChecked());
  EXPECT_FALSE(dialog.isTextPaneVisible());
  EXPECT_EQ(dialog.currentText(), "HELLO NABTS");

  dialog.setTextPaneVisible(true);
  EXPECT_TRUE(dialog.isTextPaneVisible());

  dialog.setTextPaneVisible(false);
  EXPECT_FALSE(dialog.isTextPaneVisible());
  EXPECT_EQ(dialog.currentText(), "HELLO NABTS");

  dialog.close();
}

// And the switch is only offered where there is text to put away. A listing is
// text all the way down, and a table has no drawing to sit beside.
TEST(CatalogueDialogTest, TheTextSwitchIsOfferedOnlyBesideADrawing) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());
  auto* check = dialog.findChild<QCheckBox*>("catalogueShowTextCheck");
  ASSERT_NE(check, nullptr);

  dialog.selectItem(0);  // a drawing with text beside it
  EXPECT_TRUE(check->isVisibleTo(&dialog));

  dialog.selectItem(1);  // a listing
  EXPECT_FALSE(check->isVisibleTo(&dialog));

  dialog.selectItem(2);  // a table
  EXPECT_FALSE(check->isVisibleTo(&dialog));

  dialog.close();
}

// Everything the dialogue has to say goes in one panel at the foot of the
// window. A reader should not have to sweep three corners of the frame to find
// out whether anything was said.
TEST(CatalogueDialogTest, EveryMessageIsInTheOneMessagePanel) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());
  dialog.selectItem(0);

  auto* panel = dialog.findChild<QWidget*>("catalogueMessagePanel");
  ASSERT_NE(panel, nullptr);
  EXPECT_TRUE(panel->isVisibleTo(&dialog));

  for (const char* name : {"catalogueHeadlineLabel", "catalogueConditionLabel",
                           "catalogueNoticeLabel", "catalogueSummaryLabel",
                           "observationStatusLabel"}) {
    auto* label = dialog.findChild<QLabel*>(QString::fromLatin1(name));
    ASSERT_NE(label, nullptr) << name;
    EXPECT_EQ(label->parentWidget(), panel)
        << name << " is not in the message panel";
  }

  // Cleared, the panel keeps only what is still true — that there is nothing
  // to show — rather than going blank and leaving the reader to wonder.
  dialog.clearContent();
  EXPECT_TRUE(panel->isVisibleTo(&dialog));
  EXPECT_EQ(dialog.headlineText(), "No data");
  EXPECT_EQ(dialog.conditionText(), QString());
  EXPECT_EQ(dialog.summaryText(), QString());

  // And a viewer that has been told nothing at all shows no panel, rather than
  // an empty band along the bottom of the window.
  CatalogueDialog fresh;
  fresh.show();
  auto* fresh_panel = fresh.findChild<QWidget*>("catalogueMessagePanel");
  ASSERT_NE(fresh_panel, nullptr);
  EXPECT_FALSE(fresh_panel->isVisibleTo(&fresh));
  fresh.close();

  dialog.close();
}

TEST(CatalogueDialogTest, TextPayloadIsListed) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());

  dialog.selectItem(1);
  EXPECT_EQ(dialog.currentDisplayList(), nullptr);
  EXPECT_EQ(dialog.currentText(), "2/0  [control]  reset");

  dialog.close();
}

TEST(CatalogueDialogTest, TablePayloadIsTabulated) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());

  dialog.selectItem(2);
  const auto rows = dialog.listedTableRows();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0], "1-30 | FIRST CUE");
  EXPECT_EQ(rows[1], "31-60 | SECOND CUE");

  dialog.close();
}

// --- Run-wide readouts ----------------------------------------------------

TEST(CatalogueDialogTest, SummaryAndNoticesComeFromTheDataset) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();

  dialog.setCatalogue(makePagedCatalogue(1));
  EXPECT_EQ(dialog.summaryText(), "12 packets recovered");
  EXPECT_EQ(dialog.noticeText(), QString());

  dialog.setCatalogue(makeFlatCatalogue());
  EXPECT_EQ(dialog.noticeText(), "2 captions on A00/000");

  dialog.close();
}

// A recording that carried none of the service is the ordinary case, not an
// error, so the schema's own wording is shown rather than an empty pane.
TEST(CatalogueDialogTest, EmptyCatalogueShowsTheSchemasMessage) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();

  orc::CatalogueDataset empty = makePagedCatalogue(0);
  dialog.setCatalogue(empty);

  EXPECT_TRUE(dialog.listedItems().empty());
  EXPECT_EQ(dialog.headlineText(), "No pages were recovered");

  dialog.close();
}

TEST(CatalogueDialogTest, NewDataReplacesThePreviousCatalogue) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();

  dialog.setCatalogue(makePagedCatalogue(3));
  ASSERT_EQ(dialog.listedItems().size(), 3u);

  dialog.setCatalogue(makePagedCatalogue(1));
  EXPECT_EQ(dialog.listedItems().size(), 1u);

  dialog.close();
}

TEST(CatalogueDialogTest, ClearContentResetsTheDisplay) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(2));

  dialog.clearContent();
  EXPECT_TRUE(dialog.listedItems().empty());
  EXPECT_EQ(dialog.currentItemIndex(), -1);
  EXPECT_EQ(dialog.currentGrid(), nullptr);
  EXPECT_EQ(dialog.summaryText(), QString());

  dialog.close();
}

// --- Chrome driven by the schema -----------------------------------------

TEST(CatalogueDialogTest, HighlightToggleDrivesBothRenderers) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(1));

  auto* check = dialog.findChild<QCheckBox*>("catalogueHighlightCheck");
  ASSERT_NE(check, nullptr);
  EXPECT_TRUE(check->isVisibleTo(&dialog));
  EXPECT_EQ(check->text(), "Show data errors");

  auto* grid = dialog.findChild<CatalogueCellGridWidget*>("catalogueCellGrid");
  auto* display =
      dialog.findChild<CatalogueDisplayListWidget*>("catalogueDisplayList");
  ASSERT_NE(grid, nullptr);
  ASSERT_NE(display, nullptr);
  EXPECT_FALSE(grid->showDataErrors());

  check->setChecked(true);
  EXPECT_TRUE(grid->showDataErrors());
  EXPECT_TRUE(display->showDataErrors());

  dialog.close();
}

// A schema with no highlight label has nothing to overlay, so the toggle is
// not offered at all.
TEST(CatalogueDialogTest, HighlightToggleIsHiddenWithoutASchemaLabel) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());

  auto* check = dialog.findChild<QCheckBox*>("catalogueHighlightCheck");
  ASSERT_NE(check, nullptr);
  EXPECT_FALSE(check->isVisibleTo(&dialog));

  dialog.close();
}

// --- View options ---------------------------------------------------------

/// |data| with two ways of presenting it declared, the second in force.
orc::CatalogueDataset withViewOptions(orc::CatalogueDataset data,
                                      const std::string& in_force = "fine") {
  data.schema.view_label = "Receiver";
  data.schema.view_options = {
      orc::CatalogueViewOption{"coarse", "256 x 200", "As transmitted"},
      orc::CatalogueViewOption{"fine", "512 x 400", "Twice that"}};
  data.schema.view_option = in_force;
  return data;
}

// A service that presents the same items more than one way says so in its
// schema, and the host offers the choice without knowing what the options mean.
TEST(CatalogueDialogTest, ViewDropdownIsOfferedWhenTheSchemaDeclaresOptions) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(withViewOptions(makeFlatCatalogue()));

  auto* combo = dialog.findChild<QComboBox*>("catalogueViewCombo");
  ASSERT_NE(combo, nullptr);
  EXPECT_TRUE(combo->isVisibleTo(&dialog));
  EXPECT_EQ(
      dialog.viewOptions(),
      (std::vector<QString>{QStringLiteral("coarse"), QStringLiteral("fine")}));
  // What the stage says it built under, not the first of the list.
  EXPECT_EQ(dialog.currentViewOption(), QStringLiteral("fine"));
  EXPECT_EQ(combo->itemText(0), QStringLiteral("256 x 200"));

  dialog.close();
}

// A schema declaring none has one way of presenting its items, so there is no
// choice to offer.
TEST(CatalogueDialogTest, ViewDropdownIsHiddenWithoutSchemaOptions) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());

  auto* combo = dialog.findChild<QComboBox*>("catalogueViewCombo");
  ASSERT_NE(combo, nullptr);
  EXPECT_FALSE(combo->isVisibleTo(&dialog));
  EXPECT_TRUE(dialog.viewOptions().empty());
  EXPECT_TRUE(dialog.currentViewOption().isEmpty());

  dialog.close();
}

// The dialogue cannot build the catalogue again itself, so picking an option
// asks its owner for one; and showing what the stage built is not a reader
// asking for anything.
TEST(CatalogueDialogTest, PickingAViewOptionAsksForItOnce) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();

  std::vector<QString> asked;
  QObject::connect(
      &dialog, &CatalogueDialog::viewSelectionChanged,
      [&asked](const QString& id, const QStringList&) { asked.push_back(id); });

  dialog.setCatalogue(withViewOptions(makeFlatCatalogue()));
  EXPECT_TRUE(asked.empty()) << "filling the dropdown asked for a rebuild";

  dialog.selectViewOption(QStringLiteral("coarse"));
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], QStringLiteral("coarse"));

  // The catalogue built under it arrives the same way any other does, and does
  // not ask again for what it already is.
  dialog.setCatalogue(withViewOptions(makeFlatCatalogue(), "coarse"));
  EXPECT_EQ(asked.size(), 1u);
  EXPECT_EQ(dialog.currentViewOption(), QStringLiteral("coarse"));

  dialog.close();
}

// --- View toggles ---------------------------------------------------------

/// |data| with one two-state presentation choice declared.
orc::CatalogueDataset withViewToggle(orc::CatalogueDataset data,
                                     bool active = true) {
  data.schema.toggles = {
      orc::CatalogueViewToggle{"repair", "Error correction",
                               "Present damaged data as recovered", active}};
  return data;
}

// A two-state presentation choice is a checkbox beside the dropdown, and the
// host offers it without knowing what it means.
TEST(CatalogueDialogTest, ToggleIsOfferedWhenTheSchemaDeclaresOne) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(withViewToggle(withViewOptions(makeFlatCatalogue())));

  auto* check = dialog.findChild<QCheckBox*>("catalogueViewToggle");
  ASSERT_NE(check, nullptr);
  EXPECT_TRUE(check->isVisibleTo(&dialog));
  EXPECT_EQ(check->text(), QStringLiteral("Error correction"));
  EXPECT_EQ(dialog.viewToggles(),
            (std::vector<QString>{QStringLiteral("repair")}));
  // What the stage says it built under, not a default of the host's.
  EXPECT_TRUE(dialog.isToggleActive(QStringLiteral("repair")));

  dialog.close();
}

// A stage declaring no toggles gets the browser exactly as it was.
TEST(CatalogueDialogTest, NoToggleIsOfferedWithoutSchemaToggles) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(withViewOptions(makeFlatCatalogue()));

  EXPECT_EQ(dialog.findChild<QCheckBox*>("catalogueViewToggle"), nullptr);
  EXPECT_TRUE(dialog.viewToggles().empty());
  EXPECT_FALSE(dialog.isToggleActive(QStringLiteral("repair")));

  dialog.close();
}

// A toggle is offered on its own too: a service may present its items one way
// and still have something to switch about them.
TEST(CatalogueDialogTest, ToggleIsOfferedWithoutAViewDropdown) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(withViewToggle(makeFlatCatalogue()));

  auto* check = dialog.findChild<QCheckBox*>("catalogueViewToggle");
  ASSERT_NE(check, nullptr);
  EXPECT_TRUE(check->isVisibleTo(&dialog));
  auto* combo = dialog.findChild<QComboBox*>("catalogueViewCombo");
  ASSERT_NE(combo, nullptr);
  EXPECT_FALSE(combo->isVisibleTo(&dialog));

  dialog.close();
}

// The stage builds from both axes at once and has no memory of what it was last
// asked for, so the whole selection travels on every change.
TEST(CatalogueDialogTest, ChangingAToggleAsksWithTheWholeSelection) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();

  std::vector<std::pair<QString, QStringList>> asked;
  QObject::connect(&dialog, &CatalogueDialog::viewSelectionChanged,
                   [&asked](const QString& id, const QStringList& toggles) {
                     asked.emplace_back(id, toggles);
                   });

  dialog.setCatalogue(withViewToggle(withViewOptions(makeFlatCatalogue())));
  EXPECT_TRUE(asked.empty()) << "filling the bar asked for a rebuild";

  dialog.setToggleActive(QStringLiteral("repair"), false);
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0].first, QStringLiteral("fine")) << "the view option too";
  EXPECT_TRUE(asked[0].second.isEmpty());

  dialog.setToggleActive(QStringLiteral("repair"), true);
  ASSERT_EQ(asked.size(), 2u);
  EXPECT_EQ(asked[1].second, QStringList{QStringLiteral("repair")});

  // Picking a view option carries the toggles with it.
  dialog.selectViewOption(QStringLiteral("coarse"));
  ASSERT_EQ(asked.size(), 3u);
  EXPECT_EQ(asked[2].first, QStringLiteral("coarse"));
  EXPECT_EQ(asked[2].second, QStringList{QStringLiteral("repair")});

  dialog.close();
}

// The catalogue built under a toggle arrives the same way any other does, and
// does not ask again for what it already is.
TEST(CatalogueDialogTest, ShowingACatalogueBuiltUnderAToggleAsksForNothing) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(withViewToggle(withViewOptions(makeFlatCatalogue())));

  int asked = 0;
  QObject::connect(&dialog, &CatalogueDialog::viewSelectionChanged,
                   [&asked](const QString&, const QStringList&) { ++asked; });

  dialog.setCatalogue(
      withViewToggle(withViewOptions(makeFlatCatalogue()), false));
  EXPECT_EQ(asked, 0);
  EXPECT_FALSE(dialog.isToggleActive(QStringLiteral("repair")));

  dialog.close();
}

// A catalogue offering a different set replaces the checkboxes rather than
// accumulating them.
TEST(CatalogueDialogTest, RedeliveringReplacesTheTogglesRatherThanAddingThem) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(withViewToggle(makeFlatCatalogue()));
  dialog.setCatalogue(withViewToggle(makeFlatCatalogue()));

  EXPECT_EQ(dialog.viewToggles().size(), 1u);

  // And a catalogue offering none takes them away again.
  dialog.setCatalogue(makeFlatCatalogue());
  EXPECT_TRUE(dialog.viewToggles().empty());

  dialog.close();
}

// Changing the view is asking to see what is in front of you drawn another way,
// so the item on show survives the catalogue being replaced.
TEST(CatalogueDialogTest, ANewViewKeepsTheItemOnShow) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(withViewOptions(makeFlatCatalogue()));

  dialog.selectItem(1);
  ASSERT_EQ(dialog.currentItemIndex(), 1);
  const QString shown = dialog.headlineText();

  dialog.setCatalogue(withViewOptions(makeFlatCatalogue(), "coarse"));
  EXPECT_EQ(dialog.currentItemIndex(), 1);
  EXPECT_EQ(dialog.headlineText(), shown);

  dialog.close();
}

// --- Renderers ------------------------------------------------------------

// A double-height character occupies the row below its origin, so the row
// below must carry its background: a row-sequential paint would erase the
// character's lower half.
TEST(CatalogueCellGridWidgetTest, DoubleHeightFillsTheRowBelow) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("A", /*rows=*/2, /*columns=*/2);
  grid.palette = {orc::CatalogueColour{0, 0, 0},
                  orc::CatalogueColour{255, 255, 255},
                  orc::CatalogueColour{255, 0, 0}};
  grid.cells[0].double_height = true;
  grid.cells[0].background = 2;
  // Index 2 is row 1, column 0: the lower cell of the pair, carrying the
  // origin row's local background.
  grid.cells[2].double_height_lower = true;
  grid.cells[2].background = 2;

  const QImage image = renderGrid(grid);
  const QColor lower = cellCentre(image, grid, /*row=*/1, /*column=*/0);
  EXPECT_EQ(lower.red(), 255);
  EXPECT_EQ(lower.green(), 0);
}

// The palette travels with the payload: a renderer that assumed a fixed one
// would draw the wrong colours for any other service.
TEST(CatalogueCellGridWidgetTest, ColoursComeFromThePayloadPalette) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("", /*rows=*/1, /*columns=*/1);
  grid.palette = {orc::CatalogueColour{0, 0, 0},
                  orc::CatalogueColour{10, 200, 40}};
  grid.cells[0].background = 1;

  const QImage image = renderGrid(grid);
  const QColor centre = cellCentre(image, grid, /*row=*/0, /*column=*/0);
  EXPECT_EQ(centre.green(), 200);
  EXPECT_EQ(centre.red(), 10);
}

// An index past the end of the palette resolves to the last entry rather than
// reading off it.
TEST(CatalogueCellGridWidgetTest, OutOfRangePaletteIndexIsClamped) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("", /*rows=*/1, /*columns=*/1);
  grid.palette = {orc::CatalogueColour{0, 0, 0},
                  orc::CatalogueColour{0, 0, 255}};
  grid.cells[0].background = 200;

  const QImage image = renderGrid(grid);
  EXPECT_EQ(cellCentre(image, grid, /*row=*/0, /*column=*/0).blue(), 255);
}

// Alphanumeric cells are drawn from the teletext character generator's own
// face, on the 6 by 10 character rectangle it drew into: the matrix sits
// against the rectangle's bottom right, leaving the blank column that
// separates a character from the one beside it and the blank row that
// separates a row of text from the one above.
//
// 7/F of every G0 set is the filled rectangle, whose left and top edges are
// the matrix's own, so it says where the matrix was put without depending on a
// letterform. A glyph from a proportional or centred font would have neither
// edge in the same place.
TEST(CatalogueCellGridWidgetTest, CharactersAreDrawnOnTheCharacterRectangle) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("", /*rows=*/1, /*columns=*/1);
  grid.cells[0].character = U'■';

  const QImage image = renderGrid(grid);
  // Sub-pixels of the rounded character rectangle: 12 across and 20 down.
  const double sub_w = static_cast<double>(image.width()) / 12.0;
  const double sub_h = static_cast<double>(image.height()) / 20.0;
  const auto sub_pixel = [&](double column, double row) {
    return image.pixelColor(static_cast<int>(column * sub_w),
                            static_cast<int>(row * sub_h));
  };

  // The blank column down the left of the rectangle, and the blank row along
  // the top of it.
  EXPECT_EQ(sub_pixel(0.5, 6.5), QColor(Qt::black));
  EXPECT_EQ(sub_pixel(6.5, 0.5), QColor(Qt::black));
  // The matrix itself, which the filled rectangle covers to both those edges.
  EXPECT_EQ(sub_pixel(2.5, 6.5), QColor(Qt::white));
  EXPECT_EQ(sub_pixel(11.5, 6.5), QColor(Qt::white));
  // Below the matrix's seven rows: the two rows a descender uses.
  EXPECT_EQ(sub_pixel(6.5, 18.5), QColor(Qt::black));
}

TEST(CatalogueDisplayListWidgetTest, WalksEveryOperation) {
  ensureApplication();
  CatalogueDisplayListWidget widget;

  orc::CatalogueDisplayList list;
  list.aspect_height = 1.0;
  for (int i = 0; i < 3; ++i) {
    orc::CatalogueDrawOp op;
    op.kind = orc::CatalogueDrawKind::kRectangle;
    op.origin = orc::CataloguePoint{0.1 * i, 0.1};
    op.size = orc::CatalogueSize{0.2, 0.2};
    op.filled = true;
    op.colour = orc::CatalogueColour{255, 255, 255};
    list.ops.push_back(op);
  }
  widget.setDisplayList(list);

  renderWidget(widget, 100, 100);
  EXPECT_EQ(widget.opsPainted(), 3);
}

// X3.110 §5.3.3.5.1: "For filled polygons, the area enclosed by the outline
// (including the region of the outline traced by the logical pel) is filled",
// and §5.3.2.4.3 makes that region what the pel sweeps along the outline.
//
// The regression this pins: a service draws a letterform as a path that
// encloses almost nothing and lets the pel give it its weight — the NCAA
// roundel of the reference ExtraVision recording draws every letter that way.
// Filling the enclosed area alone left the letters as disconnected slivers.
// The path here is the degenerate case in miniature: out and back along the
// same line, enclosing no area at all, so anything drawn is the pel's doing.
TEST(CatalogueDisplayListWidgetTest, AFilledFigureIncludesItsPelTracedOutline) {
  ensureApplication();

  const auto strokeOnlyPolygon = [](double pel) {
    orc::CatalogueDisplayList list;
    list.aspect_height = 1.0;
    orc::CatalogueDrawOp op;
    op.kind = orc::CatalogueDrawKind::kPolygon;
    op.filled = true;
    op.colour = orc::CatalogueColour{255, 255, 255};
    op.pen_size = orc::CatalogueSize{pel, pel};
    // Down the middle and back: zero enclosed area.
    op.points = {orc::CataloguePoint{0.5, 0.2}, orc::CataloguePoint{0.5, 0.8},
                 orc::CataloguePoint{0.5, 0.2}};
    list.ops.push_back(op);
    return list;
  };

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(strokeOnlyPolygon(0.05));
  const QImage traced = renderWidget(widget, 200, 200);
  EXPECT_GT(traced.pixelColor(100, 100).red(), 0)
      << "the pel traced no outline, so the figure drew nothing at all";

  // A dimensionless pel (§5.3.2.2.6's default) traces nothing, so the same
  // path draws nothing — the stroke is the pel's, not a minimum line width's.
  CatalogueDisplayListWidget dimensionless;
  dimensionless.setDisplayList(strokeOnlyPolygon(0.0));
  const QImage untraced = renderWidget(dimensionless, 200, 200);
  EXPECT_EQ(untraced.pixelColor(100, 100).red(), 0);
}

// The traced outline is part of the filled area, so it takes the fill's
// colour — not the black a highlight would put there (§5.3.2.4.3 makes the
// black outline the highlight attribute's doing, and it is off by default).
TEST(CatalogueDisplayListWidgetTest, ThePelTracedOutlineTakesTheFillColour) {
  ensureApplication();
  CatalogueDisplayListWidget widget;

  orc::CatalogueDisplayList list;
  list.aspect_height = 1.0;
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.filled = true;
  op.origin = orc::CataloguePoint{0.3, 0.3};
  op.size = orc::CatalogueSize{0.4, 0.4};
  op.colour = orc::CatalogueColour{0, 255, 0};
  op.pen_size = orc::CatalogueSize{0.06, 0.06};
  list.ops.push_back(op);
  widget.setDisplayList(list);

  const QImage image = renderWidget(widget, 200, 200);
  // On the rectangle's own edge, which is the middle of the traced band.
  const QColor edge = image.pixelColor(60, 100);
  EXPECT_GT(edge.green(), 0);
  EXPECT_EQ(edge.red(), 0);
}

// The drawable area follows the payload's own aspect, not the widget's.
TEST(CatalogueDisplayListWidgetTest, DrawableAreaFollowsThePayloadAspect) {
  ensureApplication();
  CatalogueDisplayListWidget widget;

  orc::CatalogueDisplayList list;
  list.aspect_height = 0.5;
  widget.setDisplayList(list);
  widget.resize(200, 200);

  const QRectF area = widget.displayAreaRect();
  EXPECT_NEAR(area.height() / area.width(), 0.5, 1e-6);
}

// A payload whose nominal pixels are not square states the shape it is
// displayed at separately from the extent of unit space it covers, and the
// drawable area takes the former. ANSI X3.110 §4.2.2 is the case in point: a
// NAPLPS page covers unit y 0 to 0.78125 and is displayed in the 4:3 area of a
// television set, so drawing it 0.78125 high squares up a picture that a
// receiver showed wide.
TEST(CatalogueDisplayListWidgetTest, DrawableAreaFollowsTheDisplayAspect) {
  ensureApplication();
  CatalogueDisplayListWidget widget;

  orc::CatalogueDisplayList list;
  list.aspect_height = 0.78125;
  list.display_aspect_height = 0.75;
  widget.setDisplayList(list);
  widget.resize(400, 400);

  const QRectF area = widget.displayAreaRect();
  EXPECT_NEAR(area.height() / area.width(), 0.75, 1e-6);
  EXPECT_EQ(widget.pageImageSize(),
            QSize(720, static_cast<int>(std::lround(720 * 0.75))));
}

// Unit y then maps at a different number of device pixels per unit than unit x
// does, and an operation covering the whole visible unit space still covers the
// whole drawable area. Squaring the two scales up would leave a band of the
// area undrawn, or push the top of the page out of it.
TEST(CatalogueDisplayListWidgetTest, TheVisibleUnitSpaceFillsTheDisplayArea) {
  ensureApplication();

  orc::CatalogueDisplayList list;
  list.aspect_height = 0.78125;
  list.display_aspect_height = 0.75;
  list.nominal_width = 256;
  list.nominal_height = 200;
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.filled = true;
  op.origin = orc::CataloguePoint{0.0, 0.0};
  op.size = orc::CatalogueSize{1.0, 0.78125};
  op.colour = orc::CatalogueColour{255, 255, 255};
  list.ops.push_back(op);

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);

  const QSize size = widget.pageImageSize();
  const QImage image = widget.renderPageImage(size);
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.pixelColor(1, 1), QColor(Qt::white))
      << "the top of the visible unit space is outside the drawable area";
  EXPECT_EQ(image.pixelColor(size.width() - 2, size.height() - 2),
            QColor(Qt::white));
}

// X3.110 §5.3.2.2.6 guarantees the logical pel "will always map to at least one
// and possibly many display pixels", so a stroke of the default dimensionless
// pel is one pixel of the receiver's grid — not one pixel of whatever surface
// the list happens to be drawn into. Deriving the floor from the payload's own
// grid is what stops stroke width tracking the size of the window: the same
// page drawn twice as large has a stroke twice as thick in device pixels, and
// the same thickness relative to the picture.
TEST(CatalogueDisplayListWidgetTest, AStrokeFloorsAtOnePixelOfTheStatedGrid) {
  ensureApplication();

  const auto pageWithGrid = [](int nominal_width, int nominal_height) {
    orc::CatalogueDisplayList list;
    list.aspect_height = 0.78125;
    list.display_aspect_height = 0.75;
    list.nominal_width = nominal_width;
    list.nominal_height = nominal_height;
    orc::CatalogueDrawOp op;
    op.kind = orc::CatalogueDrawKind::kLine;
    op.colour = orc::CatalogueColour{255, 255, 255};
    // A dimensionless pel: whatever width this draws at is the floor's doing.
    op.pen_size = orc::CatalogueSize{0.0, 0.0};
    op.points = {orc::CataloguePoint{0.0, 0.39},
                 orc::CataloguePoint{1.0, 0.39}};
    list.ops.push_back(op);
    return list;
  };

  // Counts the lit rows down the middle of the drawable area, which is the
  // stroke's thickness in device pixels.
  const auto strokeThickness = [](const QImage& image) {
    const int column = image.width() / 2;
    int lit = 0;
    for (int row = 0; row < image.height(); ++row) {
      if (image.pixelColor(column, row).red() > 0) {
        ++lit;
      }
    }
    return lit;
  };

  CatalogueDisplayListWidget coarse;
  coarse.setDisplayList(pageWithGrid(256, 200));
  const int coarse_thickness =
      strokeThickness(coarse.renderPageImage(QSize(512, 384)));

  CatalogueDisplayListWidget fine;
  fine.setDisplayList(pageWithGrid(512, 400));
  const int fine_thickness =
      strokeThickness(fine.renderPageImage(QSize(512, 384)));

  // 512 device pixels across a 256-pixel grid puts one grid pixel at two device
  // pixels, and across a 512-pixel grid at one.
  EXPECT_GT(coarse_thickness, fine_thickness)
      << "the stroke ignored the grid the page was resolved against";
  EXPECT_GE(coarse_thickness, 2);

  // The same page drawn twice as large draws its stroke twice as thick, which
  // is what "one pixel of the receiver's grid" means and what a device-pixel
  // floor cannot do.
  CatalogueDisplayListWidget large;
  large.setDisplayList(pageWithGrid(256, 200));
  const int large_thickness =
      strokeThickness(large.renderPageImage(QSize(1024, 768)));
  EXPECT_GT(large_thickness, coarse_thickness);
}

// A page whose operations tile its own pixel grid has to come out as a solid
// surface: adjacent runs meet along a boundary that is a join in the drawing,
// not an edge in the picture, and smoothing it leaves a visible seam. This is
// what a rasterised NAPLPS page is made of — one run per row of like-coloured
// pixels — so a seam here would be a grid of dark lines over every page.
TEST(CatalogueDisplayListWidgetTest, AbuttingPixelRunsLeaveNoSeam) {
  ensureApplication();

  // Two rows of runs covering a block, split into runs the way the raster
  // emitter splits them.
  orc::CatalogueDisplayList list;
  list.aspect_height = 0.78125;
  list.display_aspect_height = 0.75;
  list.nominal_width = 16;
  list.nominal_height = 12;
  list.pixel_aligned = true;
  const double pitch_x = 1.0 / 16.0;
  const double pitch_y = 0.78125 / 12.0;
  for (int row = 0; row < 12; ++row) {
    for (int column = 0; column < 16; column += 4) {
      orc::CatalogueDrawOp op;
      op.kind = orc::CatalogueDrawKind::kRectangle;
      op.filled = true;
      op.origin = orc::CataloguePoint{column * pitch_x, row * pitch_y};
      op.size = orc::CatalogueSize{4 * pitch_x, pitch_y};
      op.colour = orc::CatalogueColour{255, 255, 255};
      list.ops.push_back(op);
    }
  }

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);

  // Awkward sizes on purpose: a grid boundary lands mid-pixel at most of them,
  // which is exactly when a seam appears.
  for (const QSize size : {QSize(200, 150), QSize(301, 226), QSize(640, 480)}) {
    const QImage image = widget.renderPageImage(size);
    ASSERT_FALSE(image.isNull());
    const QRectF area = widget.displayAreaRect();
    (void)area;
    // Every pixel well inside the covered area is full white: a seam shows up
    // as a partially blended column or row between two runs.
    int blended = 0;
    for (int y = 2; y < image.height() - 2; ++y) {
      for (int x = 2; x < image.width() - 2; ++x) {
        const QColor colour = image.pixelColor(x, y);
        if (colour.red() > 0 && colour.red() < 255) {
          ++blended;
        }
      }
    }
    EXPECT_EQ(blended, 0) << "seams between abutting pixel runs at "
                          << size.width() << "x" << size.height();
  }
}

// Without a grid there is nothing to derive a floor from, so a list that names
// none keeps the single device pixel it always had.
TEST(CatalogueDisplayListWidgetTest, AListWithNoGridKeepsTheDevicePixelFloor) {
  ensureApplication();

  orc::CatalogueDisplayList list;
  list.aspect_height = 1.0;
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kLine;
  op.colour = orc::CatalogueColour{255, 255, 255};
  op.pen_size = orc::CatalogueSize{0.0, 0.0};
  op.points = {orc::CataloguePoint{0.0, 0.5}, orc::CataloguePoint{1.0, 0.5}};
  list.ops.push_back(op);

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  const QImage image = widget.renderPageImage(QSize(400, 400));
  ASSERT_FALSE(image.isNull());

  int lit = 0;
  for (int row = 0; row < image.height(); ++row) {
    if (image.pixelColor(200, row).red() > 0) {
      ++lit;
    }
  }
  // One device pixel, plus at most the antialiased edge either side of it.
  EXPECT_GE(lit, 1);
  EXPECT_LE(lit, 3);
}

// --- Textures sized by the drawing rather than by the renderer -------------

namespace {

/// A list holding one operation over a square unit screen, so a device pixel is
/// the widget's width per unit of x and per unit of y alike.
orc::CatalogueDisplayList listWith(orc::CatalogueDrawOp op) {
  orc::CatalogueDisplayList list;
  list.aspect_height = 1.0;
  list.ops.push_back(std::move(op));
  return list;
}

/// The x of every change between lit and unlit along row |row| of |image|
/// between |from| and |to|, taking the first pixel of each new state.
/// Antialiasing puts a blended pixel on a boundary, so a mid-grey counts as
/// neither and the boundary is where the image commits to one side. The range
/// is how a figure's own edges are kept out of the count.
std::vector<int> transitionsAlongRow(const QImage& image, int row, int from,
                                     int to) {
  std::vector<int> transitions;
  int state = -1;
  for (int x = from; x < to; ++x) {
    const int red = image.pixelColor(x, row).red();
    const int here = red > 200 ? 1 : (red < 55 ? 0 : -1);
    if (here < 0) {
      continue;
    }
    if (state >= 0 && here != state) {
      transitions.push_back(x);
    }
    state = here;
  }
  return transitions;
}

}  // namespace

// §5.3.2.4.4 makes a hatch line and the gap beside it each one logical pel
// across, and §5.3.2.4.4/5 register both against the unit screen's origin so
// "registration of the patterns shall be maintained across figures". Neither is
// a number of device pixels, so neither may move when the page is drawn larger:
// the bands fall at the same places in unit space at any size, and at places
// the figure's own origin has no say in.
TEST(CatalogueDisplayListWidgetTest,
     HatchBandsAreThePelAndAnchoredToTheScreen) {
  ensureApplication();

  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.filled = true;
  // Deliberately not on a band boundary: 0.1 is six and two fifths pels in.
  op.origin = orc::CataloguePoint{0.1, 0.1};
  op.size = orc::CatalogueSize{0.75, 0.75};
  op.colour = orc::CatalogueColour{255, 255, 255};
  op.fill_pattern = orc::CatalogueFillPattern::kVerticalHatch;
  const double pel = 1.0 / 64.0;
  op.pen_size = orc::CatalogueSize{pel, pel};

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(listWith(op));

  for (const int size : {512, 1024}) {
    const QImage image = widget.renderPageImage(QSize(size, size));
    ASSERT_FALSE(image.isNull());
    const double band = pel * size;

    // Strictly inside the figure, so its own edges are not counted as bands.
    const auto transitions =
        transitionsAlongRow(image, size / 2, static_cast<int>(0.12 * size),
                            static_cast<int>(0.83 * size));
    ASSERT_GE(transitions.size(), 8u) << "no hatching at " << size;
    for (const int x : transitions) {
      // Every change lands on a band boundary counted from the unit screen's
      // origin, not from the figure's left edge.
      const double bands = x / band;
      EXPECT_NEAR(bands, std::round(bands), 0.35)
          << "a band boundary at x=" << x << " of " << size
          << " is not a whole pel from the screen origin";
    }
  }
}

// The degenerate case the standard names: with no pel there is no band to draw
// and the fill is solid.
TEST(CatalogueDisplayListWidgetTest, HatchWithNoPelFillsSolid) {
  ensureApplication();

  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.filled = true;
  op.origin = orc::CataloguePoint{0.1, 0.1};
  op.size = orc::CatalogueSize{0.8, 0.8};
  op.colour = orc::CatalogueColour{255, 255, 255};
  op.fill_pattern = orc::CatalogueFillPattern::kCrossHatch;
  op.pen_size = orc::CatalogueSize{0.0, 0.0};

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(listWith(op));
  const QImage image = widget.renderPageImage(QSize(512, 512));
  ASSERT_FALSE(image.isNull());
  EXPECT_TRUE(transitionsAlongRow(image, 256, 64, 448).empty())
      << "a dimensionless pel drew bands it has no width for";
  EXPECT_EQ(image.pixelColor(256, 256), QColor(Qt::white));
}

// §5.3.2.4.5 steps and repeats a programmable mask over the mask size the
// source stated, from the unit screen's origin, "the sign bits of dx and dy
// [used] to reflect the mask pattern within the mask field". The element count
// is the mask's own business and says nothing about how large it is drawn.
TEST(CatalogueDisplayListWidgetTest, MaskTilesAtItsStatedSize) {
  ensureApplication();

  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.filled = true;
  op.origin = orc::CataloguePoint{0.0, 0.0};
  op.size = orc::CatalogueSize{1.0, 1.0};
  op.colour = orc::CatalogueColour{255, 255, 255};
  op.fill_pattern = orc::CatalogueFillPattern::kMask0;
  // A tile an eighth of the screen across, so each of the mask's two columns is
  // a sixteenth — nothing a device-pixel-pitched brush would land on.
  op.fill_mask_size = orc::CatalogueSize{1.0 / 8.0, 1.0 / 8.0};

  orc::CatalogueDisplayList list = listWith(op);
  orc::CatalogueBitmap mask;
  mask.width = 2;
  mask.height = 2;
  // Row 0 is the bottom: lit at bottom-left and top-right.
  mask.elements = {true, false, false, true};
  list.fill_masks.push_back(mask);

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  const QImage image = widget.renderPageImage(QSize(512, 512));
  ASSERT_FALSE(image.isNull());

  // One tile is 64 device pixels and one element 32. Sampled at the middle of
  // each element of the tile nearest the origin.
  EXPECT_EQ(image.pixelColor(16, 512 - 16), QColor(Qt::white)) << "bottom left";
  EXPECT_EQ(image.pixelColor(48, 512 - 16), QColor(Qt::black))
      << "bottom right";
  EXPECT_EQ(image.pixelColor(16, 512 - 48), QColor(Qt::black)) << "top left";
  EXPECT_EQ(image.pixelColor(48, 512 - 48), QColor(Qt::white)) << "top right";

  // And the next tile along repeats it, which a brush anchored to the figure
  // would also do — but one anchored to the screen keeps it in step with any
  // other figure, which is what §5.3.2.4.5 asks for.
  EXPECT_EQ(image.pixelColor(16 + 64, 512 - 16), QColor(Qt::white));
  EXPECT_EQ(image.pixelColor(48 + 64, 512 - 16), QColor(Qt::black));
}

TEST(CatalogueDisplayListWidgetTest, ANegativeMaskSizeReflectsThePattern) {
  ensureApplication();

  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.filled = true;
  op.origin = orc::CataloguePoint{0.0, 0.0};
  op.size = orc::CatalogueSize{1.0, 1.0};
  op.colour = orc::CatalogueColour{255, 255, 255};
  op.fill_pattern = orc::CatalogueFillPattern::kMask0;
  op.fill_mask_size = orc::CatalogueSize{-1.0 / 8.0, 1.0 / 8.0};

  orc::CatalogueDisplayList list = listWith(op);
  orc::CatalogueBitmap mask;
  mask.width = 2;
  mask.height = 2;
  mask.elements = {true, false, false, true};
  list.fill_masks.push_back(mask);

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  const QImage image = widget.renderPageImage(QSize(512, 512));
  ASSERT_FALSE(image.isNull());

  // Reflected across the mask field: what was the lit bottom-left element is
  // now the bottom-right.
  EXPECT_EQ(image.pixelColor(16, 512 - 16), QColor(Qt::black));
  EXPECT_EQ(image.pixelColor(48, 512 - 16), QColor(Qt::white));
}

// §5.3.2.4.2 counts a texture in pels: a dot is one pel long with a gap of one,
// and a dash three with a gap of three. Qt's own dotted style leaves twice the
// gap the standard does, so none of its styles is used.
TEST(CatalogueDisplayListWidgetTest, LineTexturesRunTheStatedNumberOfPels) {
  ensureApplication();

  struct Case {
    orc::CatalogueLineStyle style;
    double lit_pels;
    double gap_pels;
  };
  const std::vector<Case> cases = {
      {orc::CatalogueLineStyle::kDotted, 1.0, 1.0},
      {orc::CatalogueLineStyle::kDashed, 3.0, 3.0}};

  const double pel = 1.0 / 64.0;
  for (const Case& item : cases) {
    orc::CatalogueDrawOp op;
    op.kind = orc::CatalogueDrawKind::kLine;
    op.colour = orc::CatalogueColour{255, 255, 255};
    op.pen_size = orc::CatalogueSize{pel, pel};
    op.line_style = item.style;
    op.points = {orc::CataloguePoint{0.0, 0.5}, orc::CataloguePoint{1.0, 0.5}};

    CatalogueDisplayListWidget widget;
    widget.setDisplayList(listWith(op));
    const QImage image = widget.renderPageImage(QSize(512, 512));
    ASSERT_FALSE(image.isNull());

    // A horizontal line advances one pel per pel width, so the runs are the
    // pel counts above in device pixels.
    const double step = pel * 512;
    const auto transitions = transitionsAlongRow(image, 256, 0, 512);
    ASSERT_GE(transitions.size(), 4u)
        << "style " << static_cast<int>(item.style) << " drew no texture";

    for (size_t i = 0; i + 1 < transitions.size(); ++i) {
      const double run = transitions[i + 1] - transitions[i];
      // Runs alternate lit and unlit; which comes first depends on where the
      // scan starts, so both lengths are accepted and the pair is checked to
      // add up to the period.
      const bool lit = std::fabs(run - item.lit_pels * step) <= 2.0;
      const bool gap = std::fabs(run - item.gap_pels * step) <= 2.0;
      EXPECT_TRUE(lit || gap)
          << "style " << static_cast<int>(item.style) << " run of " << run
          << " device pixels is neither " << item.lit_pels << " nor "
          << item.gap_pels << " pels of " << step;
    }
  }
}

// A row no packet was recovered for is not by itself a fault: services
// habitually leave out the blank rows that space a page out. Banding every gap
// put marks on pages that had arrived perfectly, which is worse than not
// marking at all — it trains the reader to ignore them.
TEST(CatalogueCellGridWidgetTest, RowGapsAreNotBandedWithoutALoss) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("", /*rows=*/2, /*columns=*/1);
  grid.data_lost = false;
  grid.row_status[1].received = false;

  CatalogueCellGridWidget widget;
  widget.setGrid(grid);
  widget.setShowDataErrors(true);
  const int scale = gridScale(grid);
  const QImage image =
      renderWidget(widget, grid.columns * grid.cell_aspect_width * scale,
                   grid.rows * grid.cell_aspect_height * scale);

  EXPECT_EQ(cellCentre(image, grid, /*row=*/1, /*column=*/0), QColor(Qt::black))
      << "an un-received row was banded with no loss to blame it on";
}

// Once the page's own transmissions are known to have lost packets, every gap
// becomes a candidate for what went astray — the page cannot say which row each
// packet would have carried — so all of them are banded.
TEST(CatalogueCellGridWidgetTest, RowGapsAreBandedWhenPacketsWereLost) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("", /*rows=*/2, /*columns=*/1);
  grid.data_lost = true;
  grid.row_status[1].received = false;

  CatalogueCellGridWidget widget;
  widget.setGrid(grid);
  widget.setShowDataErrors(true);
  const int scale = gridScale(grid);
  const QImage image =
      renderWidget(widget, grid.columns * grid.cell_aspect_width * scale,
                   grid.rows * grid.cell_aspect_height * scale);

  // The band is a translucent red hatch, so the row is no longer plain black.
  bool banded = false;
  const int cell_h = grid.cell_aspect_height * scale;
  const int cell_w = grid.cell_aspect_width * scale;
  for (int y = cell_h; y < 2 * cell_h && !banded; ++y) {
    for (int x = 0; x < cell_w; ++x) {
      if (image.pixelColor(x, y).red() > 0) {
        banded = true;
        break;
      }
    }
  }
  EXPECT_TRUE(banded) << "a lost packet left its row unmarked";

  // The overlay is off by default: the marks are not part of the page.
  CatalogueCellGridWidget plain;
  plain.setGrid(grid);
  const QImage unmarked =
      renderWidget(plain, grid.columns * grid.cell_aspect_width * scale,
                   grid.rows * grid.cell_aspect_height * scale);
  EXPECT_EQ(cellCentre(unmarked, grid, /*row=*/1, /*column=*/0),
            QColor(Qt::black));
}

// --- Flashing and blinking ------------------------------------------------

// ETSI EN 300 706 §12.2 code 0/8 alternates the foreground pixels of the
// characters that follow it between the foreground and background colours;
// everything not flagged carries on being drawn.
TEST(CatalogueCellGridWidgetTest, FlashBlanksOnlyTheFlaggedCharacters) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("AB", /*rows=*/2, /*columns=*/2);
  grid.cells[0].flash = true;

  const QImage lit = renderGrid(grid);
  EXPECT_TRUE(cellHasInk(lit, grid, /*row=*/0, /*column=*/0));
  EXPECT_TRUE(cellHasInk(lit, grid, /*row=*/0, /*column=*/1));

  const QImage blank = renderGrid(grid, /*flash_lit=*/false);
  EXPECT_FALSE(cellHasInk(blank, grid, /*row=*/0, /*column=*/0))
      << "a flashing character was still drawn in the blank phase";
  EXPECT_TRUE(cellHasInk(blank, grid, /*row=*/0, /*column=*/1))
      << "a steady character was blanked along with its flashing neighbour";
}

// Only the foreground alternates: the cell keeps its background through the
// blank phase, so a flashing character on a coloured strip does not punch a
// hole in the strip.
TEST(CatalogueCellGridWidgetTest, FlashLeavesTheCellBackgroundAlone) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("A", /*rows=*/1, /*columns=*/1);
  grid.palette = {orc::CatalogueColour{0, 0, 0},
                  orc::CatalogueColour{255, 255, 255},
                  orc::CatalogueColour{255, 0, 0}};
  grid.cells[0].flash = true;
  grid.cells[0].background = 2;

  const QImage blank = renderGrid(grid, /*flash_lit=*/false);
  EXPECT_EQ(cellCentre(blank, grid, /*row=*/0, /*column=*/0),
            QColor(255, 0, 0));
}

// The clock is only worth running for a page something on which actually
// changes between the phases.
TEST(CatalogueCellGridWidgetTest, OnlyDrawnCharactersCountAsFlashing) {
  ensureApplication();
  CatalogueCellGridWidget widget;

  orc::CatalogueCellGrid grid = makeGrid("A", /*rows=*/1, /*columns=*/2);
  widget.setGrid(grid);
  EXPECT_FALSE(widget.hasFlashingCells());

  // The attribute is set-after and runs to the end of the row, so it lands on
  // the trailing SPACEs too — and a SPACE draws nothing in either phase.
  grid.cells[1].flash = true;
  widget.setGrid(grid);
  EXPECT_FALSE(widget.hasFlashingCells());

  grid.cells[0].flash = true;
  widget.setGrid(grid);
  EXPECT_TRUE(widget.hasFlashingCells());

  // A concealed cell displays as SPACE until revealed, and there is no reveal
  // control here.
  grid.cells[0].concealed = true;
  widget.setGrid(grid);
  EXPECT_FALSE(widget.hasFlashingCells());
}

// A catalogue browser left open must not tick forever: the clock is held only
// while a visible view has something to animate.
TEST(CatalogueCellGridWidgetTest, TheClockRunsOnlyForAVisibleFlashingPage) {
  ensureApplication();

  CatalogueFlashClock clock;
  CatalogueCellGridWidget widget;
  widget.setFlashClock(&clock);

  orc::CatalogueCellGrid grid = makeGrid("A", /*rows=*/1, /*columns=*/1);
  grid.cells[0].flash = true;
  widget.setGrid(grid);
  EXPECT_FALSE(clock.running()) << "a hidden view has no reader to flash for";

  widget.show();
  QApplication::processEvents();
  EXPECT_TRUE(clock.running());
  EXPECT_EQ(clock.subscribers(), 1);

  widget.setAnimationsEnabled(false);
  EXPECT_FALSE(clock.running());
  EXPECT_TRUE(widget.flashLit())
      << "a page held still should show its flashing text, not hide it";

  widget.setAnimationsEnabled(true);
  EXPECT_TRUE(clock.running());

  widget.hide();
  QApplication::processEvents();
  EXPECT_FALSE(clock.running());

  // A page with nothing flashing on it does not start the clock either.
  widget.show();
  QApplication::processEvents();
  widget.setGrid(makeGrid("A", /*rows=*/1, /*columns=*/1));
  EXPECT_FALSE(clock.running());
}

TEST(CatalogueFlashClockTest, RunsWhileAnyViewIsSubscribed) {
  ensureApplication();

  CatalogueFlashClock clock;
  EXPECT_FALSE(clock.running());
  EXPECT_TRUE(clock.lit());

  clock.acquire();
  clock.acquire();
  EXPECT_EQ(clock.subscribers(), 2);
  EXPECT_TRUE(clock.running());

  clock.release();
  EXPECT_TRUE(clock.running()) << "a view is still animating";
  clock.release();
  EXPECT_FALSE(clock.running());
  EXPECT_TRUE(clock.lit()) << "the still form of a flash is the lit one";

  // An unbalanced release must not underflow the count and leave the clock
  // unable to start again.
  clock.release();
  EXPECT_EQ(clock.subscribers(), 0);
  clock.acquire();
  EXPECT_TRUE(clock.running());
}

namespace {

/// A display list holding one filled square in |colour|, blinking to
/// |blink_to| when |blinking|.
orc::CatalogueDisplayList makeBlinkingSquare(
    const orc::CatalogueColour& colour, const orc::CatalogueColour& blink_to,
    bool blinking = true) {
  orc::CatalogueDisplayList list;
  list.aspect_height = 1.0;
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.origin = orc::CataloguePoint{0.1, 0.1};
  op.size = orc::CatalogueSize{0.5, 0.5};
  op.filled = true;
  op.colour = colour;
  op.blinking = blinking;
  op.blink_to = blink_to;
  list.ops.push_back(op);
  return list;
}

/// Colour of a pixel well inside that square.
QColor squareCentre(const QImage& image) {
  return image.pixelColor(image.width() / 3, image.height() / 2);
}

}  // namespace

// The NAPLPS blink process alternates a colour map entry with the blink-to
// entry the service named, so the other phase is a second colour rather than
// the figure going away.
TEST(CatalogueDisplayListWidgetTest, BlinkingOpsTakeTheirBlinkToColour) {
  ensureApplication();

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(makeBlinkingSquare(orc::CatalogueColour{0, 140, 220},
                                           orc::CatalogueColour{255, 210, 0}));
  EXPECT_TRUE(widget.hasBlinkingOps());

  EXPECT_EQ(squareCentre(renderWidget(widget, 256, 256)), QColor(0, 140, 220));
  widget.setFlashLit(false);
  EXPECT_EQ(squareCentre(renderWidget(widget, 256, 256)), QColor(255, 210, 0))
      << "the other phase blanked the figure instead of recolouring it";
}

// Where the service means the figure to disappear it says so by blinking to
// the ground colour, which is what the C1 BLINK START of X3.110 §6.2.8.1 does
// and what the default black stands for.
TEST(CatalogueDisplayListWidgetTest, BlinkingToBlackStillClearsTheFigure) {
  ensureApplication();

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(makeBlinkingSquare(orc::CatalogueColour{255, 255, 255},
                                           orc::CatalogueColour{0, 0, 0}));

  const auto lit_pixels = [](const QImage& image) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width(); ++x) {
        if (image.pixelColor(x, y) != QColor(Qt::black)) {
          ++count;
        }
      }
    }
    return count;
  };

  EXPECT_GT(lit_pixels(renderWidget(widget, 256, 256)), 0);
  widget.setFlashLit(false);
  EXPECT_EQ(lit_pixels(renderWidget(widget, 256, 256)), 0);
}

// A non-blinking operation is untouched by the phase, whatever blink_to holds.
TEST(CatalogueDisplayListWidgetTest, ASteadyOpIgnoresTheBlinkPhase) {
  ensureApplication();

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(makeBlinkingSquare(orc::CatalogueColour{0, 140, 220},
                                           orc::CatalogueColour{255, 210, 0},
                                           /*blinking=*/false));
  EXPECT_FALSE(widget.hasBlinkingOps());

  EXPECT_EQ(squareCentre(renderWidget(widget, 256, 256)), QColor(0, 140, 220));
  widget.setFlashLit(false);
  EXPECT_EQ(squareCentre(renderWidget(widget, 256, 256)), QColor(0, 140, 220));
}

// A blink process is defined over a colour map entry, so it survives the page
// being deposited as pixels: the runs carrying that entry are what alternate,
// and they do it without losing the hard edges that make them pixels.
TEST(CatalogueDisplayListWidgetTest, PixelRunsBlinkWithTheirColourMapEntry) {
  ensureApplication();

  orc::CatalogueDisplayList list;
  list.aspect_height = 0.78125;
  list.display_aspect_height = 0.75;
  list.nominal_width = 16;
  list.nominal_height = 12;
  list.pixel_aligned = true;
  const double pitch_x = 1.0 / 16.0;
  const double pitch_y = 0.78125 / 12.0;
  for (int row = 0; row < 12; ++row) {
    orc::CatalogueDrawOp op;
    op.kind = orc::CatalogueDrawKind::kRectangle;
    op.filled = true;
    op.origin = orc::CataloguePoint{0.0, row * pitch_y};
    op.size = orc::CatalogueSize{16 * pitch_x, pitch_y};
    op.colour = orc::CatalogueColour{0, 140, 220};
    // Only the lower half of the page is on the blinking entry, so the two
    // phases have to differ in one place and not the other.
    op.blinking = row < 6;
    op.blink_to = orc::CatalogueColour{255, 210, 0};
    list.ops.push_back(op);
  }

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  EXPECT_TRUE(widget.hasBlinkingOps());

  const auto sample = [&widget](bool lit, double height_fraction) {
    widget.setFlashLit(lit);
    const QImage image = renderWidget(widget, 256, 192);
    return image.pixelColor(image.width() / 2,
                            static_cast<int>(image.height() * height_fraction));
  };

  // Unit y runs upwards, so four fifths of the way down the image is grid row
  // 2 — in the blinking lower half — and one fifth down is row 9, in the
  // steady upper.
  EXPECT_EQ(sample(true, 0.8), QColor(0, 140, 220));
  EXPECT_EQ(sample(false, 0.8), QColor(255, 210, 0));
  EXPECT_EQ(sample(true, 0.2), QColor(0, 140, 220));
  EXPECT_EQ(sample(false, 0.2), QColor(0, 140, 220))
      << "a run not on the blinking entry changed phase with it";
}

// The switch belongs to the payload views, so it is offered where one of them
// is on display and nowhere else.
TEST(CatalogueDialogTest, AnimationSwitchIsOfferedForDrawnPayloadsOnly) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(1));

  auto* check = dialog.findChild<QCheckBox*>("catalogueAnimationsCheck");
  ASSERT_NE(check, nullptr);
  EXPECT_TRUE(check->isVisibleTo(&dialog));
  EXPECT_TRUE(check->isChecked()) << "a page is transmitted animated";

  auto* grid = dialog.findChild<CatalogueCellGridWidget*>("catalogueCellGrid");
  auto* display =
      dialog.findChild<CatalogueDisplayListWidget*>("catalogueDisplayList");
  ASSERT_NE(grid, nullptr);
  ASSERT_NE(display, nullptr);

  // One switch covers both payload views, so a reader is not asked twice.
  check->setChecked(false);
  EXPECT_FALSE(grid->animationsEnabled());
  EXPECT_FALSE(display->animationsEnabled());
  check->setChecked(true);
  EXPECT_TRUE(grid->animationsEnabled());
  EXPECT_TRUE(display->animationsEnabled());

  dialog.setCatalogue(makeFlatCatalogue());
  dialog.selectItem(0);  // a display list
  EXPECT_TRUE(check->isVisibleTo(&dialog));
  dialog.selectItem(1);  // a text listing, which cannot animate
  EXPECT_FALSE(check->isVisibleTo(&dialog));

  dialog.close();
}

// --- Saving the display as an image ---------------------------------------

namespace {

/// Whether anything was drawn inside |area| of |image|, against the black the
/// display sits on
bool hasInk(const QImage& image, const QRect& area) {
  for (int y = area.top(); y <= area.bottom(); ++y) {
    for (int x = area.left(); x <= area.right(); ++x) {
      if (image.pixelColor(x, y) != QColor(Qt::black)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

// A saved page is whole multiples of the character rectangle at the grid's own
// aspect: a fractional scale would put a cell boundary inside a pixel, and a
// size off the aspect would letterbox the page inside its own image.
TEST(CatalogueCellGridWidgetTest, SavedImageIsWholeCharacterRectangles) {
  ensureApplication();
  const orc::CatalogueCellGrid grid = makeGrid("100");

  CatalogueCellGridWidget widget;
  widget.setGrid(grid);

  // 8 x 4 cells of 12 x 20, scaled by 8 — the smallest whole scale that puts
  // the page past the 720-pixel minimum width.
  const QSize size = widget.pageImageSize();
  EXPECT_EQ(size, QSize(768, 640));

  const QImage image = widget.renderPageImage(size);
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.size(), size);

  // The text of the payload is on the top row and nowhere else, which is only
  // true if the page filled the image rather than being centred inside it.
  const int cell_w = size.width() / grid.columns;
  const int cell_h = size.height() / grid.rows;
  EXPECT_TRUE(hasInk(image, QRect(0, 0, cell_w, cell_h)));
  EXPECT_FALSE(hasInk(image, QRect(0, cell_h, size.width(), cell_h)));
}

// The blank phase of a flash is a moment of the transmission, not the page: a
// still caught in it would be missing the text the service chose to flash.
TEST(CatalogueCellGridWidgetTest, SavedImageHoldsTheLitPhase) {
  ensureApplication();
  orc::CatalogueCellGrid grid = makeGrid("A", 2, 2);
  grid.cells[0].flash = true;

  CatalogueCellGridWidget widget;
  widget.setGrid(grid);
  widget.setFlashLit(false);
  ASSERT_TRUE(widget.hasFlashingCells());

  const QImage image = widget.renderPageImage(widget.pageImageSize());
  ASSERT_FALSE(image.isNull());
  const int cell_w = image.width() / grid.columns;
  const int cell_h = image.height() / grid.rows;
  EXPECT_TRUE(hasInk(image, QRect(0, 0, cell_w, cell_h)))
      << "the flashed character is part of the page";
}

TEST(CatalogueDisplayListWidgetTest, SavedImageFollowsThePayloadAspect) {
  ensureApplication();
  orc::CatalogueDisplayList list;
  list.aspect_height = 0.78125;
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.origin = orc::CataloguePoint{0.0, 0.0};
  op.size = orc::CatalogueSize{1.0, 0.78125};
  op.filled = true;
  op.colour = orc::CatalogueColour{255, 255, 255};
  list.ops.push_back(op);

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);

  const QSize size = widget.pageImageSize();
  EXPECT_EQ(size.width(), 720);
  EXPECT_NEAR(static_cast<double>(size.height()) / size.width(), 0.78125, 0.01);

  // The drawable area is the whole image at that aspect, so an operation
  // covering unit space covers every corner of it.
  const QImage image = widget.renderPageImage(size);
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.pixelColor(1, 1), QColor(Qt::white));
  EXPECT_EQ(image.pixelColor(size.width() - 2, size.height() - 2),
            QColor(Qt::white));
}

// Every page that names the grid it was drawn against is saved at one size,
// whatever grid that was. Comparing the same page at two receivers is what the
// choice of receiver is for, and two images of different sizes would make the
// comparison about the sizes. The size is a whole multiple of every grid
// across, so a column of any of them lands on a whole number of image columns.
TEST(CatalogueDisplayListWidgetTest, SavedPixelPageIsOneSizeForEveryGrid) {
  ensureApplication();

  const std::vector<std::pair<int, int>> grids = {
      {256, 200}, {512, 400}, {768, 600}};

  for (const auto& [nominal_width, nominal_height] : grids) {
    orc::CatalogueDisplayList list;
    list.aspect_height = 0.78125;
    list.display_aspect_height = 0.75;
    list.nominal_width = nominal_width;
    list.nominal_height = nominal_height;
    list.pixel_aligned = true;
    orc::CatalogueDrawOp op;
    op.kind = orc::CatalogueDrawKind::kRectangle;
    op.filled = true;
    op.origin = orc::CataloguePoint{0.0, 0.0};
    op.size = orc::CatalogueSize{1.0, 0.78125};
    op.colour = orc::CatalogueColour{255, 255, 255};
    list.ops.push_back(op);

    CatalogueDisplayListWidget widget;
    widget.setDisplayList(list);

    const QSize size = widget.pageImageSize();
    EXPECT_EQ(size, QSize(1536, 1152))
        << "grid " << nominal_width << "x" << nominal_height;
    EXPECT_EQ(size.width() % nominal_width, 0)
        << "a column of the grid does not land on whole image columns";
    EXPECT_NEAR(static_cast<double>(size.height()) / size.width(), 0.75, 0.002)
        << "the saved page is not the shape it is displayed at";
  }
}

// A vector page names the grid it was resolved against as well, so it is saved
// at the same size as the pixel pages: the point of the vector mode is to be
// held up against them.
TEST(CatalogueDisplayListWidgetTest, SavedVectorPageSharesTheSize) {
  ensureApplication();
  orc::CatalogueDisplayList list;
  list.aspect_height = 0.78125;
  list.display_aspect_height = 0.75;
  list.nominal_width = 512;
  list.nominal_height = 400;
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kLine;
  op.points = {orc::CataloguePoint{0.1, 0.1}, orc::CataloguePoint{0.9, 0.6}};
  op.colour = orc::CatalogueColour{255, 255, 255};
  list.ops.push_back(op);

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  EXPECT_EQ(widget.pageImageSize(), QSize(1536, 1152));

  // A list naming no grid has nothing to line up with and keeps the plain
  // width it always had.
  list.nominal_width = 0;
  list.nominal_height = 0;
  widget.setDisplayList(list);
  EXPECT_EQ(widget.pageImageSize().width(), 720);
}

// Saved at a whole multiple, the pixels of the source stay hard-edged: nothing
// between the two colours of a boundary means nothing was smoothed.
TEST(CatalogueDisplayListWidgetTest, SavedPixelPageHasUnsmoothedEdges) {
  ensureApplication();

  orc::CatalogueDisplayList list;
  list.aspect_height = 0.78125;
  list.display_aspect_height = 0.75;
  list.nominal_width = 256;
  list.nominal_height = 200;
  list.pixel_aligned = true;
  // A chequer of single grid pixels, which is the hardest thing to keep sharp.
  const double pitch_x = 1.0 / 256.0;
  const double pitch_y = 0.78125 / 200.0;
  for (int row = 0; row < 200; ++row) {
    for (int column = row % 2; column < 256; column += 2) {
      orc::CatalogueDrawOp op;
      op.kind = orc::CatalogueDrawKind::kRectangle;
      op.filled = true;
      op.origin = orc::CataloguePoint{column * pitch_x, row * pitch_y};
      op.size = orc::CatalogueSize{pitch_x, pitch_y};
      op.colour = orc::CatalogueColour{255, 255, 255};
      list.ops.push_back(op);
    }
  }

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  const QImage image = widget.renderPageImage(widget.pageImageSize());
  ASSERT_FALSE(image.isNull());

  int blended = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const int red = image.pixelColor(x, y).red();
      if (red > 0 && red < 255) {
        ++blended;
      }
    }
  }
  EXPECT_EQ(blended, 0) << "the receiver's pixels were smoothed away";
}

// Only a drawn payload can be saved; a listing or a table is text on screen and
// there is nothing to render.
TEST(CatalogueDialogTest, SaveIsOfferedForDrawnPayloadsOnly) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(1));

  auto* button = dialog.findChild<QToolButton*>("catalogueSavePageButton");
  ASSERT_NE(button, nullptr);
  EXPECT_TRUE(button->isVisibleTo(&dialog));
  EXPECT_TRUE(dialog.canSavePage());

  dialog.setCatalogue(makeFlatCatalogue());
  dialog.selectItem(0);  // a display list
  EXPECT_TRUE(button->isVisibleTo(&dialog));
  EXPECT_TRUE(dialog.canSavePage());

  dialog.selectItem(1);  // a text listing
  EXPECT_FALSE(button->isVisibleTo(&dialog));
  EXPECT_FALSE(dialog.canSavePage());
  EXPECT_TRUE(dialog.renderedPageImage().isNull());

  dialog.selectItem(2);  // a table
  EXPECT_FALSE(button->isVisibleTo(&dialog));
  EXPECT_FALSE(dialog.canSavePage());

  dialog.close();
}

TEST(CatalogueDialogTest, SavedImageIsThePayloadOnDisplay) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(2));
  dialog.selectItem(1);

  const QImage grid_image = dialog.renderedPageImage();
  ASSERT_FALSE(grid_image.isNull());
  EXPECT_EQ(grid_image.size(), QSize(768, 640));

  dialog.setCatalogue(makeFlatCatalogue());
  dialog.selectItem(0);
  const QImage list_image = dialog.renderedPageImage();
  ASSERT_FALSE(list_image.isNull());
  EXPECT_EQ(list_image.width(), 720);
}

// The suggested name says which display it is, in the service's own terms, with
// the punctuation a file name cannot carry turned into separators.
TEST(CatalogueDialogTest, SuggestedFileNameNamesTheDisplay) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(3));

  dialog.selectItem(1);
  EXPECT_EQ(dialog.suggestedPageFileName(), "Page-101-0000.png");

  // "000/1A4 v0" would otherwise be read as a directory to save into.
  dialog.setCatalogue(makeFlatCatalogue());
  dialog.selectItem(0);
  EXPECT_EQ(dialog.suggestedPageFileName(), "Record-000-1A4-v0.png");
}

// --- Saving every display at once -------------------------------------------

// The batch save works on the catalogue rather than the display: it stands
// whenever anything drawn was recovered, whatever the reader happens to be
// looking at, and only a catalogue with nothing to render takes it away.
TEST(CatalogueDialogTest, SaveAllIsOfferedWheneverAnythingDrawnWasRecovered) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(2));

  auto* button = dialog.findChild<QToolButton*>("catalogueSaveAllButton");
  ASSERT_NE(button, nullptr);
  EXPECT_TRUE(button->isVisibleTo(&dialog));
  EXPECT_TRUE(dialog.canSaveAllPages());

  // Still offered while a text listing is on screen — the single save is not.
  dialog.setCatalogue(makeFlatCatalogue());
  dialog.selectItem(1);
  EXPECT_FALSE(dialog.canSavePage());
  EXPECT_TRUE(button->isVisibleTo(&dialog));
  EXPECT_TRUE(dialog.canSaveAllPages());

  // A catalogue of nothing but text has nothing for the batch to write.
  orc::CatalogueDataset text_only;
  text_only.schema.item_noun = "Record";
  orc::CatalogueItem item;
  item.id = "A";
  item.values = {"A"};
  text_only.items.push_back(item);
  orc::CataloguePayload payload;
  payload.kind = orc::CataloguePayload::Kind::kText;
  payload.document.text = "nothing drawn";
  text_only.payloads.push_back(payload);
  dialog.setCatalogue(text_only);
  EXPECT_FALSE(button->isVisibleTo(&dialog));
  EXPECT_FALSE(dialog.canSaveAllPages());

  dialog.close();
}

// The batch lists every drawn payload under the name the single save would
// have suggested for it, and skips the payloads that are text on screen.
TEST(CatalogueDialogTest, ExportablePagesNameEveryDrawnPayload) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(3));

  const auto pages = dialog.exportablePages();
  ASSERT_EQ(pages.size(), 3u);
  EXPECT_EQ(pages[0].file_name, "Page-100-0000.png");
  EXPECT_EQ(pages[1].file_name, "Page-101-0000.png");
  EXPECT_EQ(pages[2].file_name, "Page-102-0000.png");

  // The flat catalogue carries a display list, a text listing and a table;
  // only the display list is drawn.
  dialog.setCatalogue(makeFlatCatalogue());
  const auto records = dialog.exportablePages();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].file_name, "Record-000-1A4-v0.png");
}

// Two keys that sanitise to the same file name would overwrite each other on
// disk, so the later ones are numbered.
TEST(CatalogueDialogTest, ExportablePagesKeepCollidingNamesApart) {
  ensureApplication();
  orc::CatalogueDataset data;
  data.schema.item_noun = "Page";
  for (const std::string& key : {"10/0", "10.0"}) {
    orc::CatalogueItem item;
    item.id = key;
    item.find_key = key;
    item.values = {key};
    data.items.push_back(item);
    orc::CataloguePayload payload;
    payload.kind = orc::CataloguePayload::Kind::kCellGrid;
    payload.grid = makeGrid(key);
    data.payloads.push_back(std::move(payload));
  }

  CatalogueDialog dialog;
  dialog.setCatalogue(data);
  const auto pages = dialog.exportablePages();
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0].file_name, "Page-10-0.png");
  EXPECT_EQ(pages[1].file_name, "Page-10-0-2.png");
}

// What the batch writes for a page is the same image the single save would
// have written with that page on screen — and rendering it does not disturb
// what actually is on screen.
TEST(CatalogueDialogTest, BatchImageMatchesTheSingleSave) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(2));
  dialog.selectItem(1);  // page 101, whose variant is dataset entry 3

  const QImage on_screen = dialog.renderedPageImage();
  ASSERT_FALSE(on_screen.isNull());
  const QImage batch = dialog.renderedPageImageAt(3);
  ASSERT_FALSE(batch.isNull());
  EXPECT_EQ(batch, on_screen);

  // The display is still page 101 afterwards.
  EXPECT_EQ(dialog.suggestedPageFileName(), "Page-101-0000.png");
  EXPECT_EQ(dialog.renderedPageImage(), on_screen);

  // A text payload has nothing to render.
  dialog.setCatalogue(makeFlatCatalogue());
  EXPECT_TRUE(dialog.renderedPageImageAt(1).isNull());
}

}  // namespace gui_unit_test
