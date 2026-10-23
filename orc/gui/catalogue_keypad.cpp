/*
 * File:        catalogue_keypad.cpp
 * Module:      orc-gui
 * Purpose:     The keypad a catalogue item that takes keys is played on, and
 *              page numbers are typed on
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_keypad.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

/// The digits as a set-top keypad lays them out: three rows of three, and the
/// 0 alone under the 8 — the shape a page played on one draws its controls in.
struct KeyPosition {
  const char* key;
  int row;
  int column;
};
constexpr KeyPosition kKeypadLayout[] = {
    {"1", 0, 0}, {"2", 0, 1}, {"3", 0, 2}, {"4", 1, 0}, {"5", 1, 1},
    {"6", 1, 2}, {"7", 2, 0}, {"8", 2, 1}, {"9", 2, 2}, {"0", 3, 1},
};

/// Square keys, big enough to hit and small enough to sit beside a page.
constexpr int kKeySize = 30;
/// Gap between neighbouring keys: enough to tell them apart, close enough
/// that the pad reads as one block.
constexpr int kKeySpacing = 3;

}  // namespace

CatalogueKeypad::CatalogueKeypad(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 0, 0, 0);

  title_label_ = new QLabel(this);
  title_label_->setObjectName("catalogueKeypadTitle");
  title_label_->setAlignment(Qt::AlignHCenter);
  layout->addWidget(title_label_);

  // Play or Page, for an item that takes keys in a catalogue whose numbers
  // can also be typed: a decoder's digits did one or the other.
  mode_bar_ = new QWidget(this);
  auto* mode_layout = new QHBoxLayout(mode_bar_);
  mode_layout->setContentsMargins(0, 0, 0, 0);
  mode_layout->setSpacing(0);
  auto* modes = new QButtonGroup(mode_bar_);
  modes->setExclusive(true);
  const auto add_mode = [&](const QString& text, const char* object_name,
                            const QString& tip) {
    auto* button = new QToolButton(mode_bar_);
    button->setObjectName(QString::fromLatin1(object_name));
    button->setText(text);
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(tip);
    modes->addButton(button);
    mode_layout->addWidget(button);
    return button;
  };
  play_mode_button_ =
      add_mode(tr("Play"), "catalogueKeypadPlay",
               tr("The digits play the page, as its keys did on a decoder."));
  page_mode_button_ =
      add_mode(tr("Page"), "catalogueKeypadPage",
               tr("The digits type a page number, as on a decoder."));
  play_mode_button_->setChecked(true);
  connect(modes, &QButtonGroup::buttonToggled, this,
          [this](QAbstractButton*, bool) { refreshButtons(); });
  mode_bar_->setVisible(false);
  layout->addWidget(mode_bar_, 0, Qt::AlignHCenter);

  auto* grid = new QGridLayout();
  grid->setSpacing(kKeySpacing);
  for (const KeyPosition& position : kKeypadLayout) {
    const QString key = QString::fromLatin1(position.key);
    auto* button = new QToolButton(this);
    button->setObjectName(QStringLiteral("catalogueKey") + key);
    button->setText(key);
    button->setFixedSize(kKeySize, kKeySize);
    button->setEnabled(false);
    // The drawing keeps the keyboard focus, so typed digits keep working.
    button->setFocusPolicy(Qt::NoFocus);
    connect(button, &QToolButton::clicked, this,
            [this, key] { pressKey(key); });
    grid->addWidget(button, position.row, position.column);
    buttons_[key] = button;
  }
  layout->addLayout(grid);

  reset_button_ = new QToolButton(this);
  reset_button_->setObjectName("catalogueResetGameButton");
  reset_button_->setText(tr("Reset game"));
  reset_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  reset_button_->setFocusPolicy(Qt::NoFocus);
  reset_button_->setEnabled(false);
  reset_button_->setToolTip(
      tr("Show the page again as it was sent, before any key was pressed."));
  connect(reset_button_, &QToolButton::clicked, this,
          &CatalogueKeypad::resetRequested);
  layout->addWidget(reset_button_, 0, Qt::AlignHCenter);

  pressed_label_ = new QLabel(this);
  pressed_label_->setObjectName("catalogueKeysPressed");
  pressed_label_->setAlignment(Qt::AlignHCenter);
  pressed_label_->setWordWrap(true);
  layout->addWidget(pressed_label_);
  layout->addStretch();

  setToolTip(
      tr("A decoder's keypad. A page made to be played takes keys: press "
         "them here, or type the digits while the page has the focus, and "
         "the page is redrawn as the decoder redrew it."));
}

void CatalogueKeypad::setTitle(const QString& title) {
  title_label_->setText(title);
}

void CatalogueKeypad::setModes(bool play, bool page) {
  play_available_ = play;
  page_available_ = page;
  mode_bar_->setVisible(play && page);
  // Each item that plays starts on Play; one that does not cannot.
  play_mode_button_->setChecked(play);
  page_mode_button_->setChecked(!play);
  refreshButtons();
}

bool CatalogueKeypad::playing() const {
  return play_available_ &&
         (!page_available_ || play_mode_button_->isChecked());
}

void CatalogueKeypad::setPlaying(bool playing) {
  if (play_available_ && page_available_) {
    (playing ? play_mode_button_ : page_mode_button_)->setChecked(true);
  }
}

void CatalogueKeypad::setKeys(const QStringList& keys) {
  keys_ = keys;
  refreshButtons();
}

void CatalogueKeypad::setPressed(const QStringList& pressed) {
  reset_button_->setEnabled(!pressed.isEmpty());
  pressed_label_->setText(
      pressed.isEmpty() ? QString()
                        : tr("Keys: %1").arg(pressed.join(QLatin1Char(' '))));
}

bool CatalogueKeypad::pressKey(const QString& key) {
  if (playing()) {
    if (!keys_.contains(key)) {
      return false;
    }
    emit keyPressed(key);
    return true;
  }
  if (page_available_ && buttons_.count(key) != 0) {
    emit pageDigitPressed(key);
    return true;
  }
  return false;
}

QToolButton* CatalogueKeypad::button(const QString& key) const {
  const auto found = buttons_.find(key);
  return found == buttons_.end() ? nullptr : found->second;
}

bool CatalogueKeypad::isModeSwitchVisible() const {
  return mode_bar_->isVisibleTo(this);
}

QString CatalogueKeypad::pressedText() const { return pressed_label_->text(); }

void CatalogueKeypad::refreshButtons() {
  const bool play = playing();
  for (auto& [key, button] : buttons_) {
    button->setEnabled(play ? keys_.contains(key) : page_available_);
  }
}
