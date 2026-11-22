/*
 * File:        stage_help_dialog.cpp
 * Module:      orc-gui
 * Purpose:     Modal dialog for displaying stage help documentation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "stage_help_dialog.h"

#include <QDialogButtonBox>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTextBlock>
#include <QVBoxLayout>

StageHelpDialog::StageHelpDialog(const QString& stage_display_name,
                                 const QString& markdown, QWidget* parent)
    : QDialog(parent), browser_(new QTextBrowser(this)) {
  setWindowTitle(QString("Help — %1").arg(stage_display_name));
  setAttribute(Qt::WA_DeleteOnClose);
  resize(720, 560);
  setup_ui(markdown);
}

void StageHelpDialog::setup_ui(const QString& markdown) {
  browser_->document()->setDefaultStyleSheet(
      "h1 { margin-top: 16px; }"
      "h2 { margin-top: 14px; }"
      "h3 { margin-top: 10px; }");
  browser_->setOpenExternalLinks(false);
  browser_->setMarkdown(markdown);
  browser_->setReadOnly(true);
  browser_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(8);
  layout->addWidget(browser_);
  layout->addWidget(buttons);
  setLayout(layout);
}

bool StageHelpDialog::scrollToHeading(const QString& heading) {
  // Markdown headings carry no anchors in a QTextDocument, so the heading is
  // found as a block whose text is the heading and has a heading level.
  for (QTextBlock block = browser_->document()->begin(); block.isValid();
       block = block.next()) {
    if (block.blockFormat().headingLevel() > 0 &&
        block.text().trimmed() == heading) {
      QTextCursor cursor(block);
      browser_->setTextCursor(cursor);
      // Bring the heading to the top rather than just into view: scrolling
      // to the end first makes ensureCursorVisible() stop with it at the top.
      browser_->verticalScrollBar()->setValue(
          browser_->verticalScrollBar()->maximum());
      browser_->ensureCursorVisible();
      return true;
    }
  }
  return false;
}
