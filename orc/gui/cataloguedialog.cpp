/*
 * File:        cataloguedialog.cpp
 * Module:      orc-gui
 * Purpose:     Generic browser for any stage that exposes ICatalogueResults
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "cataloguedialog.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPalette>
#include <QProgressDialog>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStringList>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <cstddef>
#include <limits>

#include "catalogue_flash_clock.h"
#include "cataloguecellgridwidget.h"
#include "cataloguedisplaylistwidget.h"

namespace {

/// Gap that separates one group of controls on a row from the next, wider than
/// the layout's own spacing between neighbours within a group.
constexpr int kControlGroupSpacing = 18;

/// Characters the view dropdown always has room for, so what it holds is read
/// rather than guessed at.
constexpr int kViewComboMinimumCharacters = 12;

/// A hairline standing between two groups of controls, which is what makes a
/// row of switches read as the two or three things it is rather than as one
/// undifferentiated line.
QWidget* make_separator(QWidget* parent) {
  auto* line = new QFrame(parent);
  line->setFrameShape(QFrame::VLine);
  // Drawn flat in the palette's own mid tone rather than as a sunken bevel: a
  // bevel is two near-white hairlines on a light theme and disappears
  // altogether, which is worse than no rule at all.
  line->setFrameShadow(QFrame::Plain);
  line->setLineWidth(1);
  line->setFixedWidth(1);
  QPalette rule = line->palette();
  rule.setColor(QPalette::WindowText, rule.color(QPalette::Mid));
  line->setPalette(rule);
  line->setVisible(false);
  return line;
}

/// The find box matches on a trimmed, case-folded key, which is as much
/// tolerance as a host can offer without knowing what the key means.
QString normalise_key(const QString& text) { return text.trimmed().toUpper(); }

QString to_qstring(const std::string& text) {
  return QString::fromStdString(text);
}

/// Put |text| on one of the message lines, and take the line away when there is
/// nothing to say. An empty line left standing is a gap the reader reads as
/// meaning something.
void set_message(QLabel* label, const QString& text) {
  label->setText(text);
  label->setVisible(!text.isEmpty());
  // A wrapping QLabel reports a minimum height of nothing much, so a column of
  // them ends up squeezed a pixel or two short each and the last line loses its
  // descenders to the window edge. The lines it is actually being asked to hold
  // are known here, so the height is stated rather than inferred.
  const int lines = static_cast<int>(text.count(QLatin1Char('\n'))) + 1;
  label->setMinimumHeight(lines * label->fontMetrics().lineSpacing());
}

/// A catalogue key made safe to put in a file name. Services name their items
/// with whatever punctuation the standard gave them — a NABTS record is
/// "000/1A4 v0" — and a slash in a suggested name is a directory the save would
/// land in rather than part of the name.
QString to_file_name_part(const QString& text) {
  QString out;
  for (const QChar& character : text) {
    if (character.isLetterOrNumber()) {
      out += character;
    } else if (!out.isEmpty() && !out.endsWith(QLatin1Char('-'))) {
      out += QLatin1Char('-');
    }
  }
  while (out.endsWith(QLatin1Char('-'))) {
    out.chop(1);
  }
  return out;
}

}  // namespace

CatalogueDialog::CatalogueDialog(QWidget* parent) : QDialog(parent) {
  setupUI();
  setWindowTitle(tr("Catalogue"));

  // Qt::Window so the viewer can be positioned independently of the main
  // window.
  setWindowFlags(Qt::Window);

  resize(880, 600);
  setMinimumSize(640, 440);
}

CatalogueDialog::~CatalogueDialog() = default;

void CatalogueDialog::setupUI() {
  // The status bar sits flush against the window edge, so the outer layout
  // carries no margins and the content above it supplies its own.
  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  auto* content_layout = new QVBoxLayout();
  content_layout->setContentsMargins(9, 9, 9, 9);

  // Top row: the find box (where the schema asks for one), item navigation,
  // standing notices, and the damage overlay switch.
  auto* top_row = new QHBoxLayout();

  find_bar_ = new QWidget(this);
  auto* find_layout = new QHBoxLayout(find_bar_);
  find_layout->setContentsMargins(0, 0, 0, 0);
  find_label_ = new QLabel(find_bar_);
  find_layout->addWidget(find_label_);
  find_edit_ = new QLineEdit(find_bar_);
  find_edit_->setObjectName("catalogueFindEdit");
  find_edit_->setMaxLength(16);
  find_edit_->setFixedWidth(80);
  connect(find_edit_, &QLineEdit::textChanged, this,
          &CatalogueDialog::onFindTextChanged);
  find_layout->addWidget(find_edit_);
  find_bar_->setVisible(false);
  top_row->addWidget(find_bar_);

  item_nav_ = new QWidget(this);
  auto* nav_layout = new QHBoxLayout(item_nav_);
  nav_layout->setContentsMargins(0, 0, 0, 0);
  prev_item_button_ = new QToolButton(item_nav_);
  prev_item_button_->setObjectName("cataloguePrevItemButton");
  prev_item_button_->setArrowType(Qt::LeftArrow);
  connect(prev_item_button_, &QToolButton::clicked, this,
          [this] { stepItem(-1); });
  nav_layout->addWidget(prev_item_button_);
  next_item_button_ = new QToolButton(item_nav_);
  next_item_button_->setObjectName("catalogueNextItemButton");
  next_item_button_->setArrowType(Qt::RightArrow);
  connect(next_item_button_, &QToolButton::clicked, this,
          [this] { stepItem(1); });
  nav_layout->addWidget(next_item_button_);
  top_row->addWidget(item_nav_);

  top_row->addStretch();

  // Where a service presents the same items more than one way, the choice sits
  // with the other display switches rather than in the node's parameters: it
  // changes nothing the decode found, and editing a parameter rebuilds the
  // graph and closes this window over what is only a change of view.
  view_bar_ = new QWidget(this);
  auto* view_layout = new QHBoxLayout(view_bar_);
  view_layout->setContentsMargins(0, 0, 0, 0);
  view_label_ = new QLabel(view_bar_);
  view_layout->addWidget(view_label_);
  view_combo_ = new QComboBox(view_bar_);
  view_combo_->setObjectName("catalogueViewCombo");
  // Wide enough for what it holds, and never squeezed narrower: a dropdown
  // clipped to "256 x 20" is worse than no dropdown, because it looks like a
  // value rather than like a truncation.
  view_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  view_combo_->setMinimumContentsLength(kViewComboMinimumCharacters);
  connect(view_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &CatalogueDialog::onViewOptionSelected);
  view_layout->addWidget(view_combo_);
  // The toggles are built from the delivered schema, so the layout is kept to
  // add them to.
  view_layout_ = view_layout;
  view_bar_->setVisible(false);
  top_row->addWidget(view_bar_);
  top_row->addSpacing(kControlGroupSpacing);

  // A drawn payload may carry a text form of itself. It is worth having and
  // worth being able to put away: on a wide page the two side by side leave
  // neither enough room, and a reader looking at the drawing does not always
  // want the transcript.
  show_text_check_ = new QCheckBox(tr("Show text"), this);
  show_text_check_->setObjectName("catalogueShowTextCheck");
  // Off to begin with: the drawing is what the window is for, and it gets the
  // whole pane until a reader asks for the transcript beside it.
  show_text_check_->setChecked(false);
  show_text_check_->setVisible(false);
  show_text_check_->setToolTip(
      tr("Show the text form of the payload beside the drawing. Clear this to "
         "give the drawing the whole pane."));
  connect(show_text_check_, &QCheckBox::toggled, this,
          &CatalogueDialog::onShowTextToggled);
  top_row->addWidget(show_text_check_);

  // Flashing text and blinking figures are part of the page, but a page being
  // read or captured is better still, so the reader chooses. Shown only where
  // the payload on display is one that can animate.
  animations_check_ = new QCheckBox(tr("Enable animations"), this);
  animations_check_->setObjectName("catalogueAnimationsCheck");
  animations_check_->setChecked(true);
  animations_check_->setVisible(false);
  animations_check_->setToolTip(
      tr("Flash characters and blinking figures as the service transmitted "
         "them. Clear this to hold the payload still."));
  connect(animations_check_, &QCheckBox::toggled, this,
          &CatalogueDialog::onAnimationsToggled);
  top_row->addWidget(animations_check_);

  highlight_check_ = new QCheckBox(this);
  highlight_check_->setObjectName("catalogueHighlightCheck");
  highlight_check_->setVisible(false);
  connect(highlight_check_, &QCheckBox::toggled, this,
          &CatalogueDialog::onHighlightToggled);
  top_row->addWidget(highlight_check_);

  // A page worth finding is usually a page worth keeping, and the viewer is the
  // only place the assembled display exists — the packet stream the sink writes
  // is the data, not the picture. Last on the row and set apart from the
  // switches before it: those change what is on screen, and this does
  // something with it. Shown only where the payload on display is drawn.
  action_separator_ = make_separator(this);
  top_row->addWidget(action_separator_);
  save_page_button_ = new QToolButton(this);
  save_page_button_->setObjectName("catalogueSavePageButton");
  save_page_button_->setText(tr("Save PNG…"));
  save_page_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  save_page_button_->setVisible(false);
  save_page_button_->setToolTip(
      tr("Save the display on screen as a PNG image, drawn at its own size "
         "with flashing and blinking held lit."));
  connect(save_page_button_, &QToolButton::clicked, this,
          &CatalogueDialog::onSavePageClicked);
  top_row->addWidget(save_page_button_);

  // A service can carry hundreds of pages, and saving them one dialogue at a
  // time is the kind of chore a computer exists to do. Offered whenever the
  // catalogue holds anything drawn, whatever happens to be on screen.
  save_all_button_ = new QToolButton(this);
  save_all_button_->setObjectName("catalogueSaveAllButton");
  save_all_button_->setText(tr("Save All PNGs…"));
  save_all_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  save_all_button_->setVisible(false);
  save_all_button_->setToolTip(
      tr("Save every drawn display in the catalogue as a PNG image into a "
         "chosen folder, each at its own size with flashing and blinking held "
         "lit."));
  connect(save_all_button_, &QToolButton::clicked, this,
          &CatalogueDialog::onSaveAllClicked);
  top_row->addWidget(save_all_button_);
  content_layout->addLayout(top_row);

  auto* body_row = new QHBoxLayout();

  // Left column: every item the analysed range carried. Which items exist is
  // itself a discovery, so the table is how the reader finds out rather than
  // having to guess.
  auto* list_column = new QVBoxLayout();
  list_heading_ = new QLabel(this);
  list_column->addWidget(list_heading_);

  items_table_ = new QTableWidget(0, 0, this);
  items_table_->setObjectName("catalogueItemsTable");
  items_table_->setMinimumWidth(240);
  items_table_->setMaximumWidth(380);
  items_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  items_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  items_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  items_table_->setShowGrid(false);
  items_table_->setAlternatingRowColors(true);
  items_table_->setWordWrap(false);
  items_table_->setCornerButtonEnabled(false);
  items_table_->verticalHeader()->setVisible(false);
  items_table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  connect(items_table_, &QTableWidget::itemSelectionChanged, this,
          &CatalogueDialog::onItemSelected);
  list_column->addWidget(items_table_, /*stretch=*/1);
  body_row->addLayout(list_column);

  // Right column: the payload, with the variant stepper beneath it. A variant
  // is a version of the item the service cycles through, so the control belongs
  // against the display it steps, not against the table of items.
  auto* payload_column = new QVBoxLayout();

  payload_stack_ = new QStackedWidget(this);
  payload_stack_->setObjectName("cataloguePayloadStack");

  empty_label_ = new QLabel(this);
  empty_label_->setAlignment(Qt::AlignCenter);
  empty_label_->setWordWrap(true);
  payload_stack_->insertWidget(kPageNothing, empty_label_);

  flash_clock_ = new CatalogueFlashClock(this);

  grid_widget_ = new CatalogueCellGridWidget(this);
  grid_widget_->setObjectName("catalogueCellGrid");
  grid_widget_->setFlashClock(flash_clock_);
  payload_stack_->insertWidget(kPageCellGrid, grid_widget_);

  // A display list is often read as much as looked at, so a text form sits
  // beside it when the payload carries one.
  display_page_ = new QWidget(this);
  auto* display_layout = new QHBoxLayout(display_page_);
  display_layout->setContentsMargins(0, 0, 0, 0);
  auto* display_splitter = new QSplitter(Qt::Horizontal, display_page_);
  display_widget_ = new CatalogueDisplayListWidget(display_splitter);
  display_widget_->setObjectName("catalogueDisplayList");
  display_widget_->setFlashClock(flash_clock_);
  display_splitter->addWidget(display_widget_);
  companion_pane_ = new QPlainTextEdit(display_splitter);
  companion_pane_->setObjectName("catalogueCompanionPane");
  companion_pane_->setReadOnly(true);
  companion_pane_->setLineWrapMode(QPlainTextEdit::NoWrap);
  display_splitter->addWidget(companion_pane_);
  display_splitter->setStretchFactor(0, 3);
  display_splitter->setStretchFactor(1, 2);
  display_layout->addWidget(display_splitter);
  payload_stack_->insertWidget(kPageDisplayList, display_page_);

  text_pane_ = new QPlainTextEdit(this);
  text_pane_->setObjectName("catalogueTextPane");
  text_pane_->setReadOnly(true);
  text_pane_->setLineWrapMode(QPlainTextEdit::NoWrap);
  payload_stack_->insertWidget(kPageText, text_pane_);

  table_pane_ = new QTableWidget(0, 0, this);
  table_pane_->setObjectName("catalogueTablePane");
  table_pane_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_pane_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_pane_->setAlternatingRowColors(true);
  table_pane_->verticalHeader()->setVisible(false);
  payload_stack_->insertWidget(kPageTable, table_pane_);

  payload_stack_->setCurrentIndex(kPageNothing);
  payload_column->addWidget(payload_stack_, /*stretch=*/1);

  variant_bar_ = new QWidget(this);
  variant_bar_->setObjectName("catalogueVariantBar");
  auto* variant_row = new QHBoxLayout(variant_bar_);
  variant_row->setContentsMargins(0, 0, 0, 0);
  variant_row->addStretch();
  prev_variant_button_ = new QToolButton(variant_bar_);
  prev_variant_button_->setObjectName("cataloguePrevVariantButton");
  prev_variant_button_->setArrowType(Qt::LeftArrow);
  connect(prev_variant_button_, &QToolButton::clicked, this,
          [this] { stepVariant(-1); });
  variant_row->addWidget(prev_variant_button_);
  variant_label_ = new QLabel(variant_bar_);
  variant_label_->setObjectName("catalogueVariantLabel");
  variant_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  variant_row->addWidget(variant_label_);
  next_variant_button_ = new QToolButton(variant_bar_);
  next_variant_button_->setObjectName("catalogueNextVariantButton");
  next_variant_button_->setArrowType(Qt::RightArrow);
  connect(next_variant_button_, &QToolButton::clicked, this,
          [this] { stepVariant(1); });
  variant_row->addWidget(next_variant_button_);
  variant_row->addStretch();
  variant_bar_->setVisible(false);
  payload_column->addWidget(variant_bar_);

  body_row->addLayout(payload_column, /*stretch=*/1);
  content_layout->addLayout(body_row, /*stretch=*/1);

  main_layout->addLayout(content_layout, /*stretch=*/1);

  // Everything the dialogue has to say goes here, in one column at the foot of
  // the window: what the viewer is doing, what is on show, what the catalogue
  // carries, and how the run went. Scattering these around the frame — a
  // notice above the list, a condition in one corner of a status bar, a run
  // summary in another — meant a reader had to sweep the window to find out
  // whether anything had been said at all.
  //
  // Read top to bottom it narrows: the most immediate line first, the whole
  // run last.
  message_rule_ = new QFrame(this);
  static_cast<QFrame*>(message_rule_)->setFrameShape(QFrame::HLine);
  static_cast<QFrame*>(message_rule_)->setFrameShadow(QFrame::Sunken);
  message_rule_->setVisible(false);
  main_layout->addWidget(message_rule_);

  message_panel_ = new QWidget(this);
  message_panel_->setObjectName("catalogueMessagePanel");
  auto* message_layout = new QVBoxLayout(message_panel_);
  message_layout->setContentsMargins(9, 6, 9, 8);
  message_layout->setSpacing(2);
  // The payload gives way to the messages rather than the other way round: a
  // line of text half cut off by the window edge is worse than a slightly
  // shorter page, and the panel only ever asks for the few lines it has.
  message_layout->setSizeConstraint(QLayout::SetMinimumSize);
  QSizePolicy message_policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
  // The lines wrap, so how tall the panel needs to be depends on how wide it
  // is. Without this the outer layout asks for one line's worth and the last
  // message is cut off by the window edge.
  message_policy.setHeightForWidth(true);
  message_panel_->setSizePolicy(message_policy);

  const auto add_message_line = [&](const char* object_name) {
    auto* label = new QLabel(message_panel_);
    label->setObjectName(QString::fromLatin1(object_name));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setVisible(false);
    message_layout->addWidget(label);
    return label;
  };

  // What the viewer is doing, which is only ever true for as long as it is
  // doing it.
  status_label_ = add_message_line("observationStatusLabel");

  // What is on show, and what shape it is in. The reader is looking at this
  // one, so it leads and it is the line set in bold.
  headline_label_ = add_message_line("catalogueHeadlineLabel");
  QFont headline_font = headline_label_->font();
  headline_font.setBold(true);
  headline_label_->setFont(headline_font);
  condition_label_ = add_message_line("catalogueConditionLabel");

  // What the catalogue as a whole carries — the standing notices a service's
  // own contents earn.
  notice_label_ = add_message_line("catalogueNoticeLabel");

  // And how the run went, which is background to all of it and dimmed to say
  // so. The detail behind this line is in the run's report and its log.
  summary_label_ = add_message_line("catalogueSummaryLabel");
  QPalette dimmed = summary_label_->palette();
  dimmed.setColor(QPalette::WindowText,
                  dimmed.color(QPalette::Disabled, QPalette::WindowText));
  summary_label_->setPalette(dimmed);

  message_panel_->setVisible(false);
  main_layout->addWidget(message_panel_);
}

void CatalogueDialog::refreshMessagePanel() {
  const bool anything = status_label_->isVisibleTo(message_panel_) ||
                        headline_label_->isVisibleTo(message_panel_) ||
                        condition_label_->isVisibleTo(message_panel_) ||
                        notice_label_->isVisibleTo(message_panel_) ||
                        summary_label_->isVisibleTo(message_panel_);
  message_panel_->setVisible(anything);
  message_rule_->setVisible(anything);
}

void CatalogueDialog::showPending() {
  set_message(status_label_, tr("Decoding…"));
  refreshMessagePanel();
}

void CatalogueDialog::showError(const QString& message) {
  // Replaces the pending state: the viewer was opened empty on the promise of
  // a catalogue that is not coming, so it must say so rather than sit on
  // "Decoding…" forever.
  clearContent();
  set_message(headline_label_, message);
  refreshMessagePanel();
}

void CatalogueDialog::clearContent() {
  data_ = orc::CatalogueDataset{};
  has_data_ = false;
  current_row_ = -1;
  variant_index_ = 0;
  set_message(status_label_, QString());
  set_message(summary_label_, QString());
  refreshViewOptions();
  refreshItemList();
  showItem(-1);
  refreshMessagePanel();
}

void CatalogueDialog::setCatalogue(const orc::CatalogueDataset& data) {
  if (!data.consistent()) {
    // A dataset whose payloads do not line up with its items is a plugin bug;
    // showing nothing beats indexing past the end of the vector.
    clearContent();
    set_message(headline_label_,
                tr("The stage returned an inconsistent catalogue"));
    refreshMessagePanel();
    return;
  }

  // The find box's current text survives a fresh delivery of the same
  // catalogue, so a reader stepping a carousel is not thrown back to the top.
  const QString previous_key = normalise_key(find_edit_->text());

  // So does the item on show, for the service that has no find box. A reader
  // who changes the view is asking to see what is in front of them drawn
  // another way, and being returned to the first item would be the one thing
  // that made the comparison hard.
  const size_t previous_index = displayedIndex();
  const QString previous_id =
      previous_index == SIZE_MAX
          ? QString()
          : to_qstring(data_.items[previous_index].parent_id.empty()
                           ? data_.items[previous_index].id
                           : data_.items[previous_index].parent_id);
  const int previous_variant = variant_index_;

  data_ = data;
  has_data_ = true;
  set_message(status_label_, QString());

  // Schema-driven chrome, applied before the table is built from it.
  const auto& schema = data_.schema;
  const QString item_noun =
      schema.item_noun.empty() ? tr("Item") : to_qstring(schema.item_noun);
  list_heading_->setText(tr("%1s carried:").arg(item_noun));
  prev_item_button_->setToolTip(
      tr("Show the previous %1, wrapping round at the start of the list.")
          .arg(item_noun.toLower()));
  next_item_button_->setToolTip(
      tr("Show the next %1, wrapping round at the end of the list.")
          .arg(item_noun.toLower()));

  find_bar_->setVisible(!schema.find_label.empty());
  if (!schema.find_label.empty()) {
    find_label_->setText(to_qstring(schema.find_label));
    find_edit_->setPlaceholderText(to_qstring(schema.find_placeholder));
  }

  highlight_check_->setVisible(!schema.highlight_label.empty());
  if (!schema.highlight_label.empty()) {
    highlight_check_->setText(to_qstring(schema.highlight_label));
  }

  empty_label_->setText(schema.empty_message.empty()
                            ? tr("Nothing was recovered")
                            : to_qstring(schema.empty_message));

  refreshViewOptions();

  refreshItemList();

  // Restore the reader's position where the same key is still listed; fall
  // back to the first item otherwise, which is what a fresh catalogue wants.
  int row = top_level_.empty() ? -1 : 0;
  if (!previous_key.isEmpty()) {
    for (size_t i = 0; i < top_level_.size(); ++i) {
      if (normalise_key(to_qstring(data_.items[top_level_[i]].find_key)) ==
          previous_key) {
        row = static_cast<int>(i);
        break;
      }
    }
  } else if (!previous_id.isEmpty()) {
    for (size_t i = 0; i < top_level_.size(); ++i) {
      if (to_qstring(data_.items[top_level_[i]].id) == previous_id) {
        row = static_cast<int>(i);
        variant_index_ = previous_variant;
        break;
      }
    }
  }
  showItem(row);
}

void CatalogueDialog::refreshViewOptions() {
  const auto& schema = data_.schema;
  const bool has_options =
      !schema.view_options.empty() && !schema.view_label.empty();
  const bool offered = has_options || !schema.toggles.empty();
  view_bar_->setVisible(offered);

  updating_view_ = true;
  // Rebuilt from the delivered schema rather than added to, so a catalogue
  // offering a different set of toggles replaces them instead of accumulating
  // them.
  for (QCheckBox* check : toggle_checks_) {
    view_layout_->removeWidget(check);
    check->deleteLater();
  }
  toggle_checks_.clear();
  for (const auto& toggle : schema.toggles) {
    auto* check = new QCheckBox(to_qstring(toggle.label), view_bar_);
    check->setObjectName("catalogueViewToggle");
    // The id travels on the widget rather than in a parallel list: nothing here
    // knows what a toggle means, and the stage gets its own id back untouched.
    check->setProperty("toggleId", to_qstring(toggle.id));
    check->setToolTip(to_qstring(toggle.tooltip));
    // What the stage says it built under, not what was asked for — the two
    // differ when a stage declines a toggle, and the box should show what is on
    // screen.
    check->setChecked(toggle.active);
    connect(check, &QCheckBox::toggled, this, &CatalogueDialog::onViewToggled);
    view_layout_->addWidget(check);
    // Built when a catalogue arrives rather than in the constructor, which for
    // an already-open dialogue means after its parent was shown — and a widget
    // created then starts hidden.
    check->show();
    toggle_checks_.push_back(check);
  }
  updating_view_ = false;

  view_combo_->setVisible(has_options);
  view_label_->setVisible(has_options);
  if (!has_options) {
    return;
  }

  view_label_->setText(to_qstring(schema.view_label));

  // Filled from the delivered schema rather than added to, so a catalogue that
  // offers a different set of options replaces the list instead of growing it.
  updating_view_ = true;
  view_combo_->clear();
  int current = 0;
  for (size_t i = 0; i < schema.view_options.size(); ++i) {
    const auto& option = schema.view_options[i];
    view_combo_->addItem(to_qstring(option.label), to_qstring(option.id));
    view_combo_->setItemData(static_cast<int>(i), to_qstring(option.tooltip),
                             Qt::ToolTipRole);
    if (to_qstring(option.id) == to_qstring(schema.view_option)) {
      current = static_cast<int>(i);
    }
  }
  // What the stage says it built under, not what was asked for: the two differ
  // when a stage declines an option, and the dropdown should show what is on
  // screen.
  view_combo_->setCurrentIndex(current);
  view_combo_->setToolTip(
      view_combo_->itemData(current, Qt::ToolTipRole).toString());
  updating_view_ = false;
}

void CatalogueDialog::onViewOptionSelected(int index) {
  if (updating_view_ || index < 0) {
    return;
  }
  view_combo_->setToolTip(
      view_combo_->itemData(index, Qt::ToolTipRole).toString());
  emitViewSelection();
}

void CatalogueDialog::onViewToggled() {
  if (updating_view_) {
    return;
  }
  emitViewSelection();
}

void CatalogueDialog::emitViewSelection() {
  // The whole selection, not the part that changed: the stage builds the
  // catalogue from both axes at once and has no memory of what it was last
  // asked for.
  QStringList active;
  for (const QCheckBox* check : toggle_checks_) {
    if (check->isChecked()) {
      active.append(check->property("toggleId").toString());
    }
  }
  emit viewSelectionChanged(currentViewOption(), active);
}

std::vector<QString> CatalogueDialog::viewToggles() const {
  std::vector<QString> ids;
  ids.reserve(toggle_checks_.size());
  for (const QCheckBox* check : toggle_checks_) {
    ids.push_back(check->property("toggleId").toString());
  }
  return ids;
}

bool CatalogueDialog::isToggleActive(const QString& toggle_id) const {
  for (const QCheckBox* check : toggle_checks_) {
    if (check->property("toggleId").toString() == toggle_id) {
      return check->isChecked();
    }
  }
  return false;
}

void CatalogueDialog::setToggleActive(const QString& toggle_id, bool active) {
  for (QCheckBox* check : toggle_checks_) {
    if (check->property("toggleId").toString() == toggle_id) {
      check->setChecked(active);
      return;
    }
  }
}

std::vector<QString> CatalogueDialog::viewOptions() const {
  std::vector<QString> ids;
  if (!view_combo_->isVisibleTo(this)) {
    return ids;
  }
  ids.reserve(static_cast<size_t>(view_combo_->count()));
  for (int i = 0; i < view_combo_->count(); ++i) {
    ids.push_back(view_combo_->itemData(i).toString());
  }
  return ids;
}

QString CatalogueDialog::currentViewOption() const {
  if (!view_combo_->isVisibleTo(this) || view_combo_->currentIndex() < 0) {
    return {};
  }
  return view_combo_->currentData().toString();
}

void CatalogueDialog::selectViewOption(const QString& option_id) {
  const int index = view_combo_->findData(option_id);
  if (index >= 0) {
    view_combo_->setCurrentIndex(index);
  }
}

void CatalogueDialog::refreshItemList() {
  updating_list_ = true;

  top_level_.clear();
  for (size_t i = 0; i < data_.items.size(); ++i) {
    if (data_.items[i].parent_id.empty()) {
      top_level_.push_back(i);
    }
  }

  const auto& columns = data_.schema.columns;
  items_table_->clearContents();
  items_table_->setColumnCount(static_cast<int>(columns.size()));
  QStringList headings;
  headings.reserve(static_cast<int>(columns.size()));
  for (const auto& column : columns) {
    headings << to_qstring(column.title);
  }
  items_table_->setHorizontalHeaderLabels(headings);
  for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
    auto* header_item = items_table_->horizontalHeaderItem(column);
    if (header_item != nullptr &&
        columns[static_cast<size_t>(column)].numeric) {
      // Numeric columns are right-aligned so their digits line up; their
      // headings follow them.
      header_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
  }
  auto* header = items_table_->horizontalHeader();
  for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
    header->setSectionResizeMode(column,
                                 column + 1 == static_cast<int>(columns.size())
                                     ? QHeaderView::Stretch
                                     : QHeaderView::ResizeToContents);
  }

  // One trigger run delivers the catalogue whole, so the table is built once
  // per delivery rather than merged: there is no playback to keep it still for.
  items_table_->setRowCount(static_cast<int>(top_level_.size()));
  const QBrush muted = palette().brush(QPalette::Disabled, QPalette::Text);
  for (int row = 0; row < static_cast<int>(top_level_.size()); ++row) {
    const auto& item = data_.items[top_level_[static_cast<size_t>(row)]];
    const QString tooltip = to_qstring(item.tooltip);
    for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
      QString text;
      if (column < static_cast<int>(item.values.size())) {
        text = to_qstring(item.values[static_cast<size_t>(column)]);
      }
      // Badges ride on the first column: there is nowhere in a table this
      // narrow to put a legend, so they are spelled out against the value.
      if (column == 0) {
        for (const auto& badge : item.badges) {
          text += QLatin1Char(' ') + to_qstring(badge);
        }
      }
      auto* cell = new QTableWidgetItem(text);
      if (columns[static_cast<size_t>(column)].numeric) {
        cell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
      }
      if (!item.selectable) {
        // Kept listed — it is genuinely recovered data — but muted, so the
        // items a reader can actually use are the ones that stand out.
        cell->setForeground(muted);
      }
      if (!tooltip.isEmpty()) {
        cell->setToolTip(tooltip);
      }
      items_table_->setItem(row, column, cell);
    }
  }
  updating_list_ = false;

  const bool navigable = top_level_.size() > 1;
  prev_item_button_->setEnabled(navigable);
  next_item_button_->setEnabled(navigable);
  item_nav_->setVisible(!top_level_.empty());

  // One notice per line rather than run together: they are separate statements
  // about the catalogue, and reading them as a sentence is what made the old
  // single line hard to take in.
  QStringList notices;
  for (const auto& notice : data_.summary.notices) {
    notices << to_qstring(notice);
  }
  set_message(notice_label_, notices.join(QLatin1Char('\n')));
  set_message(summary_label_, to_qstring(data_.summary.headline));
  refreshMessagePanel();
}

std::vector<size_t> CatalogueDialog::variantsOf(int row) const {
  std::vector<size_t> variants;
  if (row < 0 || row >= static_cast<int>(top_level_.size())) {
    return variants;
  }
  const std::string& parent_id =
      data_.items[top_level_[static_cast<size_t>(row)]].id;
  for (size_t i = 0; i < data_.items.size(); ++i) {
    if (data_.items[i].parent_id == parent_id) {
      variants.push_back(i);
    }
  }
  return variants;
}

size_t CatalogueDialog::displayedIndex() const {
  if (current_row_ < 0 || current_row_ >= static_cast<int>(top_level_.size())) {
    return std::numeric_limits<size_t>::max();
  }
  const std::vector<size_t> variants = variantsOf(current_row_);
  if (variants.empty()) {
    return top_level_[static_cast<size_t>(current_row_)];
  }
  const size_t index = static_cast<size_t>(
      std::clamp(variant_index_, 0, static_cast<int>(variants.size()) - 1));
  return variants[index];
}

void CatalogueDialog::showItem(int row) {
  const bool valid = row >= 0 && row < static_cast<int>(top_level_.size());
  if (current_row_ != row) {
    // A different item starts at the top of its sequence; a re-render of the
    // same one (stepping the carousel, or a fresh delivery of the same
    // catalogue) keeps the variant the reader is on.
    variant_index_ = 0;
  }
  current_row_ = valid ? row : -1;

  // Keep the table in step without the selection feeding back into here.
  updating_list_ = true;
  if (valid) {
    items_table_->setCurrentCell(row, 0);
  } else {
    items_table_->setCurrentCell(-1, -1);
    items_table_->clearSelection();
  }
  updating_list_ = false;

  // Keep the find box showing what is on screen, without re-triggering a
  // lookup.
  if (valid && find_bar_->isVisibleTo(this)) {
    const QString key =
        to_qstring(data_.items[top_level_[static_cast<size_t>(row)]].find_key);
    if (!key.isEmpty() && key != find_edit_->text()) {
      updating_list_ = true;
      find_edit_->setText(key);
      updating_list_ = false;
    }
  }

  refreshVariantControl();
  renderPayload();
}

void CatalogueDialog::renderPayload() {
  const size_t index = displayedIndex();
  if (index == std::numeric_limits<size_t>::max()) {
    grid_widget_->clearGrid();
    display_widget_->clearDisplayList();
    companion_pane_->clear();
    text_pane_->clear();
    table_pane_->clearContents();
    table_pane_->setRowCount(0);
    set_message(condition_label_, QString());
    animations_check_->setVisible(false);
    show_text_check_->setVisible(false);
    save_page_button_->setVisible(false);
    // The batch save works on the catalogue rather than the display, so an
    // empty payload pane — nothing selected, or a find that matched nothing —
    // does not take it away.
    save_all_button_->setVisible(canSaveAllPages());
    action_separator_->setVisible(save_all_button_->isVisibleTo(this));
    payload_stack_->setCurrentIndex(kPageNothing);
    if (!has_data_) {
      set_message(headline_label_, tr("No data"));
    } else if (top_level_.empty()) {
      set_message(headline_label_, empty_label_->text());
    } else {
      const QString typed = find_edit_->text().trimmed();
      const QString noun = data_.schema.item_noun.empty()
                               ? tr("Item")
                               : to_qstring(data_.schema.item_noun);
      set_message(headline_label_,
                  typed.isEmpty()
                      ? tr("Nothing selected")
                      : tr("%1 %2 was not carried").arg(noun, typed));
    }
    refreshMessagePanel();
    return;
  }

  const orc::CataloguePayload& payload = data_.payloads[index];
  set_message(headline_label_, to_qstring(payload.headline));
  set_message(condition_label_, to_qstring(payload.condition));
  refreshMessagePanel();

  // Only the two drawn payloads can animate, or be saved as an image; a listing
  // or a table has nothing for either control to act on.
  const bool drawn = payload.kind == orc::CataloguePayload::Kind::kCellGrid ||
                     payload.kind == orc::CataloguePayload::Kind::kDisplayList;
  animations_check_->setVisible(drawn);
  save_page_button_->setVisible(drawn);
  // The batch save follows the catalogue, not the display: a reader on a text
  // listing can still save every page the service carried.
  const bool any_drawn = canSaveAllPages();
  save_all_button_->setVisible(any_drawn);
  action_separator_->setVisible(drawn || any_drawn);

  // The text pane belongs to the display-list payload alone, and only where
  // that payload brought text with it.
  const bool has_companion =
      payload.kind == orc::CataloguePayload::Kind::kDisplayList &&
      !payload.companion_text.empty();
  show_text_check_->setVisible(has_companion);

  switch (payload.kind) {
    case orc::CataloguePayload::Kind::kCellGrid:
      grid_widget_->setGrid(payload.grid);
      grid_widget_->setShowDataErrors(highlight_check_->isChecked());
      grid_widget_->setAnimationsEnabled(animations_check_->isChecked());
      payload_stack_->setCurrentIndex(kPageCellGrid);
      return;

    case orc::CataloguePayload::Kind::kDisplayList: {
      display_widget_->setDisplayList(payload.display_list);
      display_widget_->setShowDataErrors(highlight_check_->isChecked());
      display_widget_->setAnimationsEnabled(animations_check_->isChecked());
      const QString companion = to_qstring(payload.companion_text);
      companion_pane_->setPlainText(companion);
      companion_pane_->setVisible(!companion.isEmpty() &&
                                  show_text_check_->isChecked());
      payload_stack_->setCurrentIndex(kPageDisplayList);
      return;
    }

    case orc::CataloguePayload::Kind::kText: {
      // A listing wants its columns to line up; prose does not.
      text_pane_->setFont(
          payload.document.monospace
              ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
              : QFontDatabase::systemFont(QFontDatabase::GeneralFont));
      text_pane_->setPlainText(to_qstring(payload.document.text));
      payload_stack_->setCurrentIndex(kPageText);
      return;
    }

    case orc::CataloguePayload::Kind::kTable: {
      const auto& table = payload.table;
      table_pane_->clearContents();
      table_pane_->setColumnCount(static_cast<int>(table.columns.size()));
      QStringList headings;
      for (const auto& column : table.columns) {
        headings << to_qstring(column.title);
      }
      table_pane_->setHorizontalHeaderLabels(headings);
      table_pane_->setRowCount(static_cast<int>(table.rows.size()));
      for (int row = 0; row < static_cast<int>(table.rows.size()); ++row) {
        const auto& values = table.rows[static_cast<size_t>(row)];
        for (int column = 0; column < static_cast<int>(table.columns.size()) &&
                             column < static_cast<int>(values.size());
             ++column) {
          auto* cell = new QTableWidgetItem(
              to_qstring(values[static_cast<size_t>(column)]));
          if (table.columns[static_cast<size_t>(column)].numeric) {
            cell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
          }
          table_pane_->setItem(row, column, cell);
        }
      }
      auto* header = table_pane_->horizontalHeader();
      for (int column = 0; column < static_cast<int>(table.columns.size());
           ++column) {
        header->setSectionResizeMode(
            column, column + 1 == static_cast<int>(table.columns.size())
                        ? QHeaderView::Stretch
                        : QHeaderView::ResizeToContents);
      }
      payload_stack_->setCurrentIndex(kPageTable);
      return;
    }

    case orc::CataloguePayload::Kind::kNone:
      break;
  }

  // A parent with no payload and no variants has nothing to draw.
  grid_widget_->clearGrid();
  display_widget_->clearDisplayList();
  payload_stack_->setCurrentIndex(kPageNothing);
}

void CatalogueDialog::refreshVariantControl() {
  const std::vector<size_t> variants = variantsOf(current_row_);
  const QString noun = data_.schema.variant_noun.empty()
                           ? tr("Variant")
                           : to_qstring(data_.schema.variant_noun);

  // The stepper belongs to the service, not to the item on screen: a control
  // that came and went as the reader moved down the list would read as an
  // interface glitch. So it stands wherever the schema names a variant and
  // there is an item to step, and says so when there is nothing to step to.
  if (data_.schema.variant_noun.empty() || current_row_ < 0) {
    variant_index_ = 0;
    variant_bar_->setVisible(false);
    variant_label_->clear();
    return;
  }

  const int count = static_cast<int>(variants.size());
  variant_index_ = count < 2 ? 0 : std::clamp(variant_index_, 0, count - 1);

  if (count < 2) {
    // Said rather than hidden, so "one of them" — or none at all, which is what
    // an item that carries its own payload has — is distinguishable from a
    // control that has not been noticed.
    variant_label_->setText(tr("No %1s").arg(noun.toLower()));
  } else {
    const auto& variant =
        data_.items[variants[static_cast<size_t>(variant_index_)]];
    variant_label_->setText(tr("%1 %2 of %3 (%4)")
                                .arg(noun)
                                .arg(variant_index_ + 1)
                                .arg(count)
                                .arg(to_qstring(variant.variant_label)));
  }
  prev_variant_button_->setEnabled(count > 1);
  next_variant_button_->setEnabled(count > 1);
  variant_bar_->setVisible(true);
}

void CatalogueDialog::stepVariant(int delta) {
  const std::vector<size_t> variants = variantsOf(current_row_);
  if (variants.size() < 2) {
    return;
  }
  // Wrapping, because the carousel itself does: stepping past the last variant
  // is what the service does next.
  const int count = static_cast<int>(variants.size());
  variant_index_ = ((variant_index_ + delta) % count + count) % count;
  refreshVariantControl();
  renderPayload();
}

void CatalogueDialog::stepItem(int delta) {
  const int count = static_cast<int>(top_level_.size());
  if (count == 0) {
    return;
  }
  const int base = current_row_ < 0 ? 0 : current_row_;
  showItem(((base + delta) % count + count) % count);
}

void CatalogueDialog::onFindTextChanged() {
  if (updating_list_) {
    return;  // programmatic sync following a selection
  }
  const QString key = normalise_key(find_edit_->text());
  for (size_t i = 0; i < top_level_.size(); ++i) {
    if (normalise_key(to_qstring(data_.items[top_level_[i]].find_key)) == key) {
      showItem(static_cast<int>(i));
      return;
    }
  }
  showItem(-1);
}

void CatalogueDialog::onItemSelected() {
  if (updating_list_) {
    return;  // programmatic selection following the find box
  }
  const int row = items_table_->currentRow();
  if (row < 0) {
    return;
  }
  showItem(row);
}

void CatalogueDialog::onHighlightToggled(bool checked) {
  grid_widget_->setShowDataErrors(checked);
  display_widget_->setShowDataErrors(checked);
}

void CatalogueDialog::onAnimationsToggled(bool checked) {
  grid_widget_->setAnimationsEnabled(checked);
  display_widget_->setAnimationsEnabled(checked);
}

void CatalogueDialog::onShowTextToggled(bool checked) {
  // Nothing is asked of the stage: the text came with the payload, and putting
  // it away is a change to this window and nothing else.
  companion_pane_->setVisible(checked &&
                              !companion_pane_->toPlainText().isEmpty());
}

bool CatalogueDialog::isTextPaneVisible() const {
  return companion_pane_->isVisibleTo(display_page_);
}

void CatalogueDialog::setTextPaneVisible(bool visible) {
  show_text_check_->setChecked(visible);
}

void CatalogueDialog::onSavePageClicked() {
  const QImage image = renderedPageImage();
  if (image.isNull()) {
    return;
  }

  // The same remembered directory the rest of the application exports into, so
  // a page saved here lands beside a preview frame saved from the main window.
  QSettings settings("orc-project", "orc-gui");
  QString directory =
      settings.value("lastExportDirectory", QString()).toString();
  if (directory.isEmpty() || !QFileInfo(directory).isDir()) {
    directory = QDir::homePath();
  }

  QString path = QFileDialog::getSaveFileName(
      this, tr("Save Display as PNG"),
      QDir(directory).filePath(suggestedPageFileName()),
      tr("PNG Images (*.png);;All Files (*)"));
  if (path.isEmpty()) {
    return;  // cancelled
  }
  if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
    path += QStringLiteral(".png");
  }
  settings.setValue("lastExportDirectory", QFileInfo(path).absolutePath());

  if (!image.save(path, "PNG")) {
    QMessageBox::critical(this, tr("Save Display as PNG"),
                          tr("Could not write %1").arg(path));
  }
}

bool CatalogueDialog::canSavePage() const {
  const int page = payload_stack_->currentIndex();
  return page == kPageCellGrid || page == kPageDisplayList;
}

QImage CatalogueDialog::renderedPageImage() const {
  switch (payload_stack_->currentIndex()) {
    case kPageCellGrid:
      return grid_widget_->renderPageImage(grid_widget_->pageImageSize());
    case kPageDisplayList:
      return display_widget_->renderPageImage(display_widget_->pageImageSize());
    default:
      return {};
  }
}

QString CatalogueDialog::suggestedPageFileName() const {
  const size_t index = displayedIndex();
  if (index == std::numeric_limits<size_t>::max()) {
    return QStringLiteral("display.png");
  }
  return pageFileNameFor(index);
}

QString CatalogueDialog::pageFileNameFor(size_t index) const {
  // What the reader would call this display: the service's noun for an item,
  // the key they would type to reach it, and which variant of it this is.
  QStringList parts;
  parts << to_file_name_part(data_.schema.item_noun.empty()
                                 ? tr("Display")
                                 : to_qstring(data_.schema.item_noun));

  const orc::CatalogueItem& item = data_.items[index];
  const orc::CatalogueItem* parent = &item;
  if (!item.parent_id.empty()) {
    for (const auto& candidate : data_.items) {
      if (candidate.id == item.parent_id) {
        parent = &candidate;
        break;
      }
    }
  }
  QString key = to_qstring(parent->find_key);
  if (key.isEmpty() && !parent->values.empty()) {
    key = to_qstring(parent->values.front());
  }
  parts << to_file_name_part(key);
  parts << to_file_name_part(to_qstring(item.variant_label));

  parts.removeAll(QString());
  return parts.join(QLatin1Char('-')) + QStringLiteral(".png");
}

bool CatalogueDialog::canSaveAllPages() const {
  for (const auto& payload : data_.payloads) {
    if (payload.kind == orc::CataloguePayload::Kind::kCellGrid ||
        payload.kind == orc::CataloguePayload::Kind::kDisplayList) {
      return true;
    }
  }
  return false;
}

std::vector<CatalogueDialog::PageExport> CatalogueDialog::exportablePages()
    const {
  std::vector<PageExport> pages;
  QSet<QString> used;
  for (size_t i = 0; i < data_.payloads.size(); ++i) {
    const auto kind = data_.payloads[i].kind;
    if (kind != orc::CataloguePayload::Kind::kCellGrid &&
        kind != orc::CataloguePayload::Kind::kDisplayList) {
      continue;
    }
    QString name = pageFileNameFor(i);
    if (used.contains(name)) {
      // Two displays whose keys sanitise to the same name would silently
      // overwrite each other on disk, so the later ones are numbered.
      const QString stem = name.left(name.size() - 4);
      int copy = 2;
      do {
        name = stem + QStringLiteral("-%1.png").arg(copy++);
      } while (used.contains(name));
    }
    used.insert(name);
    pages.push_back(PageExport{i, name});
  }
  return pages;
}

QImage CatalogueDialog::renderedPageImageAt(size_t index) const {
  if (index >= data_.payloads.size()) {
    return {};
  }
  // Rendered through a scratch widget rather than the ones on screen: those
  // are stateful, and pushing every payload of the catalogue through them
  // would end the batch on whichever page happened to be last.
  const orc::CataloguePayload& payload = data_.payloads[index];
  switch (payload.kind) {
    case orc::CataloguePayload::Kind::kCellGrid: {
      CatalogueCellGridWidget widget;
      widget.setGrid(payload.grid);
      widget.setShowDataErrors(highlight_check_->isChecked());
      return widget.renderPageImage(widget.pageImageSize());
    }
    case orc::CataloguePayload::Kind::kDisplayList: {
      CatalogueDisplayListWidget widget;
      widget.setDisplayList(payload.display_list);
      widget.setShowDataErrors(highlight_check_->isChecked());
      return widget.renderPageImage(widget.pageImageSize());
    }
    default:
      return {};
  }
}

void CatalogueDialog::onSaveAllClicked() {
  const std::vector<PageExport> pages = exportablePages();
  if (pages.empty()) {
    return;
  }

  // The same remembered directory the single save uses, so the batch lands
  // where the reader last put anything.
  QSettings settings("orc-project", "orc-gui");
  QString directory =
      settings.value("lastExportDirectory", QString()).toString();
  if (directory.isEmpty() || !QFileInfo(directory).isDir()) {
    directory = QDir::homePath();
  }

  const QString chosen = QFileDialog::getExistingDirectory(
      this, tr("Save All Displays as PNG"), directory);
  if (chosen.isEmpty()) {
    return;  // cancelled
  }
  settings.setValue("lastExportDirectory", chosen);

  // A service can carry hundreds of pages, so the batch says how far it has
  // got and can be abandoned partway.
  QProgressDialog progress(tr("Saving %1 images…").arg(pages.size()),
                           tr("Cancel"), 0, static_cast<int>(pages.size()),
                           this);
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(0);

  int saved = 0;
  QStringList failures;
  for (size_t i = 0; i < pages.size(); ++i) {
    progress.setValue(static_cast<int>(i));
    if (progress.wasCanceled()) {
      break;
    }
    const QImage image = renderedPageImageAt(pages[i].index);
    const QString path = QDir(chosen).filePath(pages[i].file_name);
    if (!image.isNull() && image.save(path, "PNG")) {
      ++saved;
    } else {
      failures << pages[i].file_name;
    }
  }
  progress.setValue(static_cast<int>(pages.size()));

  if (!failures.isEmpty()) {
    QMessageBox::warning(
        this, tr("Save All Displays as PNG"),
        tr("Saved %1 of %2 images to %3.\nCould not write: %4")
            .arg(saved)
            .arg(pages.size())
            .arg(chosen, failures.mid(0, 5).join(QStringLiteral(", "))));
  } else if (!progress.wasCanceled()) {
    // Said outright: the point of the batch is that the reader did not watch
    // each file land, so the count is the receipt.
    QMessageBox::information(
        this, tr("Save All Displays as PNG"),
        tr("Saved %1 images to %2").arg(saved).arg(chosen));
  }
}

// ---------------------------------------------------------------------------
// Test seams
// ---------------------------------------------------------------------------

std::vector<QString> CatalogueDialog::listedItems() const {
  std::vector<QString> labels;
  labels.reserve(static_cast<size_t>(items_table_->rowCount()));
  for (int row = 0; row < items_table_->rowCount(); ++row) {
    const auto* cell = items_table_->item(row, 0);
    labels.push_back(cell == nullptr ? QString() : cell->text());
  }
  return labels;
}

QString CatalogueDialog::listedValue(const QString& key, int column) const {
  for (int row = 0; row < items_table_->rowCount(); ++row) {
    const size_t index = top_level_[static_cast<size_t>(row)];
    if (to_qstring(data_.items[index].find_key) == key ||
        to_qstring(data_.items[index].id) == key) {
      const auto* cell = items_table_->item(row, column);
      return cell == nullptr ? QString() : cell->text();
    }
  }
  return {};
}

void CatalogueDialog::selectItem(int index) { showItem(index); }

void CatalogueDialog::showNextItem() { stepItem(1); }

void CatalogueDialog::showPreviousItem() { stepItem(-1); }

QString CatalogueDialog::findText() const {
  return find_bar_->isVisibleTo(this) ? find_edit_->text() : QString();
}

void CatalogueDialog::setFindText(const QString& text) {
  find_edit_->setText(text);
}

// The readouts below report empty when their widget is hidden. The test is
// isVisibleTo(this) rather than isVisible(): a dialogue that has never been
// shown has no visible widgets at all, and these have to answer the same way
// whether or not the window is on screen.
QString CatalogueDialog::variantText() const {
  return variant_bar_->isVisibleTo(this) ? variant_label_->text() : QString();
}

int CatalogueDialog::variantCount() const {
  return static_cast<int>(variantsOf(current_row_).size());
}

int CatalogueDialog::variantIndex() const {
  return current_row_ < 0 ? -1 : variant_index_;
}

void CatalogueDialog::showNextVariant() { stepVariant(1); }

void CatalogueDialog::showPreviousVariant() { stepVariant(-1); }

QString CatalogueDialog::headlineText() const {
  return headline_label_->text();
}

QString CatalogueDialog::conditionText() const {
  return condition_label_->isVisibleTo(this) ? condition_label_->text()
                                             : QString();
}

QString CatalogueDialog::summaryText() const {
  return summary_label_->isVisibleTo(this) ? summary_label_->text() : QString();
}

QString CatalogueDialog::noticeText() const {
  return notice_label_->isVisibleTo(this) ? notice_label_->text() : QString();
}

const orc::CatalogueCellGrid* CatalogueDialog::currentGrid() const {
  return payload_stack_->currentIndex() == kPageCellGrid ? grid_widget_->grid()
                                                         : nullptr;
}

const orc::CatalogueDisplayList* CatalogueDialog::currentDisplayList() const {
  const size_t index = displayedIndex();
  if (index == std::numeric_limits<size_t>::max() ||
      payload_stack_->currentIndex() != kPageDisplayList) {
    return nullptr;
  }
  return &data_.payloads[index].display_list;
}

QString CatalogueDialog::currentText() const {
  if (payload_stack_->currentIndex() == kPageText) {
    return text_pane_->toPlainText();
  }
  if (payload_stack_->currentIndex() == kPageDisplayList) {
    return companion_pane_->toPlainText();
  }
  return {};
}

std::vector<QString> CatalogueDialog::listedTableRows() const {
  std::vector<QString> rows;
  if (payload_stack_->currentIndex() != kPageTable) {
    return rows;
  }
  rows.reserve(static_cast<size_t>(table_pane_->rowCount()));
  for (int row = 0; row < table_pane_->rowCount(); ++row) {
    QStringList values;
    for (int column = 0; column < table_pane_->columnCount(); ++column) {
      const auto* cell = table_pane_->item(row, column);
      values << (cell == nullptr ? QString() : cell->text());
    }
    rows.push_back(values.join(QStringLiteral(" | ")));
  }
  return rows;
}
