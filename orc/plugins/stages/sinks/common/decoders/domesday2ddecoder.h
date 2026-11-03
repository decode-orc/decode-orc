/*
 * File:        domesday2ddecoder.h
 * Module:      orc-core
 * Purpose:     Domesday2D: a PAL still-frame decoder that separates each
 *              frame on its woven raster
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef DOMESDAY2DDECODER_H
#define DOMESDAY2DDECODER_H

#include <fftw3.h>
#include <orc/stage/orc_source_parameters.h>
#include <orc/support/domesday2d.h>

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

#include "componentframe.h"
#include "decoder.h"
#include "sourcefield.h"

// Domesday2D decodes each PAL frame from its own two fields, as a woven
// still (docs-tech/domesday2d-decoder-investigation.md, Phases 3 and 4):
//
//   1. Transform PAL on the frame raster: Hann-windowed tiles of 32 rows x
//      32 samples, 50 % overlap-add, every bin of 0.5-1.5 fsc tested as U
//      (y -> 3H/4 - y) and as V (y -> H/4 - y) and kept whole when the
//      better-agreeing pair is within the threshold. In the frame U and V
//      answer to different reflections, so a sharp colour edge no longer
//      throws chroma into luma (transform2d's "||||" dots).
//   2. Per-row demodulation on the burst-fitted lattice and PalColour's
//      horizontal raised cosine.
//   3. The per-source line-alternating correction (I + s M)^-1, when M is
//      given.
//   4. Masked field-line averaging, [1/4, 1/2, 1/4] over rows r +/- 2,
//      where those rows agree and the 312-line partner deviates the same
//      way (a Hanover error repeats on the partner; a one-line stripe does
//      not).
//   5. An edge-preserving vertical fit along each column: minimise
//      |W - W0|^2 + sum lambda |W(r+1) - W(r)|_eps (vector Charbonnier) by
//      six reweighted tridiagonal solves, lambda rising with the 3.5-5 MHz
//      luma detail the transform left.
//   6. The chroma the fit does not explain handed back to luma, shrunk
//      against the chroma-band noise of the blanked lines and gated by the
//      luma detail.
//
// tools/domesday2d-bench/ (transform.py, adaptive.py, hanover.py) is the
// prototype it matches. Each frame needs only its own fields:
// getLookBehind() and getLookAhead() are 0.
//
// Thread safety: not thread-safe. Each worker thread constructs and owns its
// own instance; the FFTW plans are made under TransformPal's planner lock.
class Domesday2DDecoder : public Decoder {
 public:
  struct Configuration {
    double chromaGain = 1.0;
    double chromaPhase = 0.0;
    double transformThreshold = 0.4;
    // The source's line-alternating matrix M (orc/support/domesday2d.h);
    // all zero leaves the rows uncorrected.
    orc::domesday2d::Matrix hanover{0.0, 0.0, 0.0, 0.0};
  };

  explicit Domesday2DDecoder(const Configuration& config);
  ~Domesday2DDecoder() override;
  Domesday2DDecoder(const Domesday2DDecoder&) = delete;
  Domesday2DDecoder& operator=(const Domesday2DDecoder&) = delete;

  bool configure(const ::orc::SourceParameters& videoParameters) override;

  void decodeFrames(const std::vector<SourceField>& inputFields,
                    int32_t startIndex, int32_t endIndex,
                    std::vector<ComponentFrame>& componentFrames) override;

  // The decoder's planes for one frame, on the frame raster, in units of
  // white - blanking: for tests that compare it with the prototype.
  struct Planes {
    orc::domesday2d::Plane y;
    orc::domesday2d::Plane u;
    orc::domesday2d::Plane v;
  };
  void decodeRaster(const orc::domesday2d::Plane& raster, Planes& out);

 private:
  static constexpr int32_t kTile = 32;
  static constexpr int32_t kHalfTile = kTile / 2;
  static constexpr int32_t kComplexX = (kTile / 2) + 1;
  // The detail band-pass's convolution length: a row and the taps' reach.
  static constexpr size_t kDetailSize = 1280;
  static constexpr size_t kDetailComplex = (kDetailSize / 2) + 1;

  // Step 1: the chroma the transform finds in the raster's picture.
  void separate(const orc::domesday2d::Plane& raster,
                orc::domesday2d::Plane& chroma);
  void filterTile();
  // The luma detail's 3.5-5 MHz band-pass along one row, |x| to |y|.
  void detailBand(const double* x, double* y);

  void decodeFrame(const SourceField& first, const SourceField& second,
                   ComponentFrame& frame);

  // Planes reused from frame to frame: each is 5.7 MB, and allocating them
  // afresh costs page faults on every frame. The steps after the transform
  // go row by row where they can, through the row buffers.
  struct Workspace {
    orc::domesday2d::Plane raster;
    orc::domesday2d::Plane chroma;
    orc::domesday2d::Plane luma;
    orc::domesday2d::Plane detail;
    orc::domesday2d::Plane across;
    orc::domesday2d::Plane u;
    orc::domesday2d::Plane v;
    orc::domesday2d::Plane residual;
    std::vector<double> row_a;
    std::vector<double> row_b;
    std::vector<double> rows;
    Planes planes;
  };

  Configuration config_;
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
};

#endif  // DOMESDAY2DDECODER_H
