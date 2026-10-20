/*
 * File:        secamdecoder.cpp
 * Module:      orc-core
 * Purpose:     625-line SECAM frequency-modulated chroma decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "secamdecoder.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <utility>

#include "../video_parameter_safety.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// Constants shared with the inline per-sample helpers in secamdecoder.h.
using secam_decoder_constants::kAllowedHighHz;
using secam_decoder_constants::kAllowedLowHz;
using secam_decoder_constants::kEnvelopeLimit;
using secam_decoder_constants::kEnvelopeTolerance;
using secam_decoder_constants::kFrequencyMarginHz;
using secam_decoder_constants::kMovingLumaFraction;
using secam_decoder_constants::kSecamDeltaFobHz;
using secam_decoder_constants::kSecamDeltaForHz;
using secam_decoder_constants::kSecamFobHz;
using secam_decoder_constants::kSecamForHz;
using secam_decoder_constants::kStillLumaFraction;

// Midpoint between f_OB and f_OR, used as the demodulator mixing frequency and
// as the D'R / D'B decision threshold for the back-porch reference.
constexpr double kSecamMidHz = 0.5 * (kSecamForHz + kSecamFobHz);

// ITU-R BT.1700 Annex 1 Part C Table 4 item 9: D'R = −1.902 (E'R − E'Y),
// D'B = 1.505 (E'B − E'Y).
constexpr double kSecamDrPerRMinusY = -1.902;
constexpr double kSecamDbPerBMinusY = 1.505;

// ITU-R BT.601-7: U = kB (B'−Y'), V = kR (R'−Y'), the ComponentFrame axes.
constexpr double kB = 0.49211104112248356308804691718185;
constexpr double kR = 0.87728321993817866838972487283129;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 10f: low-frequency precorrection
// corner f1 = 85 kHz, high-frequency asymptote at 3 × f1.
constexpr double kSecamLfF1Hz = 85.0e3;
constexpr double kSecamLfRatio = 3.0;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 10d: bell filter centre and the
// numerator/denominator coefficients of (1 + j16F) / (1 + j1.26F).
constexpr double kSecamBellCentreHz = 4286.0e3;
constexpr double kSecamBellNumeratorK = 16.0;
constexpr double kSecamBellDenominatorK = 1.26;

// ITU-R BT.1700 Annex 1 Part C Table 5 item 6: subcarrier peak-to-peak
// amplitude 2·M0 = 23 % of the blanking-to-white luminance amplitude.
constexpr double kSecamM0PerLumaAmplitude = 0.23 / 2.0;
// Envelope below which a sample is treated as carrying no subcarrier (colour
// killer / dropout); a fraction of M0 after the cloche filter restores M0.
constexpr double kSecamKillFractionOfM0 = 0.2;

// Chroma and luma are separated with two different filters, because they
// fail in opposite ways:
//
// - The chroma band-pass is kept tight around the band the subcarrier can
//   actually occupy (item 10e limits it to 3.900–4.756 MHz on both line
//   types), with steep 81-tap skirts, so as little luma detail as possible
//   reaches the FM demodulator, where it would become cross-colour.
// - The luma band-stop is wider (3.3–5.4 MHz) and gentler, so the subcarrier
//   and its sidebands are removed from the luma completely; luma is the
//   composite minus that band.
constexpr double kChromaBandLowHz = 3.75e6;
constexpr double kChromaBandHighHz = 4.95e6;
constexpr int32_t kChromaBandTaps = 81;
// With cross-colour suppression switched off, chroma uses the same wide,
// gentle band as the luma band-stop instead.
constexpr double kLumaStopLowHz = 3.3e6;
constexpr double kLumaStopHighHz = 5.4e6;
constexpr int32_t kLumaStopTaps = 49;
constexpr double kBasebandCutoffHz = 1.4e6;
constexpr int32_t kBasebandTaps = 33;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 10c: the colour-difference signal
// is band-limited to about 1.3 MHz before modulation, so anything faster in
// the demodulated D is noise or luma beat products.
constexpr double kColourDifferenceCutoffHz = 1.3e6;
constexpr int32_t kColourDifferenceTaps = 33;

// Cross-colour suppression.  The FM subcarrier has a constant envelope once
// the cloche has undone the bell, and item 10e confines its frequency to
// 3.900–4.756 MHz.  Luma detail inside the chroma band breaks both: it adds
// to or cancels the carrier, and drags the measured frequency outside the
// allowed range.  Where the envelope departs from the field's typical
// subcarrier amplitude by more than kEnvelopeTolerance (as a ratio either
// way), the colour difference is faded out, reaching zero at kEnvelopeLimit;
// where the frequency leaves the allowed range by more than
// kFrequencyMarginHz it is removed (constants in secamdecoder.h).  The weight
// is then eroded and smoothed over ±kWeightSpreadSamples (~0.45 µs) so the
// beat pattern cannot leak through the gaps.
constexpr int32_t kWeightSpreadSamples = 8;
// Temporal chroma reconstruction: the mean absolute luma difference to the
// previous frame over ±kMotionWindowSamples feeds SecamDecoder::temporalWeight
// (thresholds in secamdecoder.h).
constexpr int32_t kMotionWindowSamples = 8;

// Sub-sampling step when gathering envelope values for the field median.
constexpr int32_t kEnvelopeSampleStep = 8;

// ITU-R BT.470-6 Table 2 item 2.17a: the subcarrier is present from 5.6 µs
// after 0H.  The back-porch reference is measured from 6.0 µs, once the gate
// edge and the demodulator filters have settled, to 7.9 µs, before picture
// chroma spread by the filters can reach it (active picture from ~9 µs).
constexpr double kReferenceStartSeconds = 6.0e-6;
constexpr double kReferenceEndSeconds = 7.9e-6;
// The reference must lie between the two rest frequencies ± this tolerance to
// be counted (BT.1700 Part C item 5 tolerance is ±2 kHz; real sources drift).
constexpr double kReferenceToleranceHz = 120.0e3;
// Minimum number of valid reference lines per field to decide the sequence.
constexpr int32_t kMinimumReferenceLines = 8;
// Minimum fraction of valid reference lines that must agree on the sequence.
constexpr double kMinimumSequenceConsistency = 0.6;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 13 / Figure 16: field
// identification signals on frame lines 7–15 (fields 1 and 3) and 320–328
// (fields 2 and 4).  A first field holds frame lines 1–313 and a second field
// starts at frame line 314, so both map to field lines 6–14 (0-based).
constexpr int32_t kFieldIdFirstLine = 6;
constexpr int32_t kFieldIdLastLine = 14;
// Figure 17: the trapezoid reaches its final level 15 µs (D'R) or 18 µs
// (D'B, up to 30 µs with tolerance) after the end of line blanking at
// 10.5 µs; the settled deviation (+350 kHz on D'R lines, −350 kHz on D'B
// lines, BT.470-6 item 2.18) is measured from 42 µs to 58 µs.
constexpr double kFieldIdMeasureStartSeconds = 42.0e-6;
constexpr double kFieldIdMeasureEndSeconds = 58.0e-6;
// The ramp start is measured just after line blanking, from 11 µs to 13 µs,
// where the trapezoid is still within ~60 kHz of the rest frequency.
constexpr double kFieldIdRampStartSeconds = 11.0e-6;
constexpr double kFieldIdRampEndSeconds = 13.0e-6;
// An identification line must show the trapezoid's shape, not just a large
// deviation, so that picture or data content on lines 7–15 cannot pose as
// one: its ramp starts within kFieldIdRestToleranceHz of the line's rest
// frequency and settles kFieldIdMinimumSwingHz to kFieldIdMaximumSwingHz
// away from it in the line type's direction (nominal ±350 kHz).
constexpr double kFieldIdRestToleranceHz = 120.0e3;
constexpr double kFieldIdMinimumSwingHz = 200.0e3;
constexpr double kFieldIdMaximumSwingHz = 500.0e3;
// Minimum number of identification lines that must decode to decide.
constexpr int32_t kMinimumFieldIdLines = 3;

// Hamming-windowed sinc low-pass with unity DC gain.
std::vector<double> designLowPass(double cutoffHz, double sampleRateHz,
                                  int32_t taps) {
  std::vector<double> kernel(static_cast<size_t>(taps));
  const double nc = cutoffHz / sampleRateHz;
  const int32_t half = taps / 2;
  double sum = 0.0;
  for (int32_t i = 0; i < taps; ++i) {
    const int32_t k = i - half;
    const double sinc =
        (k == 0) ? 2.0 * nc : std::sin(kTwoPi * nc * k) / (kPi * k);
    const double window =
        0.54 - 0.46 * std::cos(kTwoPi * i / static_cast<double>(taps - 1));
    kernel[static_cast<size_t>(i)] = sinc * window;
    sum += kernel[static_cast<size_t>(i)];
  }
  for (double& tap : kernel) {
    tap /= sum;
  }
  return kernel;
}

// Zero-phase FIR convolution with zero padding outside [0, count).
template <typename Sample>
void convolve(const Sample* input, int32_t count,
              const std::vector<double>& taps, std::vector<double>& output) {
  const int32_t half = static_cast<int32_t>(taps.size()) / 2;
  output.assign(static_cast<size_t>(count), 0.0);
  for (int32_t n = 0; n < count; ++n) {
    double acc = 0.0;
    const int32_t first = std::max(0, half - n);
    const int32_t last =
        std::min(static_cast<int32_t>(taps.size()), count - n + half);
    for (int32_t t = first; t < last; ++t) {
      acc += taps[static_cast<size_t>(t)] *
             static_cast<double>(input[n + t - half]);
    }
    output[static_cast<size_t>(n)] = acc;
  }
}

}  // namespace

SecamLineDemodulator::SecamLineDemodulator(double sampleRateHz,
                                           bool narrowChromaBand)
    : sampleRateHz_(sampleRateHz) {
  const double bandLowHz = narrowChromaBand ? kChromaBandLowHz : kLumaStopLowHz;
  const double bandHighHz =
      narrowChromaBand ? kChromaBandHighHz : kLumaStopHighHz;
  const int32_t bandTaps = narrowChromaBand ? kChromaBandTaps : kLumaStopTaps;
  // Band-pass as the difference of two low-pass kernels.
  const std::vector<double> high =
      designLowPass(bandHighHz, sampleRateHz, bandTaps);
  const std::vector<double> low =
      designLowPass(bandLowHz, sampleRateHz, bandTaps);
  bandPassTaps_.resize(high.size());
  for (size_t i = 0; i < high.size(); ++i) {
    bandPassTaps_[i] = high[i] - low[i];
  }
  const std::vector<double> stopHigh =
      designLowPass(kLumaStopHighHz, sampleRateHz, kLumaStopTaps);
  const std::vector<double> stopLow =
      designLowPass(kLumaStopLowHz, sampleRateHz, kLumaStopTaps);
  lumaBandTaps_.resize(stopHigh.size());
  for (size_t i = 0; i < stopHigh.size(); ++i) {
    lumaBandTaps_[i] = stopHigh[i] - stopLow[i];
  }
  basebandTaps_ = designLowPass(kBasebandCutoffHz, sampleRateHz, kBasebandTaps);

  // Cloche: the exact inverse of the bell H(s) = (16 s² + w0 s + 16 w0²) /
  // (1.26 s² + w0 s + 1.26 w0²), discretised by the matched-z transform
  // (z = e^{sT}), which keeps the frequency and damping of the sharp Q = 16
  // resonance near fs/4 where the bilinear transform would widen it ~1.5×.
  // Normalised to unity gain at f0.
  const double w0 = kTwoPi * kSecamBellCentreHz;
  const double period = 1.0 / sampleRateHz;
  const auto matchedPair = [&](double c) {
    const std::complex<double> root =
        std::complex<double>(-w0, w0 * std::sqrt((4.0 * c * c) - 1.0)) /
        (2.0 * c);
    const std::complex<double> z = std::exp(root * period);
    return std::array<double, 3>{1.0, -2.0 * z.real(), std::norm(z)};
  };
  const std::array<double, 3> num = matchedPair(kSecamBellDenominatorK);
  const std::array<double, 3> den = matchedPair(kSecamBellNumeratorK);
  const std::complex<double> zInv =
      std::exp(std::complex<double>(0.0, -w0 * period));
  const std::complex<double> atF0 =
      (num[0] + num[1] * zInv + num[2] * zInv * zInv) /
      (den[0] + den[1] * zInv + den[2] * zInv * zInv);
  const double gain = 1.0 / std::abs(atF0);
  clocheB0_ = gain * num[0];
  clocheB1_ = gain * num[1];
  clocheB2_ = gain * num[2];
  clocheA1_ = den[1];
  clocheA2_ = den[2];
}

void SecamLineDemodulator::separateChroma(const int16_t* input, int32_t count,
                                          std::vector<double>& chroma) const {
  convolve(input, count, bandPassTaps_, chroma);
}

void SecamLineDemodulator::separateLuma(const int16_t* input, int32_t count,
                                        std::vector<double>& luma) const {
  convolve(input, count, lumaBandTaps_, lumaBand_);
  luma.resize(static_cast<size_t>(count));
  for (int32_t x = 0; x < count; ++x) {
    luma[static_cast<size_t>(x)] = input[x] - lumaBand_[static_cast<size_t>(x)];
  }
}

void SecamLineDemodulator::demodulate(const std::vector<double>& chroma,
                                      std::vector<double>& freqHz,
                                      std::vector<double>& amplitude) const {
  const int32_t count = static_cast<int32_t>(chroma.size());
  deBelled_.assign(chroma.size(), 0.0);
  double x1 = 0.0;
  double x2 = 0.0;
  double y1 = 0.0;
  double y2 = 0.0;
  for (int32_t n = 0; n < count; ++n) {
    const double x0 = chroma[static_cast<size_t>(n)];
    const double y0 = clocheB0_ * x0 + clocheB1_ * x1 + clocheB2_ * x2 -
                      clocheA1_ * y1 - clocheA2_ * y2;
    deBelled_[static_cast<size_t>(n)] = y0;
    x2 = x1;
    x1 = x0;
    y2 = y1;
    y1 = y0;
  }

  // Mix to complex baseband around the midpoint of the two rest frequencies
  // and low-pass; the phase advance between samples is the instantaneous
  // frequency offset.
  if (cosTable_.size() < chroma.size()) {
    const double omega = kTwoPi * kSecamMidHz / sampleRateHz_;
    cosTable_.resize(chroma.size());
    sinTable_.resize(chroma.size());
    for (size_t n = 0; n < chroma.size(); ++n) {
      cosTable_[n] = std::cos(omega * static_cast<double>(n));
      sinTable_[n] = std::sin(omega * static_cast<double>(n));
    }
  }
  mixedI_.resize(chroma.size());
  mixedQ_.resize(chroma.size());
  for (size_t n = 0; n < chroma.size(); ++n) {
    mixedI_[n] = deBelled_[n] * cosTable_[n];
    mixedQ_[n] = -deBelled_[n] * sinTable_[n];
  }
  convolve(mixedI_.data(), count, basebandTaps_, mixI_);
  convolve(mixedQ_.data(), count, basebandTaps_, mixQ_);

  freqHz.assign(chroma.size(), kSecamMidHz);
  amplitude.assign(chroma.size(), 0.0);
  for (int32_t n = 0; n < count; ++n) {
    const std::complex<double> z(mixI_[static_cast<size_t>(n)],
                                 mixQ_[static_cast<size_t>(n)]);
    // The mixer halves the amplitude.
    amplitude[static_cast<size_t>(n)] = 2.0 * std::abs(z);
    if (n > 0) {
      const std::complex<double> previous(mixI_[static_cast<size_t>(n - 1)],
                                          mixQ_[static_cast<size_t>(n - 1)]);
      const double advance = std::arg(z * std::conj(previous));
      freqHz[static_cast<size_t>(n)] =
          kSecamMidHz + advance * sampleRateHz_ / kTwoPi;
    }
  }
  if (count > 1) {
    freqHz[0] = freqHz[1];
  }
}

SecamDecoder::SecamDecoder(const SecamConfiguration& secamConfig)
    : secamConfig_(secamConfig) {}

bool SecamDecoder::configure(const ::orc::SourceParameters& videoParameters) {
  configurationValid_ = false;

  // SECAM shares the 625-line, 50 Hz structure and is stored on the PAL 4fsc
  // lattice; 525-line PAL-M cannot carry it.
  if (videoParameters.system != orc::VideoSystem::PAL) {
    ORC_LOG_ERROR(
        "The SECAM decoder is for 625-line sources stored as PAL only");
    return false;
  }

  const auto safety = ::orc::chroma_sink::sanitize_video_parameters(
      videoParameters, ::orc::chroma_sink::DecoderVideoProfile::PalColour);
  if (!safety.warnings.empty()) {
    ORC_LOG_WARN(
        "SecamDecoder::configure(): Adjusted unsafe video parameters: {}",
        ::orc::chroma_sink::join_issues(safety.warnings));
  }
  if (!safety.ok) {
    ORC_LOG_ERROR("SecamDecoder::configure(): Invalid video parameters: {}",
                  ::orc::chroma_sink::join_issues(safety.errors));
    return false;
  }
  videoParameters_ = safety.params;
  // Sources that do not report levels use the EBU Tech. 3280-E 10-bit PAL
  // levels, which SECAM shares (ITU-R BT.1700 Annex 1 Part C Table 5).
  if (videoParameters_.blanking_level < 0) {
    videoParameters_.blanking_level = orc::kPalBlanking;
  }
  if (videoParameters_.black_level < 0) {
    videoParameters_.black_level = orc::kPalBlack;
  }
  if (videoParameters_.white_level <= videoParameters_.black_level) {
    videoParameters_.white_level = orc::kPalWhite;
  }

  const double sampleRate = orc::sample_rate_from_system(orc::VideoSystem::PAL);
  demodulator_ = std::make_unique<SecamLineDemodulator>(
      sampleRate, secamConfig_.crossColourSuppression);

  // Inverse of the item 10f precorrection, H(s) = (1 + s/(3·w1)) / (1 + s/w1),
  // bilinear transform pre-warped at f1.
  const double w1 = kTwoPi * kSecamLfF1Hz;
  const double k = w1 / std::tan(w1 / (2.0 * sampleRate));
  const double numS = 1.0 / (kSecamLfRatio * w1);
  const double denS = 1.0 / w1;
  const double a0 = denS * k + 1.0;
  deEmphB0_ = (numS * k + 1.0) / a0;
  deEmphB1_ = (1.0 - numS * k) / a0;
  deEmphA1_ = (1.0 - denS * k) / a0;

  colourDifferenceTaps_ = designLowPass(kColourDifferenceCutoffHz, sampleRate,
                                        kColourDifferenceTaps);

  referenceStart_ =
      static_cast<int32_t>(std::lround(kReferenceStartSeconds * sampleRate));
  referenceEnd_ =
      static_cast<int32_t>(std::lround(kReferenceEndSeconds * sampleRate));
  fieldIdStart_ = static_cast<int32_t>(
      std::lround(kFieldIdMeasureStartSeconds * sampleRate));
  fieldIdEnd_ =
      static_cast<int32_t>(std::lround(kFieldIdMeasureEndSeconds * sampleRate));
  fieldIdRampStart_ =
      static_cast<int32_t>(std::lround(kFieldIdRampStartSeconds * sampleRate));
  fieldIdRampEnd_ =
      static_cast<int32_t>(std::lround(kFieldIdRampEndSeconds * sampleRate));

  const double lumaAmplitude = static_cast<double>(
      videoParameters_.white_level - videoParameters_.blanking_level);
  minimumAmplitude_ =
      kSecamKillFractionOfM0 * kSecamM0PerLumaAmplitude * lumaAmplitude;

  MonoDecoder::MonoConfiguration ynrConfig;
  ynrConfig.yNRLevel = secamConfig_.yNRLevel;
  ynrConfig.filterChroma = false;
  ynrConfig.videoParameters = videoParameters_;
  ynrHelper_ = std::make_unique<MonoDecoder>(ynrConfig);

  configurationValid_ = true;
  return true;
}

int32_t SecamDecoder::detectLineSequence(
    const std::vector<double>& referenceHz,
    const std::vector<double>& referenceAmp, double minimumAmplitude) {
  // Line-sequential transmission means field lines strictly alternate, so
  // every valid reference votes on the parity of the D'R lines: a reference
  // above the midpoint is f_OR (D'R line), below it f_OB (D'B line).
  double score = 0.0;
  int32_t valid = 0;
  for (size_t line = 0; line < referenceHz.size(); ++line) {
    if (referenceAmp[line] < minimumAmplitude) {
      continue;
    }
    const double f = referenceHz[line];
    if (f < kSecamFobHz - kReferenceToleranceHz ||
        f > kSecamForHz + kReferenceToleranceHz) {
      continue;
    }
    const double vote = (f > kSecamMidHz) ? 1.0 : -1.0;
    score += (line % 2 == 0) ? vote : -vote;
    ++valid;
  }
  // A genuine SECAM field alternates on (almost) every line.  A signal whose
  // reference does not alternate — a PAL burst at 4.43 MHz sits close to
  // f_OR on every line — produces no consistent vote and is rejected.
  if (valid < kMinimumReferenceLines ||
      std::abs(score) < kMinimumSequenceConsistency * valid) {
    return 0;
  }
  return score > 0.0 ? 1 : -1;
}

void SecamDecoder::spreadWeight(std::vector<double>& weight) {
  std::vector<double> scratch;
  spreadWeight(weight, scratch);
}

void SecamDecoder::spreadWeight(std::vector<double>& weight,
                                std::vector<double>& eroded) {
  const int32_t count = static_cast<int32_t>(weight.size());
  eroded.resize(weight.size());
  for (int32_t x = 0; x < count; ++x) {
    const int32_t first = std::max(0, x - kWeightSpreadSamples);
    const int32_t last = std::min(count - 1, x + kWeightSpreadSamples);
    double minimum = 1.0;
    for (int32_t k = first; k <= last; ++k) {
      minimum = std::min(minimum, weight[static_cast<size_t>(k)]);
    }
    eroded[static_cast<size_t>(x)] = minimum;
  }
  for (int32_t x = 0; x < count; ++x) {
    const int32_t first = std::max(0, x - kWeightSpreadSamples);
    const int32_t last = std::min(count - 1, x + kWeightSpreadSamples);
    double sum = 0.0;
    for (int32_t k = first; k <= last; ++k) {
      sum += eroded[static_cast<size_t>(k)];
    }
    weight[static_cast<size_t>(x)] = sum / (last - first + 1);
  }
}

int32_t SecamDecoder::classifyIdentificationLine(double rampStartHz,
                                                 double settledHz) {
  if (std::abs(rampStartHz - kSecamForHz) <= kFieldIdRestToleranceHz) {
    const double swing = settledHz - kSecamForHz;
    if (swing >= kFieldIdMinimumSwingHz && swing <= kFieldIdMaximumSwingHz) {
      return 1;
    }
  }
  if (std::abs(rampStartHz - kSecamFobHz) <= kFieldIdRestToleranceHz) {
    const double swing = kSecamFobHz - settledHz;
    if (swing >= kFieldIdMinimumSwingHz && swing <= kFieldIdMaximumSwingHz) {
      return -1;
    }
  }
  return 0;
}

int32_t SecamDecoder::detectFieldIdentificationSequence(
    const std::vector<double>& rampStartHz,
    const std::vector<double>& identificationHz,
    const std::vector<double>& identificationAmp, double minimumAmplitude) {
  // Each identification line votes on the parity of the D'R lines, exactly
  // like the back-porch reference, but from the large ±350 kHz trapezoid
  // deviation, which survives sources whose porch reference is weak.
  double score = 0.0;
  int32_t valid = 0;
  const size_t last = std::min({identificationHz.size(), rampStartHz.size(),
                                static_cast<size_t>(kFieldIdLastLine) + 1});
  for (size_t line = kFieldIdFirstLine; line < last; ++line) {
    if (line >= identificationAmp.size() ||
        identificationAmp[line] < minimumAmplitude) {
      continue;
    }
    const int32_t kind =
        classifyIdentificationLine(rampStartHz[line], identificationHz[line]);
    if (kind == 0) {
      continue;
    }
    const double vote = static_cast<double>(kind);
    score += (line % 2 == 0) ? vote : -vote;
    ++valid;
  }
  if (valid < kMinimumFieldIdLines ||
      std::abs(score) < kMinimumSequenceConsistency * valid) {
    return 0;
  }
  return score > 0.0 ? 1 : -1;
}

void SecamDecoder::deEmphasise(std::vector<double>& colourDifference,
                               int32_t start) const {
  if (colourDifference.empty()) {
    return;
  }
  // Start at rest on the back-porch value so the filter does not ring from
  // the blanked part of the line.
  start =
      std::clamp(start, 0, static_cast<int32_t>(colourDifference.size()) - 1);
  double x1 = colourDifference[static_cast<size_t>(start)];
  double y1 = x1;
  for (size_t n = static_cast<size_t>(start); n < colourDifference.size();
       ++n) {
    const double x0 = colourDifference[n];
    const double y0 = deEmphB0_ * x0 + deEmphB1_ * x1 - deEmphA1_ * y1;
    colourDifference[n] = y0;
    x1 = x0;
    y1 = y0;
  }
}

void SecamDecoder::decodeField(const SourceField& field, FieldChroma& out) {
  const size_t lines = field.line_count;
  const int32_t width = static_cast<int32_t>(field.samples_per_line);
  // All per-field and per-line buffers are members that keep their capacity
  // from field to field; only the first field of a decoder allocates.
  out.isDr.assign(lines, false);
  out.colourDifference.resize(lines);
  for (std::vector<double>& difference : out.colourDifference) {
    difference.clear();  // empty = no usable chroma on the line
  }
  out.luma.resize(lines);

  std::vector<std::vector<double>>& frequencies = frequencies_;
  std::vector<std::vector<double>>& amplitudes = amplitudes_;
  frequencies.resize(lines);
  amplitudes.resize(lines);
  std::vector<double>& referenceHz = referenceHz_;
  std::vector<double>& referenceAmp = referenceAmp_;
  referenceHz.assign(lines, 0.0);
  referenceAmp.assign(lines, 0.0);

  const int32_t refStart = std::min(referenceStart_, width);
  const int32_t refEnd = std::min(referenceEnd_, width);
  const int32_t idStart = std::min(fieldIdStart_, width);
  const int32_t idEnd = std::min(fieldIdEnd_, width);
  const int32_t rampStart = std::min(fieldIdRampStart_, width);
  const int32_t rampEnd = std::min(fieldIdRampEnd_, width);
  std::vector<double>& rampStartHz = rampStartHz_;
  std::vector<double>& identificationHz = identificationHz_;
  std::vector<double>& identificationAmp = identificationAmp_;
  rampStartHz.assign(lines, 0.0);
  identificationHz.assign(lines, 0.0);
  identificationAmp.assign(lines, 0.0);

  // Median of the instantaneous frequency of one line over [first, end),
  // using a reusable scratch window.
  const auto medianFrequency = [this](const std::vector<double>& frequency,
                                      int32_t first, int32_t end) {
    medianWindow_.assign(frequency.begin() + first, frequency.begin() + end);
    const auto middle = medianWindow_.begin() +
                        static_cast<std::ptrdiff_t>(medianWindow_.size() / 2);
    std::nth_element(medianWindow_.begin(), middle, medianWindow_.end());
    return *middle;
  };

  for (size_t line = 0; line < lines; ++line) {
    const int16_t* chromaInput =
        field.is_yc ? field.getChromaLine(line) : field.getLine(line);
    demodulator_->separateChroma(chromaInput, width, chroma_);

    std::vector<double>& luma = out.luma[line];
    luma.resize(static_cast<size_t>(width));
    if (field.is_yc) {
      const int16_t* lumaInput = field.getLumaLine(line);
      for (int32_t x = 0; x < width; ++x) {
        luma[static_cast<size_t>(x)] = lumaInput[x];
      }
    } else {
      demodulator_->separateLuma(chromaInput, width, luma);
    }

    demodulator_->demodulate(chroma_, frequencies[line], amplitudes[line]);

    if (refEnd > refStart) {
      referenceHz[line] = medianFrequency(frequencies[line], refStart, refEnd);
      double ampSum = 0.0;
      for (int32_t x = refStart; x < refEnd; ++x) {
        ampSum += amplitudes[line][static_cast<size_t>(x)];
      }
      referenceAmp[line] = ampSum / (refEnd - refStart);
    }

    if (secamConfig_.fieldIdentification &&
        line >= static_cast<size_t>(kFieldIdFirstLine) &&
        line <= static_cast<size_t>(kFieldIdLastLine) && idEnd > idStart &&
        rampEnd > rampStart) {
      rampStartHz[line] =
          medianFrequency(frequencies[line], rampStart, rampEnd);
      identificationHz[line] =
          medianFrequency(frequencies[line], idStart, idEnd);
      double ampSum = 0.0;
      for (int32_t x = idStart; x < idEnd; ++x) {
        ampSum += amplitudes[line][static_cast<size_t>(x)];
      }
      identificationAmp[line] = ampSum / (idEnd - idStart);
    }
  }

  // With field identification enabled, the identification lines decide the
  // line sequence; the back-porch reference is the fallback for fields
  // without usable identification signals.
  int32_t sequence = 0;
  if (secamConfig_.fieldIdentification) {
    sequence = detectFieldIdentificationSequence(
        rampStartHz, identificationHz, identificationAmp, minimumAmplitude_);
  }
  if (sequence == 0) {
    sequence = detectLineSequence(referenceHz, referenceAmp, minimumAmplitude_);
  }
  if (sequence == 0) {
    // No SECAM colour reference in this field: colour killer.
    return;
  }

  // The typical subcarrier envelope of this field: the median over the
  // active picture of the lines that carry a reference.  It is measured in
  // the picture rather than on the back porch, where the cloche filter is
  // still settling after the subcarrier gate opens.
  const int32_t activeStart =
      std::min(videoParameters_.active_video_start, width);
  const int32_t activeEnd = std::min(videoParameters_.active_video_end, width);
  envelopeSamples_.clear();
  for (size_t line = 0; line < lines; ++line) {
    if (referenceAmp[line] < minimumAmplitude_) {
      continue;
    }
    for (int32_t x = activeStart; x < activeEnd; x += kEnvelopeSampleStep) {
      const double a = amplitudes[line][static_cast<size_t>(x)];
      if (a >= minimumAmplitude_) {
        envelopeSamples_.push_back(a);
      }
    }
  }
  double typicalAmplitude = minimumAmplitude_ / kSecamKillFractionOfM0;
  if (!envelopeSamples_.empty()) {
    const auto middle =
        envelopeSamples_.begin() +
        static_cast<std::ptrdiff_t>(envelopeSamples_.size() / 2);
    std::nth_element(envelopeSamples_.begin(), middle, envelopeSamples_.end());
    typicalAmplitude = *middle;
  }

  for (size_t line = 0; line < lines; ++line) {
    const bool drLine = ((line % 2 == 0) == (sequence > 0));
    out.isDr[line] = drLine;

    weight_.resize(static_cast<size_t>(width));
    for (int32_t x = 0; x < width; ++x) {
      const size_t i = static_cast<size_t>(x);
      weight_[i] =
          secamConfig_.crossColourSuppression
              ? crossColourWeight(amplitudes[line][i], frequencies[line][i],
                                  typicalAmplitude)
              : 1.0;
    }
    if (secamConfig_.crossColourSuppression) {
      spreadWeight(weight_, eroded_);
    }

    std::vector<double>& difference = out.colourDifference[line];
    difference.resize(static_cast<size_t>(width));
    for (int32_t x = 0; x < width; ++x) {
      const size_t i = static_cast<size_t>(x);
      difference[i] = (amplitudes[line][i] < minimumAmplitude_)
                          ? 0.0
                          : weight_[i] * frequencyToColourDifference(
                                             frequencies[line][i], drLine);
    }
    deEmphasise(difference, refStart);
    // Band-limit to the item 10c colour-difference bandwidth, then fade out
    // again so the filters cannot spread suppressed cross-colour back in.
    convolve(difference.data(), width, colourDifferenceTaps_, filtered_);
    for (int32_t x = 0; x < width; ++x) {
      const size_t i = static_cast<size_t>(x);
      difference[i] = filtered_[i] * weight_[i];
    }
  }
}

int32_t SecamDecoder::getLookBehind() const {
  return secamConfig_.temporalChroma ? 1 : 0;
}

void SecamDecoder::decodeFrames(const std::vector<SourceField>& inputFields,
                                int32_t startIndex, int32_t endIndex,
                                std::vector<ComponentFrame>& componentFrames) {
  if (!configurationValid_) {
    ORC_LOG_ERROR(
        "SecamDecoder::decodeFrames(): Decoder configuration is invalid");
    return;
  }

  const ::orc::SourceParameters& vp = videoParameters_;
  const int32_t lineOffset =
      vp.active_area_cropping_applied ? vp.first_active_frame_line : 0;
  const int32_t xOffset =
      vp.active_area_cropping_applied ? vp.active_video_start : 0;
  // ComponentFrame U/V share the luma excursion (see OutputWriter).
  const double lumaRange = static_cast<double>(vp.white_level - vp.black_level);
  const double uvRange = lumaRange * secamConfig_.chromaGain;

  const auto fieldAt = [&](int32_t index) -> const SourceField* {
    return (index >= 0 && index < static_cast<int32_t>(inputFields.size()))
               ? &inputFields[static_cast<size_t>(index)]
               : nullptr;
  };

  // The previous frame's decoded fields, carried from one loop iteration to
  // the next; the first frame of the call decodes its look-behind frame.
  bool previousValid = false;
  if (secamConfig_.temporalChroma) {
    previousValid = true;
    for (int32_t f = 0; f < 2; ++f) {
      const SourceField* field = fieldAt(startIndex - 2 + f);
      if (field != nullptr) {
        decodeField(*field, previousFields_[static_cast<size_t>(f)]);
      } else {
        previousValid = false;
      }
    }
  }

  for (int32_t fieldIndex = startIndex, frameIndex = 0; fieldIndex < endIndex;
       fieldIndex += 2, frameIndex++) {
    ComponentFrame& frame = componentFrames[static_cast<size_t>(frameIndex)];
    frame.init(vp);

    const SourceField* fieldPtrs[2] = {fieldAt(fieldIndex),
                                       fieldAt(fieldIndex + 1)};
    for (int32_t f = 0; f < 2; ++f) {
      if (fieldPtrs[f] != nullptr) {
        decodeField(*fieldPtrs[f], fields_[static_cast<size_t>(f)]);
      } else {
        fields_[static_cast<size_t>(f)] = FieldChroma{};
      }
    }

    for (int32_t y = vp.first_active_frame_line; y < vp.last_active_frame_line;
         y++) {
      int32_t f = -1;
      for (int32_t candidate = 0; candidate < 2; ++candidate) {
        if (fieldPtrs[candidate] != nullptr &&
            (y & 1) == fieldPtrs[candidate]->getOffset()) {
          f = candidate;
          break;
        }
      }
      if (f < 0) {
        continue;
      }
      const SourceField& field = *fieldPtrs[f];
      const FieldChroma& chroma = fields_[static_cast<size_t>(f)];
      const int32_t fieldLine = (y - field.getOffset()) / 2;
      if (fieldLine < 0 || static_cast<size_t>(fieldLine) >= field.line_count) {
        continue;
      }
      const size_t fl = static_cast<size_t>(fieldLine);

      double* outY = frame.y(y - lineOffset);
      double* outU = frame.u(y - lineOffset);
      double* outV = frame.v(y - lineOffset);

      // Spatial candidate for the missing component: the average of the
      // time-adjacent lines of the same field, which carry the other colour
      // difference.  It locates a horizontal colour edge only to within one
      // field line, and because 625 is odd every line swaps component from
      // frame to frame, so on its own the edge moves by a line each frame.
      const std::vector<double>& own = chroma.colourDifference[fl];
      const std::vector<double>* above =
          (fl > 0 && !chroma.colourDifference[fl - 1].empty())
              ? &chroma.colourDifference[fl - 1]
              : nullptr;
      const std::vector<double>* below =
          (fl + 1 < chroma.colourDifference.size() &&
           !chroma.colourDifference[fl + 1].empty())
              ? &chroma.colourDifference[fl + 1]
              : nullptr;
      const bool ownIsDr = own.empty() ? false : chroma.isDr[fl];

      // Temporal candidate: the same line of the previous frame carried the
      // other component (BT.1700 Part C Figure 16), so on still picture it
      // supplies the missing colour difference at the right line.  It is
      // blended in only where the luma has not changed between the frames.
      const FieldChroma& previous = previousFields_[static_cast<size_t>(f)];
      const bool temporalAvailable =
          previousValid && !own.empty() && fl < previous.isDr.size() &&
          !previous.colourDifference[fl].empty() &&
          previous.isDr[fl] != ownIsDr && fl < previous.luma.size();
      if (temporalAvailable) {
        const std::vector<double>& lumaNow = chroma.luma[fl];
        const std::vector<double>& lumaThen = previous.luma[fl];
        const size_t n = std::min(lumaNow.size(), lumaThen.size());
        motionPrefix_.assign(n + 1, 0.0);
        for (size_t i = 0; i < n; ++i) {
          motionPrefix_[i + 1] =
              motionPrefix_[i] + std::abs(lumaNow[i] - lumaThen[i]);
        }
      }

      for (int32_t x = vp.active_video_start; x < vp.active_video_end; x++) {
        const size_t i = static_cast<size_t>(x);
        outY[x - xOffset] =
            i < chroma.luma[fl].size() ? chroma.luma[fl][i] : 0.0;

        if (own.empty()) {
          outU[x - xOffset] = 0.0;
          outV[x - xOffset] = 0.0;
          continue;
        }

        const double ownValue = i < own.size() ? own[i] : 0.0;
        double otherValue = 0.0;
        int32_t otherCount = 0;
        if (above != nullptr && i < above->size()) {
          otherValue += (*above)[i];
          ++otherCount;
        }
        if (below != nullptr && i < below->size()) {
          otherValue += (*below)[i];
          ++otherCount;
        }
        if (otherCount > 0) {
          otherValue /= otherCount;
        }

        if (temporalAvailable && i + 1 < motionPrefix_.size()) {
          const size_t first = (i > static_cast<size_t>(kMotionWindowSamples))
                                   ? i - kMotionWindowSamples
                                   : 0;
          const size_t last =
              std::min(motionPrefix_.size() - 1, i + kMotionWindowSamples + 1);
          const double meanDifference =
              (motionPrefix_[last] - motionPrefix_[first]) /
              static_cast<double>(last - first);
          const double w = temporalWeight(meanDifference, lumaRange);
          const double temporalValue = previous.colourDifference[fl][i];
          otherValue = w * temporalValue + (1.0 - w) * otherValue;
        }

        const double dr = ownIsDr ? ownValue : otherValue;
        const double db = ownIsDr ? otherValue : ownValue;
        const double bMinusY = db / kSecamDbPerBMinusY;
        const double rMinusY = dr / kSecamDrPerRMinusY;
        outU[x - xOffset] = kB * bMinusY * uvRange;
        outV[x - xOffset] = kR * rMinusY * uvRange;
      }
    }

    ynrHelper_->doYNR(frame);

    if (secamConfig_.temporalChroma) {
      std::swap(previousFields_, fields_);
      previousValid = fieldPtrs[0] != nullptr && fieldPtrs[1] != nullptr;
    }
  }
}
