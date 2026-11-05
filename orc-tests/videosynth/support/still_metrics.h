/*
 * File:        still_metrics.h
 * Module:      orc-videosynth-support
 * Purpose:     Source-referenced accuracy of a decoded still against its
 *              picture truth: component RMS, RGB PSNR, CIEDE2000, chroma
 *              vector error, cross-colour, cross-luminance and edge response
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/common_types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "picture_truth.h"

namespace orc_videosynth {

// A rectangle of the truth grid: samples across, rows down.
struct Region {
  size_t x = 0;
  size_t y = 0;
  size_t width = 0;
  size_t height = 0;
};

// The truth-grid region under a rectangle of the 720-pixel source raster,
// which videosynth maps across the |samples_per_line| active samples.
Region source_region(size_t pixel_x, size_t row, size_t pixel_width,
                     size_t rows, size_t samples_per_line);

// One byte per sample of a |width| x |height| picture, non-zero inside
// |region|.
std::vector<uint8_t> region_mask(size_t width, size_t height,
                                 const Region& region);

// |a| with every sample that |b| clears cleared.
std::vector<uint8_t> mask_and(std::vector<uint8_t> a,
                              const std::vector<uint8_t>& b);

struct StillMetricOptions {
  orc::VideoSystem system = orc::VideoSystem::PAL;
  double sample_rate_hz = 0.0;
  // Samples left out at each end of every row: the active window's edges,
  // where the picture starts and stops against blanking.
  size_t edge_exclusion = 16;
};

// Errors are in percent of the black-to-white excursion; U and V in the
// same units as Y (ComponentPicture).
struct StillMetrics {
  size_t samples = 0;
  double y_rms_pct = 0.0;
  double u_rms_pct = 0.0;
  double v_rms_pct = 0.0;

  // Over R'G'B' (0 black, 1 white), unclipped: 10 log10(1 / MSE).
  double rgb_psnr_db = 0.0;

  // CIEDE2000 between the two pictures as a display shows them: R'G'B'
  // clipped to 0..1, a gamma 2.4 display (BT.1886 with zero black), EBU
  // Tech 3213 primaries for PAL and SMPTE C for NTSC and PAL-M, D65 white.
  double delta_e_mean = 0.0;
  double delta_e_p95 = 0.0;

  // The chroma vector (U, V) where the truth carries colour (|C| >= 0.05):
  // the error of its length, and of its angle in degrees.
  // Its gain: the least-squares scale from the truth's chroma length to the
  // decode's, 1 for a decoder with the right chroma gain.
  size_t chroma_samples = 0;
  double chroma_magnitude_rms_pct = 0.0;
  double chroma_phase_rms_deg = 0.0;
  double chroma_gain = 0.0;

  // Decoded chroma |C| where the truth has none (|C| < 0.005 over the 5
  // samples by 3 rows around) but has luma detail near the subcarrier
  // (truth luma above 3 MHz, PAL, with a local RMS of 0.02 or more).
  size_t cross_colour_samples = 0;
  double cross_colour_rms_pct = 0.0;

  // Luma error in the subcarrier band (3.5 to 5 MHz, PAL) at the truth's
  // chroma edges: where its chroma vector changes by 0.05 or more across
  // two samples or two rows, within one sample or row.
  size_t cross_luma_samples = 0;
  double cross_luma_rms_pct = 0.0;
};

// |decoded| against |truth| (the same geometry) over the samples |mask|
// keeps, less the edge exclusion. The band filters run over whole rows, so
// the mask only selects where each error is counted.
StillMetrics measure_still(const ComponentPicture& decoded,
                           const ComponentPicture& truth,
                           const std::vector<uint8_t>& mask,
                           const StillMetricOptions& options);

// The RMS of |plane| in [low_hz, high_hz), in percent, over the samples
// |mask| keeps less |edge_exclusion| at each end of a row: for example the
// energy a decode leaves above a band edge. The filter runs over whole rows.
double band_rms_pct(const std::vector<double>& plane, size_t width,
                    size_t height, const std::vector<uint8_t>& mask,
                    size_t edge_exclusion, double low_hz, double high_hz,
                    double sample_rate_hz);

enum class Plane { Y, U, V };

// One edge's step response: the mean row profile of a region holding a
// single transition, its levels taken from the outer quarters.
struct EdgeResponse {
  double step = 0.0;           // end level minus start level
  double rise_ns = 0.0;        // 10 % to 90 % of the step
  double overshoot_pct = 0.0;  // largest excursion past either level
};

// nullopt when the region's step is under 0.05 or it never crosses 90 %.
std::optional<EdgeResponse> edge_response(const ComponentPicture& picture,
                                          Plane plane, const Region& region,
                                          double sample_rate_hz);

// ── Colour difference ──────────────────────────────────────────────────────

struct Lab {
  double l = 0.0;
  double a = 0.0;
  double b = 0.0;
};

// CIEDE2000 (CIE 142-2001; G. Sharma, W. Wu, E. N. Dalal, "The CIEDE2000
// color-difference formula", Color Res. Appl. 30(1), 2005), kL = kC = kH = 1.
double ciede2000(const Lab& first, const Lab& second);

// Y'UV (ComponentPicture units) as CIELAB, by the display model
// measure_still() states.
Lab yuv_to_lab(double y, double u, double v, orc::VideoSystem system);

}  // namespace orc_videosynth
