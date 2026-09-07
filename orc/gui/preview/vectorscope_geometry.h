/*
 * File:        vectorscope_geometry.h
 * Module:      orc-gui
 * Purpose:     Shared vectorscope geometry and target math helpers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_PREVIEW_VECTORSCOPE_GEOMETRY_H
#define ORC_GUI_PREVIEW_VECTORSCOPE_GEOMETRY_H

#include <orc/stage/preview/orc_vectorscope.h>

#include <QPointF>
#include <QRectF>
#include <cmath>

namespace orc::gui {

constexpr int kVectorscopeCanvasSize = 1024;
constexpr int kVectorscopePlotPadding = 16;
constexpr int kVectorscopeCircleStrokeWidth = 4;
constexpr int kVectorscopeAxisStrokeWidth = 2;
constexpr int kVectorscopeMajorMarkerStrokeWidth = 2;
constexpr int kVectorscopeMinorMarkerStrokeWidth = 1;
constexpr double kVectorscopeSignedFullScale = 32768.0;
constexpr double kVectorscopeUvRange = kVectorscopeSignedFullScale * 2.0;

inline double standardDegreesToScopeRadians(double standard_degrees) {
  // Standard vectorscope degrees are counterclockwise (0=right, 90=up).
  // This renderer uses clockwise-positive angles for screen mapping.
  return (-standard_degrees * M_PI) / 180.0;
}

struct VectorscopePlotGeometry {
  explicit VectorscopePlotGeometry(
      int canvas_size_pixels = kVectorscopeCanvasSize)
      : canvas_size(canvas_size_pixels),
        plot_padding(std::min(kVectorscopePlotPadding, canvas_size_pixels / 4)),
        plot_span_pixels(
            static_cast<double>(canvas_size_pixels - (plot_padding * 2))),
        pixels_per_uv_unit(plot_span_pixels / kVectorscopeUvRange),
        plot_area(static_cast<double>(plot_padding),
                  static_cast<double>(plot_padding), plot_span_pixels,
                  plot_span_pixels),
        centre_point(plot_area.center()) {}

  QPointF mapUV(double u, double v) const {
    return {centre_point.x() + (u * pixels_per_uv_unit),
            centre_point.y() - (v * pixels_per_uv_unit)};
  }

  QPointF pointFromVectorscopeAngle(double angle_radians,
                                    double magnitude_uv) const {
    return mapUV(std::cos(angle_radians) * magnitude_uv,
                 -std::sin(angle_radians) * magnitude_uv);
  }

  QPointF pointFromStandardDegrees(double standard_degrees,
                                   double magnitude_uv) const {
    return pointFromVectorscopeAngle(
        standardDegreesToScopeRadians(standard_degrees), magnitude_uv);
  }

  double magnitudeToPixels(double magnitude_uv) const {
    return magnitude_uv * pixels_per_uv_unit;
  }

  double pixelsToMagnitude(double magnitude_pixels) const {
    return magnitude_pixels / pixels_per_uv_unit;
  }

  int canvas_size;
  int plot_padding;
  double plot_span_pixels;
  double pixels_per_uv_unit;
  QRectF plot_area;
  QPointF centre_point;
};

// ============================================================================
// normalizedRgbToUv
// ============================================================================
// Convert normalised linear RGB (components in [0, 1]) to the U/V chrominance
// representation used for vectorscope display.
//
// Matrix derivation (ITU-R BT.470-6 §1.1.2 / EBU Tech. 3280-E §2.1):
//
//   Y  =  0.299 R + 0.587 G + 0.114 B          (BT.601 luminance equation)
//
//   U  = ku * (B - Y)     ku = 0.492111          (PAL/NTSC U modulation factor)
//   V  = kv * (R - Y)     kv = 0.877283          (PAL/NTSC V modulation factor)
//
//   Expanding (B - Y) and (R - Y) with the BT.601 Y equation:
//
//   U_R = ku * ( 0 - 0.299) = -0.147141
//   U_G = ku * ( 0 - 0.587) = -0.288869    [columns: kU * {-Y_R, -Y_G, 1-Y_B}]
//   U_B = ku * ( 1 - 0.114) =  0.436010
//
//   V_R = kv * ( 1 - 0.299) =  0.614975
//   V_G = kv * ( 0 - 0.587) = -0.514965    [columns: kV * {1-Y_R, -Y_G, -Y_B}]
//   V_B = kv * ( 0 - 0.114) = -0.100010
//
// amplitude_scale maps the U/V result to the display's signed full-scale.
inline orc::UVSample normalizedRgbToUv(double red, double green, double blue,
                                       double amplitude_scale) {
  // ITU-R BT.470-6 §1.1.2 / EBU Tech. 3280-E §2.1
  const double u = (red * -0.147141) + (green * -0.288869) + (blue * 0.436010);
  const double v = (red * 0.614975) + (green * -0.514965) + (blue * -0.100010);
  return {u * amplitude_scale, v * amplitude_scale};
}

inline orc::UVSample calibrateVectorscopeDisplayUv(
    const orc::UVSample& sample, orc::VideoSystem /*system*/) {
  // The NTSC comb decoder converts I/Q → U/V (comb.cpp transformIQ) before
  // writing to ComponentFrame, so decoded NTSC samples are already in the
  // same BT.601 U/V coordinate space as PAL.  No calibration is needed.
  return sample;
}

// ============================================================================
// vectorscopeTargetUv
// ============================================================================
// Return the U/V position for an ideal colour-bar patch at a given saturation
// percentage.  `rgb` encodes the primary/secondary colour as a 3-bit mask
// (bit2=R, bit1=G, bit0=B).
//
// GBR encoder inputs have no setup (SMPTE 170M-2004 §4.3 / ITU-R BT.470-6):
// "off" components are at 0 (blanking) and "on" components are at `percent`.
//
// These are the targets for a decoded-component acquisition, whose samples
// extract_vectorscope_from_component_frame() normalises over the picture
// excursion (black → white).  That is the excursion the encoding equation
// scales chroma by, so a correctly decoded bar lands here whatever the system
// and whatever its setup: the 0.925 of SMPTE 170M-2004 §10 is 92.5/100 IRE,
// already accounted for by dividing decoded chroma by a 92.5 IRE excursion
// rather than a 100 IRE one.  It is a composite acquisition, measured against
// blanking, that still needs the factor applied — see measurementTargetUv().
inline orc::UVSample vectorscopeTargetUv(int rgb, double percent,
                                         double ire_range) {
  const double red = ((rgb >> 2) & 1) ? percent : 0.0;
  const double green = ((rgb >> 1) & 1) ? percent : 0.0;
  const double blue = (rgb & 1) ? percent : 0.0;
  return normalizedRgbToUv(red, green, blue, ire_range);
}

inline orc::UVSample vectorscopeDisplayTargetUv(int rgb, double percent,
                                                double ire_range,
                                                orc::VideoSystem system) {
  return calibrateVectorscopeDisplayUv(
      vectorscopeTargetUv(rgb, percent, ire_range), system);
}

// ============================================================================
// Measurement (composite) graticule geometry
// ============================================================================
// A composite acquisition plots the signal rather than a decoded picture, so
// its graticule carries the two things a grading graticule cannot: the colour
// burst, and — for PAL — both V-switch line phases.

// Nominal colour-burst peak amplitude as a fraction of the active video range
// (blanking → white).
// EBU Tech. 3280-E §1.2: PAL burst is 300 mV peak-to-peak against a 700 mV
// luminance range, so its peak is 150/700 of that range.
constexpr double kPalBurstAmplitudeFraction = 150.0 / 700.0;
// SMPTE 170M-2004 §8.4: NTSC burst is 40 IRE peak-to-peak → 20 IRE peak, and
// blanking → white is 100 IRE.  ITU-R BT.1700-1 Annex 1 Part B: PAL-M follows
// the 525-line levels.
constexpr double kNtscBurstAmplitudeFraction = 0.20;

// Burst vector positions in standard vectorscope degrees (0 = right, CCW
// positive).  ITU-R BT.470-6 Table 2 item 2.16: the PAL burst swings ±45°
// about the −U axis with the V-switch.  SMPTE 170M-2004 §8.4: the NTSC burst
// sits on the −U axis.
constexpr double kPalBurstVPositiveDegrees = 135.0;
constexpr double kPalBurstVNegativeDegrees = 225.0;
constexpr double kNtscBurstDegrees = 180.0;

// Peak burst magnitude in the display's signed full-scale units.
inline double nominalBurstMagnitudeUv(orc::VideoSystem system,
                                      double full_scale) {
  const double fraction = (system == orc::VideoSystem::PAL)
                              ? kPalBurstAmplitudeFraction
                              : kNtscBurstAmplitudeFraction;
  return fraction * full_scale;
}

// True when |system| swings the V component line by line, so a measurement
// graticule needs two mirrored sets of colour-bar targets and two burst
// vectors rather than one of each.
// ITU-R BT.1700-1 Annex 1 Part B: PAL_M carries PAL colour encoding — V-axis
// switch included — on the 525-line raster, so it swings with PAL even though
// it takes its signal levels and burst amplitude from NTSC.
inline bool hasSwitchedVAxis(orc::VideoSystem system) {
  return system == orc::VideoSystem::PAL || system == orc::VideoSystem::PAL_M;
}

// Colour-bar target for one V-switch line phase.  A −V line inverts the V
// component only, so its targets are the +V targets mirrored about the U axis
// — which is exactly what an undelayed composite display shows.
//
// A composite acquisition plots the signal itself, normalised over the active
// video range (blanking → white) so that burst reads in IRE, so here the
// encoding equation's chroma scale is visible and the targets carry it.
// SMPTE 170M-2004 §10 / Annex A.2:
//   N = 0.925(Y) + 7.5 + 0.925(Q)sin(…+33°) + 0.925(I)cos(…+33°)
// The factor is the ratio of the 92.5 IRE picture excursion to the 100 IRE
// range the samples are measured against.  It is taken from the system rather
// than from the signal's own levels because VectorscopeData carries no
// picture-black anchor, so an NTSC-J acquisition — which has no pedestal and
// therefore no 0.925 — still lands 8% inside these targets.
inline orc::UVSample measurementTargetUv(int rgb, double percent,
                                         double ire_range,
                                         orc::VideoSystem system,
                                         orc::VectorscopeLinePhase phase) {
  orc::UVSample target = vectorscopeTargetUv(rgb, percent, ire_range);

  constexpr double kSmpteChromaScale = 0.925;
  if (system == orc::VideoSystem::NTSC || system == orc::VideoSystem::PAL_M) {
    target = {target.u * kSmpteChromaScale, target.v * kSmpteChromaScale};
  }

  if (phase == orc::VectorscopeLinePhase::VNegative) {
    return {target.u, -target.v};
  }
  return target;
}

}  // namespace orc::gui

#endif  // ORC_GUI_PREVIEW_VECTORSCOPE_GEOMETRY_H