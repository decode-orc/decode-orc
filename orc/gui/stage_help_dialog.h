/*
 * File:        stage_help_dialog.h
 * Module:      orc-gui
 * Purpose:     Modal dialog for displaying stage help documentation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QDialog>
#include <QString>
#include <QTextBrowser>

/**
 * @brief Modal dialog that renders stage help documentation as Markdown.
 *
 * Each stage plugin supplies its documentation via get_instructions(). This
 * dialog is created on demand from the stage context menu "Help" action,
 * renders the Markdown using QTextBrowser, and is destroyed when closed.
 */
class StageHelpDialog : public QDialog {
  Q_OBJECT

 public:
  /**
   * @param stage_display_name  Human-readable stage name shown in the title
   * bar.
   * @param markdown            Markdown text returned by the stage's
   *                            get_instructions() method.
   * @param parent              Parent widget.
   */
  explicit StageHelpDialog(const QString& stage_display_name,
                           const QString& markdown, QWidget* parent = nullptr);

  /**
   * @brief Scroll so the first heading that reads @p heading is in view.
   * @return False when the text has no such heading.
   */
  bool scrollToHeading(const QString& heading);

 private:
  void setup_ui(const QString& markdown);

  QTextBrowser* browser_;
};
