/*
 * File:        catalogue_keypad.h
 * Module:      orc-gui
 * Purpose:     The keypad a catalogue item that takes keys is played on, and
 *              page numbers are typed on
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef CATALOGUE_KEYPAD_H
#define CATALOGUE_KEYPAD_H

#include <QString>
#include <QStringList>
#include <QWidget>
#include <map>

class QLabel;
class QToolButton;

/**
 * @brief A decoder's keypad, for a catalogue item played on keys
 *
 * The ten digits laid out as a set-top keypad lays them — 1 2 3, 4 5 6,
 * 7 8 9, then 0 under the 8 — because the pages that were played on one
 * arrange their keys by where they sit on it: a game can move its cursor
 * with the 5, 7, 9 and 0 round the 8 that erases a square. Under them,
 * Reset game, and which keys have been played so far.
 *
 * The digits do one of two things, as a decoder's did: play the item on show,
 * where it takes keys (orc::CatalogueItem::keys), or type a page number. Where
 * both are open to the reader a switch above the keys chooses, on Play to
 * begin with; where only one is, the keypad does that. Playing, only the keys
 * the item takes are enabled.
 *
 * The keypad only reports presses: the owner keeps the item's sequence of keys
 * and asks the stage what it shows, and it owns what a page digit does.
 *
 * Not thread-safe: a widget, used from the GUI thread only.
 */
class CatalogueKeypad : public QWidget {
  Q_OBJECT

 public:
  explicit CatalogueKeypad(QWidget* parent = nullptr);

  /// What the service calls the keys — "Option keys" — as the heading.
  void setTitle(const QString& title);

  /// Which of the two things the digits can do here: play the item on show,
  /// and type a page number. The switch between them is offered only where
  /// both are open.
  void setModes(bool play, bool page);
  /// Whether a digit plays the item on show rather than typing a page number.
  bool playing() const;
  /// Choose playing (true) or typing a page number (false), as the reader
  /// would with the switch; ignored where that is not open.
  void setPlaying(bool playing);

  /// The keys the item on show takes, enabled while playing.
  void setKeys(const QStringList& keys);

  /// The keys played so far, for the readout under the buttons; Reset game is
  /// offered while there are any.
  void setPressed(const QStringList& pressed);

  /// Press |key| as the reader would: emits keyPressed() where playing and
  /// the item takes it, pageDigitPressed() where typing a page number, and
  /// does nothing otherwise. Returns whether it did something.
  bool pressKey(const QString& key);

  /// The button for |key|, or nullptr (test seam).
  QToolButton* button(const QString& key) const;
  /// The Reset game button (test seam).
  QToolButton* resetButton() const { return reset_button_; }
  /// Whether the Play/Page switch is offered (test seam).
  bool isModeSwitchVisible() const;
  /// The readout of the keys played (test seam).
  QString pressedText() const;

 signals:
  /// The reader pressed |key|, one the item on show takes, to play it.
  void keyPressed(const QString& key);
  /// The reader typed |digit| of a page number.
  void pageDigitPressed(const QString& digit);
  /// The reader asked to start the item again.
  void resetRequested();

 private:
  /// Enable the digits for what they do now.
  void refreshButtons();

  QLabel* title_label_ = nullptr;
  QWidget* mode_bar_ = nullptr;
  QToolButton* play_mode_button_ = nullptr;
  QToolButton* page_mode_button_ = nullptr;
  std::map<QString, QToolButton*> buttons_;
  QToolButton* reset_button_ = nullptr;
  QLabel* pressed_label_ = nullptr;
  QStringList keys_;
  bool play_available_ = false;
  bool page_available_ = false;
};

#endif  // CATALOGUE_KEYPAD_H
