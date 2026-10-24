/*
 * File:        catalogue_svg_document.cpp
 * Module:      orc-gui
 * Purpose:     A display list as an SVG document, its presentation as SMIL
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_svg_document.h"

#include <QColor>
#include <QImage>
#include <QStringList>
#include <QTransform>
#include <algorithm>
#include <cmath>

#include "catalogue_presentation_timeline.h"

namespace orc::gui {

namespace {

using orc::CatalogueColour;
using orc::CatalogueColourStep;
using orc::CataloguePaletteTrack;

/// Characters a path element takes as SVG path data on average — a command
/// letter and two short coordinates — for reserving the string up front.
constexpr qsizetype kPathDataCharsPerElement = 14;

/// Decimal places of an SMIL keyTimes entry (SMIL 3.0 §5.4.3), a fraction of
/// the animation's duration: to a ten-millionth, finer than a millisecond in
/// any period a page plays for.
constexpr int kKeyTimeDecimals = 7;

/// Coordinates to a hundredth of a unit: finer than a page drawn at the size
/// it is saved at can show, and short.
QString number(double value) {
  if (!std::isfinite(value)) {
    value = 0.0;
  }
  QString text = QString::number(value, 'f', 2);
  if (text.contains(QLatin1Char('.'))) {
    while (text.endsWith(QLatin1Char('0'))) {
      text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
      text.chop(1);
    }
  }
  return text == QLatin1String("-0") ? QStringLiteral("0") : text;
}

QString hex(const QColor& colour) { return colour.name(QColor::HexRgb); }

/// A moment of the presentation as an SMIL clock value (SMIL 3.0 §5.4.5).
QString clock_value(uint32_t time_ms) {
  return QString::number(time_ms) + QStringLiteral("ms");
}

/// |path| as SVG path data (SVG 1.1 §8.3). A subpath's closing edge is
/// already in the path as a line back to its start, and a fill closes an
/// open subpath the same way, so no closepath command is needed.
QString path_data(const QPainterPath& path) {
  QString data;
  data.reserve(static_cast<qsizetype>(path.elementCount()) *
               kPathDataCharsPerElement);
  for (int i = 0; i < path.elementCount(); ++i) {
    const QPainterPath::Element& element = path.elementAt(i);
    switch (element.type) {
      case QPainterPath::MoveToElement:
        data += QLatin1Char('M') + number(element.x) + QLatin1Char(' ') +
                number(element.y);
        break;
      case QPainterPath::LineToElement:
        data += QLatin1Char('L') + number(element.x) + QLatin1Char(' ') +
                number(element.y);
        break;
      case QPainterPath::CurveToElement: {
        // A cubic's first control point, followed by its second and its end
        // point as data elements.
        if (i + 2 >= path.elementCount()) {
          return data;
        }
        const QPainterPath::Element& second = path.elementAt(i + 1);
        const QPainterPath::Element& end = path.elementAt(i + 2);
        data += QLatin1Char('C') + number(element.x) + QLatin1Char(' ') +
                number(element.y) + QLatin1Char(' ') + number(second.x) +
                QLatin1Char(' ') + number(second.y) + QLatin1Char(' ') +
                number(end.x) + QLatin1Char(' ') + number(end.y);
        i += 2;
        break;
      }
      case QPainterPath::CurveToDataElement:
        break;  // consumed with the curve it belongs to
    }
  }
  return data;
}

/**
 * @brief The gradient standing for palette entry |index| through the
 *        presentation
 *
 * The stop starts at the track's first colour, which is what a view shows
 * before the first step. Each step up to the loop sets the colour at its
 * moment; the loop is one discrete animation over the first cycle, begun where
 * the cycle begins and repeated for good. SMIL gives an animation begun later
 * the higher priority (SMIL 3.0 §3.6.5), so the loop takes over from the steps
 * before it, which is how a view reads a moment past the first cycle as the
 * same moment of it.
 */
QString entry_gradient(size_t index, const CataloguePaletteTrack& track) {
  QString out =
      QStringLiteral("<linearGradient id=\"p") + QString::number(index) +
      QStringLiteral("\" gradientUnits=\"userSpaceOnUse\">") +
      QStringLiteral("<stop offset=\"0\" stop-color=\"") +
      hex(shown_colour(track.steps.front().colour)) + QStringLiteral("\">");
  const bool repeats = CataloguePresentationTimeline::loops(track);
  for (size_t i = 1; i < track.steps.size(); ++i) {
    const CatalogueColourStep& step = track.steps[i];
    if (repeats && step.time_ms >= track.loop_start_ms) {
      break;
    }
    out += QStringLiteral("<set attributeName=\"stop-color\" to=\"") +
           hex(shown_colour(step.colour)) + QStringLiteral("\" begin=\"") +
           clock_value(step.time_ms) + QStringLiteral("\"/>");
  }
  if (repeats) {
    const uint64_t start = track.loop_start_ms;
    const uint64_t period = track.loop_period_ms;
    QStringList values{hex(shown_colour(
        track.colour_at(track.loop_start_ms, track.steps.front().colour)))};
    QStringList key_times{QStringLiteral("0")};
    for (const CatalogueColourStep& step : track.steps) {
      if (step.time_ms <= start || step.time_ms >= start + period) {
        continue;
      }
      values << hex(shown_colour(step.colour));
      key_times << QString::number(static_cast<double>(step.time_ms - start) /
                                       static_cast<double>(period),
                                   'f', kKeyTimeDecimals);
    }
    out += QStringLiteral(
               "<animate attributeName=\"stop-color\" calcMode=\"discrete\" "
               "begin=\"") +
           clock_value(track.loop_start_ms) + QStringLiteral("\" dur=\"") +
           clock_value(track.loop_period_ms) +
           QStringLiteral("\" repeatCount=\"indefinite\" values=\"") +
           values.join(QLatin1Char(';')) + QStringLiteral("\" keyTimes=\"") +
           key_times.join(QLatin1Char(';')) + QStringLiteral("\"/>");
  }
  out += QStringLiteral("</stop></linearGradient>\n");
  return out;
}

}  // namespace

SvgDocumentCanvas::SvgDocumentCanvas(const QSizeF& size, const QRectF& viewport,
                                     const orc::CatalogueDisplayList& list,
                                     bool animated)
    : size_(size),
      viewport_(viewport),
      animated_(animated),
      pixel_aligned_(list.pixel_aligned),
      tracks_(list.palette_tracks),
      entries_used_(list.palette_tracks.size(), false) {}

void SvgDocumentCanvas::fillPath(const QPainterPath& path,
                                 const CanvasPaint& paint) {
  if (path.isEmpty()) {
    return;
  }
  const QString fill = paintValue(paint);
  if (fill.isEmpty()) {
    return;
  }
  body_ += QStringLiteral("<path d=\"") + path_data(path) +
           QStringLiteral("\" fill=\"") + fill + QLatin1Char('"');
  // Qt's winding fill is SVG's default nonzero rule.
  if (path.fillRule() == Qt::OddEvenFill) {
    body_ += QStringLiteral(" fill-rule=\"evenodd\"");
  }
  body_ += QStringLiteral("/>\n");
}

void SvgDocumentCanvas::fillRect(const QRectF& rect, const CanvasPaint& paint,
                                 bool hard_edged) {
  const QRectF box = rect.normalized();
  if (!(box.width() > 0.0) || !(box.height() > 0.0)) {
    return;
  }
  const QString fill = paintValue(paint);
  if (fill.isEmpty()) {
    return;
  }
  body_ += QStringLiteral("<rect x=\"") + number(box.x()) +
           QStringLiteral("\" y=\"") + number(box.y()) +
           QStringLiteral("\" width=\"") + number(box.width()) +
           QStringLiteral("\" height=\"") + number(box.height()) +
           QStringLiteral("\" fill=\"") + fill + QLatin1Char('"');
  // A pixel list is drawn hard-edged as a whole (see document()).
  if (hard_edged && !pixel_aligned_) {
    body_ += QStringLiteral(" shape-rendering=\"crispEdges\"");
  }
  body_ += QStringLiteral("/>\n");
}

void SvgDocumentCanvas::beginStage(uint32_t time_ms,
                                   const std::vector<RevealFrame>& reveal) {
  if (!animated_ ||
      (stage_open_ && time_ms == stage_time_ && reveal == stage_reveal_)) {
    return;
  }
  if (stage_open_) {
    body_ += QStringLiteral("</g>\n");
  }
  stage_open_ = true;
  stage_time_ = time_ms;
  stage_reveal_ = reveal;
  const uint32_t shown_from = reveal.empty() ? time_ms : reveal.front().time_ms;
  QString clip;
  if (!reveal.empty() && time_ms > shown_from) {
    // The stage is swept on: clipped to a rectangle that takes each frame's
    // clip from that frame's moment (SVG 1.1 §14.3.5, a discrete animation
    // of SMIL 3.0 §3.4.1), and grows to the whole viewport when the stage is
    // whole. A set begun later wins over the animation (SMIL 3.0 §3.6.5).
    const QString id = QStringLiteral("r") + QString::number(reveal_clips_++);
    const uint32_t length = time_ms - shown_from;
    QStringList ys;
    QStringList heights;
    QStringList key_times;
    for (const RevealFrame& frame : reveal) {
      const QRectF box = frame.clip.normalized();
      ys << number(box.y());
      heights << number(box.height());
      key_times << QString::number(
          static_cast<double>(frame.time_ms - shown_from) / length, 'f',
          kKeyTimeDecimals);
    }
    const QRectF first = reveal.front().clip.normalized();
    const auto animate = [&](const QString& attribute,
                             const QStringList& values) {
      return QStringLiteral("<animate attributeName=\"") + attribute +
             QStringLiteral("\" calcMode=\"discrete\" begin=\"") +
             clock_value(shown_from) + QStringLiteral("\" dur=\"") +
             clock_value(length) +
             QStringLiteral("\" fill=\"freeze\" values=\"") +
             values.join(QLatin1Char(';')) + QStringLiteral("\" keyTimes=\"") +
             key_times.join(QLatin1Char(';')) + QStringLiteral("\"/>");
    };
    const auto whole = [&](const QString& attribute, double value) {
      return QStringLiteral("<set attributeName=\"") + attribute +
             QStringLiteral("\" to=\"") + number(value) +
             QStringLiteral("\" begin=\"") + clock_value(time_ms) +
             QStringLiteral("\"/>");
    };
    body_ += QStringLiteral("<clipPath id=\"") + id +
             QStringLiteral("\" clipPathUnits=\"userSpaceOnUse\"><rect x=\"") +
             number(viewport_.x()) + QStringLiteral("\" y=\"") +
             number(first.y()) + QStringLiteral("\" width=\"") +
             number(viewport_.width()) + QStringLiteral("\" height=\"") +
             number(first.height()) + QStringLiteral("\">") +
             animate(QStringLiteral("y"), ys) +
             animate(QStringLiteral("height"), heights) +
             whole(QStringLiteral("y"), viewport_.y()) +
             whole(QStringLiteral("height"), viewport_.height()) +
             QStringLiteral("</rect></clipPath>\n");
    clip = QStringLiteral(" clip-path=\"url(#") + id + QStringLiteral(")\"");
  }
  if (shown_from == 0 && clip.isEmpty()) {
    body_ += QStringLiteral("<g>\n");
    return;
  }
  // Out of the rendering until the stage appears (SVG 1.1 §11.5), then in
  // it for good: a set with no end stays applied.
  body_ += QStringLiteral("<g display=\"none\"") + clip +
           QStringLiteral(
               "><set attributeName=\"display\" "
               "to=\"inline\" begin=\"") +
           clock_value(shown_from) + QStringLiteral("\"/>\n");
}

void SvgDocumentCanvas::outline(const QRectF& rect, const QColor& colour) {
  // A one-unit line inside the rectangle's edge, dashed four on and two off as
  // a view's dashed pen is.
  overlay_ +=
      QStringLiteral("<rect x=\"") + number(rect.x() + 0.5) +
      QStringLiteral("\" y=\"") + number(rect.y() + 0.5) +
      QStringLiteral("\" width=\"") + number(rect.width() - 1.0) +
      QStringLiteral("\" height=\"") + number(rect.height() - 1.0) +
      QStringLiteral("\" fill=\"none\" stroke=\"") + hex(colour) +
      QStringLiteral("\" stroke-width=\"1\" stroke-dasharray=\"4 2\"/>\n");
}

QString SvgDocumentCanvas::colourValue(const orc::CatalogueColour& colour,
                                       const QColor& resolved) {
  if (animated_ && colour.palette_index >= 0) {
    const auto index = static_cast<size_t>(colour.palette_index);
    if (index < tracks_.size() && !tracks_[index].steps.empty()) {
      entries_used_[index] = true;
      return QStringLiteral("url(#p") + QString::number(index) +
             QLatin1Char(')');
    }
  }
  return hex(resolved);
}

QString SvgDocumentCanvas::paintValue(const CanvasPaint& paint) {
  switch (paint.brush.style()) {
    case Qt::NoBrush:
      return {};
    case Qt::TexturePattern:
      return patternFor(paint);
    default:
      // The only other brush a display list is drawn with is a solid one.
      return colourValue(paint.colour, paint.brush.color());
  }
}

QString SvgDocumentCanvas::patternFor(const CanvasPaint& paint) {
  // The tile is the brush's own: each of its elements in the drawing colour,
  // the background, or nothing, and the brush's transform puts it on the page
  // at the size and registration the standard gives it. An element is written
  // in the colour it was drawn from, so a texture drawn in a blinking entry
  // blinks with it.
  const QImage tile = paint.brush.textureImage();
  if (tile.isNull()) {
    return colourValue(paint.colour, shown_colour(paint.colour));
  }
  const QColor lit = shown_colour(paint.colour);
  const QColor ground =
      paint.ground ? shown_colour(*paint.ground) : QColor(Qt::transparent);
  QString elements;
  for (int y = 0; y < tile.height(); ++y) {
    for (int x = 0; x < tile.width(); ++x) {
      const QColor element = tile.pixelColor(x, y);
      if (element.alpha() == 0) {
        continue;
      }
      QString fill;
      if (element.rgb() == lit.rgb()) {
        fill = colourValue(paint.colour, lit);
      } else if (paint.ground && element.rgb() == ground.rgb()) {
        fill = colourValue(*paint.ground, ground);
      } else {
        fill = hex(element);
      }
      elements += QStringLiteral("<rect x=\"") + QString::number(x) +
                  QStringLiteral("\" y=\"") + QString::number(y) +
                  QStringLiteral("\" width=\"1\" height=\"1\" fill=\"") + fill +
                  QStringLiteral("\"/>");
    }
  }
  const QTransform& to_page = paint.brush.transform();
  const QString matrix =
      number(to_page.m11()) + QLatin1Char(' ') + number(to_page.m12()) +
      QLatin1Char(' ') + number(to_page.m21()) + QLatin1Char(' ') +
      number(to_page.m22()) + QLatin1Char(' ') + number(to_page.dx()) +
      QLatin1Char(' ') + number(to_page.dy());
  const QString key = matrix + QLatin1Char('|') +
                      QString::number(tile.width()) + QLatin1Char('x') +
                      QString::number(tile.height()) + QLatin1Char('|') +
                      elements;
  const auto found = pattern_ids_.constFind(key);
  if (found != pattern_ids_.constEnd()) {
    return QStringLiteral("url(#") + *found + QLatin1Char(')');
  }
  const QString id = QStringLiteral("t") + QString::number(pattern_ids_.size());
  pattern_ids_.insert(key, id);
  patterns_defs_ +=
      QStringLiteral("<pattern id=\"") + id +
      QStringLiteral("\" patternUnits=\"userSpaceOnUse\" width=\"") +
      QString::number(tile.width()) + QStringLiteral("\" height=\"") +
      QString::number(tile.height()) +
      QStringLiteral("\" patternTransform=\"matrix(") + matrix +
      QStringLiteral(")\" shape-rendering=\"crispEdges\">") + elements +
      QStringLiteral("</pattern>\n");
  return QStringLiteral("url(#") + id + QLatin1Char(')');
}

QByteArray SvgDocumentCanvas::document() const {
  QString out;
  out.reserve(body_.size() + patterns_defs_.size() + 1024);
  out += QStringLiteral(
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" "
             "width=\"") +
         number(size_.width()) + QStringLiteral("\" height=\"") +
         number(size_.height()) + QStringLiteral("\" viewBox=\"") +
         number(viewport_.x()) + QLatin1Char(' ') + number(viewport_.y()) +
         QLatin1Char(' ') + number(viewport_.width()) + QLatin1Char(' ') +
         number(viewport_.height()) + QStringLiteral("\">\n");

  QString defs = patterns_defs_;
  for (size_t i = 0; i < entries_used_.size(); ++i) {
    if (entries_used_[i]) {
      defs += entry_gradient(i, tracks_[i]);
    }
  }
  if (!defs.isEmpty()) {
    out += QStringLiteral("<defs>\n") + defs + QStringLiteral("</defs>\n");
  }

  // The ground a view draws the page on, where nothing is drawn.
  out += QStringLiteral("<rect x=\"") + number(viewport_.x()) +
         QStringLiteral("\" y=\"") + number(viewport_.y()) +
         QStringLiteral("\" width=\"") + number(viewport_.width()) +
         QStringLiteral("\" height=\"") + number(viewport_.height()) +
         QStringLiteral("\" fill=\"#000000\"/>\n");

  // A list that tiles its own pixel grid is drawn hard-edged, as a view draws
  // it: smoothing would blur pixels a receiver had sharp.
  out += pixel_aligned_ ? QStringLiteral("<g shape-rendering=\"crispEdges\">\n")
                        : QStringLiteral("<g>\n");
  out += body_;
  if (stage_open_) {
    out += QStringLiteral("</g>\n");
  }
  out += QStringLiteral("</g>\n") + overlay_ + QStringLiteral("</svg>\n");
  return out.toUtf8();
}

}  // namespace orc::gui
