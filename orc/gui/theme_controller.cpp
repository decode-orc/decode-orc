/*
 * File:        theme_controller.cpp
 * Module:      orc-gui
 * Purpose:     Runtime application of theme modes and OS colour-scheme tracking
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "theme_controller.h"

#include <orc/view-support/plot_widget.h>

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QSettings>
#include <QString>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QWidget>

#include "logging.h"

namespace {
// QSettings key persisting the user's chosen theme mode across runs.
constexpr auto kThemeModeSettingsKey = "theme/mode";
}  // namespace

ThemeController* ThemeController::s_instance = nullptr;

ThemeController::ThemeController(QApplication& app,
                                 const QString& initialModeArgument,
                                 QObject* parent)
    : QObject(parent), app_(app), mode_(ThemeManager::Mode::Auto) {
  ThemeManager manager(initialModeArgument);
  if (manager.hadInvalidMode()) {
    ORC_LOG_WARN("Unknown theme value '{}', falling back to auto",
                 manager.invalidMode().toStdString());
  }
  mode_ = manager.mode();

  s_instance = this;

  // Capture the platform's own style before the first theme is applied; it is
  // the style to restore whenever the fallback is not needed. The name is only
  // readable here: once an application stylesheet is installed, QApplication
  // wraps the style in an unnamed QStyleSheetStyle.
  if (const QStyle* style = app_.style()) {
    native_style_name_ = style->name();
  }
  applied_style_name_ = native_style_name_;
  if (native_style_name_.isEmpty()) {
    ORC_LOG_WARN(
        "Platform widget style is unnamed; the palette-driven style fallback "
        "is unavailable");
  }

  applyCurrentMode(QStringLiteral("startup"));
  updateSystemTracking();
}

ThemeController::~ThemeController() {
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

ThemeController* ThemeController::instance() { return s_instance; }

ThemeManager::Mode ThemeController::mode() const { return mode_; }

void ThemeController::setMode(ThemeManager::Mode mode) {
  if (mode == mode_) {
    return;
  }

  mode_ = mode;

  QSettings settings;
  settings.setValue(kThemeModeSettingsKey, ThemeManager::modeToString(mode_));

  applyCurrentMode(QStringLiteral("user selection"));
  updateSystemTracking();

  emit modeChanged(mode_);
}

void ThemeController::applyCurrentMode(const QString& changeReason) {
  ThemeManager manager(ThemeManager::modeToString(mode_));
  const ThemeManager::Resolution resolution = manager.resolve(app_);
  applyResolution(resolution);

  ORC_LOG_INFO(
      "Theme mode '{}' resolved to '{}' via {} ({})",
      manager.modeName().toStdString(),
      ThemeManager::colorSchemeToString(resolution.scheme).toStdString(),
      resolution.source.toStdString(), changeReason.toStdString());
  if (resolution.usedPaletteFallback) {
    ORC_LOG_WARN(
        "Theme auto-detection fell back to palette heuristic because Qt "
        "reported unknown color scheme");
  }
}

void ThemeController::applyResolution(
    const ThemeManager::Resolution& resolution) {
  app_.setProperty("isDarkTheme", resolution.isDark);
  app_.setProperty("themeMode", ThemeManager::modeToString(resolution.mode));
  // The style has to be settled first: QApplication::setStyle() resets the
  // application palette to the new style's standard palette.
  applyStyle(resolution.isDark);
  applyPalette(app_, resolution.isDark);

  const auto widgets = app_.allWidgets();
  for (QWidget* widget : widgets) {
    if (auto* plotWidget = qobject_cast<orc::view::PlotWidget*>(widget)) {
      plotWidget->updateTheme();
    }
  }
}

void ThemeController::updateSystemTracking() {
  const bool shouldTrack = (mode_ == ThemeManager::Mode::Auto);

  if (!shouldTrack) {
    if (tracking_connection_) {
      QObject::disconnect(tracking_connection_);
      tracking_connection_ = {};
      ORC_LOG_DEBUG("Runtime OS theme tracking disabled (forced theme mode)");
    }
    return;
  }

  if (tracking_connection_ || !app_.styleHints()) {
    return;
  }

  tracking_connection_ = QObject::connect(
      app_.styleHints(), &QStyleHints::colorSchemeChanged, this,
      [this](Qt::ColorScheme newScheme) {
        applyCurrentMode(
            QStringLiteral("OS color scheme changed to '%1'")
                .arg(ThemeManager::colorSchemeToString(newScheme)));
      });
  ORC_LOG_DEBUG("Runtime OS theme tracking enabled (auto mode)");
}

QString ThemeController::styleNameForTheme(const QString& nativeStyleName,
                                           bool isDark,
                                           Qt::ColorScheme osScheme) {
  if (ThemeManager::styleNeedsPaletteFallback(nativeStyleName, isDark,
                                              osScheme)) {
    return ThemeManager::paletteFallbackStyleName();
  }
  return nativeStyleName;
}

// Ensure the active widget style can actually render the requested theme.
void ThemeController::applyStyle(bool isDark) {
  if (native_style_name_.isEmpty()) {
    return;
  }

  const Qt::ColorScheme osScheme = app_.styleHints()
                                       ? app_.styleHints()->colorScheme()
                                       : Qt::ColorScheme::Unknown;
  const QString wanted =
      styleNameForTheme(native_style_name_, isDark, osScheme);

  if (wanted.compare(applied_style_name_, Qt::CaseInsensitive) == 0) {
    return;
  }

  QStyle* style = QStyleFactory::create(wanted);
  if (!style) {
    ORC_LOG_WARN("Widget style '{}' is unavailable, keeping '{}'",
                 wanted.toStdString(), applied_style_name_.toStdString());
    return;
  }

  // QApplication takes ownership of the style and deletes the previous one.
  app_.setStyle(style);
  applied_style_name_ = wanted;
  ORC_LOG_INFO("Widget style changed to '{}' for the {} theme (native '{}')",
               wanted.toStdString(), isDark ? "dark" : "light",
               native_style_name_.toStdString());
}

// Apply dark or light palette to the application.
void ThemeController::applyPalette(QApplication& app, bool isDark) {
  if (isDark) {
    // Dark theme palette
    QPalette darkPalette;

    // Base colors
    QColor darkGray(53, 53, 53);
    QColor darkerGray(42, 42, 42);
    QColor darkestGray(25, 25, 25);
    QColor blue(42, 130, 218);

    darkPalette.setColor(QPalette::Window, darkGray);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, darkestGray);
    darkPalette.setColor(QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, darkGray);
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Link, blue);
    darkPalette.setColor(QPalette::Highlight, blue);
    darkPalette.setColor(QPalette::HighlightedText, Qt::black);

    // Shading roles used for frames, bevels and separators. Roles left unset
    // fall back to the platform's system palette, which on Windows is derived
    // from GetSysColor() and stays light even when the desktop is in dark mode
    // - so every role a palette-driven style may read has to be specified.
    darkPalette.setColor(QPalette::Light, QColor(75, 75, 75));
    darkPalette.setColor(QPalette::Midlight, QColor(64, 64, 64));
    darkPalette.setColor(QPalette::Mid, darkerGray);
    darkPalette.setColor(QPalette::Dark, QColor(35, 35, 35));
    darkPalette.setColor(QPalette::Shadow, QColor(20, 20, 20));
    darkPalette.setColor(QPalette::PlaceholderText, QColor(127, 127, 127));

    // Disabled colors
    darkPalette.setColor(QPalette::Disabled, QPalette::Window, darkGray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Base, darkerGray);
    darkPalette.setColor(QPalette::Disabled, QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Button, darkGray);
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText,
                         QColor(127, 127, 127));
    darkPalette.setColor(QPalette::Disabled, QPalette::Text,
                         QColor(127, 127, 127));
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText,
                         QColor(127, 127, 127));
    darkPalette.setColor(QPalette::Disabled, QPalette::Highlight,
                         QColor(80, 80, 80));
    darkPalette.setColor(QPalette::Disabled, QPalette::HighlightedText,
                         QColor(127, 127, 127));

    app.setPalette(darkPalette);
  } else {
    // Light theme: use Qt style palette
    QPalette lightPalette = app.style()->standardPalette();
    app.setPalette(lightPalette);
  }

  const QString disabled_menu_text_color =
      isDark ? "rgb(127, 127, 127)" : "palette(mid)";
  // palette(mid) is darker than the button face in the dark palette, which
  // would make the button outline disappear; use a lighter grey instead.
  const QString button_border_color =
      isDark ? "rgb(90, 90, 90)" : "palette(mid)";

  app.setStyleSheet(
      QString("QMenuBar { background-color: palette(window); color: "
              "palette(window-text); }"
              "QMenuBar::item:selected { background-color: palette(highlight); "
              "color: palette(highlighted-text); }"
              "QMenuBar::item:disabled { color: %1; }"
              "QMenu { background-color: palette(window); color: "
              "palette(window-text); }"
              "QMenu::item:selected { background-color: palette(highlight); "
              "color: palette(highlighted-text); }"
              "QMenu::item:disabled { color: %1; }"
              "QMessageBox { background-color: palette(window); color: "
              "palette(window-text); }"
              "QMessageBox QLabel { color: palette(window-text); }"
              "QMessageBox QPushButton { background-color: palette(button); "
              "color: palette(button-text); border: 1px solid %2; "
              "padding: 4px 12px; border-radius: 3px; min-width: 60px; }"
              "QMessageBox QPushButton:hover { background-color: "
              "palette(highlight); color: palette(highlighted-text); }"
              "QMessageBox QPushButton:pressed { background-color: "
              "palette(dark); color: palette(button-text); }")
          .arg(disabled_menu_text_color, button_border_color));
}
