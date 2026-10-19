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

#include <QByteArray>
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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CatalogueCellGridWidget;
class CatalogueDisplayListWidget;
class CatalogueFlashClock;
class CatalogueKeypad;
class CataloguePlaybackBar;
class ICatalogueAnimationClock;

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
  /// Plays a timed display list against @p clock rather than the platform's
  /// monotonic clock; a null clock means the platform's.
  explicit CatalogueDialog(
      std::shared_ptr<const ICatalogueAnimationClock> clock,
      QWidget* parent = nullptr);
  ~CatalogueDialog() override;

  /// Show a "decoding" pending state while the stage trigger is in flight
  void showPending();

  /// Replace the pending state with why no catalogue is coming
  void showError(const QString& message);

  /// Clear the payload, the item list and the readouts
  void clearContent();

  /// Show one trigger run's catalogue, replacing whatever was displayed
  void setCatalogue(const orc::CatalogueDataset& data);

  /// Show what the stage answered for |keys| pressed on item |item_id|
  /// (keysPressed()). Ignored unless that item is still on display and those
  /// are still the keys pressed on it: an answer overtaken by another key, or
  /// by the reader moving on, is not what the reader is looking for.
  void setPlayedPayload(const QString& item_id, const QStringList& keys,
                        const orc::CataloguePayload& payload);

 public slots:
  /// Type |digit| of a page number, as the keypad does where it is not
  /// playing an item: added to the find box the way a decoder's entry filled
  /// — a new number starts once one has reached an item, or once the box is
  /// full
  void enterPageDigit(QChar digit);

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

  /// The reader pressed a key on an item that takes keys
  /// (orc::CatalogueItem::keys): |keys| is every key pressed on |item_id|
  /// since it was shown, in order. The dialogue cannot play the item itself,
  /// so the owner asks the stage what they show
  /// (orc::ICatalogueResults::played()) under the presentation on show,
  /// |option_id| and |active_toggles|, and delivers the answer back through
  /// setPlayedPayload(). Emitted again, with the same keys, when the reader
  /// changes the presentation, so a game in progress is redrawn the new way.
  void keysPressed(const QString& item_id, const QStringList& keys,
                   const QString& option_id, const QStringList& active_toggles);

 public:
  // ---- Test seams --------------------------------------------------------

  /// First-column values of the listed items, in table order
  std::vector<QString> listedItems() const;
  /// First-column values of the items the list shows, in table order: every
  /// listed item, unless the find box names a mark, when only those carrying
  /// it
  std::vector<QString> shownItems() const;
  /// The line over the list — "Records carried:", or how many carry the mark
  /// the find box names
  QString listHeadingText() const;
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
  /// Press Enter in the find box, with Shift where |backwards| (test seam)
  void pressFindEnter(bool backwards = false);

  /// Follow the first link of |kind| the item on display carries, as its
  /// button does; false where it carries none
  bool followLink(orc::CatalogueLink::Kind kind);
  /// Follow the link labelled |label| among the numbers the item on display
  /// offers (CatalogueLink::Kind::kReference); false where there is none
  bool followReference(const QString& label);
  /// Return to the item shown before the last move by link, by find box or by
  /// the list; false where there is nothing to return to
  bool goBack();
  /// The labels of the numbers the item on display offers, in order
  std::vector<QString> offeredReferences() const;
  /// Id of the item on display, a variant's own id where one is (empty when
  /// nothing is on display)
  QString displayedItemId() const;

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

  /// Whether the payload on display is a display list, and so can be saved as
  /// an SVG drawing. A cell grid is a raster of character cells, which the PNG
  /// already is.
  bool canSaveSvg() const;

  /// The SVG document a save would write for the display on screen — the
  /// presentation, animated (test seam; empty when no display list is on
  /// display)
  QByteArray renderedSvgDocument() const;

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

  /// Whether the catalogue holds any display list, which is what saving every
  /// display as SVG has to work on. Like the PNG batch, independent of what is
  /// on display.
  bool canSaveAllSvgs() const;

  /// Every display list in the catalogue, in dataset order, each with the name
  /// the single SVG save would suggest, made unique within the batch (test
  /// seam). Cell grids are rasters, which the PNG batch already writes.
  std::vector<PageExport> exportableSvgs() const;

  /// The SVG document the SVG batch would write for dataset entry |index| —
  /// the presentation, animated — rendered off screen without disturbing what
  /// is on display (test seam; empty when that entry is not a display list)
  QByteArray renderedSvgDocumentAt(size_t index) const;

  /// Whether the keypad is offered, which it is while an item that takes keys
  /// is on display (test seam)
  bool isKeypadVisible() const;
  /// The keypad (test seam)
  CatalogueKeypad* keypad() const { return keypad_; }
  /// Keys pressed on the item on display since it was shown, in order (test
  /// seam)
  QStringList playedKeys() const { return played_keys_; }
  /// Press |key| as the reader would, on the keypad or the keyboard; false
  /// where the item on display does not take it (test seam)
  bool pressKey(const QString& key);
  /// Start the item on display again: the Reset game action
  void resetGame();

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
  void onSaveSvgClicked();
  void onSaveAllClicked();
  void onSaveAllSvgClicked();
  void onKeypadKeyPressed(const QString& key);

 protected:
  /// Digits typed while the drawing has the focus play the item on display,
  /// as the keypad does; Enter in the find box while it names a mark shows
  /// the next item carrying it, and Shift+Enter the one before.
  bool eventFilter(QObject* watched, QEvent* event) override;

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
  /// Step |delta| of the top-level items the list shows, wrapping at either
  /// end
  void stepItem(int delta);
  /// Show variant |variant_index_| of the displayed item
  void renderPayload();
  /// Step |delta| variants of the displayed item, wrapping at either end
  void stepVariant(int delta);
  /// Update the variant stepper for the item currently displayed
  void refreshVariantControl();
  /// Offer the playback controls while a display list that changes as it is
  /// shown is on display with animation on, and take them away otherwise
  void refreshPlaybackBar();
  /// Offer the keypad while an item that takes keys is on display, with the
  /// keys it takes and those pressed on it, and take it away otherwise
  void refreshKeypad(size_t index);
  /// The ids of the toggles the reader has on
  QStringList activeToggles() const;

  /// Indices into data_.items of the top-level items, in listing order
  const std::vector<size_t>& topLevelRows() const { return top_level_; }
  /// Indices into data_.items of the variants of top-level row |row|
  std::vector<size_t> variantsOf(int row) const;
  /// The dataset index the payload pane is showing, or SIZE_MAX
  size_t displayedIndex() const;
  /// Whether |key| (already normalised) is |item|'s find key or one of its
  /// aliases
  static bool matchesKey(const orc::CatalogueItem& item, const QString& key);
  /// Whether |item| carries the mark |mark| (already normalised): one of its
  /// badges, whole or up to its colon — "GAME" for "Game: quiz"
  static bool carriesMark(const orc::CatalogueItem& item, const QString& mark);
  /// For each top-level row, the first of its variants that carries |mark|
  /// (0 where the row's own item does), or -1 where none does
  std::vector<int> markedRows(const QString& mark) const;
  /// List only the rows |marked| (markedRows() of |mark|) gives a variant,
  /// and say how many over the list; an empty |mark| lists every row again
  void setMarkFilter(const QString& mark, const std::vector<int>& marked = {});
  /// Show dataset entry |index|: a top-level item as itself, a variant as its
  /// parent with the variant stepped to; false for an index out of range
  bool showDatasetIndex(size_t index);
  /// Show top-level |row| at variant |variant|
  void showItemVariant(int row, int variant);
  /// Enable the link buttons and list the offered numbers for the item on
  /// display
  void refreshLinksBar();
  /// The file name the display at dataset index |index| would save under
  QString pageFileNameFor(size_t index) const;
  /// The payloads a batch writes — every drawn one, or display lists only —
  /// each named as the single save would name it, with |extension|, and made
  /// unique within the batch
  std::vector<PageExport> exportablesOf(bool display_lists_only,
                                        const QString& extension) const;
  /// Write |pages| into a folder the reader chooses, each with |write| (the
  /// dataset entry and the path; true once written), showing how far the
  /// batch has got and saying what could not be written. |title| names the
  /// dialogues, |what| the files ("images", "drawings").
  void saveBatch(const QString& title, const QString& what,
                 const std::vector<PageExport>& pages,
                 const std::function<bool(size_t, const QString&)>& write);
  /// A path the reader chooses to save |file_name| under, in the directory
  /// the application last exported into, with |extension| added where the
  /// reader left it off; the directory is remembered for the next save. Empty
  /// where the reader cancelled. |title| names the dialogue and |filter| the
  /// files it lists.
  QString chooseSavePath(const QString& title, const QString& file_name,
                         const QString& filter, const QString& extension);

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
  // Whether the find box's text reached an item: the next digit a keypad
  // enters then starts a new number (enterPageDigit()).
  bool find_matched_ = false;
  // The mark the find box narrowed the list to, normalised (carriesMark()),
  // or empty while the list shows every item. A mark is a badge the stage
  // gave items — "Game: quiz", or "game" for every mark starting so — which a
  // reader looking for a kind of item types rather than a number.
  QString mark_filter_;

  // The links the service gave its items — its keypad's keys and the numbers a
  // page offers (CatalogueItem::links) — offered beside the find box where the
  // catalogue carries any.
  QWidget* links_bar_ = nullptr;
  QToolButton* back_button_ = nullptr;
  QToolButton* index_button_ = nullptr;
  QToolButton* next_link_button_ = nullptr;
  QToolButton* more_button_ = nullptr;
  QLabel* references_label_ = nullptr;
  // Ids of the items shown before, most recent last, for Back. Ids rather than
  // positions, so a fresh delivery of the same catalogue keeps them good.
  std::vector<std::string> history_;
  // The item last on display, kept while a find that matches nothing blanks
  // the pane, so the item the reader typed their way from is what Back
  // returns to.
  std::string last_shown_id_;
  // Set while Back is showing an item, which is a move not to be remembered.
  bool going_back_ = false;
  // Dataset index of each item id, for following a link.
  std::map<std::string, size_t> index_of_id_;
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
  QToolButton* save_svg_button_ = nullptr;
  QToolButton* save_all_button_ = nullptr;
  QToolButton* save_all_svg_button_ = nullptr;
  QToolButton* prev_item_button_ = nullptr;
  QToolButton* next_item_button_ = nullptr;
  QWidget* item_nav_ = nullptr;

  QLabel* list_heading_ = nullptr;
  QTableWidget* items_table_ = nullptr;

  QStackedWidget* payload_stack_ = nullptr;
  // The flash phase the cell grid runs on, owned here so stepping between
  // pages does not restart the cycle.
  CatalogueFlashClock* flash_clock_ = nullptr;
  // What a display list is played against — the one on display, and the
  // scratch views a batch save renders through.
  std::shared_ptr<const ICatalogueAnimationClock> animation_clock_;
  CatalogueCellGridWidget* grid_widget_ = nullptr;
  CatalogueDisplayListWidget* display_widget_ = nullptr;
  /// Plays, pauses, steps, seeks, speeds and loops a timed display list,
  /// under the drawing.
  CataloguePlaybackBar* playback_bar_ = nullptr;
  /// The keypad an item that takes keys is played on, and a page number is
  /// typed on, beside the drawing.
  CatalogueKeypad* keypad_ = nullptr;
  /// The item the keypad's Play/Page choice was made for.
  QString keypad_item_id_;
  /// The item being played, the keys pressed on it so far, and what the stage
  /// answered for them — shown in place of the item's own payload once it
  /// arrives. All empty when nothing is being played; forgotten as soon as
  /// another item is on display.
  QString played_item_id_;
  QStringList played_keys_;
  std::optional<orc::CataloguePayload> played_payload_;
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
