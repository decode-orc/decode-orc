/*
 * File:        rhi_window_support.h
 * Module:      orc-gui
 * Purpose:     Decides whether a window can render through the Qt RHI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_RHI_WINDOW_SUPPORT_H
#define ORC_GUI_RHI_WINDOW_SUPPORT_H

#include <QRhiWidget>
#include <QSurface>

class QString;

namespace orc::gui::gpu {

/**
 * @brief The backend a QRhiWidget uses when nothing has called setApi().
 *
 * Qt's documented default: Metal on macOS and iOS, Direct 3D 11 on Windows,
 * OpenGL elsewhere. Repeated here so a window can be judged before there is a
 * widget to ask.
 */
QRhiWidget::Api defaultRhiApi();

/**
 * @brief The QWindow surface type a QRhiWidget using @p api has to be in.
 *
 * Qt sets this on the top-level window when it creates it, from the RHI
 * configuration it finds in the widget tree. The mapping is repeated here so
 * a surface can check the window it ended up in; QBackingStoreRhiSupport owns
 * the same knowledge but is private.
 */
QSurface::SurfaceType surfaceTypeForRhiApi(QRhiWidget::Api api);

/**
 * @brief Whether a window of surface type @p actual can back @p api.
 *
 * Exact match, with one exception: Api::Null renders nowhere, so no window is
 * the wrong one for it. (Qt 6.11 deprecates RasterGLSurface in favour of
 * RasterSurface, so there is no second raster type to admit for OpenGL.)
 */
bool windowSurfaceSupportsRhiApi(QSurface::SurfaceType actual,
                                 QRhiWidget::Api api);

/**
 * @brief Whether @p widget's window is one its RHI backend can render into.
 *
 * A widget with no window yet is not refused: Qt sets the surface type when
 * it creates one, and QRhiWidget::paintEvent already declines to paint while
 * there is no QRhi.
 */
bool rhiWidgetWindowUsable(const QRhiWidget* widget);

/**
 * @brief Whether any QRhiWidget is left anywhere in @p top_level's tree.
 */
bool windowHoldsRhiWidget(const QWidget* top_level);

/**
 * @brief Let @p widget's window go back to flushing without the RHI.
 *
 * Whether a window's backingstore composites through the RHI is decided when
 * the window is created, from the RHI widgets in its tree. Dropping the last
 * of them does not undo that: the backingstore keeps trying to build a
 * swapchain on every flush, and on a window that cannot give it one every
 * flush fails — the log fills and nothing reaches the screen, whatever the
 * replacement surface paints. Re-creating the window is what re-decides it.
 *
 * Does nothing while an RHI widget is still there. The work is queued, so
 * this is safe to call from a paint or from the middle of a surface swap.
 */
void releaseWindowRhiIfUnused(QWidget* widget);

/**
 * @brief Settle @p top_level's backingstore while its tree is still its own.
 *
 * Whether a window's backingstore composites through the RHI is decided once,
 * when its native window is created, by walking the whole child *object* tree
 * of the widget being created — and that walk does not stop at a child that
 * is a window in its own right. A dialogue built hidden in a main window's
 * constructor therefore hands its render surface's requirement to the main
 * window: the main window is given an OpenGL surface and builds an RHI it has
 * no use for, because a preview nobody has opened yet contains a QRhiWidget.
 *
 * That is not merely wasteful. Building the RHI creates an OpenGL context,
 * and on X11 a GLX that cannot supply a config for it does not report the
 * failure — Qt's GLX integration calls qFatal("Could not initialize GLX") and
 * the process aborts before the first window ever appears. A machine whose GL
 * stack is broken has to be able to start the application and use everything
 * that does not draw through the GPU.
 *
 * Creating the window here is what fixes the decision: made against a tree
 * that holds no render surface, it comes out raster, and a later creation
 * does not revisit it. The child dialogues are unaffected — each is still
 * evaluated on its own tree when it is first shown, and still gets the
 * OpenGL surface its preview needs.
 *
 * Call this **before** any child window holding a render surface is
 * constructed; afterwards there is nothing left to decide.
 *
 * (Qt::WA_NativeWindow on the child windows is the other half of Qt's rule
 * and looks like the fix, but it is not: creating the parent then creates
 * every native child with it, which brings up the very context this avoids.)
 */
void settleBackingStoreBeforeChildWindows(QWidget* top_level);

/**
 * @brief Whether an RHI widget may be put into @p owner's window at all.
 *
 * A widget window's surface type is chosen once, when the window is created,
 * from the RHI widgets Qt finds in its tree; a later creation does not revisit
 * it. So an RHI widget added to a window that already exists — and was
 * therefore decided without it — can never render: the window keeps a surface
 * type its backend cannot use, while the backingstore is switched to RHI
 * compositing, and every flush then fails. Building the raster path instead
 * keeps the window out of that state altogether.
 *
 * True while @p owner has no window yet, which is the ordinary case.
 */
bool windowCanAdoptRhiWidget(const QWidget* owner,
                             QRhiWidget::Api api = defaultRhiApi());

/**
 * @brief Refuse a paint the window cannot support, and latch the failure.
 *
 * Qt's own guard in QRhiWidget::paintEvent covers a missing QRhi only. A
 * window that carries an RHI-enabled backingstore it cannot render from — a
 * non-Metal surface on macOS, say — hands out a QRhi that is present but
 * unusable, and QRhi::beginOffscreenFrame() then dereferences null and takes
 * the process with it. Calling this first turns that into the run-time
 * fallback the surfaces already have: the failure is reported to
 * GpuSurfacePolicy and the owner is asked to repaint, which is where
 * downgradeSurfaceIfFailed()/dropScopeSurfaceIfFailed() swap in the CPU path.
 *
 * @return True when the caller must return without painting.
 */
bool refuseRhiPaintIfWindowUnusable(QRhiWidget* widget, const QString& context);

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_RHI_WINDOW_SUPPORT_H
