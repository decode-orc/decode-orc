/*
 * File:        rhi_window_support.cpp
 * Module:      orc-gui
 * Purpose:     Decides whether a window can render through the Qt RHI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "rhi_window_support.h"

#include <QApplication>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <string>

#include "../logging.h"
#include "gpu_surface_policy.h"

namespace orc::gui::gpu {

QRhiWidget::Api defaultRhiApi() {
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
  return QRhiWidget::Api::Metal;
#elif defined(Q_OS_WIN)
  return QRhiWidget::Api::Direct3D11;
#else
  return QRhiWidget::Api::OpenGL;
#endif
}

QSurface::SurfaceType surfaceTypeForRhiApi(QRhiWidget::Api api) {
  switch (api) {
    case QRhiWidget::Api::OpenGL:
      return QSurface::OpenGLSurface;
    case QRhiWidget::Api::Metal:
      return QSurface::MetalSurface;
    case QRhiWidget::Api::Vulkan:
      return QSurface::VulkanSurface;
    case QRhiWidget::Api::Direct3D11:
    case QRhiWidget::Api::Direct3D12:
      return QSurface::Direct3DSurface;
    case QRhiWidget::Api::Null:
      break;
  }
  return QSurface::RasterSurface;
}

bool windowSurfaceSupportsRhiApi(QSurface::SurfaceType actual,
                                 QRhiWidget::Api api) {
  if (api == QRhiWidget::Api::Null) {
    return true;
  }
  return actual == surfaceTypeForRhiApi(api);
}

bool rhiWidgetWindowUsable(const QRhiWidget* widget) {
  if (widget == nullptr) {
    return false;
  }
  const QWidget* top_level = widget->window();
  if (top_level == nullptr) {
    return true;
  }
  const QWindow* handle = top_level->windowHandle();
  if (handle == nullptr) {
    return true;  // Not created yet; the surface type is chosen with it.
  }
  return windowSurfaceSupportsRhiApi(handle->surfaceType(), widget->api());
}

bool windowHoldsRhiWidget(const QWidget* top_level) {
  if (top_level == nullptr) {
    return false;
  }
  if (qobject_cast<const QRhiWidget*>(top_level) != nullptr) {
    return true;
  }
  for (const QObject* child : top_level->children()) {
    const auto* child_widget = qobject_cast<const QWidget*>(child);
    // A child that is a window of its own carries its own backingstore, and
    // its RHI widgets are that window's business.
    if (child_widget == nullptr || child_widget->isWindow()) {
      continue;
    }
    if (windowHoldsRhiWidget(child_widget)) {
      return true;
    }
  }
  return false;
}

void settleBackingStoreBeforeChildWindows(QWidget* top_level) {
  if (top_level == nullptr || !top_level->isWindow()) {
    return;
  }
  // winId() is what forces the native window, and with it the backingstore
  // decision, into existence. The widget stays hidden.
  top_level->winId();
}

bool windowCanAdoptRhiWidget(const QWidget* owner, QRhiWidget::Api api) {
  if (owner == nullptr) {
    return false;
  }
  const QWidget* top_level = owner->window();
  if (top_level == nullptr) {
    return false;
  }
  const QWindow* handle = top_level->windowHandle();
  if (handle == nullptr) {
    return true;  // Not created yet: the surface type is still open.
  }
  if (windowSurfaceSupportsRhiApi(handle->surfaceType(), api)) {
    return true;
  }
  ORC_LOG_WARN(
      "GPU surface: window '{}' was created before this surface existed, so "
      "its surface type ({}) cannot back the {} backend it needs ({}); this "
      "window draws on the CPU",
      top_level->metaObject()->className(),
      static_cast<int>(handle->surfaceType()), static_cast<int>(api),
      static_cast<int>(surfaceTypeForRhiApi(api)));
  return false;
}

void releaseWindowRhiIfUnused(QWidget* widget) {
  if (widget == nullptr) {
    return;
  }
  QPointer<QWidget> top_level = widget->window();
  if (top_level.isNull()) {
    return;
  }

  QTimer::singleShot(0, top_level, [top_level]() {
    if (top_level.isNull() || windowHoldsRhiWidget(top_level) ||
        top_level->windowHandle() == nullptr) {
      return;
    }

    // Re-parenting onto the same parent with the same flags is what destroys
    // and re-creates the native window; it also hides the widget and forgets
    // where it was, so both are put back.
    const bool was_visible = top_level->isVisible();
    const QRect geometry = top_level->geometry();
    top_level->setParent(top_level->parentWidget(), top_level->windowFlags());
    top_level->setGeometry(geometry);
    if (was_visible) {
      top_level->show();
    }
    const std::string name = top_level->objectName().isEmpty()
                                 ? top_level->metaObject()->className()
                                 : top_level->objectName().toStdString();
    ORC_LOG_DEBUG(
        "GPU surface: '{}' re-created without an RHI backingstore after the "
        "last render surface in it was dropped",
        name);
  });
}

bool refuseRhiPaintIfWindowUnusable(QRhiWidget* widget,
                                    const QString& context) {
  if (rhiWidgetWindowUsable(widget)) {
    return false;
  }

  // Named once, in full: which window came out wrong, and how, is what says
  // where the surface type was decided — and that is not yet understood.
  static bool reported = false;
  if (!reported) {
    reported = true;
    const QWidget* top_level = widget->window();
    const QWindow* handle =
        top_level != nullptr ? top_level->windowHandle() : nullptr;
    for (const QWidget* top : QApplication::topLevelWidgets()) {
      const QWindow* top_handle = top->windowHandle();
      ORC_LOG_WARN(
          "GPU surface: top-level '{}' ({}) created={} visible={} "
          "surfaceType={} holdsRhiWidget={}",
          top->objectName().toStdString(), top->metaObject()->className(),
          top->testAttribute(Qt::WA_WState_Created), top->isVisible(),
          top_handle != nullptr ? static_cast<int>(top_handle->surfaceType())
                                : -1,
          windowHoldsRhiWidget(top));
    }
    ORC_LOG_WARN(
        "GPU surface: {} sits in window '{}' ({}), whose surface type is {} "
        "where its {} backend needs {}",
        context.toStdString(),
        top_level != nullptr ? top_level->objectName().toStdString()
                             : std::string(),
        top_level != nullptr ? top_level->metaObject()->className() : "none",
        handle != nullptr ? static_cast<int>(handle->surfaceType()) : -1,
        static_cast<int>(widget->api()),
        static_cast<int>(surfaceTypeForRhiApi(widget->api())));
  }

  GpuSurfacePolicy::instance().noteRenderFailure(
      context +
      QStringLiteral(": window surface cannot back an RHI swapchain"));

  // The owner is what holds the CPU renderer and what performs the swap, and
  // it only looks at the policy when it repaints.
  if (QWidget* owner = widget->parentWidget()) {
    owner->update();
  }
  return true;
}

}  // namespace orc::gui::gpu
