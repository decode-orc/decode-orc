/*
 * File:        closedcaptiondialog.cpp
 * Module:      orc-gui
 * Purpose:     Closed caption preview dialog (observer dialog)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "closedcaptiondialog.h"

#include <QAbstractItemView>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QStringList>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <cstddef>

namespace {

// One recovered byte as the caption world writes it: two hexadecimal digits,
// with the odd-parity bit the transmission carried restored, because that is
// how the same byte appears in an SCC file and in every caption tool.
QString formatCaptionByte(int32_t value, bool parity_valid) {
  uint8_t byte = static_cast<uint8_t>(value & 0x7F);
  int ones = 0;
  for (uint8_t bit = byte; bit != 0; bit >>= 1) {
    ones += static_cast<int>(bit & 1U);
  }
  if (ones % 2 == 0) {
    byte |= 0x80;  // CTA-608-E §4.4: each byte carries odd parity
  }
  const QString hex = QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0'));
  // A byte whose parity did not check out is still shown — it is what the
  // recording holds — but it must not read as a confirmed value.
  return parity_valid ? hex : hex + QStringLiteral("?");
}

}  // namespace

ClosedCaptionDialog::ClosedCaptionDialog(QWidget* parent) : QDialog(parent) {
  setupUI();
  setWindowTitle(tr("Closed Captions"));

  // Use Qt::Window flag to allow independent positioning
  setWindowFlags(Qt::Window);

  // Don't destroy on close, just hide
  setAttribute(Qt::WA_DeleteOnClose, false);

  resize(560, 420);
  setMinimumSize(360, 240);
}

ClosedCaptionDialog::~ClosedCaptionDialog() = default;

void ClosedCaptionDialog::setupUI() {
  // The status bar sits flush against the window edge, so the outer layout
  // carries no margins and the content above it supplies its own.
  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  auto* content_layout = new QVBoxLayout();
  content_layout->setContentsMargins(9, 9, 9, 9);

  // Line 21 carries four services in one byte-pair stream and they cannot be
  // read together — decoded as one they interleave, a caption running through
  // a page of listings. CC1 is what a captioned programme uses; the others are
  // there when a recording turns out to carry more than one.
  auto* service_layout = new QHBoxLayout();
  auto* service_heading = new QLabel(tr("Service:"), this);
  service_selector_ = new QComboBox(this);
  service_selector_->setObjectName("closedCaptionServiceSelector");
  service_selector_->addItem(
      tr("CC1 — primary captions"),
      QVariant::fromValue(static_cast<int>(orc::EIA608Service::CC1)));
  service_selector_->addItem(
      tr("CC2 — second caption service"),
      QVariant::fromValue(static_cast<int>(orc::EIA608Service::CC2)));
  service_selector_->addItem(
      tr("TEXT1 — text service"),
      QVariant::fromValue(static_cast<int>(orc::EIA608Service::T1)));
  service_selector_->addItem(
      tr("TEXT2 — text service"),
      QVariant::fromValue(static_cast<int>(orc::EIA608Service::T2)));
  service_selector_->setToolTip(
      tr("Which of the services multiplexed onto line 21 to decode. CC1 is the "
         "primary caption service; CC2 often carries a translation; the text "
         "services carry pages of information rather than captions."));
  service_layout->addWidget(service_heading);
  service_layout->addWidget(service_selector_, /*stretch=*/1);
  content_layout->addLayout(service_layout);
  connect(service_selector_, &QComboBox::currentIndexChanged, this,
          [this](int) { onServiceChanged(); });

  // The captions decoded so far. A caption is on screen for a few seconds and
  // then gone, so without a transcript the only way to read one is to be
  // looking at the right frame when it goes by.
  auto* list_heading = new QLabel(tr("Captions decoded:"), this);
  content_layout->addWidget(list_heading);

  captions_table_ = new QTableWidget(0, kColumnCount, this);
  captions_table_->setObjectName("closedCaptionTable");
  captions_table_->setHorizontalHeaderLabels({tr("Frame"), tr("Caption")});
  captions_table_->horizontalHeaderItem(kColumnFrame)
      ->setToolTip(tr("Frame the caption appeared at."));
  captions_table_->horizontalHeaderItem(kColumnFrame)
      ->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
  // Read-only: the transcript reports where the previewer is rather than
  // driving it, and a selection of the reader's own would fight the marker.
  captions_table_->setSelectionMode(QAbstractItemView::NoSelection);
  captions_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  captions_table_->setShowGrid(false);
  captions_table_->setAlternatingRowColors(true);
  captions_table_->setWordWrap(false);
  captions_table_->setCornerButtonEnabled(false);
  captions_table_->verticalHeader()->setVisible(false);
  // Per-pixel scrolling keeps the view anchored where the user left it while
  // rows are appended underneath during playback.
  captions_table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  auto* header = captions_table_->horizontalHeader();
  header->setSectionResizeMode(kColumnFrame, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColumnCaption, QHeaderView::Stretch);
  content_layout->addWidget(captions_table_, /*stretch=*/1);

  main_layout->addLayout(content_layout, /*stretch=*/1);

  status_bar_ = new QStatusBar(this);
  status_bar_->setObjectName("closedCaptionStatusBar");
  status_bar_->setSizeGripEnabled(false);

  status_summary_ = new QLabel(tr("No caption data"), this);
  status_summary_->setObjectName("closedCaptionStatusSummary");
  status_bar_->addWidget(status_summary_, /*stretch=*/1);

  data_label_ = new QLabel(this);
  data_label_->setObjectName("closedCaptionDataLabel");
  data_label_->setToolTip(
      tr("The EIA-608 byte pair recovered from each field's caption data "
         "line, shown with the odd parity bit the transmission carried. A byte "
         "whose parity did not check out is marked with a question mark. A "
         "pair whose first byte failed is not fed to the caption decoder, nor "
         "is a pair that is no code EIA-608 defines for its field; when only "
         "the second byte failed, the first is fed alone if it is a "
         "character. A recording whose first stored field is television field "
         "2 is read the other way round, and its fields are numbered as "
         "television fields."));
  status_bar_->addPermanentWidget(data_label_);

  mode_label_ = new QLabel(this);
  mode_label_->setObjectName("closedCaptionModeLabel");
  mode_label_->setToolTip(
      tr("How the caption service is putting text on screen: pop-on captions "
         "are prepared off screen and appear complete, roll-up captions scroll "
         "a line at a time, paint-on captions appear character by character."));
  mode_label_->setVisible(false);
  status_bar_->addPermanentWidget(mode_label_);

  status_label_ = new QLabel(this);
  status_label_->setObjectName("observationStatusLabel");
  status_bar_->addPermanentWidget(status_label_);
  status_label_->setVisible(false);

  main_layout->addWidget(status_bar_);
}

void ClosedCaptionDialog::onServiceChanged() {
  const auto selected =
      static_cast<orc::EIA608Service>(service_selector_->currentData().toInt());
  if (selected == assembler_.service()) {
    return;
  }
  assembler_.setService(selected);
  list_populated_ = false;
  current_screen_ = ClosedCaptionAssembler::CaptionScreen{};
  has_current_screen_ = false;
  current_row_ = -1;
  renderCurrentFrame();
  emit dataRequestNeeded();
}

void ClosedCaptionDialog::selectService(orc::EIA608Service service) {
  const int index = service_selector_->findData(
      QVariant::fromValue(static_cast<int>(service)));
  if (index >= 0) {
    service_selector_->setCurrentIndex(index);
  }
}

void ClosedCaptionDialog::showPending() { updatePendingStatus(); }

void ClosedCaptionDialog::updatePendingStatus() {
  // The whole outstanding count, not the capped batch, so the readout counts
  // down towards zero instead of sitting at the batch size.
  const std::size_t outstanding = assembler_.framesNeedingDataCount();
  if (outstanding == 0) {
    status_label_->setVisible(false);
    status_label_->clear();
    return;
  }
  status_label_->setText(
      tr("Reading %1 frames…").arg(static_cast<qulonglong>(outstanding)));
  status_label_->setVisible(true);
}

void ClosedCaptionDialog::clearContent() {
  assembler_.clear();
  list_populated_ = false;
  status_label_->setVisible(false);
  status_label_->clear();
  refreshCaptionList();
  status_summary_->setText(tr("No caption data"));
  mode_label_->setVisible(false);
  data_label_->clear();
  current_screen_ = ClosedCaptionAssembler::CaptionScreen{};
  has_current_screen_ = false;
  current_row_ = -1;
}

void ClosedCaptionDialog::clearCache() {
  assembler_.clear();
  list_populated_ = false;
  renderCurrentFrame();
}

void ClosedCaptionDialog::setCurrentFrame(uint64_t frame_index) {
  assembler_.setCurrentFrame(frame_index);
  renderCurrentFrame();
}

void ClosedCaptionDialog::deliverFrameData(
    bool available, uint64_t field1_id_value,
    const orc::presenters::ClosedCaptionFieldDataView& field1,
    const orc::presenters::ClosedCaptionFieldDataView& field2) {
  const uint64_t frame_index = field1_id_value / 2;
  if (available) {
    assembler_.storeFrame(frame_index, field1, field2);
  } else {
    assembler_.markFrameUnavailable(frame_index);
  }
  updatePendingStatus();
  renderCurrentFrame();
}

QString ClosedCaptionDialog::currentCaptionText() const {
  return has_current_screen_ ? QString::fromStdString(current_screen_.text())
                             : QString();
}

std::vector<QString> ClosedCaptionDialog::listedCaptions() const {
  std::vector<QString> captions;
  captions.reserve(static_cast<std::size_t>(captions_table_->rowCount()));
  for (int row = 0; row < captions_table_->rowCount(); ++row) {
    captions.push_back(captions_table_->item(row, kColumnCaption)->text());
  }
  return captions;
}

QString ClosedCaptionDialog::modeText() const {
  return mode_label_->isVisible() ? mode_label_->text() : QString();
}

QString ClosedCaptionDialog::dataText() const { return data_label_->text(); }

QString ClosedCaptionDialog::statusText() const {
  return status_summary_->text();
}

QString ClosedCaptionDialog::formatMode(
    const ClosedCaptionAssembler::ScreenChange& change) {
  switch (change.mode) {
    case orc::CaptionMode::ROLL_UP:
      return tr("Roll-up %n row(s)", nullptr, change.rollup_rows);
    case orc::CaptionMode::PAINT_ON:
      return tr("Paint-on");
    case orc::CaptionMode::TEXT:
      return tr("Text service");
    case orc::CaptionMode::POP_ON:
      break;
  }
  return tr("Pop-on");
}

QString ClosedCaptionDialog::formatFrameData(
    const ClosedCaptionAssembler::FrameData* data, bool fields_reversed) {
  if (data == nullptr) {
    return {};  // frame not decoded yet; the pending notice says why
  }

  QStringList fields;
  int stored_field = 0;
  for (const auto* field : {&data->field1, &data->field2}) {
    const int field_number =
        (fields_reversed ? 1 - stored_field : stored_field) + 1;
    ++stored_field;
    if (field->present) {
      fields << tr("F%1 %2 %3")
                    .arg(field_number)
                    .arg(formatCaptionByte(field->data0, field->parity0_valid),
                         formatCaptionByte(field->data1, field->parity1_valid));
    }
  }
  if (fields.isEmpty()) {
    // Every frame of an uncaptioned recording lands here, and so does every
    // frame between two captions of a captioned one: the caption data line is
    // only written to while there is something to say.
    return tr("No caption data on this frame");
  }
  return fields.join(QStringLiteral("  "));
}

void ClosedCaptionDialog::refreshCaptionList() {
  const uint64_t revision = assembler_.historyRevision();
  if (list_populated_ && revision == listed_revision_) {
    return;  // nothing new decoded; leave the table (and its scroll) alone
  }
  listed_revision_ = revision;
  list_populated_ = true;

  const auto captions = assembler_.captions();

  // Rows are merged rather than rebuilt. Playing forward appends a caption
  // every few seconds, and rebuilding the table each time would drop the
  // reader's scroll position while they were part-way through reading it.
  // Everything is appended in frame order, so the merge is a common prefix
  // plus whatever follows it.
  std::size_t common = 0;
  while (common < listed_frames_.size() && common < captions.size() &&
         listed_frames_[common] == captions[common].frame) {
    ++common;
  }
  while (captions_table_->rowCount() > static_cast<int>(common)) {
    captions_table_->removeRow(captions_table_->rowCount() - 1);
  }
  listed_frames_.resize(common);

  for (std::size_t index = common; index < captions.size(); ++index) {
    const auto& caption = captions[index];
    const int row = captions_table_->rowCount();
    captions_table_->insertRow(row);

    // Frame numbers are 1-based in the UI (see frame_numbering.h).
    auto* frame_item = new QTableWidgetItem(
        QString::number(static_cast<qulonglong>(caption.frame) + 1));
    frame_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* caption_item =
        new QTableWidgetItem(QString::fromStdString(caption.screen.text()));
    caption_item->setToolTip(caption_item->text());

    captions_table_->setItem(row, kColumnFrame, frame_item);
    captions_table_->setItem(row, kColumnCaption, caption_item);
    listed_frames_.push_back(caption.frame);
  }

  // Row indices have moved; the marker is re-applied by the caller.
  current_row_ = -1;
}

void ClosedCaptionDialog::markCurrentCaption(
    const ClosedCaptionAssembler::ScreenChange* current) {
  int row = -1;
  if (current != nullptr && !current->screen.blank()) {
    const auto it =
        std::find(listed_frames_.begin(), listed_frames_.end(), current->frame);
    if (it != listed_frames_.end()) {
      row = static_cast<int>(std::distance(listed_frames_.begin(), it));
    }
  }
  if (row == current_row_) {
    // Already marked. Re-applying would scroll the row back into view on every
    // delivered frame, fighting a reader who has scrolled elsewhere.
    return;
  }

  const auto set_row_bold = [this](int target, bool bold) {
    if (target < 0 || target >= captions_table_->rowCount()) {
      return;
    }
    for (const int column : {kColumnFrame, kColumnCaption}) {
      auto* item = captions_table_->item(target, column);
      if (item == nullptr) {
        continue;
      }
      QFont font = item->font();
      font.setBold(bold);
      item->setFont(font);
    }
  };

  set_row_bold(current_row_, false);
  set_row_bold(row, true);
  current_row_ = row;
  if (row >= 0) {
    captions_table_->scrollToItem(captions_table_->item(row, kColumnCaption),
                                  QAbstractItemView::EnsureVisible);
  }
}

void ClosedCaptionDialog::renderCurrentFrame() {
  refreshCaptionList();

  const uint64_t frame = assembler_.currentFrame();
  data_label_->setText(formatFrameData(assembler_.frameData(frame),
                                       assembler_.fieldsReversed()));

  const auto* current = assembler_.screenAt(frame);
  if (current == nullptr) {
    // Nothing decoded at or before this frame: either the read is still in
    // flight, or the recording has carried no caption data since the window
    // opened.
    current_screen_ = ClosedCaptionAssembler::CaptionScreen{};
    has_current_screen_ = false;
    status_summary_->setText(captions_table_->rowCount() == 0
                                 ? tr("No captions decoded yet")
                                 : tr("No caption data"));
    mode_label_->setVisible(false);
    markCurrentCaption(nullptr);
    return;
  }

  current_screen_ = current->screen;
  has_current_screen_ = !current->screen.blank();
  mode_label_->setText(formatMode(*current));
  mode_label_->setVisible(true);

  if (current->screen.blank()) {
    // The caption stream is running but has cleared the screen — between two
    // captions, which is most of a captioned recording.
    status_summary_->setText(tr("No caption on screen"));
  } else {
    status_summary_->setText(
        tr("Caption shown from frame %1")
            .arg(static_cast<qulonglong>(current->frame) + 1));
  }
  markCurrentCaption(current);
}
