/*
 * File:        orc_preview_types.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Shared preview taxonomy: video data types, colorimetric
 * metadata, and the shared preview coordinate model.
 *
 *              Phase 1 of the preview-refactor plan.  These types establish the
 *              explicit six-value video data taxonomy and the colorimetric
 *              metadata carrier for colour-domain types, replacing the implicit
 *              inference previously done from PreviewOutputType.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: stage/preview — stage contract type crossing the plugin boundary.
// A layout change here bumps the host ABI version.

#include <orc/stage/preview/orc_vectorscope.h>

#include <cstdint>

namespace orc {

// =============================================================================
// Video Data Taxonomy
// =============================================================================

/**
 * @brief The six first-class video data types a stage can expose for preview.
 *
 * Signal-domain types (Composite*, YC_*) are carried by
 * VideoFrameRepresentation accessed via
 * DAGFrameRenderer::render_frame_at_node().  Colour-domain types (Colour*) are
 * carried by the structured decoder-output carrier which includes
 * ColorimetricMetadata.
 *
 * The NTSC/PAL distinction within a type affects video dimensions and a small
 * number of signal parameters (field rate, FSC, IRE scale); the structural
 * differences between signal-domain and colour-domain types are independent of
 * the video system standard.
 */
enum class VideoDataType {
  CompositeNTSC,  ///< Single 16-bit sample stream (mV); NTSC field dimensions
  CompositePAL,   ///< Single 16-bit sample stream (mV); PAL field dimensions
  YC_NTSC,  ///< Two 16-bit sample streams (Y luma + C chroma, mV); NTSC field
            ///< dimensions
  YC_PAL,   ///< Two 16-bit sample streams (Y luma + C chroma, mV); PAL field
            ///< dimensions
  ColourNTSC,  ///< Decoded colour frame with colorimetric metadata; NTSC active
               ///< picture dimensions
  ColourPAL,   ///< Decoded colour frame with colorimetric metadata; PAL active
               ///< picture dimensions
};

// =============================================================================
// Colorimetric Metadata
// =============================================================================

/**
 * @brief Matrix coefficients used to convert between YUV and RGB.
 *
 * Values correspond to the equivalent ISO/IEC 23091-2 / H.273 indexes.
 * The Unspecified value is the safe default for a default-constructed or
 * partially-populated ColorimetricMetadata.
 */
enum class ColorimetricMatrixCoefficients {
  Unspecified,   ///< Not specified / unknown
  NTSC1953_FCC,  ///< NTSC-1953 / FCC (coefficient 4) — early NTSC recordings
  BT601_625,     ///< BT.470-6 625-line / BT.601 PAL (coefficient 5)
  BT601_525,     ///< BT.470-6 525-line / BT.601 NTSC / ST 170M (coefficient 6)
};

/**
 * @brief Colour primaries defining the gamut of the colour space.
 *
 * Identifies the chromaticity coordinates of the red, green, and blue
 * primaries. The Unspecified value is the safe default for a
 * default-constructed or partially-populated ColorimetricMetadata.
 */
enum class ColorimetricPrimaries {
  Unspecified,  ///< Not specified / unknown
  NTSC1953,     ///< NTSC-1953 primaries (early CRT phosphors)
  SMPTE_C,  ///< SMPTE C primaries (NTSC analog broadcasts, c. 1979 – present)
  EBU_BT470_PAL,  ///< EBU / BT.470-6 PAL primaries
  BT709,          ///< BT.709 primaries (HD and modern SDR display target)
};

/**
 * @brief Electro-optical / opto-electronic transfer characteristics.
 *
 * Defines the relationship between scene luminance and encoded signal value.
 * The Unspecified value is the safe default for a default-constructed or
 * partially-populated ColorimetricMetadata.
 */
enum class ColorimetricTransferCharacteristics {
  Unspecified,  ///< Not specified / unknown
  Gamma22,      ///< 2.2 γ — NTSC, ST 170M, BT.470 525-line
  Gamma28,      ///< 2.8 γ — BT.470 625-line / PAL
  BT709,        ///< BT.709 OETF
  BT1886,       ///< BT.1886 (effective 2.4 γ)
  BT1886App1,   ///< BT.1886 Appendix 1 (camera-referred OETF)
};

/**
 * @brief Colorimetric description of a decoded colour signal.
 *
 * Carried as part of colour-domain types (ColourNTSC / ColourPAL).  Drives
 * two downstream concerns:
 *
 *  1. Preview display conversion — the rendering path must convert from the
 *     declared colorimetry to BT.709 primaries + sRGB transfer for display.
 *  2. Downstream sink handoff — the declared matrix/primaries/transfer must be
 *     part of the chroma-decoder output contract passed to sink stages.
 *
 * A default-constructed ColorimetricMetadata has all three fields set to
 * Unspecified, which is a valid sentinel meaning "caller has not declared
 * colorimetry".  Concrete factory methods default_ntsc() and default_pal()
 * return the historically-correct starting assumptions for each system.
 */
struct ColorimetricMetadata {
  ColorimetricMatrixCoefficients matrix_coefficients{
      ColorimetricMatrixCoefficients::Unspecified};
  ColorimetricPrimaries primaries{ColorimetricPrimaries::Unspecified};
  ColorimetricTransferCharacteristics transfer_characteristics{
      ColorimetricTransferCharacteristics::Unspecified};

  /**
   * @brief Historically-correct default colorimetry for analog NTSC recordings.
   *
   * Based on SMPTE C primaries, BT.601-525 matrix, and 2.2 gamma transfer,
   * which reflects the majority of NTSC consumer and broadcast recordings
   * from the late 1970s onwards.  Earlier NTSC recordings (pre-SMPTE-C era)
   * may use NTSC1953_FCC matrix and NTSC1953 primaries instead.
   */
  static ColorimetricMetadata default_ntsc() {
    return {
        ColorimetricMatrixCoefficients::BT601_525,
        ColorimetricPrimaries::SMPTE_C,
        ColorimetricTransferCharacteristics::Gamma22,
    };
  }

  /**
   * @brief Historically-correct default colorimetry for analog PAL-M
   *        recordings.
   *
   * PAL-M carries PAL chroma encoding on the 525-line System M raster, and
   * BT.470 assigns colorimetry by system rather than by colour encoding: it is
   * System M throughout, so the 2.2 gamma transfer and SMPTE C primaries
   * apply, not the 2.8 gamma of the 625-line systems.  The matrix is the same
   * BT.601 one either way.
   */
  static ColorimetricMetadata default_pal_m() {
    return {
        ColorimetricMatrixCoefficients::BT601_525,
        ColorimetricPrimaries::SMPTE_C,
        ColorimetricTransferCharacteristics::Gamma22,
    };
  }

  /**
   * @brief Historically-correct default colorimetry for analog PAL recordings.
   *
   * Based on EBU/BT.470 PAL primaries, BT.601-625 matrix, and 2.8 gamma
   * transfer as specified in BT.470 for 625-line systems.  PAL-M is a System M
   * signal despite its PAL chroma encoding — use default_pal_m() for it.
   */
  static ColorimetricMetadata default_pal() {
    return {
        ColorimetricMatrixCoefficients::BT601_625,
        ColorimetricPrimaries::EBU_BT470_PAL,
        ColorimetricTransferCharacteristics::Gamma28,
    };
  }

  bool operator==(const ColorimetricMetadata& other) const {
    return matrix_coefficients == other.matrix_coefficients &&
           primaries == other.primaries &&
           transfer_characteristics == other.transfer_characteristics;
  }

  bool operator!=(const ColorimetricMetadata& other) const {
    return !(*this == other);
  }
};

// =============================================================================
// Shared Preview Coordinate Model
// =============================================================================

/**
 * @brief A position within preview data shared across all supplementary views.
 *
 * Coordinates are always in the space of the active picture, fully independent
 * of the specific rendering dimensions used by any particular view.  The
 * data_type_context records which of the six VideoDataType values the
 * coordinate was derived from so that views can interpret line/sample units
 * correctly.
 *
 * All indices are 0-based.  Full bounds validation (against actual field count,
 * line count, or sample width) requires video-system context and is left to
 * calling code; is_valid() only performs structural sentinel checks.
 */
struct PreviewCoordinate {
  uint64_t field_index{0};    ///< Absolute field index within the source
  uint32_t line_index{0};     ///< Line within the active picture (0-based)
  uint32_t sample_offset{0};  ///< Horizontal sample within that line (0-based)
  VideoDataType data_type_context{
      VideoDataType::CompositeNTSC};  ///< Data type from which this coordinate
                                      ///< was captured
  /// Vectorscope line select: true restricts the acquisition to the active
  /// picture lines, false plots every line of the frame.  Applies to both
  /// acquisitions, and intersects with the explicit range below.
  bool vectorscope_active_area_only{true};

  /// Portion of each line a composite-carrier vectorscope acquisition samples.
  ///
  /// Which acquisition runs is not part of the request: it follows the data
  /// type being previewed — a colour-domain type has decoder output to plot,
  /// a signal-domain type has a carrier to demodulate — so there is nothing
  /// for a caller to choose.
  VectorscopeSampleWindow vectorscope_window{
      VectorscopeSampleWindow::WholeLine};

  /// Inclusive interlaced frame-line range (0-based) for either acquisition:
  /// line 0 is the top line of the frame and consecutive numbers alternate
  /// fields.  vectorscope_last_line == 0 means "to the last line of the
  /// frame".
  uint32_t vectorscope_first_line{0};
  uint32_t vectorscope_last_line{0};

  /**
   * @brief Structural validity check.
   *
   * Catches sentinel/wrapping values (UINT64_MAX / UINT32_MAX) that
   * indicate uninitialized or arithmetic-overflowed coordinates.  A
   * default-constructed coordinate (all zero) is considered valid.
   */
  bool is_valid() const {
    return field_index != UINT64_MAX && line_index != UINT32_MAX &&
           sample_offset != UINT32_MAX;
  }

  bool operator==(const PreviewCoordinate& other) const {
    return field_index == other.field_index && line_index == other.line_index &&
           sample_offset == other.sample_offset &&
           data_type_context == other.data_type_context &&
           vectorscope_active_area_only == other.vectorscope_active_area_only &&
           vectorscope_window == other.vectorscope_window &&
           vectorscope_first_line == other.vectorscope_first_line &&
           vectorscope_last_line == other.vectorscope_last_line;
  }

  bool operator!=(const PreviewCoordinate& other) const {
    return !(*this == other);
  }
};

}  // namespace orc
