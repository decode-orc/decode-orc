/*
 * File:        preview_image_qt.h
 * Module:      orc-gui
 * Purpose:     Conversion from core PreviewImage RGB888 data to QImage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef PREVIEW_IMAGE_QT_H
#define PREVIEW_IMAGE_QT_H

#include <QImage>

namespace orc {
struct PreviewImage;
}  // namespace orc

namespace orc::gui {

/**
 * @brief Convert a core PreviewImage (tightly packed RGB888) into a QImage.
 *
 * Handles QImage's 4-byte scanline alignment. When @p reuse has matching
 * dimensions and format its buffer is reused to avoid reallocation.
 *
 * @param image Source RGB888 preview image from the render presenter
 * @param reuse Optional previous QImage whose buffer may be reused
 * @return Converted image, or a null QImage when @p image is empty/invalid
 */
QImage previewImageToQImage(const orc::PreviewImage& image,
                            QImage reuse = QImage());

/**
 * @brief Expand a core PreviewImage into a QImage with an alpha channel.
 *
 * RHI has no packed RGB texture format, so a GPU surface needs RGBA8888. The
 * expansion is a per-pixel loop over a whole frame, which is why it runs on
 * the render worker rather than in the paint event: the GUI thread's share of
 * a frame then becomes one texture upload.
 *
 * Alpha is set opaque throughout; the fragment shader ignores it in any case.
 *
 * @param image Source RGB888 preview image from the render presenter
 * @param reuse Optional previous QImage whose buffer may be reused
 * @return Converted image, or a null QImage when @p image is empty/invalid
 */
QImage previewImageToRgbaQImage(const orc::PreviewImage& image,
                                QImage reuse = QImage());

}  // namespace orc::gui

#endif  // PREVIEW_IMAGE_QT_H
