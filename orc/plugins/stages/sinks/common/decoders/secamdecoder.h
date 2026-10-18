/*
 * File:        secamdecoder.h
 * Module:      orc-core
 * Purpose:     625-line SECAM frequency-modulated chroma decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#ifndef SECAMDECODER_H
#define SECAMDECODER_H

#include <orc/stage/orc_source_parameters.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "componentframe.h"
#include "decoder.h"
#include "monodecoder.h"
#include "sourcefield.h"

// Per-line SECAM chroma demodulator.
//
// Implements the receiver half of ITU-R BT.1700 Annex 1 Part C Table 4 on one
// line of samples:
//   1. Chroma band-pass around the two subcarriers (f_OB = 4.25 MHz,
//      f_OR = 4.40625 MHz, deviations up to ±506 kHz).
//   2. Inverse of the item 10d high-frequency precorrection ("cloche"
//      filter), (1 + j1.26F) / (1 + j16F), F = f/f0 − f0/f, f0 = 4.286 MHz.
//   3. Quadrature FM demodulation to instantaneous frequency and envelope.
//
// The resulting frequency is converted to D'R / D'B by SecamDecoder once the
// line's component is known, and de-emphasised there (inverse of item 10f).
//
// Thread safety: not thread-safe; holds per-line scratch buffers.  Construct
// one instance per worker thread.
class SecamLineDemodulator {
 public:
  explicit SecamLineDemodulator(double sampleRateHz);

  // Band-pass filters one line, returning the separated chroma.  The filter
  // is linear-phase and zero-delay, so composite − chroma is the luma.
  void separateChroma(const int16_t* input, int32_t count,
                      std::vector<double>& chroma) const;

  // De-bells and FM-demodulates separated chroma.  freqHz receives the
  // instantaneous frequency and amplitude the subcarrier envelope, both one
  // value per input sample.
  void demodulate(const std::vector<double>& chroma,
                  std::vector<double>& freqHz,
                  std::vector<double>& amplitude) const;

  double sampleRateHz() const { return sampleRateHz_; }

 private:
  double sampleRateHz_;
  std::vector<double> bandPassTaps_;
  std::vector<double> basebandTaps_;
  // Inverse bell biquad: y = b0 x + b1 x1 + b2 x2 − a1 y1 − a2 y2.
  double clocheB0_ = 0.0;
  double clocheB1_ = 0.0;
  double clocheB2_ = 0.0;
  double clocheA1_ = 0.0;
  double clocheA2_ = 0.0;

  mutable std::vector<double> deBelled_;
  mutable std::vector<double> cosTable_;
  mutable std::vector<double> sinTable_;
  mutable std::vector<double> mixedI_;
  mutable std::vector<double> mixedQ_;
  mutable std::vector<double> mixI_;
  mutable std::vector<double> mixQ_;
};

// SECAM chroma decoder for 625-line sources carried on the PAL 4fsc lattice
// (the CVBS file format and ld-decode family TBCs have no SECAM system and
// store SECAM as PAL).
//
// Per field it demodulates every line, identifies which lines carry D'R and
// which D'B from the undeviated subcarrier on the back porch (BT.1700 Part C
// Table 4 item 13, line identification), converts frequency to colour
// difference, de-emphasises it, and fills the missing component of each line
// from the field lines above and below (line-sequential transmission).
//
// Thread safety: not thread-safe.  Each worker thread must construct and own
// its own instance.
class SecamDecoder : public Decoder {
 public:
  struct SecamConfiguration {
    double chromaGain = 1.0;
    double yNRLevel = 0.0;
  };

  explicit SecamDecoder(const SecamConfiguration& secamConfig);

  bool configure(const ::orc::SourceParameters& videoParameters) override;

  void decodeFrames(const std::vector<SourceField>& inputFields,
                    int32_t startIndex, int32_t endIndex,
                    std::vector<ComponentFrame>& componentFrames) override;

  // Decides the line sequence of a field from the per-line back-porch
  // reference frequency.  Lines whose reference amplitude is below
  // minimumAmplitude are ignored.  Returns +1 when even field lines carry
  // D'R, −1 when odd field lines do, and 0 when the field has too few valid
  // reference lines to decide (no SECAM colour present).
  static int32_t detectLineSequence(const std::vector<double>& referenceHz,
                                    const std::vector<double>& referenceAmp,
                                    double minimumAmplitude);

  // Converts an instantaneous subcarrier frequency into the precorrected
  // colour difference D* of a D'R (drLine) or D'B line (item 10e).
  static double frequencyToColourDifference(double freqHz, bool drLine);

 private:
  struct FieldChroma {
    // Per field line: true for D'R, false for D'B.
    std::vector<bool> isDr;
    // Per field line: de-emphasised D'R or D'B over the full line, or empty
    // when the line carries no usable chroma.
    std::vector<std::vector<double>> colourDifference;
    // Per field line luma (composite − chroma, or the Y/C luma channel).
    std::vector<std::vector<double>> luma;
  };

  void decodeField(const SourceField& field, FieldChroma& out);
  void deEmphasise(std::vector<double>& colourDifference, int32_t start) const;

  SecamConfiguration secamConfig_;
  ::orc::SourceParameters videoParameters_;
  std::unique_ptr<SecamLineDemodulator> demodulator_;
  std::unique_ptr<MonoDecoder> ynrHelper_;
  // Inverse low-frequency precorrection, first-order IIR.
  double deEmphB0_ = 0.0;
  double deEmphB1_ = 0.0;
  double deEmphA1_ = 0.0;
  int32_t referenceStart_ = 0;
  int32_t referenceEnd_ = 0;
  double minimumAmplitude_ = 0.0;
  bool configurationValid_ = false;

  std::vector<double> chroma_;
  std::vector<double> freq_;
  std::vector<double> amp_;
  std::array<FieldChroma, 2> fields_;
};

#endif  // SECAMDECODER_H
