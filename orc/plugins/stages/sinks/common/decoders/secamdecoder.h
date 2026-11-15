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

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "componentframe.h"
#include "decoder.h"
#include "sourcefield.h"

// Constants used by the per-sample helpers that SecamDecoder defines inline
// below.  They live in the header so the helpers can be inlined into the
// decoder's sample loops: plugins are built with -fPIC and default symbol
// visibility, so GCC treats out-of-line member functions as interposable and
// calls them through the PLT even within secamdecoder.cpp.
namespace secam_decoder_constants {

// ITU-R BT.1700 Annex 1 Part C Table 4 item 5: undeviated subcarriers.
inline constexpr double kSecamForHz = 4406250.0;
inline constexpr double kSecamFobHz = 4250000.0;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 10e: nominal deviation for D* = 1.
inline constexpr double kSecamDeltaForHz = 280.0e3;
inline constexpr double kSecamDeltaFobHz = 230.0e3;

// Cross-colour suppression (see crossColourWeight): envelope ratio at which
// the fade-out starts and where it reaches zero, and the item 10e frequency
// range with its margin.
inline constexpr double kEnvelopeTolerance = 1.3;
inline constexpr double kEnvelopeLimit = 1.8;
inline constexpr double kAllowedLowHz = 3.900e6;
inline constexpr double kAllowedHighHz = 4.756e6;
inline constexpr double kFrequencyMarginHz = 100.0e3;

// Temporal chroma reconstruction (see temporalWeight): mean absolute luma
// difference, as a fraction of the luma range, below which the picture counts
// as still and above which it counts as moving.
inline constexpr double kStillLumaFraction = 0.02;
inline constexpr double kMovingLumaFraction = 0.06;

}  // namespace secam_decoder_constants

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
  // narrowChromaBand selects the tight cross-colour-reducing chroma band
  // (3.75–4.95 MHz, 81 taps); otherwise chroma uses the wide 3.3–5.4 MHz band
  // of the luma band-stop.
  explicit SecamLineDemodulator(double sampleRateHz,
                                bool narrowChromaBand = true);

  // Band-pass filters one line, returning the separated chroma.
  void separateChroma(const int16_t* input, int32_t count,
                      std::vector<double>& chroma) const;

  // Returns the luma of a composite line: the input minus a wider
  // (3.3–5.4 MHz) linear-phase band that holds the subcarrier and its
  // sidebands, so no subcarrier residue is left in the picture.
  void separateLuma(const int16_t* input, int32_t count,
                    std::vector<double>& luma) const;

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
  std::vector<double> lumaBandTaps_;
  std::vector<double> basebandTaps_;
  // Inverse bell biquad: y = b0 x + b1 x1 + b2 x2 − a1 y1 − a2 y2.
  double clocheB0_ = 0.0;
  double clocheB1_ = 0.0;
  double clocheB2_ = 0.0;
  double clocheA1_ = 0.0;
  double clocheA2_ = 0.0;

  mutable std::vector<double> deBelled_;
  mutable std::vector<double> lumaBand_;
  mutable std::vector<double> cosTable_;
  mutable std::vector<double> sinTable_;
  mutable std::vector<double> mixedI_;
  mutable std::vector<double> mixedQ_;
  mutable std::vector<double> mixI_;
  mutable std::vector<double> mixQ_;
};

// SECAM chroma decoder.  Its input is VideoSystem::SECAM, stored on the
// 625-line 4fsc lattice that PAL also uses.
//
// Per field it demodulates every line, takes which lines carry D'R and which
// D'B from the field's colour-frame observer id (the observer reads each
// line's undeviated back-porch sub-carrier, BT.1700 Part C Table 4 item 13,
// and the field identification lines 7–15 / 320–328), converts frequency to
// colour difference, suppresses cross-colour where the subcarrier envelope or
// frequency shows luma interference, de-emphasises and band-limits it, and
// fills the missing component of each line (line-sequential transmission)
// from the same line of the previous frame where the picture is still, and
// from the field lines above and below otherwise.
//
// Thread safety: not thread-safe.  Each worker thread must construct and own
// its own instance.
class SecamDecoder : public Decoder {
 public:
  struct SecamConfiguration {
    double chromaGain = 1.0;
    // Take the missing colour difference of each line from the same line of
    // the previous frame where the picture is still (motion-adaptive), so
    // horizontal colour edges stay put instead of moving by one line every
    // frame.  Needs one frame of look-behind.
    bool temporalChroma = true;
    // Reduce cross-colour from fine luma detail: tight chroma band-pass plus
    // fading out colour where the subcarrier envelope or frequency shows luma
    // interference.  Off (default) gives the plain wide band-pass decoder.
    bool crossColourSuppression = false;
  };

  explicit SecamDecoder(const SecamConfiguration& secamConfig);

  bool configure(const ::orc::SourceParameters& videoParameters) override;
  int32_t getLookBehind() const override;

  void decodeFrames(const std::vector<SourceField>& inputFields,
                    int32_t startIndex, int32_t endIndex,
                    std::vector<ComponentFrame>& componentFrames) override;

  // The line sequence of a field from its colour-frame observer
  // field_phase_id (SECAM: 1/2 for the fields of sequence frame 1, 3/4 for
  // frame 2).  Returns +1 when even field lines carry D'R, -1 when odd ones
  // do, and 0 when the id is unknown (the field is decoded without colour).
  static int32_t lineSequenceFromFieldPhase(std::optional<int32_t> fieldPhaseId,
                                            bool isFirstField);

  // Weight in [0, 1] applied to the colour difference of one sample to
  // suppress cross-colour.  It is zero when freqHz is outside the item 10e
  // deviation range (with margin), and fades out as the envelope departs
  // from typicalAmplitude by a ratio between 1.3 and 1.8 in either
  // direction; an intact FM subcarrier has a constant envelope.
  static double crossColourWeight(double amplitude, double freqHz,
                                  double typicalAmplitude);

  // Erodes (running minimum) and then smooths (running mean) a weight
  // vector over ±8 samples, so suppression covers the whole of a luma burst.
  // The two-argument form uses the caller's scratch buffer for the eroded
  // weights instead of allocating one.
  static void spreadWeight(std::vector<double>& weight);
  static void spreadWeight(std::vector<double>& weight,
                           std::vector<double>& eroded);

  // Blend weight of the temporal candidate for a given mean absolute luma
  // difference to the previous frame: 1 below 2 % of lumaRange (still), 0
  // above 6 % (motion), linear between.
  static double temporalWeight(double lumaDifference, double lumaRange);

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
  // Inverse low-frequency precorrection, first-order IIR.
  double deEmphB0_ = 0.0;
  double deEmphB1_ = 0.0;
  double deEmphA1_ = 0.0;
  std::vector<double> colourDifferenceTaps_;
  int32_t referenceStart_ = 0;
  int32_t referenceEnd_ = 0;
  double minimumAmplitude_ = 0.0;
  bool configurationValid_ = false;

  std::vector<double> chroma_;
  std::vector<double> freq_;
  std::vector<double> amp_;
  std::vector<double> envelopeSamples_;
  // decodeField() scratch, reused from field to field (per line, then per
  // field line index).
  std::vector<std::vector<double>> frequencies_;
  std::vector<std::vector<double>> amplitudes_;
  std::vector<double> referenceAmp_;
  std::vector<double> eroded_;
  std::vector<double> weight_;
  std::vector<double> filtered_;
  std::array<FieldChroma, 2> fields_;
  std::array<FieldChroma, 2> previousFields_;
  std::vector<double> motionPrefix_;
};

// The per-sample helpers are defined inline so the compiler can inline them
// into the sample loops (see secam_decoder_constants).

inline double SecamDecoder::frequencyToColourDifference(double freqHz,
                                                        bool drLine) {
  using secam_decoder_constants::kSecamDeltaFobHz;
  using secam_decoder_constants::kSecamDeltaForHz;
  using secam_decoder_constants::kSecamFobHz;
  using secam_decoder_constants::kSecamForHz;
  return drLine ? (freqHz - kSecamForHz) / kSecamDeltaForHz
                : (freqHz - kSecamFobHz) / kSecamDeltaFobHz;
}

inline double SecamDecoder::crossColourWeight(double amplitude, double freqHz,
                                              double typicalAmplitude) {
  using secam_decoder_constants::kAllowedHighHz;
  using secam_decoder_constants::kAllowedLowHz;
  using secam_decoder_constants::kEnvelopeLimit;
  using secam_decoder_constants::kEnvelopeTolerance;
  using secam_decoder_constants::kFrequencyMarginHz;
  if (freqHz < kAllowedLowHz - kFrequencyMarginHz ||
      freqHz > kAllowedHighHz + kFrequencyMarginHz) {
    return 0.0;
  }
  if (typicalAmplitude <= 0.0 || amplitude <= 0.0) {
    return 0.0;
  }
  double ratio = amplitude / typicalAmplitude;
  if (ratio < 1.0) {
    ratio = 1.0 / ratio;
  }
  return std::clamp(
      (kEnvelopeLimit - ratio) / (kEnvelopeLimit - kEnvelopeTolerance), 0.0,
      1.0);
}

inline double SecamDecoder::temporalWeight(double lumaDifference,
                                           double lumaRange) {
  using secam_decoder_constants::kMovingLumaFraction;
  using secam_decoder_constants::kStillLumaFraction;
  if (lumaRange <= 0.0) {
    return 0.0;
  }
  const double low = kStillLumaFraction * lumaRange;
  const double high = kMovingLumaFraction * lumaRange;
  return std::clamp((high - lumaDifference) / (high - low), 0.0, 1.0);
}

#endif  // SECAMDECODER_H
