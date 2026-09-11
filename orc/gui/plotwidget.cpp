/******************************************************************************
 * plotwidget.cpp
 * orc-gui - Adapted from ld-analyse
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 *
 * This file is part of decode-orc.
 ******************************************************************************/

#include "plotwidget.h"

#include <QApplication>
#include <QDebug>
#include <QtMath>
#include <cmath>
#include <cstdint>

#include "plot_series_decimation.h"
#include "theme_color_tokens.h"

PlotWidget::PlotWidget(QWidget* parent)
    : QWidget(parent),
      m_view(nullptr),
      m_scene(nullptr),
      m_mainLayout(nullptr),
      m_plotRect(0, 0, 400, 300),
      m_dataRect(0, 0, 100, 100),
      m_xMin(0),
      m_xMax(100),
      m_yMin(0),
      m_yMax(100),
      m_xAutoScale(true),
      m_yAutoScale(true),
      m_xIntegerLabels(false),
      m_yIntegerLabels(false),
      m_yAbbreviatedLabels(false),
      m_isDarkTheme(false),
      m_secondaryYAxisEnabled(false),
      m_secondaryYMin(0),
      m_secondaryYMax(100),
      m_xAxisTickStep(0),
      m_xAxisTickOrigin(0),
      m_xAxisUseCustomTicks(false),
      m_yAxisTickStep(0),
      m_yAxisTickOrigin(0),
      m_secondaryYAxisTickStep(0),
      m_secondaryYAxisTickOrigin(0),
      m_yAxisUseCustomTicks(false),
      m_secondaryYAxisUseCustomTicks(false),
      m_grid(nullptr),
      m_legend(nullptr),
      m_axisLabels(nullptr),
      m_gridEnabled(true),
      m_legendEnabled(false),
      m_zoomEnabled(true),
      m_panEnabled(true),
      m_canvasBackground(),
      m_usePaletteCanvasBackground(true),
      m_isDragging(false),
      m_noDataTextItem(nullptr) {
  setupView();
}

PlotWidget::~PlotWidget() {
  clearSeries();
  clearMarkers();
}

void PlotWidget::setupView() {
  m_mainLayout = new QVBoxLayout(this);
  m_mainLayout->setContentsMargins(0, 0, 0, 0);

  m_scene = new QGraphicsScene(this);
  // Disable BSP indexing for dynamic plot scenes to prevent crashes
  m_scene->setItemIndexMethod(QGraphicsScene::NoIndex);

  m_view = new QGraphicsView(m_scene, this);

  m_view->setRenderHint(QPainter::Antialiasing, true);
  m_view->setDragMode(QGraphicsView::NoDrag);  // Disable default drag to allow
                                               // custom interaction
  m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  // Install event filter to capture mouse events
  m_view->viewport()->installEventFilter(this);

  m_mainLayout->addWidget(m_view);

  // Create grid
  m_grid = new PlotGrid(this);
  m_scene->addItem(m_grid);

  // Create legend
  m_legend = new PlotLegend(this);
  m_scene->addItem(m_legend);

  // Create axis labels
  m_axisLabels = new PlotAxisLabels(this);
  m_scene->addItem(m_axisLabels);

  // Detect and apply theme
  updateTheme();

  connect(m_scene, &QGraphicsScene::selectionChanged, this,
          &PlotWidget::onSceneSelectionChanged);

  updatePlotArea();
}

void PlotWidget::setAxisTitle(Qt::Orientation orientation,
                              const QString& title) {
  if (orientation == Qt::Horizontal) {
    m_xAxisTitle = title;
  } else {
    m_yAxisTitle = title;
  }
  replot();
}

void PlotWidget::setAxisRange(Qt::Orientation orientation, double min,
                              double max) {
  if (orientation == Qt::Horizontal) {
    m_xMin = min;
    m_xMax = max;
    m_xAutoScale = false;
  } else {
    m_yMin = min;
    m_yMax = max;
    m_yAutoScale = false;
  }
  replot();
}

void PlotWidget::setAxisAutoScale(Qt::Orientation orientation, bool enable) {
  if (orientation == Qt::Horizontal) {
    m_xAutoScale = enable;
  } else {
    m_yAutoScale = enable;
  }
  if (enable) {
    calculateDataRange();
  }
  replot();
}

void PlotWidget::setYAxisIntegerLabels(bool integerOnly) {
  m_yIntegerLabels = integerOnly;
  replot();
}

void PlotWidget::setXAxisIntegerLabels(bool integerOnly) {
  m_xIntegerLabels = integerOnly;
  replot();
}

void PlotWidget::setYAxisAbbreviatedLabels(bool abbreviated) {
  m_yAbbreviatedLabels = abbreviated;
  replot();
}

void PlotWidget::setAxisTickStep(Qt::Orientation orientation, double step,
                                 double origin) {
  if (orientation == Qt::Horizontal) {
    m_xAxisTickStep = step;
    m_xAxisTickOrigin = origin;
    m_xAxisUseCustomTicks = (step > 0);
    replot();
  } else if (orientation == Qt::Vertical) {
    m_yAxisTickStep = step;
    m_yAxisTickOrigin = origin;
    m_yAxisUseCustomTicks = (step > 0);
    replot();
  }
}

void PlotWidget::setSecondaryYAxisEnabled(bool enabled) {
  m_secondaryYAxisEnabled = enabled;
  replot();
}

void PlotWidget::setSecondaryYAxisTitle(const QString& title) {
  m_secondaryYAxisTitle = title;
  replot();
}

void PlotWidget::setSecondaryYAxisRange(double min, double max) {
  m_secondaryYMin = min;
  m_secondaryYMax = max;
  replot();
}

void PlotWidget::setSecondaryYAxisTickStep(double step, double origin) {
  m_secondaryYAxisTickStep = step;
  m_secondaryYAxisTickOrigin = origin;
  m_secondaryYAxisUseCustomTicks = (step > 0);
  replot();
}

void PlotWidget::setGridEnabled(bool enabled) {
  m_gridEnabled = enabled;
  if (m_grid) {
    m_grid->setEnabled(enabled);
  }
}

void PlotWidget::setGridPen(const QPen& pen) {
  if (m_grid) {
    m_grid->setPen(pen);
  }
}

PlotSeries* PlotWidget::addSeries(const QString& title) {
  PlotSeries* series = new PlotSeries(this);
  series->setTitle(title);
  m_series.append(series);
  m_scene->addItem(series);
  return series;
}

void PlotWidget::removeSeries(PlotSeries* series) {
  if (series && m_series.contains(series)) {
    m_series.removeAll(series);
    m_scene->removeItem(series);
    delete series;
  }
}

void PlotWidget::clearSeries() {
  for (PlotSeries* series : m_series) {
    m_scene->removeItem(series);
    delete series;
  }
  m_series.clear();
}

PlotMarker* PlotWidget::addMarker() {
  PlotMarker* marker = new PlotMarker(this);
  m_markers.append(marker);
  m_scene->addItem(marker);
  return marker;
}

void PlotWidget::removeMarker(PlotMarker* marker) {
  if (marker && m_markers.contains(marker)) {
    m_markers.removeAll(marker);
    m_scene->removeItem(marker);
    delete marker;
  }
}

void PlotWidget::clearMarkers() {
  for (PlotMarker* marker : m_markers) {
    m_scene->removeItem(marker);
    delete marker;
  }
  m_markers.clear();
}

void PlotWidget::showNoDataMessage(const QString& message) {
  // Clear all series and markers
  clearSeries();
  clearMarkers();

  // Remove any existing no-data text item
  if (m_noDataTextItem) {
    m_scene->removeItem(m_noDataTextItem);
    delete m_noDataTextItem;
    m_noDataTextItem = nullptr;
  }

  // Disable secondary Y-axis
  setSecondaryYAxisEnabled(false);

  // Add a centered text item to the scene
  m_noDataTextItem = new QGraphicsTextItem(message);

  // Set font and color based on theme
  QFont font = m_noDataTextItem->font();
  font.setPointSize(14);
  m_noDataTextItem->setFont(font);

  m_noDataTextItem->setDefaultTextColor(
      theme_tokens::mutedText(QApplication::palette()));

  // Center the text in the plot area
  QRectF textRect = m_noDataTextItem->boundingRect();
  m_noDataTextItem->setPos(m_plotRect.center().x() - textRect.width() / 2,
                           m_plotRect.center().y() - textRect.height() / 2);

  m_scene->addItem(m_noDataTextItem);
}

void PlotWidget::clearNoDataMessage() {
  if (m_noDataTextItem) {
    m_scene->removeItem(m_noDataTextItem);
    delete m_noDataTextItem;
    m_noDataTextItem = nullptr;
  }
}

void PlotWidget::setLegendEnabled(bool enabled) {
  m_legendEnabled = enabled;
  if (m_legend) {
    m_legend->setEnabled(enabled);
  }
}

void PlotWidget::setZoomEnabled(bool enabled) {
  m_zoomEnabled = enabled;
  if (enabled) {
    m_view->setDragMode(QGraphicsView::RubberBandDrag);
  }
}

void PlotWidget::setPanEnabled(bool enabled) { m_panEnabled = enabled; }

void PlotWidget::resetZoom() {
  m_view->fitInView(m_plotRect, Qt::KeepAspectRatio);
}

void PlotWidget::setCanvasBackground(const QColor& color) {
  m_usePaletteCanvasBackground = false;
  m_canvasBackground = color;
  m_scene->setBackgroundBrush(QBrush(color));
}

bool PlotWidget::isDarkTheme() {
  // Check for command line overrides first
  QVariant themeProperty = QApplication::instance()->property("isDarkTheme");
  if (themeProperty.isValid()) {
    return themeProperty.toBool();
  }

  // Otherwise, use Qt's automatic palette detection (OS provides this)
  QPalette appPalette = QApplication::palette();
  QColor windowColor = appPalette.color(QPalette::Window);
  QColor textColor = appPalette.color(QPalette::WindowText);

  // Simple heuristic: if window is darker than text, we're in dark mode
  return windowColor.lightness() < textColor.lightness();
}

void PlotWidget::updateTheme() {
  // Use the static utility function
  m_isDarkTheme = isDarkTheme();

  if (m_usePaletteCanvasBackground) {
    m_canvasBackground = QApplication::palette().color(QPalette::Base);
    m_scene->setBackgroundBrush(QBrush(m_canvasBackground));
  }

  if (m_noDataTextItem) {
    m_noDataTextItem->setDefaultTextColor(
        theme_tokens::mutedText(QApplication::palette()));
  }

  // Update all plot elements for the new theme
  replot();
}

void PlotWidget::replot() {
  if (m_xAutoScale || m_yAutoScale) {
    calculateDataRange();
  }

  updatePlotArea();

  // Set scene rectangle to match our plot area with margins for labels
  QRectF sceneRect = QRectF(0, 0, m_view->width(), m_view->height());
  m_scene->setSceneRect(sceneRect);

  // Update all series
  for (PlotSeries* series : m_series) {
    series->updatePath(m_plotRect, m_dataRect);
  }

  // Update grid
  if (m_grid) {
    m_grid->updateGrid(m_plotRect, m_dataRect, m_isDarkTheme, m_xMin, m_xMax,
                       m_yMin, m_yMax, m_xAxisUseCustomTicks, m_xAxisTickStep,
                       m_xAxisTickOrigin, m_yAxisUseCustomTicks,
                       m_yAxisTickStep, m_yAxisTickOrigin,
                       m_secondaryYAxisEnabled, m_secondaryYMin,
                       m_secondaryYMax, m_secondaryYAxisUseCustomTicks,
                       m_secondaryYAxisTickStep, m_secondaryYAxisTickOrigin);
  }

  // Update markers
  for (PlotMarker* marker : m_markers) {
    marker->updateMarker(m_plotRect, m_dataRect);
  }

  // Update legend
  if (m_legend) {
    m_legend->updateLegend(m_series, m_plotRect);
  }

  // Update axis labels
  if (m_axisLabels) {
    m_axisLabels->updateLabels(
        m_plotRect, m_dataRect, m_xAxisTitle, m_yAxisTitle, m_xMin, m_xMax,
        m_yMin, m_yMax, m_xIntegerLabels, m_yIntegerLabels, m_isDarkTheme,
        m_secondaryYAxisEnabled, m_secondaryYAxisTitle, m_secondaryYMin,
        m_secondaryYMax, m_xAxisUseCustomTicks, m_xAxisTickStep,
        m_xAxisTickOrigin, m_yAxisUseCustomTicks, m_yAxisTickStep,
        m_yAxisTickOrigin, m_secondaryYAxisUseCustomTicks,
        m_secondaryYAxisTickStep, m_secondaryYAxisTickOrigin,
        m_yAbbreviatedLabels);
  }

  // Reset view transformation to 1:1 scale
  m_view->resetTransform();
  m_view->setSceneRect(sceneRect);
}

void PlotWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  updatePlotArea();
  replot();
}

void PlotWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    // Map click position to scene coordinates
    QPoint viewPos = m_view->mapFromParent(event->pos());
    QPointF scenePos = m_view->mapToScene(viewPos);

    // Check if click is within plot area
    if (m_plotRect.contains(scenePos)) {
      m_isDragging = true;
      // Convert to data coordinates
      QPointF dataPoint = mapToData(scenePos);
      emit plotClicked(dataPoint);
    }
  }

  QWidget::mousePressEvent(event);
}

void PlotWidget::mouseMoveEvent(QMouseEvent* event) {
  if (m_isDragging) {
    // Map position to scene coordinates
    QPoint viewPos = m_view->mapFromParent(event->pos());
    QPointF scenePos = m_view->mapToScene(viewPos);

    // Check if still within plot area
    if (m_plotRect.contains(scenePos)) {
      // Convert to data coordinates
      QPointF dataPoint = mapToData(scenePos);
      emit plotDragged(dataPoint);
    }
  }

  QWidget::mouseMoveEvent(event);
}

void PlotWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    m_isDragging = false;
  }

  QWidget::mouseReleaseEvent(event);
}

bool PlotWidget::eventFilter(QObject* obj, QEvent* event) {
  if (obj == m_view->viewport()) {
    if (event->type() == QEvent::MouseButtonPress) {
      QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
      if (mouseEvent->button() == Qt::LeftButton) {
        // Map to scene coordinates
        QPointF scenePos = m_view->mapToScene(mouseEvent->pos());

        // Check if click is within plot area
        if (m_plotRect.contains(scenePos)) {
          m_isDragging = true;
          // Convert to data coordinates
          QPointF dataPoint = mapToData(scenePos);
          emit plotClicked(dataPoint);
          return true;  // Event handled
        }
      }
    } else if (event->type() == QEvent::MouseMove) {
      QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
      if (m_isDragging) {
        // Map to scene coordinates
        QPointF scenePos = m_view->mapToScene(mouseEvent->pos());

        // Check if still within plot area
        if (m_plotRect.contains(scenePos)) {
          // Convert to data coordinates
          QPointF dataPoint = mapToData(scenePos);
          emit plotDragged(dataPoint);
        }
        return true;  // Event handled
      }
    } else if (event->type() == QEvent::MouseButtonRelease) {
      QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
      if (mouseEvent->button() == Qt::LeftButton) {
        m_isDragging = false;
        return true;  // Event handled
      }
    }
  }

  return QWidget::eventFilter(obj, event);
}

void PlotWidget::onSceneSelectionChanged() {
  // Handle selection changes if needed
}

void PlotWidget::updatePlotArea() {
  QSize viewSize = m_view->size();
  const int leftMargin = 80;    // Space for Y-axis labels and title
  const int bottomMargin = 60;  // Space for X-axis labels and title
  const int topMargin = 20;     // Small top margin
  int rightMargin =
      20;  // Small right margin (or larger if secondary Y-axis enabled)

  // Increase right margin if secondary Y-axis is enabled
  if (m_secondaryYAxisEnabled) {
    rightMargin = 100;  // Space for secondary Y-axis labels and title
  }

  m_plotRect =
      QRectF(leftMargin, topMargin, viewSize.width() - leftMargin - rightMargin,
             viewSize.height() - topMargin - bottomMargin);

  m_dataRect = QRectF(m_xMin, m_yMin, m_xMax - m_xMin, m_yMax - m_yMin);
}

void PlotWidget::calculateDataRange() {
  if (m_series.isEmpty()) return;

  bool firstPoint = true;
  double xMin = 0, xMax = 0, yMin = 0, yMax = 0;

  for (PlotSeries* series : m_series) {
    const QVector<QPointF>& data = series->data();
    for (const QPointF& point : data) {
      if (firstPoint) {
        xMin = xMax = point.x();
        yMin = yMax = point.y();
        firstPoint = false;
      } else {
        xMin = qMin(xMin, point.x());
        xMax = qMax(xMax, point.x());
        yMin = qMin(yMin, point.y());
        yMax = qMax(yMax, point.y());
      }
    }
  }

  if (!firstPoint) {
    if (m_xAutoScale) {
      m_xMin = xMin;
      m_xMax = xMax;
    }
    if (m_yAutoScale) {
      m_yMin = yMin;
      m_yMax = yMax;
    }
  }
}

QPointF PlotWidget::mapToData(const QPointF& scenePos) const {
  double x = m_xMin + (scenePos.x() - m_plotRect.left()) * (m_xMax - m_xMin) /
                          m_plotRect.width();
  double y = m_yMax - (scenePos.y() - m_plotRect.top()) * (m_yMax - m_yMin) /
                          m_plotRect.height();
  return QPointF(x, y);
}

QPointF PlotWidget::mapFromData(const QPointF& dataPos) const {
  double x = m_plotRect.left() +
             (dataPos.x() - m_xMin) * m_plotRect.width() / (m_xMax - m_xMin);
  double y = m_plotRect.top() +
             (m_yMax - dataPos.y()) * m_plotRect.height() / (m_yMax - m_yMin);
  return QPointF(x, y);
}

// PlotSeries implementation
PlotSeries::PlotSeries(PlotWidget* parent)
    : QGraphicsPathItem(), m_plotWidget(parent), m_style(Lines) {
  setPen(QPen(Qt::blue, 1.0));
}

void PlotSeries::setTitle(const QString& title) { m_title = title; }

void PlotSeries::setPen(const QPen& pen) { QGraphicsPathItem::setPen(pen); }

void PlotSeries::setBrush(const QBrush& brush) {
  QGraphicsPathItem::setBrush(brush);
}

void PlotSeries::setStyle(PlotStyle style) { m_style = style; }

void PlotSeries::setData(const QVector<QPointF>& data) { m_data = data; }

void PlotSeries::setData(const QVector<double>& xData,
                         const QVector<double>& yData) {
  m_data.clear();
  int count = static_cast<int>(qMin(xData.size(), yData.size()));
  for (int i = 0; i < count; ++i) {
    m_data.append(QPointF(xData[i], yData[i]));
  }
}

void PlotSeries::setVisible(bool visible) {
  QGraphicsPathItem::setVisible(visible);
}

void PlotSeries::updatePath(const QRectF& plotRect, const QRectF& dataRect) {
  if (m_data.isEmpty() || !m_plotWidget) return;

  // A series with more points than the plot has pixel columns builds a path
  // whose extra points land on pixels the path already covers. Dropping all
  // but each column's extremes draws the same trace from a path bounded by the
  // plot width, which matters most for the per-frame plots (line scope,
  // histogram) that rebuild this on every displayed frame.
  const QVector<QPointF>& points = orc::gui::decimateSeriesToColumns(
      m_data, dataRect.left(), dataRect.right(),
      static_cast<int>(std::lround(plotRect.width())));

  QPainterPath path;

  if (m_style == Bars) {
    // Draw vertical bars from x-axis (y=0) to each data point
    for (const QPointF& dataPoint : points) {
      QPointF scenePoint = m_plotWidget->mapFromData(dataPoint);
      QPointF basePoint =
          m_plotWidget->mapFromData(QPointF(dataPoint.x(), 0.0));

      // Draw vertical line from base (y=0) to the data point
      path.moveTo(basePoint);
      path.lineTo(scenePoint);
    }
  } else {
    // Default Lines style: connect points with continuous line
    bool firstPoint = true;

    for (const QPointF& dataPoint : points) {
      QPointF scenePoint = m_plotWidget->mapFromData(dataPoint);

      if (firstPoint) {
        path.moveTo(scenePoint);
        firstPoint = false;
      } else {
        path.lineTo(scenePoint);
      }
    }
  }

  setPath(path);
}

// PlotGrid implementation
PlotGrid::PlotGrid(PlotWidget* parent)
    : QGraphicsItem(),
      m_pen(QPen(Qt::gray, 1.0)),
      m_usePalettePen(true),
      m_enabled(true),
      m_isDarkTheme(false),
      m_xMin(0),
      m_xMax(100),
      m_yMin(0),
      m_yMax(100),
      m_xUseCustomTicks(false),
      m_yUseCustomTicks(false),
      m_xTickStep(0),
      m_xTickOrigin(0),
      m_yTickStep(0),
      m_yTickOrigin(0),
      m_secondaryYEnabled(false),
      m_secondaryYMin(0),
      m_secondaryYMax(100),
      m_secondaryYUseCustomTicks(false),
      m_secondaryYTickStep(0),
      m_secondaryYTickOrigin(0),
      m_plotWidget(parent) {
  setZValue(-1);  // Draw behind curves
}

void PlotGrid::setPen(const QPen& pen) {
  m_usePalettePen = false;
  m_pen = pen;
  update();
}

void PlotGrid::setEnabled(bool enabled) {
  m_enabled = enabled;
  setVisible(enabled);
}

QRectF PlotGrid::boundingRect() const { return m_plotRect; }

void PlotGrid::paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
                     QWidget* widget) {
  Q_UNUSED(option)
  Q_UNUSED(widget)

  if (!m_enabled) return;

  if (m_usePalettePen) {
    painter->setPen(QPen(theme_tokens::gridLine(QApplication::palette()), 1.0));
  } else {
    painter->setPen(m_pen);
  }

  // Draw vertical grid lines
  if (m_xUseCustomTicks && m_xTickStep > 0) {
    // Use custom tick positions for vertical gridlines
    double firstTick =
        std::ceil((m_xMin - m_xTickOrigin) / m_xTickStep) * m_xTickStep +
        m_xTickOrigin;
    for (double dataX = firstTick; dataX <= m_xMax; dataX += m_xTickStep) {
      if (dataX < m_xMin) continue;
      double fraction = (dataX - m_xMin) / (m_xMax - m_xMin);
      double x = m_plotRect.left() + m_plotRect.width() * fraction;
      painter->drawLine(QPointF(x, m_plotRect.top()),
                        QPointF(x, m_plotRect.bottom()));
    }
  } else {
    // Default: 10 evenly spaced vertical lines
    int numVerticalLines = 10;
    for (int i = 0; i <= numVerticalLines; ++i) {
      double x = m_plotRect.left() + i * m_plotRect.width() / numVerticalLines;
      painter->drawLine(QPointF(x, m_plotRect.top()),
                        QPointF(x, m_plotRect.bottom()));
    }
  }

  // Draw horizontal grid lines
  // If a secondary Y-axis (IRE) is enabled, align grid lines to its ticks
  if (m_secondaryYEnabled) {
    if (m_secondaryYUseCustomTicks && m_secondaryYTickStep > 0) {
      double firstTick = std::ceil((m_secondaryYMin - m_secondaryYTickOrigin) /
                                   m_secondaryYTickStep) *
                             m_secondaryYTickStep +
                         m_secondaryYTickOrigin;
      for (double dataY = firstTick; dataY <= m_secondaryYMax;
           dataY += m_secondaryYTickStep) {
        if (dataY < m_secondaryYMin) continue;
        double fraction =
            (dataY - m_secondaryYMin) / (m_secondaryYMax - m_secondaryYMin);
        double y = m_plotRect.bottom() - m_plotRect.height() * fraction;
        painter->drawLine(QPointF(m_plotRect.left(), y),
                          QPointF(m_plotRect.right(), y));
      }
    } else {
      // Default: match secondary axis auto ticks (10 divisions)
      int numHorizontalLines = 10;
      for (int i = 0; i <= numHorizontalLines; ++i) {
        double y =
            m_plotRect.bottom() - m_plotRect.height() * i / numHorizontalLines;
        painter->drawLine(QPointF(m_plotRect.left(), y),
                          QPointF(m_plotRect.right(), y));
      }
    }
  } else {
    // No secondary axis: use primary Y-axis ticks
    if (m_yUseCustomTicks && m_yTickStep > 0) {
      double firstTick =
          std::ceil((m_yMin - m_yTickOrigin) / m_yTickStep) * m_yTickStep +
          m_yTickOrigin;
      for (double dataY = firstTick; dataY <= m_yMax; dataY += m_yTickStep) {
        if (dataY < m_yMin) continue;
        double fraction = (dataY - m_yMin) / (m_yMax - m_yMin);
        double y = m_plotRect.bottom() - m_plotRect.height() * fraction;
        painter->drawLine(QPointF(m_plotRect.left(), y),
                          QPointF(m_plotRect.right(), y));
      }
    } else {
      int numHorizontalLines = 8;
      for (int i = 0; i <= numHorizontalLines; ++i) {
        double y =
            m_plotRect.top() + i * m_plotRect.height() / numHorizontalLines;
        painter->drawLine(QPointF(m_plotRect.left(), y),
                          QPointF(m_plotRect.right(), y));
      }
    }
  }
}

void PlotGrid::updateGrid(
    const QRectF& plotRect, const QRectF& dataRect, bool isDarkTheme,
    double xMin, double xMax, double yMin, double yMax, bool xUseCustomTicks,
    double xTickStep, double xTickOrigin, bool yUseCustomTicks,
    double yTickStep, double yTickOrigin, bool secondaryYEnabled,
    double secondaryYMin, double secondaryYMax, bool secondaryYUseCustomTicks,
    double secondaryYTickStep, double secondaryYTickOrigin) {
  prepareGeometryChange();
  m_plotRect = plotRect;
  m_dataRect = dataRect;
  m_isDarkTheme = isDarkTheme;
  m_xMin = xMin;
  m_xMax = xMax;
  m_yMin = yMin;
  m_yMax = yMax;
  m_xUseCustomTicks = xUseCustomTicks;
  m_xTickStep = xTickStep;
  m_xTickOrigin = xTickOrigin;
  m_yUseCustomTicks = yUseCustomTicks;
  m_yTickStep = yTickStep;
  m_yTickOrigin = yTickOrigin;
  m_secondaryYEnabled = secondaryYEnabled;
  m_secondaryYMin = secondaryYMin;
  m_secondaryYMax = secondaryYMax;
  m_secondaryYUseCustomTicks = secondaryYUseCustomTicks;
  m_secondaryYTickStep = secondaryYTickStep;
  m_secondaryYTickOrigin = secondaryYTickOrigin;
  update();
}

// PlotMarker implementation
PlotMarker::PlotMarker(PlotWidget* parent)
    : QGraphicsItem(),
      m_style(VLine),
      m_pen(QPen(Qt::red, 1.0)),
      m_dataPos(0, 0),
      m_plotWidget(parent) {}

void PlotMarker::setStyle(MarkerStyle style) {
  m_style = style;
  update();
}

void PlotMarker::setPen(const QPen& pen) {
  m_pen = pen;
  update();
}

void PlotMarker::setPosition(const QPointF& pos) {
  prepareGeometryChange();
  m_dataPos = pos;
  update();
}

void PlotMarker::setLabel(const QString& label) {
  m_label = label;
  update();
}

QRectF PlotMarker::boundingRect() const {
  if (!m_plotWidget || m_plotRect.isEmpty()) return QRectF();

  QPointF scenePos = m_plotWidget->mapFromData(m_dataPos);

  // Only return the actual area occupied by the marker line (plus small margin)
  // This prevents unnecessary repainting of the entire plot
  const qreal margin = 2.0;

  switch (m_style) {
    case VLine:
      return QRectF(scenePos.x() - margin, m_plotRect.top(), margin * 2,
                    m_plotRect.height());
    case HLine:
      return QRectF(m_plotRect.left(), scenePos.y() - margin,
                    m_plotRect.width(), margin * 2);
    case Cross:
      return m_plotRect;  // Cross needs full area
  }

  return QRectF();
}

void PlotMarker::paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) {
  Q_UNUSED(option)
  Q_UNUSED(widget)

  if (!m_plotWidget) return;

  painter->setPen(m_pen);

  QPointF scenePos = m_plotWidget->mapFromData(m_dataPos);

  switch (m_style) {
    case VLine:
      painter->drawLine(QPointF(scenePos.x(), m_plotRect.top()),
                        QPointF(scenePos.x(), m_plotRect.bottom()));
      break;
    case HLine:
      painter->drawLine(QPointF(m_plotRect.left(), scenePos.y()),
                        QPointF(m_plotRect.right(), scenePos.y()));
      break;
    case Cross:
      painter->drawLine(QPointF(scenePos.x(), m_plotRect.top()),
                        QPointF(scenePos.x(), m_plotRect.bottom()));
      painter->drawLine(QPointF(m_plotRect.left(), scenePos.y()),
                        QPointF(m_plotRect.right(), scenePos.y()));
      break;
  }

  if (!m_label.isEmpty()) {
    QFont font = painter->font();
    QFontMetrics fm(font);
    QRect textRect = fm.boundingRect(m_label);
    QPointF textPos = scenePos + QPointF(5, -5);
    painter->drawText(textPos, m_label);
  }
}

void PlotMarker::updateMarker(const QRectF& plotRect, const QRectF& dataRect) {
  prepareGeometryChange();
  m_plotRect = plotRect;
  m_dataRect = dataRect;
  update();
}

// PlotLegend implementation
PlotLegend::PlotLegend(PlotWidget* parent)
    : QGraphicsItem(), m_enabled(false), m_plotWidget(parent) {
  setZValue(1);  // Draw on top
}

void PlotLegend::setEnabled(bool enabled) {
  m_enabled = enabled;
  setVisible(enabled);
}

void PlotLegend::updateLegend(const QList<PlotSeries*>& series,
                              const QRectF& plotRect) {
  m_series = series;

  if (!m_enabled || series.isEmpty()) {
    m_boundingRect = QRectF();
    return;
  }

  // Calculate legend size
  QFont font;
  QFontMetrics fm(font);

  int maxWidth = 0;
  int totalHeight = 10;  // Top padding

  for (PlotSeries* s : series) {
    if (!s->title().isEmpty()) {
      int width = fm.horizontalAdvance(s->title()) + 30;  // 30 for line sample
      maxWidth = qMax(maxWidth, width);
      totalHeight += fm.height() + 2;
    }
  }

  totalHeight += 5;  // Bottom padding
  maxWidth += 10;    // Left and right padding (5 on each side)

  // Position legend in top-right corner
  m_boundingRect = QRectF(plotRect.right() - maxWidth - 10, plotRect.top() + 10,
                          maxWidth, totalHeight);

  update();
}

QRectF PlotLegend::boundingRect() const { return m_boundingRect; }

void PlotLegend::paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) {
  Q_UNUSED(option)
  Q_UNUSED(widget)

  if (!m_enabled || m_series.isEmpty()) return;

  const QPalette palette = QApplication::palette();

  // Draw legend background
  QColor legend_background = palette.color(QPalette::Window);
  legend_background.setAlpha(220);
  painter->fillRect(m_boundingRect, legend_background);
  painter->setPen(QPen(palette.color(QPalette::Mid), 1.0));
  painter->drawRect(m_boundingRect);

  QFont font;
  QFontMetrics fm(font);
  painter->setFont(font);

  int y = static_cast<int>(m_boundingRect.top()) + 5;

  for (PlotSeries* series : m_series) {
    if (!series->title().isEmpty()) {
      // Draw line sample
      painter->setPen(series->pen());
      painter->drawLine(
          QPointF(m_boundingRect.left() + 5,
                  y + fm.height() / 2),  // NOLINT(bugprone-integer-division)
          QPointF(m_boundingRect.left() + 25,
                  y + fm.height() / 2));  // NOLINT(bugprone-integer-division)

      // Draw text
      painter->setPen(QPen(palette.color(QPalette::WindowText)));
      painter->drawText(QPointF(m_boundingRect.left() + 30, y + fm.ascent()),
                        series->title());

      y += fm.height() + 2;
    }
  }
}

// PlotAxisLabels implementation
PlotAxisLabels::PlotAxisLabels(PlotWidget* parent)
    : QGraphicsItem(),
      m_xMin(0),
      m_xMax(100),
      m_yMin(0),
      m_yMax(100),
      m_xIntegerLabels(false),
      m_yIntegerLabels(false),
      m_yAbbreviatedLabels(false),
      m_isDarkTheme(false),
      m_plotWidget(parent) {
  setZValue(2);  // Draw on top of grid but below curves
}

void PlotAxisLabels::updateLabels(
    const QRectF& plotRect, const QRectF& dataRect, const QString& xTitle,
    const QString& yTitle, double xMin, double xMax, double yMin, double yMax,
    bool xIntegerLabels, bool yIntegerLabels, bool isDarkTheme,
    bool secondaryYEnabled, const QString& secondaryYTitle,
    double secondaryYMin, double secondaryYMax, bool xUseCustomTicks,
    double xTickStep, double xTickOrigin, bool yUseCustomTicks,
    double yTickStep, double yTickOrigin, bool secondaryYUseCustomTicks,
    double secondaryYTickStep, double secondaryYTickOrigin,
    bool yAbbreviatedLabels) {
  prepareGeometryChange();
  m_plotRect = plotRect;
  m_dataRect = dataRect;
  m_xTitle = xTitle;
  m_yTitle = yTitle;
  m_secondaryYTitle = secondaryYTitle;
  m_xUseCustomTicks = xUseCustomTicks;
  m_xTickStep = xTickStep;
  m_xTickOrigin = xTickOrigin;
  m_xMin = xMin;
  m_xMax = xMax;
  m_yMin = yMin;
  m_yMax = yMax;
  m_secondaryYMin = secondaryYMin;
  m_secondaryYMax = secondaryYMax;
  m_xIntegerLabels = xIntegerLabels;
  m_yIntegerLabels = yIntegerLabels;
  m_yAbbreviatedLabels = yAbbreviatedLabels;
  m_isDarkTheme = isDarkTheme;
  m_secondaryYEnabled = secondaryYEnabled;
  m_xUseCustomTicks = xUseCustomTicks;
  m_xTickStep = xTickStep;
  m_xTickOrigin = xTickOrigin;
  m_yUseCustomTicks = yUseCustomTicks;
  m_yTickStep = yTickStep;
  m_yTickOrigin = yTickOrigin;
  m_secondaryYUseCustomTicks = secondaryYUseCustomTicks;
  m_secondaryYTickStep = secondaryYTickStep;
  m_secondaryYTickOrigin = secondaryYTickOrigin;
  update();
}

QRectF PlotAxisLabels::boundingRect() const {
  // Expand beyond plot area to include space for labels
  // Extra space on right if secondary Y-axis is enabled
  int rightSpace = m_secondaryYEnabled ? 100 : 50;
  return QRectF(0, 0, m_plotRect.right() + rightSpace,
                m_plotRect.bottom() + 50);
}

void PlotAxisLabels::paint(QPainter* painter,
                           const QStyleOptionGraphicsItem* option,
                           QWidget* widget) {
  Q_UNUSED(option)
  Q_UNUSED(widget)

  QColor axisColor = QApplication::palette().color(QPalette::WindowText);

  painter->setPen(QPen(axisColor));
  QFont font = painter->font();
  font.setPointSize(9);
  painter->setFont(font);
  QFontMetrics fm(font);

  // Draw X-axis labels
  if (m_xUseCustomTicks && m_xTickStep > 0) {
    // Use custom tick positions starting from origin
    double firstTick =
        std::ceil((m_xMin - m_xTickOrigin) / m_xTickStep) * m_xTickStep +
        m_xTickOrigin;
    std::vector<double> tickValues;
    for (double dataX = firstTick; dataX <= m_xMax + 1e-9;
         dataX += m_xTickStep) {
      if (dataX < m_xMin - 1e-9) continue;
      if (dataX > m_xMax + 1e-9) break;
      tickValues.push_back(dataX);
    }

    // Always include the max value if not already present
    if (tickValues.empty() || tickValues.back() < m_xMax - 1e-9) {
      tickValues.push_back(m_xMax);
    }

    for (double dataX : tickValues) {
      double fraction = (dataX - m_xMin) / (m_xMax - m_xMin);
      double sceneX = m_plotRect.left() + m_plotRect.width() * fraction;

      // Draw tick mark
      painter->setPen(QPen(axisColor, 1));
      painter->drawLine(QPointF(sceneX, m_plotRect.bottom()),
                        QPointF(sceneX, m_plotRect.bottom() + 5));

      // Draw label
      QString label;
      if (m_xIntegerLabels) {
        label = QString::number(qRound(dataX));
      } else {
        double intPart;
        if (std::modf(dataX, &intPart) == 0.0) {
          label = QString::number(static_cast<int64_t>(dataX));
        } else {
          label = QString::number(dataX, 'f', 1);
        }
      }
      QRect textRect = fm.boundingRect(label);
      painter->drawText(
          QRectF(sceneX - textRect.width() / 2.0, m_plotRect.bottom() + 5,
                 textRect.width(), textRect.height()),
          Qt::AlignCenter, label);
    }
  } else {
    // Default: use 10 ticks
    int numXTicks = 10;
    for (int i = 0; i <= numXTicks; ++i) {
      double dataX = m_xMin + (m_xMax - m_xMin) * i / numXTicks;
      // Ensure we don't go past the max due to floating point errors
      if (dataX > m_xMax + 1e-9) dataX = m_xMax;

      double sceneX = m_plotRect.left() + m_plotRect.width() * i / numXTicks;

      // Draw tick mark
      painter->setPen(QPen(axisColor, 1));
      painter->drawLine(QPointF(sceneX, m_plotRect.bottom()),
                        QPointF(sceneX, m_plotRect.bottom() + 5));

      // Draw label
      QString label;
      if (m_xIntegerLabels) {
        label = QString::number(qRound(dataX));
      } else {
        double intPart;
        if (std::modf(dataX, &intPart) == 0.0) {
          label = QString::number(static_cast<int64_t>(dataX));
        } else {
          label = QString::number(dataX, 'f', 1);
        }
      }
      QRect textRect = fm.boundingRect(label);
      painter->drawText(
          QRectF(sceneX - textRect.width() / 2.0, m_plotRect.bottom() + 5,
                 textRect.width(), textRect.height()),
          Qt::AlignCenter, label);
    }
  }

  // Draw Y-axis labels
  if (m_yUseCustomTicks && m_yTickStep > 0) {
    // Use custom tick positions starting from origin
    double firstTick =
        std::ceil((m_yMin - m_yTickOrigin) / m_yTickStep) * m_yTickStep +
        m_yTickOrigin;
    for (double dataY = firstTick; dataY <= m_yMax; dataY += m_yTickStep) {
      if (dataY < m_yMin) continue;

      double fraction = (dataY - m_yMin) / (m_yMax - m_yMin);
      double sceneY = m_plotRect.bottom() - m_plotRect.height() * fraction;

      // Draw tick mark
      painter->setPen(QPen(axisColor, 1));
      painter->drawLine(QPointF(m_plotRect.left() - 5, sceneY),
                        QPointF(m_plotRect.left(), sceneY));

      QString label;
      if (m_yAbbreviatedLabels && m_yIntegerLabels) {
        double v = qRound(dataY);
        if (v >= 1'000'000.0) {
          double x = v / 1'000'000.0;
          label = (x == std::floor(x))
                      ? QString::number(static_cast<int>(x)) + "M"
                      : QString::number(x, 'f', 1) + "M";
        } else if (v >= 1'000.0) {
          double x = v / 1'000.0;
          label = (x == std::floor(x))
                      ? QString::number(static_cast<int>(x)) + "K"
                      : QString::number(x, 'f', 1) + "K";
        } else {
          label = QString::number(static_cast<int>(v));
        }
      } else if (m_yIntegerLabels) {
        label = QString::number(qRound(dataY));
      } else {
        double intPart;
        if (std::modf(dataY, &intPart) == 0.0) {
          label = QString::number(static_cast<int64_t>(dataY));
        } else {
          label = QString::number(dataY, 'f', 1);
        }
      }
      QRect textRect = fm.boundingRect(label);
      QPointF textPos(
          m_plotRect.left() - 10 - textRect.width(),
          sceneY + textRect.height() / 4);  // NOLINT(bugprone-integer-division)
      painter->drawText(textPos, label);
    }
  } else {
    // Use automatic tick generation
    int numYTicks = 8;
    for (int i = 0; i <= numYTicks; ++i) {
      double dataY = m_yMin + (m_yMax - m_yMin) * i / numYTicks;
      double sceneY = m_plotRect.bottom() - m_plotRect.height() * i / numYTicks;

      // Draw tick mark
      painter->setPen(QPen(axisColor, 1));
      painter->drawLine(QPointF(m_plotRect.left() - 5, sceneY),
                        QPointF(m_plotRect.left(), sceneY));

      QString label;
      if (m_yAbbreviatedLabels && m_yIntegerLabels) {
        double v = qRound(dataY);
        if (v >= 1'000'000.0) {
          double x = v / 1'000'000.0;
          label = (x == std::floor(x))
                      ? QString::number(static_cast<int>(x)) + "M"
                      : QString::number(x, 'f', 1) + "M";
        } else if (v >= 1'000.0) {
          double x = v / 1'000.0;
          label = (x == std::floor(x))
                      ? QString::number(static_cast<int>(x)) + "K"
                      : QString::number(x, 'f', 1) + "K";
        } else {
          label = QString::number(static_cast<int>(v));
        }
      } else if (m_yIntegerLabels) {
        label = QString::number(qRound(dataY));
      } else {
        double intPart;
        if (std::modf(dataY, &intPart) == 0.0) {
          label = QString::number(static_cast<int64_t>(dataY));
        } else {
          label = QString::number(dataY, 'f', 1);
        }
      }
      QRect textRect = fm.boundingRect(label);
      QPointF textPos(
          m_plotRect.left() - 10 - textRect.width(),
          sceneY + textRect.height() / 4);  // NOLINT(bugprone-integer-division)
      painter->drawText(textPos, label);
    }
  }

  // Draw X-axis title
  if (!m_xTitle.isEmpty()) {
    QRect titleRect = fm.boundingRect(m_xTitle);
    QPointF titlePos(
        m_plotRect.center().x() -
            titleRect.width() / 2,  // NOLINT(bugprone-integer-division)
        m_plotRect.bottom() + 40);
    painter->drawText(titlePos, m_xTitle);
  }

  // Draw Y-axis title (rotated)
  if (!m_yTitle.isEmpty()) {
    painter->save();
    painter->translate(20, m_plotRect.center().y());
    painter->rotate(-90);
    QRect titleRect = fm.boundingRect(m_yTitle);
    painter->drawText(-titleRect.width() / 2, titleRect.height() / 2,
                      m_yTitle);  // NOLINT(bugprone-integer-division)
    painter->restore();
  }

  // Draw secondary Y-axis (right side) if enabled
  if (m_secondaryYEnabled) {
    if (m_secondaryYUseCustomTicks && m_secondaryYTickStep > 0) {
      // Use custom tick positions starting from origin
      double firstTick = std::ceil((m_secondaryYMin - m_secondaryYTickOrigin) /
                                   m_secondaryYTickStep) *
                             m_secondaryYTickStep +
                         m_secondaryYTickOrigin;
      for (double dataY = firstTick; dataY <= m_secondaryYMax;
           dataY += m_secondaryYTickStep) {
        if (dataY < m_secondaryYMin) continue;

        double fraction =
            (dataY - m_secondaryYMin) / (m_secondaryYMax - m_secondaryYMin);
        double sceneY = m_plotRect.bottom() - m_plotRect.height() * fraction;

        // Draw tick mark
        painter->setPen(QPen(axisColor, 1));
        painter->drawLine(QPointF(m_plotRect.right(), sceneY),
                          QPointF(m_plotRect.right() + 5, sceneY));

        // Draw label
        QString label = QString::number(qRound(dataY));
        QRect textRect = fm.boundingRect(label);
        QPointF textPos(m_plotRect.right() + 10,
                        sceneY + static_cast<double>(textRect.height()) / 4.0);
        painter->drawText(textPos, label);
      }
    } else {
      // Use automatic tick generation
      int numYTicks = 10;  // 0, 10, 20, ..., 100 IRE
      for (int i = 0; i <= numYTicks; ++i) {
        double dataY = m_secondaryYMin +
                       (m_secondaryYMax - m_secondaryYMin) * i / numYTicks;
        double sceneY =
            m_plotRect.bottom() - m_plotRect.height() * i / numYTicks;

        // Draw tick mark
        painter->setPen(QPen(axisColor, 1));
        painter->drawLine(QPointF(m_plotRect.right(), sceneY),
                          QPointF(m_plotRect.right() + 5, sceneY));

        // Draw label
        QString label = QString::number(qRound(dataY));
        QRect textRect = fm.boundingRect(label);
        QPointF textPos(m_plotRect.right() + 10,
                        sceneY + static_cast<double>(textRect.height()) / 4.0);
        painter->drawText(textPos, label);
      }
    }

    // Draw secondary Y-axis title (rotated)
    if (!m_secondaryYTitle.isEmpty()) {
      painter->save();
      painter->translate(m_plotRect.right() + 60, m_plotRect.center().y());
      painter->rotate(-90);
      QRect titleRect = fm.boundingRect(m_secondaryYTitle);
      painter->drawText(-titleRect.width() / 2, titleRect.height() / 2,
                        m_secondaryYTitle);
      painter->restore();
    }
  }

  // Draw plot border
  painter->setPen(QPen(axisColor, 1));
  painter->drawRect(m_plotRect);
}
