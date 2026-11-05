/*
 * File:        palstilldecoder.h
 * Module:      orc-core
 * Purpose:     PAL Still: a PAL still-frame decoder that separates each
 *              frame on its woven raster
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef PALSTILLDECODER_H
#define PALSTILLDECODER_H

#include <fftw3.h>
#include <orc/stage/orc_source_parameters.h>

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

#include "componentframe.h"
#include "decoder.h"
#include "pal_still.h"
#include "sourcefield.h"

// PAL Still decodes each PAL frame from its own two fields, as a woven
// still. It is for single-frame stills (photographs, maps, text pages): a
// picture that does not move between its fields, with unrelated pictures
// in the frames either side. In the woven frame the carrier of each line
// sits between the other field's lines, and U and V fall in different
// places in the frame's spectrum, so colour and fine luma detail separate
// far more cleanly there than within one field.
//
//   1. Transform PAL on the frame raster: Hann-windowed tiles of 32 rows x
//      32 samples, 50 % overlap-add, every bin of 0.5-1.5 fsc tested as U
//      (y -> 3H/4 - y) and as V (y -> H/4 - y) and kept whole when the
//      better-agreeing pair is within the threshold. In the frame U and V
//      answer to different reflections, so a sharp colour edge no longer
//      throws chroma into luma (transform2d's "||||" dots).
//   2. Per-row demodulation on the burst-fitted lattice and PalColour's
//      horizontal raised cosine.
//   3. The line-alternating (Hanover) correction (I + s M)^-1, s the
//      row's V-switch sign and M a rotation fitted to each frame's own
//      flat colour, the 312-line partner checked (pal_still.h). On the
//      Domesday discs the rotation is mastered into each still and changes
//      from still to still, so it is fitted per frame; a frame with too
//      little flat colour takes chromaPhase's turn of the two switch states
//      alone. A frame without a burst (monochrome) skips steps 1-6: no U or
//      V, and the composite is its luma.
//   4. Masked field-line averaging, [1/4, 1/2, 1/4] over rows r +/- 2,
//      where those rows agree and the 312-line partner deviates the same
//      way (a Hanover error repeats on the partner; a one-line stripe does
//      not).
//   5. An edge-preserving vertical fit along each column: minimise
//      |W - W0|^2 + sum lambda |W(r+1) - W(r)|_eps (vector Charbonnier) by
//      six reweighted tridiagonal solves, lambda rising with the blanked
//      lines' noise and the 3.5-5 MHz luma detail the transform left. It
//      keeps colour edges sharp where the picture is quiet and smooths
//      where fine luma could leak into the colour. The fit runs in single
//      precision, built without fused multiply-adds so every platform gives
//      the same codes.
//   6. The chroma the fit does not explain handed back to luma, shrunk
//      against the chroma-band noise of the blanked lines and gated by the
//      luma detail. This sharpens map lines and lettering.
//
// The constants, and the data they were chosen on, are documented where
// they are defined in palstilldecoder.cpp. Each frame needs only its own
// fields: getLookBehind() and getLookAhead() are 0.
//
// Known limits:
//   - Motion between the two fields is combed: a pan of 2 samples a field
//     costs little, by 8 it is no better than transform2d, by 24 it is the
//     worst of the 2D decoders. Film is not combed but is moving footage.
//   - A picture held over many frames is better decoded by transform3d,
//     which combines the copies.
//   - On a stacked, low-noise source it trails transform2d by about 0.2
//     Delta E on dense maps and on photographs with fine luma detail; on a
//     single capture it leads on both.
//   - A held test signal with its own symmetric U/V leak (colour bars) is
//     left with that leak: only the rotation is corrected.
//   - Cost: about 2.6 times transform2d's decode time (43.9 ms a frame
//     against 17.0 on one x86-64 core) and about 57 MB more per worker.
//
// Thread safety: not thread-safe. Each worker thread constructs and owns its
// own instance; the FFTW plans are made under TransformPal's planner lock.
class PalStillDecoder : public Decoder {
 public:
  struct Configuration {
    double chromaGain = 1.0;
    double chromaPhase = 0.0;
    double transformThreshold = 0.4;
  };

  explicit PalStillDecoder(const Configuration& config);
  ~PalStillDecoder() override;
  PalStillDecoder(const PalStillDecoder&) = delete;
  PalStillDecoder& operator=(const PalStillDecoder&) = delete;

  bool configure(const ::orc::SourceParameters& videoParameters) override;

  void decodeFrames(const std::vector<SourceField>& inputFields,
                    int32_t startIndex, int32_t endIndex,
                    std::vector<ComponentFrame>& componentFrames) override;

  // The decoder's planes for one frame, on the frame raster, in units of
  // white - blanking: for tests that check the decode before it is scaled
  // into a ComponentFrame.
  struct Planes {
    orc::pal_still::Plane y;
    orc::pal_still::Plane u;
    orc::pal_still::Plane v;
  };
  void decodeRaster(const orc::pal_still::Plane& raster, Planes& out);

 private:
  static constexpr int32_t kTile = 32;
  static constexpr int32_t kHalfTile = kTile / 2;
  static constexpr int32_t kComplexX = (kTile / 2) + 1;
  // The detail band-pass's convolution length: a row and the taps' reach.
  static constexpr size_t kDetailSize = 1280;
  static constexpr size_t kDetailComplex = (kDetailSize / 2) + 1;

  // Step 1: the chroma the transform finds in the raster's picture.
  void separate(const orc::pal_still::Plane& raster,
                orc::pal_still::Plane& chroma);
  void filterTile();
  // The luma detail's band-passes along one row, |x| to |high| (3.5-5 MHz)
  // and |low| (2.5-3.2 MHz).
  void detailBand(const double* x, double* high, double* low);

  void decodeFrame(const SourceField& first, const SourceField& second,
                   ComponentFrame& frame);

  // Planes reused from frame to frame: each is 5.7 MB, and allocating them
  // afresh costs page faults on every frame. The steps after the transform
  // go row by row where they can, through the row buffers.
  struct Workspace {
    orc::pal_still::Plane raster;
    orc::pal_still::Plane chroma;
    orc::pal_still::Plane luma;
    orc::pal_still::Plane detail;
    orc::pal_still::Plane high;
    orc::pal_still::Plane across;
    orc::pal_still::Plane u;
    orc::pal_still::Plane v;
    orc::pal_still::Plane residual;
    std::vector<double> row_a;
    std::vector<double> row_b;
    std::vector<double> rows;
    orc::pal_still::FlatPairs pairs;
    Planes planes;
  };

  // The matrix |lattice|'s frame is corrected with, from its demodulated
  // planes in work_.u / work_.v.
  orc::pal_still::Matrix frameMatrix(const orc::pal_still::Lattice& lattice);

  Configuration config_;
  // The line-alternating matrix that demodulating at config_.chromaPhase
  // alone puts on the rows: a rotation of tan(chromaPhase), zero near
  // +/-90 degrees.
  orc::pal_still::Matrix phaseTurn_{0.0, 0.0, 0.0, 0.0};
  Workspace work_;
  ::orc::SourceParameters videoParameters_;
  bool configurationValid_ = false;

  std::array<std::array<double, kTile>, kTile> window_{};
  double* fftReal_ = nullptr;
  fftw_complex* fftComplex_ = nullptr;
  fftw_plan forwardPlan_ = nullptr;
  fftw_plan inversePlan_ = nullptr;
  double* detailReal_ = nullptr;
  fftw_complex* detailComplex_ = nullptr;
  fftw_plan detailForward_ = nullptr;
  fftw_plan detailInverse_ = nullptr;
  std::vector<std::complex<double>> detailKernel_;
  std::vector<std::complex<double>> belowChromaKernel_;
  // A row's spectrum, kept while each band is taken from it.
  std::vector<std::complex<double>> detailSpectrum_;
};

#endif  // PALSTILLDECODER_H
