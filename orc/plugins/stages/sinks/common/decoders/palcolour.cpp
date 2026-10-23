/*
 * File:        palcolour.cpp
 * Module:      orc-core
 * Purpose:     PAL colour decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 * SPDX-FileCopyrightText: 2019-2021 Adam Sampson
 */

// PALcolour original copyright notice:
// Copyright (C) 2018  William Andrew Steer
// Contact the author at palcolour@techmind.org

#include "palcolour.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <type_traits>

#include "firfilter.h"
#include "transformpal2d.h"
#include "transformpal3d.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <orc/support/logging.h>  // ORC logging

#include "../video_parameter_safety.h"

/*!
    \class PalColour

    PALcolour, originally written by William Andrew Steer, is a line-locked PAL
    decoder using 2D FIR filters.

    For a good overview of line-locked PAL decoding techniques, see
    BBC Research Department Report 1986/02
   (https://www.bbc.co.uk/rd/publications/rdreport_1986_02), "Colour encoding
   and decoding techniques for line-locked sampled PAL and NTSC television
   signals" by C.K.P. Clarke. PALcolour uses the architecture shown in Figure
   23(c), except that it has three separate baseband filters, one each for Y, U
   and V, with different characteristics. Rather than tracking the colour
   subcarrier using a PLL, PALcolour detects the phase of the subcarrier at the
   colourburst, and rotates the U/V output to compensate when decoding.

    BBC Research Department Report 1988/11
   (https://www.bbc.co.uk/rd/publications/rdreport_1988_11), "PAL decoding:
   Multi-dimensional filter design for chrominance-luminance separation", also
   by C.K.P. Clarke, describes the design concerns behind these filters. As
   PALcolour is a software implementation, it can use larger filters with more
   complex coefficients than the report describes.
 */

PalColour::PalColour() : configurationSet(false) {}

int32_t PalColour::Configuration::getThresholdsSize() const {
  if (chromaFilter == transform2DFilter) {
    return TransformPal2D::getThresholdsSize();
  } else if (chromaFilter == transform3DFilter) {
    return TransformPal3D::getThresholdsSize();
  } else {
    return 0;
  }
}

int32_t PalColour::Configuration::getLookBehind() const {
  if (chromaFilter == transform3DFilter) {
    // EBU Tech. 3280-E / design §8.7: 3D Transform PAL requires one frame of
    // look-behind context (frame-based, not field-based).
    return 1;
  } else {
    return 0;
  }
}

int32_t PalColour::Configuration::getLookAhead() const {
  if (chromaFilter == transform3DFilter) {
    return TransformPal3D::getLookAhead();
  } else {
    return 0;
  }
}

// Return the current configuration
const PalColour::Configuration& PalColour::getConfiguration() const {
  return configuration;
}

void PalColour::updateConfiguration(
    const ::orc::SourceParameters& _videoParameters,
    const Configuration& _configuration) {
  configuration = _configuration;

  const auto safety = ::orc::chroma_sink::sanitize_video_parameters(
      _videoParameters, ::orc::chroma_sink::DecoderVideoProfile::PalColour);

  if (!safety.warnings.empty()) {
    ORC_LOG_WARN(
        "PalColour::updateConfiguration(): Adjusted unsafe video parameters: "
        "{}",
        ::orc::chroma_sink::join_issues(safety.warnings));
  }

  if (!safety.ok) {
    ORC_LOG_ERROR(
        "PalColour::updateConfiguration(): Invalid video parameters: {}",
        ::orc::chroma_sink::join_issues(safety.errors));
    configurationSet = false;
    transformPal.reset();
    return;
  }

  videoParameters = safety.params;

  // Build the look-up tables
  buildLookUpTables();

  if (configuration.chromaFilter == transform2DFilter ||
      configuration.chromaFilter == transform3DFilter) {
    // Create the Transform PAL filter
    if (configuration.chromaFilter == transform2DFilter) {
      transformPal = std::make_unique<TransformPal2D>();
    } else {
      transformPal = std::make_unique<TransformPal3D>();
    }

    // Configure the filter
    transformPal->updateConfiguration(videoParameters,
                                      configuration.transformThreshold,
                                      configuration.transformThresholds);
  }

  configurationSet = true;
}

// Rebuild the lookup tables based on the configuration
void PalColour::buildLookUpTables() {
  // Generate the reference carrier: quadrature samples of a sine wave at the
  // subcarrier frequency. We'll use this for two purposes below:
  // - product-detecting the line samples, to give us quadrature samples of
  //   the chroma information centred on 0 Hz
  // - working out what the phase of the subcarrier is on each line,
  //   so we can rotate the chroma samples to put U/V on the right axes
  //
  // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f: PAL burst phase referenced
  // to the +U axis. SMPTE 170M-2004: NTSC burst reference is the -I axis.
  // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f: PAL-M V-switch follows PAL
  // sign convention, but Sc-H phase origin differs from 625-line PAL.
  const double fsc = fsc_from_system(videoParameters.system);
  const double sample_rate = sample_rate_from_system(videoParameters.system);
  const int32_t field_width = videoParameters.frame_width_nominal;
  const size_t padded_field_height =
      calculate_padded_field_height(videoParameters.system);

  // For PAL-M (525-line, padded_field_height == 263), vhs-decode TBC data uses
  // the NTSC Sc-H phase convention, which places the subcarrier in the cosine
  // quadrant at the start of each line rather than the sine quadrant of the PAL
  // +U-axis convention (EBU Statement D23 / EBU Tech. 3280-E §1.1.1).
  // Swapping sin↔cos in the reference compensates for this 90° Sc-H offset.
  const bool pal_m_phase = (padded_field_height == 263);
  for (int32_t i = 0; i < field_width; i++) {
    const double rad = 2 * M_PI * i * fsc / sample_rate;
    sine[i] = pal_m_phase ? cos(rad) : sin(rad);
    cosine[i] = pal_m_phase ? sin(rad) : cos(rad);
  }

  // Create filter profiles for colour filtering.
  //
  // One can argue over merits of different filters, but I stick with simple
  // raised cosine unless there's compelling reason to do otherwise.
  // PAL-I colour bandwidth should be around 1.1 or 1.2 MHz:
  // acc to Rec.470, +1066 or -1300kHz span of colour sidebands!
  // The width of the filter window should scale with the sample rate.
  //
  // chromaBandwidthHz values between 1.1MHz and 1.3MHz can be tried. Some
  // specific values in that range may work best at minimising residual dot
  // pattern at given sample rates due to the discrete nature of the filters.
  // It'd be good to find ways to optimise this more rigourously.
  //
  // Note in principle you could have different bandwidths for extracting the
  // luma and chroma, according to aesthetic tradeoffs. Not really very
  // justifyable though. Keeping the Y and C bandwidth the same (or at least
  // similar enough for the filters to be the same size) allows them to be
  // computed together later.
  //
  // The 0.93 is a bit empirical for the 4Fsc sampled LaserDisc scans.
  const double chromaBandwidthHz = 1100000.0 / 0.93;

  // Compute filter widths based on chroma bandwidth.
  // FILTER_SIZE must be wide enough to hold both filters (and ideally no
  // wider, else we're doing more computation than we need to).
  // XXX where does the 0.5* come from?
  const double ca = 0.5 * sample_rate / chromaBandwidthHz;
  const double ya = 0.5 * sample_rate / chromaBandwidthHz;
  assert(FILTER_SIZE >= static_cast<int32_t>(ca));
  assert(FILTER_SIZE >= static_cast<int32_t>(ya));

  // Note that we choose to make the y-filter *much* less selective in the
  // vertical direction: this is to prevent castellation on horizontal colour
  // boundaries.
  //
  // We may wish to broaden vertical bandwidth *slightly* so as to better
  // pass one- or two-line colour bars - underlines/graphics etc.

  double cdiv = 0, ydiv = 0;
  for (int32_t f = 0; f <= FILTER_SIZE; f++) {
    // 0-2-4-6 sequence here because we're only processing one field.
    const double fc = std::min(ca, static_cast<double>(f));
    const double ff = std::min(ca, sqrt(f * f + 2 * 2));
    const double fff = std::min(ca, sqrt(f * f + 4 * 4));
    const double ffff = std::min(ca, sqrt(f * f + 6 * 6));

    // We will sum the zero-th horizontal tap twice later (when b == 0 in
    // the filter loop), so halve the coefficient to compensate
    const int32_t d = (f == 0) ? 2 : 1;

    // For U/V.
    // 0, 2, 1, 3 are vertical taps 0, +/- 1, +/- 2, +/- 3 (see filter loop
    // below).
    cfilt[f][0] = (1 + cos(M_PI * fc / ca)) / d;
    cfilt[f][2] = (1 + cos(M_PI * ff / ca)) / d;
    cfilt[f][1] = (1 + cos(M_PI * fff / ca)) / d;
    cfilt[f][3] = (1 + cos(M_PI * ffff / ca)) / d;

    // Each horizontal coefficient is applied to 2 columns (when b == 0,
    // it's the same column twice).
    // The zero-th vertical coefficient is applied to 1 line, and the
    // others are applied to pairs of lines.
    cdiv += 2 * (1 * cfilt[f][0] + 2 * cfilt[f][2] + 2 * cfilt[f][1] +
                 2 * cfilt[f][3]);

    const double fy = std::min(ya, static_cast<double>(f));
    const double fffy = std::min(ya, sqrt(f * f + 4 * 4));

    // For Y, only use lines n, n+/-2: the others cancel!!!
    //  *have tried* using lines +/-1 & 3 --- can be made to work, but
    //  introduces *phase-sensitivity* to the filter -> leaks too much
    //  subcarrier if *any* phase-shifts!
    // note omission of yfilt taps 1 and 3 for PAL
    //
    // Tap 2 is only used for PAL; 0.2 factor makes it much less sensitive
    // to adjacent lines and reduces castellations and residual dot
    // patterning.
    //
    // 0, 1 are vertical taps 0, +/- 2 (see filter loop below).
    yfilt[f][0] = (1 + cos(M_PI * fy / ya)) / d;
    yfilt[f][1] = 0.2 * (1 + cos(M_PI * fffy / ya)) / d;

    ydiv += 2 * (1 * yfilt[f][0] + 2 * 0 + 2 * yfilt[f][1] + 2 * 0);
  }

  // Normalise the filter coefficients.
  for (int32_t f = 0; f <= FILTER_SIZE; f++) {
    for (int32_t i = 0; i < 4; i++) {
      cfilt[f][i] /= cdiv;
    }
    for (int32_t i = 0; i < 2; i++) {
      yfilt[f][i] /= ydiv;
    }
  }
}

void PalColour::decodeFrames(const std::vector<SourceField>& inputFields,
                             int32_t startIndex, int32_t endIndex,
                             std::vector<ComponentFrame>& componentFrames) {
  if (!configurationSet) {
    ORC_LOG_ERROR(
        "PalColour::decodeFrames(): Decoder configuration is invalid");
    return;
  }
  assert((componentFrames.size() * 2) == (endIndex - startIndex));

  // Check if we have YC sources (separate Y and C channels)
  bool is_yc_source = !inputFields.empty() && inputFields[0].is_yc;

  if (is_yc_source) {
    // YC DECODE PATH - simplified decoding for separate Y/C
    // For YC sources:
    // - Y is already clean (no filtering needed)
    // - C only needs demodulation (no Y/C separation needed)
    ORC_LOG_INFO("PalColour: Using YC decode path (separate Y/C channels)");

    for (int32_t i = startIndex, k = 0; i < endIndex; i += 2, k++) {
      // Initialize and clear the component frame
      componentFrames[k].init(videoParameters);

      // Decode both fields directly - Y is clean, C needs demodulation
      decodeFieldYC(inputFields[i], componentFrames[k]);
      decodeFieldYC(inputFields[i + 1], componentFrames[k]);
    }
    return;
  }

  // COMPOSITE DECODE PATH - full PAL color decoding with Y/C separation
  std::vector<const double*> chromaData(endIndex - startIndex);
  if (configuration.chromaFilter != palColourFilter) {
    // Use Transform PAL filter to extract chroma
    transformPal->filterFields(inputFields, startIndex, endIndex, chromaData);
  }

  for (int32_t i = startIndex, j = 0, k = 0; i < endIndex;
       i += 2, j += 2, k++) {
    // Initialise and clear the component frame
    componentFrames[k].init(videoParameters);

    decodeField(inputFields[i], chromaData[j], componentFrames[k]);
    decodeField(inputFields[i + 1], chromaData[j + 1], componentFrames[k]);
  }

  if (configuration.showFFTs && configuration.chromaFilter != palColourFilter) {
    // Overlay the FFT visualisation
    transformPal->overlayFFT(configuration.showPositionX,
                             configuration.showPositionY, inputFields,
                             startIndex, endIndex, componentFrames);
  }
}

// Decode one field into componentFrame
void PalColour::decodeField(const SourceField& inputField,
                            const double* chromaData,
                            ComponentFrame& componentFrame) {
  // Convert frame-based active area limits to field-based coordinates
  // This ensures proper indexing when active area cropping is applied
  const int32_t firstLine =
      (videoParameters.first_active_frame_line + 1 - inputField.getOffset()) /
      2;
  const int32_t lastLine =
      (videoParameters.last_active_frame_line + 1 - inputField.getOffset()) / 2;
  for (int32_t fieldLine = firstLine; fieldLine < lastLine; fieldLine++) {
    LineInfo line(fieldLine);

    // Detect the colourburst from the composite signal
    detectBurst(line, inputField, /*use_chroma_channel=*/false);

    // Rotate and scale line.bp/line.bq to apply gain and phase adjustment
    const double oldBp = line.bp, oldBq = line.bq;
    const double theta = (configuration.chromaPhase * M_PI) / 180;
    line.bp =
        (oldBp * cos(theta) - oldBq * sin(theta)) * configuration.chromaGain;
    line.bq =
        (oldBp * sin(theta) + oldBq * cos(theta)) * configuration.chromaGain;

    if (configuration.chromaFilter == palColourFilter) {
      // Decode chroma and luma from the composite signal
      decodeLine<int16_t, false>(inputField, nullptr, line, componentFrame);
    } else {
      // Decode chroma and luma from the Transform PAL output
      decodeLine<double, true>(inputField, chromaData, line, componentFrame);
    }
  }
}

// Decode one YC field into componentFrame
// For YC sources: Y is already clean, C needs demodulation with 2D filtering
void PalColour::decodeFieldYC(const SourceField& inputField,
                              ComponentFrame& componentFrame) {
  // Convert frame-based active area limits to field-based coordinates
  const int32_t firstLine =
      (videoParameters.first_active_frame_line + 1 - inputField.getOffset()) /
      2;
  const int32_t lastLine =
      (videoParameters.last_active_frame_line + 1 - inputField.getOffset()) / 2;

  for (int32_t fieldLine = firstLine; fieldLine < lastLine; fieldLine++) {
    LineInfo line(fieldLine);

    // Detect the colourburst from the C channel (not composite)
    // This also detects the V-switch state for PAL
    detectBurst(line, inputField, /*use_chroma_channel=*/true);

    // Rotate and scale burst to apply gain and phase adjustment
    const double oldBp = line.bp, oldBq = line.bq;
    const double theta = (configuration.chromaPhase * M_PI) / 180;
    line.bp =
        (oldBp * cos(theta) - oldBq * sin(theta)) * configuration.chromaGain;
    line.bq =
        (oldBp * sin(theta) + oldBq * cos(theta)) * configuration.chromaGain;

    // Calculate frame line number
    const int32_t absoluteLineNumber = (fieldLine * 2) + inputField.getOffset();
    const int32_t lineNumber =
        videoParameters.active_area_cropping_applied
            ? (absoluteLineNumber - videoParameters.first_active_frame_line)
            : absoluteLineNumber;

    // Get output pointers
    double* outY = componentFrame.y(lineNumber);
    double* outU = componentFrame.u(lineNumber);
    double* outV = componentFrame.v(lineNumber);

    // Apply 2D chroma filter to extract U and V from the C channel
    apply2DChromaFilter(inputField, line, fieldLine, firstLine, lastLine, outU,
                        outV);

    // Copy clean Y data directly (no filtering needed for YC sources)
    const int16_t* yLine =
        inputField.getLumaLine(static_cast<size_t>(fieldLine));
    const auto endPos = std::min(videoParameters.active_video_end, MAX_WIDTH);
    for (int32_t i = videoParameters.active_video_start; i < endPos; i++) {
      int32_t outIdx = videoParameters.active_area_cropping_applied
                           ? (i - videoParameters.active_video_start)
                           : i;
      outY[outIdx] = static_cast<double>(yLine[i]);
    }
  }
}

// Apply 2D chroma filter to extract U and V components from chroma data
// This is shared between composite and YC decode paths (YC path only in
// practice, since composite uses the 2D filter inside decodeLine).
void PalColour::apply2DChromaFilter(const SourceField& inputField,
                                    const LineInfo& line, int32_t fieldLine,
                                    int32_t firstLine, int32_t lastLine,
                                    double* outU, double* outV) {
  // Black line used when the filter needs to look outside the field.
  static constexpr int16_t blackLine[MAX_WIDTH] = {0};

  // Helper: return pointer to chroma line, or blackLine for out-of-range.
  auto getChromaLine = [&](int32_t ln) -> const int16_t* {
    if (ln < firstLine || ln >= lastLine) return blackLine;
    return inputField.getChromaLine(static_cast<size_t>(ln));
  };

  // Get pointers to the surrounding lines of chroma data
  const int16_t* in0 = getChromaLine(fieldLine);
  const int16_t* in1 = getChromaLine(fieldLine - 1);
  const int16_t* in2 = getChromaLine(fieldLine + 1);
  const int16_t* in3 = getChromaLine(fieldLine - 2);
  const int16_t* in4 = getChromaLine(fieldLine + 2);
  const int16_t* in5 = getChromaLine(fieldLine - 3);
  const int16_t* in6 = getChromaLine(fieldLine + 3);

  // Multiply the chroma signal by the reference carrier, giving quadrature
  // samples where the colour subcarrier is now at 0 Hz
  double m[4][MAX_WIDTH], n[4][MAX_WIDTH];
  const auto endPos2 =
      std::min(videoParameters.active_video_end + FILTER_SIZE + 1, MAX_WIDTH);
  for (int32_t i = videoParameters.active_video_start - FILTER_SIZE;
       i < endPos2; i++) {
    // Promote to int32_t before addition to avoid signed overflow on int16_t.
    m[0][i] = in0[i] * sine[i];
    m[2][i] = (static_cast<int32_t>(in1[i]) - in2[i]) * sine[i];
    m[1][i] = -(static_cast<int32_t>(in3[i]) + in4[i]) * sine[i];
    m[3][i] = (-(static_cast<int32_t>(in5[i])) + in6[i]) * sine[i];

    n[0][i] = in0[i] * cosine[i];
    n[2][i] = (static_cast<int32_t>(in1[i]) - in2[i]) * cosine[i];
    n[1][i] = -(static_cast<int32_t>(in3[i]) + in4[i]) * cosine[i];
    n[3][i] = (-(static_cast<int32_t>(in5[i])) + in6[i]) * cosine[i];
  }

  // Apply 2D filters to get U and V components
  double pu[MAX_WIDTH], qu[MAX_WIDTH], pv[MAX_WIDTH], qv[MAX_WIDTH];
  const auto endPos = std::min(videoParameters.active_video_end, MAX_WIDTH);

  for (int32_t i = videoParameters.active_video_start; i < endPos; i++) {
    double PU = 0, QU = 0, PV = 0, QV = 0;

    // Apply 2D filter coefficients
    for (int32_t b = 0; b <= FILTER_SIZE; b++) {
      const int32_t l = i - b;
      const int32_t r = i + b;

      PU += (m[0][r] + m[0][l]) * cfilt[b][0] +
            (m[1][r] + m[1][l]) * cfilt[b][1] +
            (n[2][r] + n[2][l]) * cfilt[b][2] +
            (n[3][r] + n[3][l]) * cfilt[b][3];
      QU += (n[0][r] + n[0][l]) * cfilt[b][0] +
            (n[1][r] + n[1][l]) * cfilt[b][1] -
            (m[2][r] + m[2][l]) * cfilt[b][2] -
            (m[3][r] + m[3][l]) * cfilt[b][3];
      PV += (m[0][r] + m[0][l]) * cfilt[b][0] +
            (m[1][r] + m[1][l]) * cfilt[b][1] -
            (n[2][r] + n[2][l]) * cfilt[b][2] -
            (n[3][r] + n[3][l]) * cfilt[b][3];
      QV += (n[0][r] + n[0][l]) * cfilt[b][0] +
            (n[1][r] + n[1][l]) * cfilt[b][1] +
            (m[2][r] + m[2][l]) * cfilt[b][2] +
            (m[3][r] + m[3][l]) * cfilt[b][3];
    }

    pu[i] = PU;
    qu[i] = QU;
    pv[i] = PV;
    qv[i] = QV;
  }

  // Rotate filtered quadrature components by burst phase to recover U and V
  // Apply V-switch for PAL alternating lines
  // Multiply by 2.0 because filtering extracts chroma at half amplitude
  for (int32_t i = videoParameters.active_video_start; i < endPos; i++) {
    int32_t outIdx = videoParameters.active_area_cropping_applied
                         ? (i - videoParameters.active_video_start)
                         : i;

    outU[outIdx] = -(pu[i] * line.bp + qu[i] * line.bq) * 2.0;
    outV[outIdx] = line.Vsw * -(qv[i] * line.bp - pv[i] * line.bq) * 2.0;
  }
}

PalColour::LineInfo::LineInfo(int32_t _number) : number(_number) {}

// Detect the colourburst on a line.
// Stores the burst details into line.
// use_chroma_channel: true to read from the C channel (YC path), false for
// composite (or the V-switch is detected on the composite channel).
void PalColour::detectBurst(LineInfo& line, const SourceField& inputField,
                            bool use_chroma_channel) {
  // Dummy black line, used when the filter needs to look outside the field.
  static constexpr int16_t blackLine[MAX_WIDTH] = {0};

  // Helper: return a pointer to line ln in the appropriate channel, or
  // blackLine when ln is outside [0, line_count).
  auto getLine = [&](int32_t ln) -> const int16_t* {
    if (ln < 0 || static_cast<size_t>(ln) >= inputField.line_count) {
      return blackLine;
    }
    if (use_chroma_channel) {
      return inputField.getChromaLine(static_cast<size_t>(ln));
    }
    return inputField.getLine(static_cast<size_t>(ln));
  };

  const int16_t* in0 = getLine(line.number);
  const int16_t* in1 = getLine(line.number - 1);
  const int16_t* in2 = getLine(line.number + 1);
  const int16_t* in3 = getLine(line.number - 2);
  const int16_t* in4 = getLine(line.number + 2);

  // Find absolute burst phase relative to the reference carrier by
  // product detection.
  //
  // To avoid hue-shifts on alternate lines, the phase is determined by
  // averaging the phase on the current-line with the average of two
  // other lines, one above and one below the current line.
  //
  // For PAL we use the next-but-one line above and below (in the field),
  // which will have the same V-switch phase as the current-line (and 180
  // degree change of phase), and we also analyse the average (bpo/bqo
  // 'old') of the line immediately above and below, which have the
  // opposite V-switch phase (and a 90 degree subcarrier phase shift).
  //
  // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f / ITU-R BT.470-6 Table 2
  // item 2.16: PAL burst phase at 135°/225° relative to +U axis.
  double bp = 0, bq = 0, bpo = 0, bqo = 0;
  const auto [burst_start, burst_end] =
      colour_burst_range(videoParameters.system);
  if (burst_end > videoParameters.frame_width_nominal) {
    line.bp = 0.0;
    line.bq = 0.0;
    line.Vsw = -1;
    return;
  }
  for (int32_t i = burst_start; i < burst_end; i++) {
    // Promote int16_t to int32_t before addition to avoid signed overflow.
    const double d0 = in0[i] - (static_cast<int32_t>(in3[i]) + in4[i]) / 2.0;
    bp += (d0 / 2.0) * sine[i];
    bq += (d0 / 2.0) * cosine[i];
    const double dv = (static_cast<int32_t>(in2[i]) - in1[i]) / 2.0;
    bpo += dv * sine[i];
    bqo += dv * cosine[i];
  }

  // Normalise the sums above
  const int32_t colourBurstLength = burst_end - burst_start;
  bp /= colourBurstLength;
  bq /= colourBurstLength;
  bpo /= colourBurstLength;
  bqo /= colourBurstLength;

  // Detect the V-switch state on this line.
  //
  // I forget exactly why this works, but it's essentially comparing the
  // vector magnitude /difference/ between the phases of the burst on the
  // present line and previous line to the magnitude of the burst. This
  // may effectively be a dot-product operation...
  line.Vsw = -1;
  if ((((bp - bpo) * (bp - bpo) + (bq - bqo) * (bq - bqo)) <
       (bp * bp + bq * bq) * 2)) {
    line.Vsw = 1;
  }

  // Average the burst phase to get -U (reference) phase out -- burst
  // phase is (-U +/-V).
  line.bp = (bp - bqo) / 2;
  line.bq = (bq + bpo) / 2;

  // Normalise the magnitude of the bp/bq vector to 1.
  // Kill colour if burst amplitude is below ~12% of nominal.
  // The original constant 130000/128 was calibrated for the PAL 16-bit TBC
  // domain (kTbcPalWhite − kTbcPalBlanking = 54016 − 16384 = 37632 counts).
  // Scale to the CVBS_U10_4FSC active range (white_level − blanking_level) so
  // the threshold is domain-independent.
  // EBU Tech. 3280-E §1.1.1 Table 1 / kTbcPalBlanking / kTbcPalWhite.
  const double burstKillThreshold =
      130000.0 / 128.0 *
      static_cast<double>(videoParameters.white_level -
                          videoParameters.blanking_level) /
      static_cast<double>(orc::kTbcPalWhite - orc::kTbcPalBlanking);
  const double burstNorm =
      std::max(sqrt(line.bp * line.bp + line.bq * line.bq), burstKillThreshold);
  line.bp /= burstNorm;
  line.bq /= burstNorm;
}

// Decode one line into componentFrame.
// chromaData (templated, so it can be any numeric type) is the input to
// the chroma demodulator; for the PREFILTERED_CHROMA=true case (Transform PAL)
// it points to the Transform PAL output (uniform-stride double array).
// For PREFILTERED_CHROMA=false (composite int16_t path) chromaData is unused;
// composite sample access goes through inputField.getLine() for correct PAL
// non-uniform line handling.
template <typename ChromaSample, bool PREFILTERED_CHROMA>
void PalColour::decodeLine(const SourceField& inputField,
                           const ChromaSample* chromaData, const LineInfo& line,
                           ComponentFrame& componentFrame) {
  // Dummy black line, used when the filter needs to look outside the active
  // region.
  static constexpr ChromaSample blackLine[MAX_WIDTH] = {};

  // Convert frame-based active area limits to field-based coordinates
  const int32_t firstLine =
      (videoParameters.first_active_frame_line + 1 - inputField.getOffset()) /
      2;
  const int32_t lastLine =
      (videoParameters.last_active_frame_line + 1 - inputField.getOffset()) / 2;

  // Helper: return pointer to line ln in the chroma source.
  // For int16_t (composite): uses SourceField::getLine() for non-uniform PAL.
  // For double (Transform PAL): uses uniform chromaData stride.
  auto getChromaLine = [&](int32_t ln) -> const ChromaSample* {
    if (ln < firstLine || ln >= lastLine) return blackLine;
    if constexpr (std::is_same_v<ChromaSample, int16_t>) {
      return inputField.getLine(static_cast<size_t>(ln));
    } else {
      return chromaData +
             static_cast<ptrdiff_t>(ln * videoParameters.frame_width_nominal);
    }
  };

  const ChromaSample* in0 = getChromaLine(line.number);
  const ChromaSample* in1 = getChromaLine(line.number - 1);
  const ChromaSample* in2 = getChromaLine(line.number + 1);
  const ChromaSample* in3 = getChromaLine(line.number - 2);
  const ChromaSample* in4 = getChromaLine(line.number + 2);
  const ChromaSample* in5 = getChromaLine(line.number - 3);
  const ChromaSample* in6 = getChromaLine(line.number + 3);

  // Clamp end position to max width so we don't go out of bounds.
  const auto endPos = std::min(videoParameters.active_video_end, MAX_WIDTH);
  if (endPos < videoParameters.active_video_end) {
    ORC_LOG_WARN("Tried to decode video outside max width!");
  }

  double pu[MAX_WIDTH], qu[MAX_WIDTH], pv[MAX_WIDTH], qv[MAX_WIDTH],
      py[MAX_WIDTH], qy[MAX_WIDTH];
  if (PREFILTERED_CHROMA && configuration.simplePAL) {
    // Use Simple PAL 1D filter.
    // (Only for Transform PAL mode, since we don't have a 1D notch filter.)

    // LPF equivalent to the BBC Transform PAL implementation's UV postfilter.
    // Generated by: sps.remez(17, [0.0, 2.15e6, 4.6e6, rate/2], [1.0, 0.0],
    // [1.0, 1.0], fs=rate)
    static constexpr std::array<double, 17> palUvFilterCoeffs{
        -0.00199265, 0.01226292,  0.01767698,  -0.01034077, -0.05538487,
        -0.03793064, 0.09913768,  0.29007115,  0.38112572,  0.29007115,
        0.09913768,  -0.03793064, -0.05538487, -0.01034077, 0.01767698,
        0.01226292,  -0.00199265};
    static constexpr auto palUvFilter = makeFIRFilter(palUvFilterCoeffs);

    // As above, but at PAL-M's sample rate.
    static constexpr std::array<double, 17> palMUvFilterCoeffs{
        -0.00304101, -0.00750668, 0.01028936,  0.02719209, -0.01941656,
        -0.07939037, 0.02759901,  0.30843697,  0.4691384,  0.30843697,
        0.02759901,  -0.07939037, -0.01941656, 0.02719209, 0.01028936,
        -0.00750668, -0.00304101};
    static constexpr auto palMUvFilter = makeFIRFilter(palMUvFilterCoeffs);

    const auto& uvFilter = (videoParameters.system == orc::VideoSystem::PAL)
                               ? palUvFilter
                               : palMUvFilter;

    const int32_t overlap = palUvFilterCoeffs.size() / 2;
    const int32_t startPos = videoParameters.active_video_start - overlap;
    const int32_t endPos2 = videoParameters.active_video_end + overlap + 1;

    // Multiply the composite input signal by the reference carrier, giving
    // quadrature samples where the colour subcarrier is now at 0 Hz
    double m[MAX_WIDTH], n[MAX_WIDTH];
    for (int32_t i = startPos; i < endPos2; i++) {
      m[i] = in0[i] * sine[i];
      n[i] = in0[i] * cosine[i];
    }

    // Apply the filter to U, and copy the result to V
    uvFilter.apply(&m[startPos], &pu[startPos], endPos2 - startPos);
    uvFilter.apply(&n[startPos], &qu[startPos], endPos2 - startPos);
    for (int32_t i = videoParameters.active_video_start; i < endPos2; i++) {
      pv[i] = pu[i];
      qv[i] = qu[i];
    }
  } else {
    // Use PALcolour's 2D filter
    // For composite (int16_t) input, we also compute py/qy for luma extraction

    // Multiply the composite input signal by the reference carrier, giving
    // quadrature samples where the colour subcarrier is now at 0 Hz.
    //
    // Vertical taps 1 and 2 are swapped in the array to save one addition
    // in the filter loop, as U and V use the same sign for taps 0 and 2.
    double m[4][MAX_WIDTH], n[4][MAX_WIDTH];
    const auto endPos2 =
        std::min(videoParameters.active_video_end + FILTER_SIZE + 1, MAX_WIDTH);
    for (int32_t i = videoParameters.active_video_start - FILTER_SIZE;
         i < endPos2; i++) {
      // Promote int16_t to int32_t before subtraction/addition to avoid
      // signed overflow on narrow sample types.
      m[0][i] = in0[i] * sine[i];
      m[2][i] = (static_cast<double>(in1[i]) - in2[i]) * sine[i];
      m[1][i] = -(static_cast<double>(in3[i]) + in4[i]) * sine[i];
      m[3][i] = (-(static_cast<double>(in5[i])) + in6[i]) * sine[i];

      n[0][i] = in0[i] * cosine[i];
      n[2][i] = (static_cast<double>(in1[i]) - in2[i]) * cosine[i];
      n[1][i] = -(static_cast<double>(in3[i]) + in4[i]) * cosine[i];
      n[3][i] = (-(static_cast<double>(in5[i])) + in6[i]) * cosine[i];
    }

    for (int32_t i = videoParameters.active_video_start; i < endPos; i++) {
      double PU = 0, QU = 0, PV = 0, QV = 0, PY = 0, QY = 0;

      for (int32_t b = 0; b <= FILTER_SIZE; b++) {
        const int32_t l = i - b;
        const int32_t r = i + b;

        PY += (m[0][r] + m[0][l]) * yfilt[b][0] +
              (m[1][r] + m[1][l]) * yfilt[b][1];
        QY += (n[0][r] + n[0][l]) * yfilt[b][0] +
              (n[1][r] + n[1][l]) * yfilt[b][1];

        PU += (m[0][r] + m[0][l]) * cfilt[b][0] +
              (m[1][r] + m[1][l]) * cfilt[b][1] +
              (n[2][r] + n[2][l]) * cfilt[b][2] +
              (n[3][r] + n[3][l]) * cfilt[b][3];
        QU += (n[0][r] + n[0][l]) * cfilt[b][0] +
              (n[1][r] + n[1][l]) * cfilt[b][1] -
              (m[2][r] + m[2][l]) * cfilt[b][2] -
              (m[3][r] + m[3][l]) * cfilt[b][3];
        PV += (m[0][r] + m[0][l]) * cfilt[b][0] +
              (m[1][r] + m[1][l]) * cfilt[b][1] -
              (n[2][r] + n[2][l]) * cfilt[b][2] -
              (n[3][r] + n[3][l]) * cfilt[b][3];
        QV += (n[0][r] + n[0][l]) * cfilt[b][0] +
              (n[1][r] + n[1][l]) * cfilt[b][1] +
              (m[2][r] + m[2][l]) * cfilt[b][2] +
              (m[3][r] + m[3][l]) * cfilt[b][3];
      }

      pu[i] = PU;
      qu[i] = QU;
      pv[i] = PV;
      qv[i] = QV;
      py[i] = PY;
      qy[i] = QY;
    }
  }

  // Composite signal for luma computation (int16_t path); accessed via
  // getLine() to handle PAL non-uniform line lengths correctly.
  const int16_t* comp = inputField.getLine(static_cast<size_t>(line.number));

  // Pointers to component output
  const int32_t absoluteLineNumber = (line.number * 2) + inputField.getOffset();
  const int32_t lineNumber =
      videoParameters.active_area_cropping_applied
          ? (absoluteLineNumber - videoParameters.first_active_frame_line)
          : absoluteLineNumber;
  double* outY = componentFrame.y(lineNumber);
  double* outU = componentFrame.u(lineNumber);
  double* outV = componentFrame.v(lineNumber);

  for (int32_t i = videoParameters.active_video_start; i < endPos; i++) {
    int32_t outIdx = videoParameters.active_area_cropping_applied
                         ? (i - videoParameters.active_video_start)
                         : i;

    // Compute luma by...
    if (PREFILTERED_CHROMA) {
      // ... subtracting pre-filtered chroma from the composite input
      outY[outIdx] = comp[i] - in0[i];
    } else {
      // ... resynthesising the chroma signal that the Y filter
      // extracted (at half amplitude), and subtracting it from the
      // composite input
      outY[outIdx] = comp[i] - ((py[i] * sine[i] + qy[i] * cosine[i]) * 2.0);
    }

    // Rotate the p&q components (at the arbitrary sine/cosine
    // reference phase) backwards by the burst phase (relative to the
    // reference phase), in order to recover U and V. The Vswitch is
    // applied to flip the V-phase on alternate lines for PAL.
    // The result is doubled because the filter extracts the chroma signal
    // at half amplitude.
    outU[outIdx] = -(pu[i] * line.bp + qu[i] * line.bq) * 2.0;
    outV[outIdx] = line.Vsw * -(qv[i] * line.bp - pv[i] * line.bq) * 2.0;
  }
}
