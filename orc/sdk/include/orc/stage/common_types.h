/*
 * File:        common_types.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Common type definitions shared across all layers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/stage/field_id.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace orc {

// Re-export core types for convenience
// These are defined in their respective headers
// This file just provides a single include point for common types

// ============================================================================
// Video system and source type enums
// ============================================================================

/**
 * @brief Video format/system
 *
 * SECAM is appended after Unknown so that the released values keep their
 * numbers (ABI 21). Code that means "625-line" rather than "PAL colour" must
 * test is_625_line() or raster_system(), never `== VideoSystem::PAL`.
 */
enum class VideoSystem {
  PAL,    // 625-line PAL
  NTSC,   // 525-line NTSC
  PAL_M,  // 525-line PAL
  Unknown,
  SECAM  // 625-line SECAM (FM chroma on two sub-carriers)
};

/// Source type for parameter filtering
enum class SourceType {
  Unknown,    // Source type not determined yet
  Composite,  // Composite source (Y+C modulated together, e.g., .tbc files)
  YC          // YC source (separate Y and C channels, e.g., .tbcy/.tbcc files)
};

// ============================================================================
// Video format coarse bucket (PAL vs NTSC)
// ============================================================================

inline std::string video_system_to_string(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
      return "PAL";
    case VideoSystem::NTSC:
      return "NTSC";
    case VideoSystem::PAL_M:
      return "PAL-M";
    case VideoSystem::SECAM:
      return "SECAM";
    case VideoSystem::Unknown:
      break;
  }
  return "Unknown";
}

inline VideoSystem video_system_from_string(const std::string& name) {
  if (name == "PAL") return VideoSystem::PAL;
  if (name == "NTSC") return VideoSystem::NTSC;
  if (name == "PAL_M" || name == "PAL-M") return VideoSystem::PAL_M;
  if (name == "SECAM") return VideoSystem::SECAM;
  return VideoSystem::Unknown;
}

// ============================================================================
// Raster (line and field geometry) of a video system
// ============================================================================

/// True for the 625-line systems (PAL and SECAM).
/// ITU-R BT.1700-1 Annex 1 Part C: SECAM has the line and field timing of
/// 625-line PAL. Use this where a branch means "625-line", not "PAL colour".
inline bool is_625_line(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
    case VideoSystem::SECAM:
      return true;
    case VideoSystem::NTSC:
    case VideoSystem::PAL_M:
    case VideoSystem::Unknown:
      return false;
  }
  return false;
}

/// The system whose geometry a system's raster has: PAL for SECAM, the system
/// itself otherwise. CVBS File Format Specification v1.8.1,
/// video-standard-presets.md, Preset SECAM: the SECAM storage lattice is
/// identical to the PAL 4fsc lattice (1135.0064 samples a line, 709,379 a
/// frame, the same stored origin). Use it only where geometry or timing is
/// meant (line counts, field split, lattice, frame size and rate, audio
/// cadence, active window, VBI line numbers); colour still needs the real
/// system.
inline VideoSystem raster_system(VideoSystem system) {
  switch (system) {
    case VideoSystem::SECAM:
      return VideoSystem::PAL;
    case VideoSystem::PAL:
    case VideoSystem::NTSC:
    case VideoSystem::PAL_M:
    case VideoSystem::Unknown:
      return system;
  }
  return system;
}

/// Coarse two-bucket video format. Prefer VideoSystem for new code.
enum class VideoFormat { NTSC, PAL, Unknown };

// ============================================================================
// Amplitude display unit
// ============================================================================

/**
 * @brief Project-level amplitude display unit preference.
 *
 * All internal storage uses 10-bit sample values (CVBS_U10_4FSC domain).
 * This enum governs how those values are presented to the user.
 *
 * Default per system: IRE for NTSC (SMPTE 170M-2004), mV for PAL/PAL-M
 * (EBU Tech. 3280-E).
 */
enum class AmplitudeDisplayUnit {
  IRE,  ///< Percentage of full active-video amplitude (0=blanking, 100=white)
  Millivolts,   ///< mV referenced to blanking (700 mV PAL, 714.3 mV NTSC/PAL-M)
  Samples10Bit  ///< Raw 10-bit CVBS_U10_4FSC sample value (0-1023)
};

inline std::string amplitude_unit_to_string(AmplitudeDisplayUnit u) {
  switch (u) {
    case AmplitudeDisplayUnit::IRE:
      return "IRE";
    case AmplitudeDisplayUnit::Millivolts:
      return "mV";
    case AmplitudeDisplayUnit::Samples10Bit:
      return "10bit";
    default:
      return "IRE";
  }
}

inline AmplitudeDisplayUnit amplitude_unit_from_string(const std::string& s) {
  if (s == "mV") return AmplitudeDisplayUnit::Millivolts;
  if (s == "10bit") return AmplitudeDisplayUnit::Samples10Bit;
  return AmplitudeDisplayUnit::IRE;
}

/// Return the conventional display unit for a given video system.
/// NTSC convention: IRE (SMPTE 170M-2004).
/// PAL / PAL-M / SECAM convention: mV (EBU Tech. 3280-E; ITU-R BT.1700-1
/// Annex 1 Part C Table 5 gives SECAM levels in mV).
/// Unknown: 10-bit samples (no video-system convention assumed).
inline AmplitudeDisplayUnit default_amplitude_unit(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
    case VideoSystem::PAL_M:
    case VideoSystem::SECAM:
      return AmplitudeDisplayUnit::Millivolts;
    case VideoSystem::NTSC:
      return AmplitudeDisplayUnit::IRE;
    case VideoSystem::Unknown:
      break;
  }
  return AmplitudeDisplayUnit::Samples10Bit;
}

// ITU-R BT.1700-1 / SMPTE 170M-2004: map VideoSystem to coarse PAL/NTSC bucket.
// SECAM falls in the PAL bucket (ITU-R BT.1700-1 Annex 1 Part C: 625-line
// timing).
inline VideoFormat video_format_from_system(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
    case VideoSystem::PAL_M:
    case VideoSystem::SECAM:
      return VideoFormat::PAL;
    case VideoSystem::NTSC:
      return VideoFormat::NTSC;
    case VideoSystem::Unknown:
      break;
  }
  return VideoFormat::Unknown;
}

// EBU Tech. 3280-E / SMPTE 244M-2003: padded field height for frame-flat
// storage (both fields equal length, matching ld-decode .tbc file convention).
inline size_t calculate_padded_field_height(VideoSystem system) {
  switch (system) {
    case VideoSystem::NTSC:
    case VideoSystem::PAL_M:
      return 263;  // SMPTE 244M-2003: 525-line, second field = 263 lines
    case VideoSystem::PAL:
    case VideoSystem::SECAM:
      return 313;  // EBU Tech. 3280-E: 625-line, second field = 313 lines
    case VideoSystem::Unknown:
      break;
  }
  return 0;
}

// ============================================================================
// Preview and rendering types
// ============================================================================

/**
 * @brief Output types available for preview
 */
enum class PreviewOutputType {
  Frame_Field1,  ///< Display field 1 of the frame as a flat (non-interlaced)
                 ///< image
  Frame_Field2,  ///< Display field 2 of the frame as a flat image
  Frame_Field1_First,  ///< Interlaced frame display, field 1 on top (natural
                       ///< temporal order)
  Frame_Reversed,      ///< Interlaced frame display, field 2 on top (reversed
                       ///< temporal order)
  Split,     ///< Frame with field 1 stacked above field 2 (non-interlaced)
  Luma,      ///< Luma component only
  Chroma,    ///< Chroma component only (future)
  Composite  ///< Composite video (future)
};

/**
 * @brief Aspect ratio display modes
 */
enum class AspectRatioMode {
  SAR_1_1,  ///< Sample Aspect Ratio 1:1 (square pixels, no correction)
  DAR_4_3   ///< Display Aspect Ratio 4:3 (corrected for non-square pixels)
};

// ============================================================================
// Analysis mode enums
// ============================================================================

/**
 * @brief Mode for SNR analysis
 */
enum class SNRAnalysisMode {
  WHITE,  ///< Analyze white SNR only
  BLACK,  ///< Analyze black PSNR only
  BOTH    ///< Analyze both white SNR and black PSNR
};

/**
 * @brief Mode for dropout analysis
 */
enum class DropoutAnalysisMode {
  FULL_FIELD,   ///< Analyze dropouts across the entire field
  VISIBLE_AREA  ///< Analyze dropouts only in the visible area
};

// ============================================================================
// Analysis result types - shared between core and GUI/CLI layers
// ============================================================================

/**
 * @brief Dropout statistics for a single field
 */
struct FieldDropoutStats {
  FieldID field_id;
  double total_dropout_length = 0.0;    ///< Total dropout length in samples
  size_t dropout_count = 0;             ///< Number of dropout regions
  std::optional<int32_t> frame_number;  ///< Frame number if available from VBI
  bool has_data = false;  ///< True if dropout data was successfully extracted
};

/**
 * @brief Dropout statistics for a single analysed frame
 *
 * One record represents exactly one analysed frame. Counts and lengths are the
 * integer totals measured in that frame; no aggregation across frames occurs at
 * capture time (display decimation is applied separately by the graph path).
 */
struct FrameDropoutStats {
  int32_t frame_number = 0;   ///< Analysed frame number (1-based)
  int32_t dropout_count = 0;  ///< Number of dropout runs in this frame
  int64_t dropout_length_samples =
      0;                  ///< Total dropout length in this frame (samples)
  bool has_data = false;  ///< True if this frame contained one or more dropouts
};

/**
 * @brief SNR statistics for a single field
 */
struct FieldSNRStats {
  FieldID field_id;
  double white_snr = 0.0;       ///< White SNR value (dB)
  double black_psnr = 0.0;      ///< Black PSNR value (dB)
  bool has_white_snr = false;   ///< True if white SNR data is available
  bool has_black_psnr = false;  ///< True if black PSNR data is available
  std::optional<int32_t> frame_number;  ///< Frame number if available from VBI
  bool has_data = false;  ///< True if any SNR data was successfully extracted
};

/**
 * @brief SNR statistics for a single analysed frame
 *
 * One record represents exactly one analysed frame. Values are that frame's
 * white SNR / black PSNR (averaged across the frame's two fields); no
 * aggregation across frames occurs at capture time.
 */
struct FrameSNRStats {
  int32_t frame_number = 0;     ///< Analysed frame number (1-based)
  double white_snr = 0.0;       ///< White SNR for this frame (dB)
  double black_psnr = 0.0;      ///< Black PSNR for this frame (dB)
  bool has_white_snr = false;   ///< True if white SNR data is available
  bool has_black_psnr = false;  ///< True if black PSNR data is available
  bool has_data = false;        ///< True if any SNR data was captured
};

/**
 * @brief Burst level statistics for a single field
 */
struct FieldBurstLevelStats {
  FieldID field_id;
  double median_burst_10bit =
      0.0;  ///< Median burst amplitude in 10-bit sample units
  std::optional<int32_t> frame_number;  ///< Frame number if available from VBI
  bool has_data =
      false;  ///< True if burst level data was successfully extracted
};

/**
 * @brief Burst level statistics for a single analysed frame
 *
 * One record represents exactly one analysed frame. The value is that frame's
 * median burst amplitude (averaged across the frame's two fields); no
 * aggregation across frames occurs at capture time.
 */
struct FrameBurstLevelStats {
  int32_t frame_number = 0;  ///< Analysed frame number (1-based)
  double median_burst_10bit =
      0.0;  ///< Median burst amplitude for this frame (10-bit sample units)
  bool has_data = false;  ///< True if burst level data was captured
};

}  // namespace orc
