/*
 * File:        fm_beat.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     The PAL LaserDisc FM-beat herringbone of one field, predicted
 *              from the field itself, fitted and subtracted
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/common_types.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/orc_source_parameters.h>

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

// Some PAL LaserDisc masters carry a beat between the FM video carrier and
// the colour: a tone at f_c - f_sc, f_c the instantaneous carrier, seen as a
// faint herringbone in flat saturated colour. Its phase is that of
// e^{j (Phi - phi_sc)}, Phi the carrier's phase (the integral of the carrier
// frequency the pre-emphasised composite sets) and phi_sc the chroma's own,
// so its waveform can be predicted from the field and only one complex
// amplitude per line has to be fitted. The model follows `deherring.py`
// (model-2026-10-27b) of github.com/dp111/domesday-aiv-restore, as
// videosynth's fm_beat effect does.

namespace orc::fm_beat {

// The FM carrier at blanking (0 IRE on PAL) and its deviation per IRE, from
// the PAL LaserDisc standard: 6.76 MHz at sync tip, 7.1 MHz at blanking,
// 7.9 MHz at peak white. A master's own carrier differs by some kHz, which
// the beat's phase along a line is sensitive to: FieldBeat::fit() measures
// that offset.
inline constexpr double kNominalCarrierBlackHz = 7.1e6;
inline constexpr double kCarrierHzPerIre = 8000.0;

// A field opens the canceller only with at least this many samples of flat
// saturated colour in its picture: a beat is only seen there.
inline constexpr size_t kMinFlatColourSamples = 1500;

// Where one field of a frame-flat buffer lies, and its levels.
struct FieldGeometry {
  VideoSystem system = VideoSystem::PAL;
  size_t samples_per_line = 0;  // nominal
  size_t first_line = 0;        // the field's first flat line in the frame
  size_t line_count = 0;        // the field's lines
  size_t picture_first = 0;     // field lines [picture_first, picture_end)
  size_t picture_end = 0;       // carry picture
  int32_t active_start = 0;     // samples [active_start, active_end) of each
  int32_t active_end = 0;       // picture line carry picture
  double black_level = 0.0;     // 0 IRE, in sample codes
  double white_level = 0.0;     // 100 IRE
};

// The two fields of a frame, with the picture area and levels of |params|.
std::array<FieldGeometry, 2> frame_fields(const FrameDescriptor& desc,
                                          const SourceParameters& params);

// The samples of flat saturated colour in a field's picture: chroma above
// about 10.6 IRE where the luma changes by less than about 4 IRE over 8
// samples.
size_t flat_colour_samples(const int16_t* frame, const FieldGeometry& field);

// The fitted beat of one field.
struct BeatFit {
  // The carrier offset from the one FieldBeat predicted with.
  double carrier_offset_hz = 0.0;
  // Per picture line, the amplitude subtracted (zero where the fit is not
  // significant or not physical).
  std::vector<std::complex<double>> amplitude;
  size_t lines_corrected = 0;
  // Sum of squares of what subtract() removes, in IRE^2.
  double removed_energy = 0.0;
};

// The predicted beat of one field, built once; fit() then fits it, at the
// carrier it was built with or offset from it.
class FieldBeat {
 public:
  FieldBeat(const int16_t* frame, const FieldGeometry& field,
            double carrier_black_hz);

  // Fits one amplitude per line, tracking the beat's phase from line to
  // line, over flat parts of the picture only. A line is corrected only
  // where the amplitude is well above its standard error and below the
  // largest a disc carries. |carrier_offset_hz| shifts the carrier: within
  // the band-pass the beat is then the predicted one times
  // e^{j 2 pi offset t}, to well under 0.1 % at the few tens of kHz a
  // master's carrier is off.
  BeatFit fit(double carrier_offset_hz = 0.0) const;

  // Writes the field's picture with |fit|'s beat removed into |frame_out|, a
  // frame-flat buffer laid out like the one the beat was built from. Lines
  // the fit does not correct, and every sample outside the picture, are not
  // written.
  void subtract(const BeatFit& fit, int16_t* frame_out) const;

 private:
  FieldGeometry field_;
  size_t width_ = 0;  // active samples a line
  // Per picture line, the field sample of its first active sample, and its
  // first active sample in the frame.
  std::vector<size_t> field_offset_;
  std::vector<size_t> frame_offset_;
  // Per picture sample, row-major: the predicted beat, the field band-passed
  // to the beat band, the fit weight and the subtraction mask.
  std::vector<std::complex<double>> basis_;
  std::vector<double> band_;
  std::vector<double> weight_;
  std::vector<double> mask_;
  std::vector<int16_t> input_;
  double codes_per_ire_ = 1.0;

  // One picture line's predicted beat, at the carrier offset by |offset_hz|.
  void line_basis(size_t line, double offset_hz,
                  std::vector<std::complex<double>>& out) const;
};

}  // namespace orc::fm_beat
