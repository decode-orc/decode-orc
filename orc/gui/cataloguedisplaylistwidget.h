/*
 * File:        cataloguedisplaylistwidget.h
 * Module:      orc-gui
 * Purpose:     Widget rasterising a CataloguePayload 2D display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef CATALOGUEDISPLAYLISTWIDGET_H
#define CATALOGUEDISPLAYLISTWIDGET_H

#include <orc/stage/tooling/catalogue_results.h>

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <memory>
#include <optional>

#include "catalogue_animation_clock.h"
#include "catalogue_playback.h"
#include "catalogue_presentation_timeline.h"

class QKeyEvent;
class QPainter;

namespace orc::gui {
class DisplayListCanvas;
}  // namespace orc::gui

/**
 * @brief Draws a display-list payload, and plays one that changes as it is
 *        shown
 *
 * The list is walked front to back into a drawable area of the payload's own
 * aspect, centred in the widget. Unit space has y running upwards from the
 * bottom left, which is inverted here once and never again.
 *
 * Nothing outside the drawable area is painted: a display list is authored
 * against a screen the receiver guarantees is visible, and the strip around it
 * is border.
 *
 * Geometry is drawn the way ANSI X3.110 describes it. The logical pel is a
 * rectangle hung from the drawing point by the corner its signs name, and a
 * line is that rectangle swept along it (§5.3.2.2.6); textures step in whole
 * pels along the stroke, curves included (§5.3.2.4.2); a filled figure takes
 * in the outline the pel traces round it; a spline is its series of lines
 * (§5.3.3.3); colour mode 2 fills the spaces textures leave with the
 * background. Characters — text, mosaics and downloadable glyphs — fill their
 * whole field, reflect within it for a negative dimension, and each turn about
 * its own origin (§5.3.2.3). A list that tiles its own pixel grid
 * (CatalogueDisplayList::pixel_aligned) is drawn as the pixels it lists.
 *
 * A timed list (CatalogueDisplayList::timed()) is played over time: each
 * operation is drawn from its CatalogueDrawOp::appear_ms — swept on a row at a
 * time before then where its source drew it so, clipped to the side of the
 * sweep the sweep has passed (CatalogueDrawOp::reveal_duration_ms) — and a
 * colour naming a palette entry takes the colour the entry's
 * CataloguePaletteTrack gives it at the time shown — a blink is such a track,
 * looping. The list carries its
 * timing already worked out; this view only plays it, from the start whenever
 * it is given a list, against an ICatalogueAnimationClock. Every paint draws
 * the list as it stands at the clock's time, and a single-shot timer wakes the
 * view at the next change and no sooner. Nothing is armed once the list has
 * settled, while the view is hidden, while the presentation is paused, or with
 * animation switched off.
 *
 * The presentation is played through a CataloguePlayback, which a reader
 * drives as a transport: play and pause, seek anywhere in it, step from one
 * change of the display to the next, play it faster or slower, and loop it.
 * What is on screen at a time is what the SVG document the view writes shows
 * at that time of its own animation, because both are drawn from the one
 * timeline. With the view focused, Space plays and pauses, Left and Right
 * step to the display before and the next, and with Shift they seek five
 * seconds back and on.
 *
 * With animation off, and in every saved image, the list is drawn still: every
 * operation, and each colour its own RGB, which is the payload as it stands
 * once it has finished drawing with its blinks at rest.
 *
 * Not thread-safe: a widget, used from the GUI thread only.
 */
class CatalogueDisplayListWidget : public QWidget {
  Q_OBJECT

 public:
  /// Plays timed lists against the platform's monotonic clock.
  explicit CatalogueDisplayListWidget(QWidget* parent = nullptr);
  /// Plays timed lists against @p clock; a null clock means the platform's
  /// monotonic one. The widget shares ownership of the clock.
  explicit CatalogueDisplayListWidget(
      std::shared_ptr<const ICatalogueAnimationClock> clock,
      QWidget* parent = nullptr);
  ~CatalogueDisplayListWidget() override;

  /// Show @p list, playing it from the start. A time pinned with
  /// setPresentationTime() is released.
  void setDisplayList(const orc::CatalogueDisplayList& list);
  void clearDisplayList();
  bool hasDisplayList() const { return list_.has_value(); }

  /**
   * @brief Outline the drawable area
   *
   * A list drawn entirely in one corner looks mis-scaled without it. Off by
   * default, because the outline is not part of the content.
   */
  void setShowDataErrors(bool show);
  bool showDataErrors() const { return show_data_errors_; }

  /// Play a timed list as it changes. Off draws it still; turning it back on
  /// plays it again from the start.
  void setAnimationsEnabled(bool enabled);
  bool animationsEnabled() const { return animations_enabled_; }

  /// Whether the list on show changes as it is shown: drawn in stages, or with
  /// palette entries that change.
  bool isTimed() const { return list_.has_value() && timeline_.timed(); }

  /// Play the list again from the start, following the clock. Releases a time
  /// pinned with setPresentationTime().
  void replay();

  // ---- Transport ------------------------------------------------------------

  /// Play on from where the presentation stands, or from its start once it has
  /// stopped at its end.
  void play();
  /// Hold the presentation where it stands.
  void pause();
  /// Pause a presentation that is playing, and play one that is not.
  void togglePlayback();
  /// Move to @p time_ms, playing on from there if it was playing and held
  /// there if it was not (CataloguePlayback::seek()).
  void seek(uint32_t time_ms);
  /// Hold the presentation at the start of the display after the one on
  /// screen (@p direction above zero) or before it (below zero), as far as
  /// the presentation's first and last displays.
  void stepDisplay(int direction);
  /// Play @p speed times as fast as the service timed it. Kept from one list
  /// to the next.
  void setPlaybackSpeed(double speed);
  double playbackSpeed() const { return playback_.speed(); }
  /// Whether playing past the end starts the presentation again from its
  /// start. Off, a presentation that settles stops at its end and one that
  /// blinks goes on blinking. Kept from one list to the next.
  void setLooping(bool looping);
  bool looping() const { return playback_.looping(); }
  /// The length of the presentation the transport seeks over, in
  /// milliseconds: CataloguePlayback::length(). 0 when nothing is timed.
  uint32_t presentationLength() const;

  /**
   * @brief Act on a transport key, as the view does when it has focus
   *
   * Space plays or pauses; Left and Right step to the display before or the
   * next one; with Shift they seek kSeekStepMs back or on. Lets the controls
   * that go with the view share its keys.
   *
   * @return Whether @p event was a transport key and was acted on; nothing is
   *         while the view has nothing to play.
   */
  bool handlePlaybackKey(const QKeyEvent& event);

  /// How far Shift with Left or Right seeks.
  static constexpr uint32_t kSeekStepMs = 5000;

  /// Operations painted by the last paint (test seam)
  int opsPainted() const { return ops_painted_; }

  // ---- Presentation time (test seams) ------------------------------------

  /// Hold the presentation at @p time_ms instead of following the clock, until
  /// it is played again or the next list. Draws nothing different while
  /// animation is off, which always shows the still render.
  void setPresentationTime(uint32_t time_ms);
  /// The presentation time the view is at: where it was held, or how far the
  /// clock has taken it since it was last played, seeked or looped.
  uint32_t presentationTime() const;
  /// Whether the presentation is playing: following the clock, and not
  /// stopped at its end. Stays true while the view is hidden, because the
  /// presentation runs on: a view shown again takes it up where the clock has
  /// got to.
  bool isPlaying() const;
  /// The interval the wake timer is armed with, in milliseconds — the time
  /// from the moment it was armed to the next change — or nullopt when nothing
  /// is armed.
  std::optional<int> scheduledWakeMs() const;
  /// Do now what the wake timer does when it fires: repaint at the clock's
  /// time and arm for the change after it. Lets a test that moves a clock by
  /// hand step playback without waiting on a real timer.
  void fireScheduledWake();

  /// Placeholder shown when no list is set
  void setPlaceholderText(const QString& text);

  QSize sizeHint() const override;

  /// The drawable area within the widget, in device pixels (test seam)
  QRectF displayAreaRect() const { return displayAreaRectIn(QSizeF(size())); }

  /**
   * @brief The drawable area alone as an image |size| pixels across and down
   *
   * The area is fitted to |size| at the list's own aspect exactly as it is
   * fitted to the widget, so a size of that aspect — which is what
   * pageImageSize() gives — leaves no border around it.
   *
   * Always the still render, whatever moment of the presentation the view is
   * showing: a still is kept as a record of the page, so it has every stage
   * drawn and every blinking colour at rest. The drawable-area outline follows
   * the view's own setting.
   *
   * Null when there is no list to draw or |size| is empty.
   */
  QImage renderPageImage(const QSize& size) const;

  /// The drawable area as it stands at |time_ms| of the presentation, framed
  /// exactly as renderPageImage() frames it. Null when there is no list to
  /// draw or |size| is empty.
  QImage renderFrameImage(const QSize& size, uint32_t time_ms) const;

  /// The size renderPageImage() is worth being asked for, at the list's own
  /// aspect. Null when there is no list.
  QSize pageImageSize() const;

  /**
   * @brief The drawable area as an SVG document, pageImageSize() across
   *
   * The same drawing a view makes of the list, as vectors. With |animated|
   * the document plays the presentation as the view does — each stage
   * appearing when it did, each changing palette entry changing, loops
   * repeating — as SMIL animation a web browser plays; without it, the still
   * render renderPageImage() draws. Empty when there is no list.
   */
  QByteArray renderSvgDocument(bool animated = true) const;

 Q_SIGNALS:
  /// The transport has changed: a new list, playing or pausing, a seek or a
  /// step, a speed or loop setting, or animation turned on or off. Not
  /// emitted as a playing presentation moves on by itself.
  void playbackChanged();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  /**
   * @brief How one paint maps unit space onto the surface it draws to
   *
   * The two scales differ wherever the list's nominal pixels are not square:
   * the drawable area takes the shape the list is displayed at, while unit y
   * still runs over the extent the list covers, so a unit of y is not the same
   * number of device pixels as a unit of x.
   */
  struct Mapping {
    /// Device pixels per unit of x, and per unit of y.
    qreal unit_x = 0.0;
    qreal unit_y = 0.0;
    /// The narrowest the logical pel is drawn across and down, in device
    /// pixels: one pixel of the list's own grid where it names one, and one
    /// device pixel otherwise.
    qreal pel_floor_x = 1.0;
    qreal pel_floor_y = 1.0;
  };

  /// Character outlines in the display font, laid out once at a reference
  /// size and fitted to each character field as it is drawn.
  struct GlyphOutlines;

  /// Draw one operation. Its colours are resolved through |timeline| at
  /// |time_ms|, or taken as they stand where |time_ms| is nullopt.
  static void paintOp(orc::gui::DisplayListCanvas& canvas,
                      const orc::CatalogueDisplayList& list,
                      const orc::CatalogueDrawOp& op, const QRectF& area,
                      const Mapping& mapping,
                      const CataloguePresentationTimeline& timeline,
                      std::optional<uint32_t> time_ms, GlyphOutlines& glyphs);

  /// The mapping from unit space onto a drawable area of |area|
  Mapping mappingFor(const QRectF& area) const;

  /// The drawable area within a paint surface |bounds| pixels across and down,
  /// whose origin is (0, 0) — the widget itself, or the image a save renders
  /// into
  QRectF displayAreaRectIn(const QSizeF& bounds) const;

  /// Draw the list into |bounds| of |painter| as it stands at |time_ms| of the
  /// presentation, or still where |time_ms| is nullopt. What both the widget's
  /// own paint and an exported image are.
  void paintContent(QPainter& painter, const QRectF& bounds,
                    std::optional<uint32_t> time_ms) const;

  /// The drawable area alone at |time_ms|, or still, as a new image
  QImage renderImage(const QSize& size, std::optional<uint32_t> time_ms) const;

  /// The time the widget's own paint draws at, or nullopt for the still render
  std::optional<uint32_t> shownTime() const;

  /// Arm the wake timer for the next change after the presentation time, or
  /// leave it stopped where there is nothing to play
  void schedulePlayback();
  /// After a change of transport: arm for what comes next, repaint and tell
  /// the controls
  void transportChanged();
  /// The wake timer's handler
  void onWake();

  std::optional<orc::CatalogueDisplayList> list_;
  CataloguePresentationTimeline timeline_;
  QString placeholder_;
  bool show_data_errors_ = false;
  mutable int ops_painted_ = 0;
  /// Built on the first paint, and kept: a page is drawn from a few dozen
  /// distinct characters, and every blink repaints all of them.
  mutable std::unique_ptr<GlyphOutlines> glyph_outlines_;

  /// Where the presentation stands, played against the view's clock.
  CataloguePlayback playback_;
  QTimer wake_timer_;
  bool animations_enabled_ = true;
  /// Between a show event and a hide event. Kept from the events rather than
  /// read from isVisible(), which stays true through the spontaneous hide a
  /// minimised window sends.
  bool shown_ = false;
};

#endif  // CATALOGUEDISPLAYLISTWIDGET_H
