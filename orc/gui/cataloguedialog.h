/*
 * File:        cataloguedialog.h
 * Module:      orc-gui
 * Purpose:     Generic browser for any stage that exposes ICatalogueResults
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef CATALOGUEDIALOG_H
#define CATALOGUEDIALOG_H

#include <orc/stage/tooling/catalogue_results.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QString>
#include <QTableWidget>
#include <QToolButton>
#include <QWidget>
#include <cstdint>
#include <string>
#include <vector>

class CatalogueCellGridWidget;
class CatalogueDisplayListWidget;
class CatalogueFlashClock;

/**
 * @brief Browser for the catalogue a stage produced from one trigger run
 *
 * The stage tool of any node whose stage advertises
 * StageToolKind::CatalogueBrowser: triggering the node decodes the whole frame
 * range in a single pass and this dialogue shows what it found. It does not
 * follow the previewer — a catalogue describes the entire source, so there is
 * no window to slide and nothing to accumulate here.
 *
 * The catalogue is a table of items on the left and the selected item's payload
 * on the right. What the columns are called, what one item is called, whether
 * there is a find box, whether items have variants to step through — all of it
 * comes from the dataset, so this dialogue knows nothing about teletext, NABTS
 * or whatever service arrives next.
 *
 * An item may nest one level: a variant is a version of its parent that the
 * service cycles through, stepped under the payload rather than listed
 * separately. Selecting a parent shows its first variant.
 *
 * MainWindow drives it: setCatalogue() with whatever the coordinator delivered
 * for the node, and clearContent() when there is nothing to show.
 */
class CatalogueDialog : public QDialog {
  Q_OBJECT

 public:
  explicit CatalogueDialog(QWidget* parent = nullptr);
  ~CatalogueDialog() override;

  /// Show a "decoding" pending state while the stage trigger is in flight
  void showPending();

  /// Replace the pending state with why no catalogue is coming
  void showError(const QString& message);

  /// Clear the payload, the item list and the readouts
  void clearContent();

  /// Show one trigger run's catalogue, replacing whatever was displayed
  void setCatalogue(const orc::CatalogueDataset& data);

 signals:
  /// The reader changed how the catalogue is presented — picked one of
  /// CatalogueSchema::view_options, or turned one of its toggles on or off. The
  /// dialogue has no way of building the catalogue again itself, so the owner
  /// asks the stage for it under @p option_id with @p active_toggles and
  /// delivers it back through setCatalogue(). Both travel together because the
  /// stage needs the whole selection to build anything, not the part that
  /// changed.
  void viewSelectionChanged(const QString& option_id,
                            const QStringList& active_toggles);

 public:
  // ---- Test seams --------------------------------------------------------

  /// First-column values of the listed items, in table order
  std::vector<QString> listedItems() const;
  /// Value of |column| for the row whose first column is |key|, or empty
  QString listedValue(const QString& key, int column) const;
  /// Index of the listed item on display, or -1 when none is
  int currentItemIndex() const { return current_row_; }
  void selectItem(int index);
  void showNextItem();
  void showPreviousItem();

  /// Find-box text (empty when the schema offers no find box)
  QString findText() const;
  void setFindText(const QString& text);

  /// Ids of the view options offered, in order (empty when none are)
  std::vector<QString> viewOptions() const;
  /// Id of the view option on show, or empty when no dropdown is offered
  QString currentViewOption() const;
  /// Pick a view option by id, as the reader would (no-op for an unknown id)
  void selectViewOption(const QString& option_id);

  /// Whether the text pane beside a drawn payload is on show (false when the
  /// payload on display carries no text to show)
  bool isTextPaneVisible() const;
  /// Show or hide the text pane, as the reader would
  void setTextPaneVisible(bool visible);

  /// Ids of the toggles offered, in order (empty when none are)
  std::vector<QString> viewToggles() const;
  /// Whether the toggle |toggle_id| is on (false for an unknown id)
  bool isToggleActive(const QString& toggle_id) const;
  /// Turn a toggle on or off by id, as the reader would (no-op for an unknown
  /// id, and for a value it is already at)
  void setToggleActive(const QString& toggle_id, bool active);

  /// Variant readout — "Sub-page 2 of 8 (0002)" — for the displayed item
  /// (empty when the variant bar is hidden)
  QString variantText() const;
  int variantCount() const;
  int variantIndex() const;
  void showNextVariant();
  void showPreviousVariant();

  /// Message-panel readouts (empty when hidden)
  QString headlineText() const;
  QString conditionText() const;
  QString summaryText() const;
  QString noticeText() const;

  /// Whether the payload on display is a drawn one, and so can be saved as an
  /// image. A listing or a table is text and there is nothing to render.
  bool canSavePage() const;

  /// The image a save would write, at the payload's own size (test seam; null
  /// when the payload on display is not a drawn one)
  QImage renderedPageImage() const;

  /// The file name the save dialogue opens on, derived from what is displayed —
  /// "Page-100-0002.png" (test seam)
  QString suggestedPageFileName() const;

  /// Whether the catalogue holds any drawn payload at all, which is what a
  /// "save all" has to work on. Independent of what is on display: a reader
  /// looking at a text listing can still save every page the service carried.
  bool canSaveAllPages() const;

  /// One display a "save all" would write: which dataset entry it renders, and
  /// the file name it gets — the same name the single save would suggest, made
  /// unique within the batch.
  struct PageExport {
    size_t index;
    QString file_name;
  };

  /// Every drawn payload in the catalogue, in dataset order, each with a
  /// unique file name (test seam). Listings and tables are text and are not
  /// included.
  std::vector<PageExport> exportablePages() const;

  /// The image a "save all" would write for dataset entry |index|, rendered
  /// off screen without disturbing what is on display (test seam; null when
  /// that entry's payload is not a drawn one)
  QImage renderedPageImageAt(size_t index) const;

  /// The payload on display (test seams; nullptr when none is)
  const orc::CatalogueCellGrid* currentGrid() const;
  const orc::CatalogueDisplayList* currentDisplayList() const;
  /// Text of a text payload, or the companion text beside a visual one
  QString currentText() const;
  /// Rows of a table payload, one joined string per row
  std::vector<QString> listedTableRows() const;

 private slots:
  void onFindTextChanged();
  void onItemSelected();
  void onHighlightToggled(bool checked);
  void onAnimationsToggled(bool checked);
  void onShowTextToggled(bool checked);
  void onViewOptionSelected(int index);
  void onViewToggled();
  /// Emit the whole presentation selection — view option and active toggles.
  void emitViewSelection();
  void onSavePageClicked();
  void onSaveAllClicked();

 private:
  /// Which pane the payload side is showing
  enum PayloadPage {
    kPageNothing = 0,
    kPageCellGrid = 1,
    kPageDisplayList = 2,
    kPageText = 3,
    kPageTable = 4,
  };

  void setupUI();
  /// Fill the view dropdown from the current schema, hiding it where the
  /// service offers only one way of presenting its items
  void refreshViewOptions();
  /// Rebuild the item table and the notices from the current dataset
  void refreshItemList();
  /// Show the top-level item at |row| of the table, or nothing when out of
  /// range
  void showItem(int row);
  /// Step |delta| top-level items, wrapping at either end
  void stepItem(int delta);
  /// Show variant |variant_index_| of the displayed item
  void renderPayload();
  /// Step |delta| variants of the displayed item, wrapping at either end
  void stepVariant(int delta);
  /// Update the variant stepper for the item currently displayed
  void refreshVariantControl();

  /// Indices into data_.items of the top-level items, in listing order
  const std::vector<size_t>& topLevelRows() const { return top_level_; }
  /// Indices into data_.items of the variants of top-level row |row|
  std::vector<size_t> variantsOf(int row) const;
  /// The dataset index the payload pane is showing, or SIZE_MAX
  size_t displayedIndex() const;
  /// The file name the display at dataset index |index| would save under
  QString pageFileNameFor(size_t index) const;

  orc::CatalogueDataset data_;
  bool has_data_ = false;

  // Listing order: the dataset's own order, parents only.
  std::vector<size_t> top_level_;
  // Which top-level row is selected, and which of its variants is on screen.
  int current_row_ = -1;
  int variant_index_ = 0;

  // Set while the table is being rebuilt or programmatically selected, so
  // selection changes do not feed back into the find box.
  bool updating_list_ = false;
  // Set while the view dropdown is being filled from a delivered schema, so
  // showing what the stage built does not read as the reader asking for it.
  bool updating_view_ = false;

  QWidget* find_bar_ = nullptr;
  QLabel* find_label_ = nullptr;
  QLineEdit* find_edit_ = nullptr;
  QCheckBox* highlight_check_ = nullptr;
  QCheckBox* animations_check_ = nullptr;
  QCheckBox* show_text_check_ = nullptr;
  QWidget* view_bar_ = nullptr;
  QLabel* view_label_ = nullptr;
  QComboBox* view_combo_ = nullptr;
  /// One checkbox per CatalogueSchema::toggles entry, rebuilt whenever a
  /// delivered schema offers a different set. Each carries its opaque id in
  /// its object property so nothing here has to know what a toggle means.
  std::vector<QCheckBox*> toggle_checks_;
  /// Layout the checkboxes are added to, beside the view dropdown.
  class QHBoxLayout* view_layout_ = nullptr;
  /// The rule that sets the switches changing what is on screen apart from the
  /// button that does something with it. Shown only while the button is.
  QWidget* action_separator_ = nullptr;
  QToolButton* save_page_button_ = nullptr;
  QToolButton* save_all_button_ = nullptr;
  QToolButton* prev_item_button_ = nullptr;
  QToolButton* next_item_button_ = nullptr;
  QWidget* item_nav_ = nullptr;

  QLabel* list_heading_ = nullptr;
  QTableWidget* items_table_ = nullptr;

  QStackedWidget* payload_stack_ = nullptr;
  // One clock for every payload view, so a page and a display list flash in
  // step and stepping between them does not restart the cycle.
  CatalogueFlashClock* flash_clock_ = nullptr;
  CatalogueCellGridWidget* grid_widget_ = nullptr;
  CatalogueDisplayListWidget* display_widget_ = nullptr;
  QPlainTextEdit* companion_pane_ = nullptr;
  QWidget* display_page_ = nullptr;
  QPlainTextEdit* text_pane_ = nullptr;
  QTableWidget* table_pane_ = nullptr;
  QLabel* empty_label_ = nullptr;

  QWidget* variant_bar_ = nullptr;
  QToolButton* prev_variant_button_ = nullptr;
  QToolButton* next_variant_button_ = nullptr;
  QLabel* variant_label_ = nullptr;

  /// Everything the dialogue has to say, in one place at the foot of the
  /// window: what the viewer is doing, what is on show, what the catalogue as a
  /// whole carries, and how the run went. Each line hides when it has nothing
  /// to say, and the panel with them when they all do.
  QWidget* message_panel_ = nullptr;
  QWidget* message_rule_ = nullptr;
  QLabel* status_label_ = nullptr;
  QLabel* headline_label_ = nullptr;
  QLabel* condition_label_ = nullptr;
  QLabel* notice_label_ = nullptr;
  QLabel* summary_label_ = nullptr;

  /// Refresh the message panel's visibility from the lines inside it.
  void refreshMessagePanel();
};

#endif  // CATALOGUEDIALOG_H
