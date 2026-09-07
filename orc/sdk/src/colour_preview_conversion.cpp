/*
 * File:        colour_preview_conversion.cpp
 * Module:      orc-sdk-support
 * Purpose:     Render-boundary conversion from colour carriers to PreviewImage.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <orc/support/colour_preview_conversion.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace orc {
namespace {

struct MatrixCoefficients {
  double kr;
  double kb;
};

// Composite modulation factors: the decoders hand this conversion the U/V
// that ride on the subcarrier, which are the colour-difference signals scaled
// down so the composite signal stays within its excursion limits
//
//   U = kU (B' - Y')    kU = sqrt(209556997 / 96146491) / 3
//   V = kV (R' - Y')    kV = sqrt(221990474 / 288439473)
//
// [ITU-R BT.470-6 s1.1.2 / EBU Tech. 3280-E s2.1, Poynton eq 28.1 p336].
// They must be divided out before the Y'CbCr matrix is applied, exactly as
// OutputWriter does on the export path; applying the matrix to the weighted
// signals over-drives R' by 1/(kV (2 - 2 kr)) ~ 23% and under-drives B'.
inline constexpr double kCompositeU = 0.49211104112248356308804691718185;
inline constexpr double kCompositeV = 0.87728321993817866838972487283129;

MatrixCoefficients coefficients_for(ColorimetricMatrixCoefficients matrix) {
  switch (matrix) {
    case ColorimetricMatrixCoefficients::BT601_625:
    case ColorimetricMatrixCoefficients::BT601_525:
      return {0.2990, 0.1140};
    case ColorimetricMatrixCoefficients::NTSC1953_FCC:
      return {0.3000, 0.1100};
    case ColorimetricMatrixCoefficients::Unspecified:
    default:
      return {0.2990, 0.1140};
  }
}

double decode_transfer_to_linear(double value,
                                 ColorimetricTransferCharacteristics transfer) {
  value = std::clamp(value, 0.0, 1.0);

  switch (transfer) {
    case ColorimetricTransferCharacteristics::Gamma22:
      return std::pow(value, 2.2);
    case ColorimetricTransferCharacteristics::Gamma28:
      return std::pow(value, 2.8);
    case ColorimetricTransferCharacteristics::BT709:
      if (value < 0.081) {
        return value / 4.5;
      }
      return std::pow((value + 0.099) / 1.099, 1.0 / 0.45);
    case ColorimetricTransferCharacteristics::BT1886:
    case ColorimetricTransferCharacteristics::BT1886App1:
      return std::pow(value, 2.4);
    case ColorimetricTransferCharacteristics::Unspecified:
    default:
      return std::pow(value, 2.2);
  }
}

double encode_linear_to_srgb(double linear) {
  linear = std::clamp(linear, 0.0, 1.0);

  if (linear <= 0.0031308) {
    return 12.92 * linear;
  }

  return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

uint8_t to_u8(double value) {
  value = std::clamp(value, 0.0, 1.0);
  const double scaled = value * 255.0 + 0.5;
  return static_cast<uint8_t>(scaled);
}

// ---------------------------------------------------------------------------
// Transfer-function lookup tables
// ---------------------------------------------------------------------------
//
// decode_transfer_to_linear() followed by encode_linear_to_srgb() is a pure,
// monotonic function of one scalar, so the pair collapses into a single table.
// Evaluating them directly costs two std::pow per component — six per pixel,
// over four million per PAL frame — which dominated colour preview rendering.
//
// The composed curve is smooth apart from its piecewise knees, so linear
// interpolation between 4096 samples reconstructs it well below the 1/255 step
// of the 8-bit output. Measured against direct evaluation over two million
// inputs per characteristic: peak reconstruction error 2.0e-4 of full scale
// (the BT.709 knee at 0.081, which falls between two table nodes) and 1.0e-6
// elsewhere. That leaves the rendered byte identical for all but ~0.0002 % of
// inputs, which land within half a code of a rounding boundary and come out one
// code away; no input differs by more than one code. See the accuracy
// assertions in preview_carrier_conversion_test.cpp.
constexpr size_t kTransferLutIntervals = 4096;

class TransferLut {
 public:
  explicit TransferLut(ColorimetricTransferCharacteristics transfer) {
    for (size_t i = 0; i <= kTransferLutIntervals; ++i) {
      const double nl =
          static_cast<double>(i) / static_cast<double>(kTransferLutIntervals);
      samples_[i] =
          encode_linear_to_srgb(decode_transfer_to_linear(nl, transfer));
    }
  }

  // @p non_linear must already be clamped to [0, 1].
  double encode(double non_linear) const {
    const double scaled =
        non_linear * static_cast<double>(kTransferLutIntervals);
    const size_t index = static_cast<size_t>(scaled);
    if (index >= kTransferLutIntervals) {
      return samples_[kTransferLutIntervals];
    }
    const double fraction = scaled - static_cast<double>(index);
    return samples_[index] + fraction * (samples_[index + 1] - samples_[index]);
  }

 private:
  std::array<double, kTransferLutIntervals + 1> samples_{};
};

// One table per transfer characteristic, built on first use. The tables are
// immutable once constructed, so the function-local statics are safe to share
// across the render threads that reach this conversion.
const TransferLut& transfer_lut_for(
    ColorimetricTransferCharacteristics transfer) {
  switch (transfer) {
    case ColorimetricTransferCharacteristics::Gamma22: {
      static const TransferLut lut(
          ColorimetricTransferCharacteristics::Gamma22);
      return lut;
    }
    case ColorimetricTransferCharacteristics::Gamma28: {
      static const TransferLut lut(
          ColorimetricTransferCharacteristics::Gamma28);
      return lut;
    }
    case ColorimetricTransferCharacteristics::BT709: {
      static const TransferLut lut(ColorimetricTransferCharacteristics::BT709);
      return lut;
    }
    case ColorimetricTransferCharacteristics::BT1886: {
      static const TransferLut lut(ColorimetricTransferCharacteristics::BT1886);
      return lut;
    }
    case ColorimetricTransferCharacteristics::BT1886App1: {
      static const TransferLut lut(
          ColorimetricTransferCharacteristics::BT1886App1);
      return lut;
    }
    case ColorimetricTransferCharacteristics::Unspecified:
    default: {
      static const TransferLut lut(
          ColorimetricTransferCharacteristics::Unspecified);
      return lut;
    }
  }
}

}  // namespace

PreviewImage render_preview_from_colour_carrier(
    const ColourFrameCarrier& carrier) {
  PreviewImage image{};

  if (!carrier.is_valid()) {
    return image;
  }

  image.width = carrier.width;
  image.height = carrier.height;
  image.rgb_data.resize(static_cast<size_t>(carrier.width) *
                        static_cast<size_t>(carrier.height) * 3);

  const MatrixCoefficients matrix =
      coefficients_for(carrier.colorimetry.matrix_coefficients);
  const double kg = 1.0 - matrix.kr - matrix.kb;

  // CVBS_U10_4FSC normative levels from carrier (set by chroma_sink).
  // Y is normalized from picture-black (not blanking) so that sub-black content
  // (below the setup pedestal on NTSC/PAL_M) clamps to zero rather than
  // rendering as a dark visible level.
  //
  // U/V are normalized over the same black-to-white excursion, not over
  // blanking-to-white.  The setup pedestal does not offset chroma, but it does
  // shorten the picture excursion that defines the volts per unit of
  // colour difference, so the subcarrier amplitude shrinks with it: on a
  // 7.5 IRE setup system one unit of Y'/U/V spans 92.5 IRE, not 100.  Dividing
  // by blanking-to-white under-recovers NTSC and PAL-M chroma by 7.5%.
  // Cross-checked against the EIA-189-A 75% bars, whose chroma peak-to-peak
  // amplitudes (62 / 88 / 82 IRE for yellow / cyan / green) only come out
  // right on the black-to-white excursion.  PAL has no pedestal, so the two
  // ranges coincide there.  This matches the FFmpeg export path.
  const double y_range = carrier.cvbs_white - carrier.cvbs_black;
  const double uv_range = y_range;

  // Transfer decode and sRGB encode are resolved once per frame into a single
  // interpolated table; the pixel loop then costs no transcendentals at all.
  const TransferLut& transfer =
      transfer_lut_for(carrier.colorimetry.transfer_characteristics);

  const size_t samples =
      static_cast<size_t>(carrier.width) * static_cast<size_t>(carrier.height);
  for (size_t i = 0; i < samples; ++i) {
    double y = (carrier.y_plane[i] - carrier.cvbs_black) / y_range;

    // Un-weight the modulated U/V back into colour-difference signals, then
    // reconstruct R'G'B' from Y' and the two differences:
    //   R' = Y' + (R' - Y')
    //   B' = Y' + (B' - Y')
    //   G' = Y' - (kb (B' - Y') + kr (R' - Y')) / kg   [from the Y' equation]
    const double b_minus_y = (carrier.u_plane[i] / uv_range) / kCompositeU;
    const double r_minus_y = (carrier.v_plane[i] / uv_range) / kCompositeV;

    double r_nl = y + r_minus_y;
    double b_nl = y + b_minus_y;
    double g_nl = y - ((matrix.kb * b_minus_y) + (matrix.kr * r_minus_y)) / kg;

    r_nl = std::clamp(r_nl, 0.0, 1.0);
    g_nl = std::clamp(g_nl, 0.0, 1.0);
    b_nl = std::clamp(b_nl, 0.0, 1.0);

    const double r_srgb = transfer.encode(r_nl);
    const double g_srgb = transfer.encode(g_nl);
    const double b_srgb = transfer.encode(b_nl);

    const size_t pixel = i * 3;
    image.rgb_data[pixel + 0] = to_u8(r_srgb);
    image.rgb_data[pixel + 1] = to_u8(g_srgb);
    image.rgb_data[pixel + 2] = to_u8(b_srgb);
  }

  return image;
}

void reorder_preview_image_to_sequential_fields(PreviewImage& image) {
  if (!image.is_valid() || image.height < 2) {
    return;
  }

  const size_t row_bytes = static_cast<size_t>(image.width) * 3;
  const uint32_t field1_rows = (image.height + 1) / 2;

  // Weaved row → sequential row: even rows (field 1) map to the top block,
  // odd rows (field 2) to the bottom block.
  auto sequential_row = [field1_rows](uint32_t weaved_row) -> uint32_t {
    return (weaved_row % 2 == 0) ? (weaved_row / 2)
                                 : (field1_rows + weaved_row / 2);
  };

  std::vector<uint8_t> reordered(image.rgb_data.size());
  for (uint32_t row = 0; row < image.height; ++row) {
    const size_t src = static_cast<size_t>(row) * row_bytes;
    const size_t dst = static_cast<size_t>(sequential_row(row)) * row_bytes;
    std::copy_n(image.rgb_data.begin() + static_cast<std::ptrdiff_t>(src),
                row_bytes,
                reordered.begin() + static_cast<std::ptrdiff_t>(dst));
  }
  image.rgb_data = std::move(reordered);

  for (auto& region : image.dropout_regions) {
    if (region.line < image.height) {
      region.line = sequential_row(region.line);
    }
  }
}

}  // namespace orc
