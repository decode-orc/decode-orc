/*
 * File:        vbi_cri_correlator.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Locates the clock run-in within a stored line record
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_CRI_CORRELATOR_H
#define ORC_VBI_CRI_CORRELATOR_H

#include <cstdint>
#include <vector>

#include "vbi_cri_template.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {

// Range of run-in positions a search considers, in record samples, measured to
// the leading edge of the first one bit.
struct VBICRISearchWindow {
  double begin_samples = 0.0;
  double end_samples = 0.0;

  // True lets the run-in begin before the record does.  A card whose window
  // opens late clips the head off the run-in — a cx23885 dump can start 44
  // samples into its line-21 run-in — and the search
  // then correlates the part of the template that is inside the record, so
  // begin_samples may be negative.  False keeps every position inside the
  // record, whatever begin_samples says.
  bool clipped_head = false;

  bool empty() const { return !(end_samples > begin_samples); }
};

// The window a source format's own configuration implies: the position its
// capture offset predicts, widened by the format's search tolerance.
//
// Deriving the window from the descriptor rather than from a global constant
// is what lets a tape-sourced format search wider than a broadcast one without
// either of them being wrong (design §5.3.6).
VBICRISearchWindow vbi_cri_search_window(const VBISourceFormat& format,
                                         const VBITeletextService& service);

// What a search found in one record.
struct VBICRIDetection {
  // False when the peak did not reach the acceptance threshold, which means
  // the line carries no data service.  There are many such lines and they are
  // not an error (design §5.3.4).
  bool accepted = false;

  // Refined position of the leading edge of the first run-in one bit, in
  // record samples.
  double anchor_position_samples = 0.0;

  // Normalised correlation at the peak, in [-1, 1].
  double peak_correlation = 0.0;

  // Whole-sample lag at which the peak was found, before refinement.
  int64_t peak_lag = 0;

  // Sub-sample correction applied to the peak, in samples.
  double refinement_samples = 0.0;

  // False when the peak sat at the edge of the search window, where there is
  // no neighbour to interpolate against.  The position is then whole-sample
  // only, and a peak against the window edge is itself a sign that the window
  // is in the wrong place.
  bool refined = false;
};

// Normalised cross-correlation of a template against a record at a whole-sample
// lag, in [-1, 1].
//
// The record window is zero-meaned and scaled by its own energy, so the result
// depends on the shape of the waveform and not at all on the line's black
// level or gain.  That is what makes a single threshold meaningful across
// lines whose amplitudes differ by automatic gain control.
//
// A negative lag puts the head of the template before the record's first
// sample: the part of the template that overlaps the record is correlated with
// it, both sides zero-meaned and scaled over that overlap alone, so a clipped
// run-in is measured on the samples it really has and the samples it lost
// count for nothing either way.  Less than half the template overlapping is
// not a measurement, and correlates with nothing.
double vbi_normalised_correlation(const std::vector<double>& record_samples,
                                  const VBICRITemplate& tmpl, int64_t lag);

// Find the run-in in one record (design §5.3.4 steps 2 to 4).
//
// The peak of the correlation is refined by fitting a parabola through it and
// its two neighbours, which recovers the sub-sample position the whole-sample
// search cannot see.  On a clean line that is good to about a tenth of a
// sample, which is far finer than the placement needs, and it is what makes
// the median over a few hundred lines meaningful rather than quantised.
VBICRIDetection detect_vbi_cri_position(
    const std::vector<double>& record_samples, const VBICRITemplate& tmpl,
    const VBICRISearchWindow& window, double acceptance_correlation);

}  // namespace orc

#endif  // ORC_VBI_CRI_CORRELATOR_H
