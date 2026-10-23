/*
 * File:        frame_quality_score.h
 * Module:      orc-core/analysis
 * Purpose:     Frame signal-quality scoring used to pick between duplicate
 *              disc pictures during disc mapping
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CORE_ANALYSIS_DISC_MAPPER_FRAME_QUALITY_SCORE_H
#define ORC_CORE_ANALYSIS_DISC_MAPPER_FRAME_QUALITY_SCORE_H

#include <orc/stage/common_types.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/observation/observation_context_interface.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace orc {

/**
 * @brief Per-frame signal-quality readings
 *
 * The first three readings come from the observation context and are
 * frame-scoped: BurstLevelObserver, WhiteSNRObserver and BlackPSNRObserver
 * each publish a single value per frame, keyed on the frame's first FieldID.
 * Any of them may be absent — the white/black VITS test signals are not
 * present on every disc, and the burst reading is dropped when no usable
 * burst window could be measured. The dropout share comes from the source's
 * dropout hints; a source that carries no hints reads as unflagged.
 */
struct FrameQualityMetrics {
  std::optional<double> median_burst_10bit;  ///< "burst_level"
  std::optional<double> white_snr_db;        ///< "white_snr"
  std::optional<double> black_psnr_db;       ///< "black_psnr"
  std::optional<double> dropout_fraction;    ///< flagged share of samples

  bool empty() const {
    return !median_burst_10bit && !white_snr_db && !black_psnr_db &&
           !dropout_fraction;
  }
};

// Score returned when a frame carries no quality observations at all. Sits at
// the midpoint so that scored and unscored frames are ordered by whether the
// scored one actually measured well, not merely by having been measured.
inline constexpr double kNeutralFrameQualityScore = 50.0;

// Nominal colour-burst peak amplitude in IRE, against the 0 IRE blanking /
// 100 IRE white reference. Halved from the spec-quoted peak-to-peak figures
// because BurstLevelObserver reports peak amplitude (RMS x sqrt(2)).

// ITU-R BT.1700 Annex 1 Part B Table item 5 (625 PAL): burst amplitude is
// 300 mV peak-to-peak; item 2 puts white level at 700 mV above blanking.
inline constexpr double kPalBurstPeakIre = (300.0 / 2.0) / 700.0 * 100.0;

// ITU-R BT.1700 Annex 1 Part B Table item 5 (525 PAL / PAL-M): burst
// amplitude is 316-317 mV peak-to-peak against the same 700 mV white level.
inline constexpr double kPalMBurstPeakIre = (316.5 / 2.0) / 700.0 * 100.0;

// SMPTE 170M-2004 Table 1: burst amplitude is 40 IRE peak-to-peak.
inline constexpr double kNtscBurstPeakIre = 40.0 / 2.0;

// Decibel window over which the SNR readings are mapped onto 0-100. Below the
// floor a LaserDisc capture is dominated by noise and the exact figure carries
// no ranking information; above the ceiling the reading is limited by the VITS
// reference itself rather than by disc condition.
inline constexpr double kSnrFloorDb = 20.0;
inline constexpr double kSnrCeilingDb = 48.0;

// Relative weights of the three readings. Burst level is weighted highest
// because it is the only reading available on every disc and it collapses
// directly with the RF dropouts that make one copy of a picture worse than
// another; the two SNR readings measure the same underlying noise floor from
// opposite ends of the luma range, so they share the remaining weight.
inline constexpr double kBurstQualityWeight = 0.4;
inline constexpr double kWhiteSnrQualityWeight = 0.3;
inline constexpr double kBlackPsnrQualityWeight = 0.3;

// The dropout share counts as much as the burst reading: the samples a copy
// has flagged are exactly what a stack built on it has to fill from the other
// discs, or leave as dropout where they cannot.
inline constexpr double kDropoutQualityWeight = 0.4;

// Share of a frame's samples flagged as dropout at which the dropout
// sub-score reaches zero. Undamaged LaserDisc frames flag well under 0.1 %.
inline constexpr double kDropoutFractionCeiling = 0.01;

/**
 * @brief Nominal colour-burst peak amplitude in the CVBS_U10_4FSC domain
 *
 * @param system Video system the source was captured as
 * @param blanking_level 0 IRE reference in 10-bit sample units
 * @param white_level 100 IRE reference in 10-bit sample units
 * @return Nominal peak amplitude, or nullopt when the level references are
 *         missing or inverted (both are -1 on sources with no parameters)
 */
std::optional<double> nominal_burst_peak_10bit(VideoSystem system,
                                               int32_t blanking_level,
                                               int32_t white_level);

/**
 * @brief Read the frame-scoped quality observations for one frame
 *
 * Absent or wrongly-typed observations are reported as nullopt rather than
 * treated as zero, so that a missing reading never looks like a bad one.
 *
 * @param context Observation context populated by the quality observers
 * @param frame_id Frame to read
 */
FrameQualityMetrics read_frame_quality_metrics(
    const IObservationContext& context, FrameID frame_id);

/**
 * @brief Share of a frame's samples its dropout hints flag
 *
 * @param runs The frame's dropout hints
 * @param frame_samples Samples in the frame
 * @return The flagged share, 0-1, or nullopt when the frame size is unknown
 */
std::optional<double> dropout_fraction(const std::vector<DropoutRun>& runs,
                                       size_t frame_samples);

/**
 * @brief Combine the quality readings into a single 0-100 score
 *
 * Each available reading contributes a 0-100 sub-score; the result is their
 * weighted mean, renormalised over whichever readings are present. Frames with
 * no readings score kNeutralFrameQualityScore, which keeps them comparable
 * with — but never preferred over — a frame that measured well.
 *
 * The burst sub-score penalises deviation from nominal in both directions: a
 * collapsed burst indicates an RF dropout over the back porch, an inflated one
 * indicates noise inside the burst window rather than a stronger burst. The
 * dropout sub-score falls linearly from 100 for an unflagged frame to 0 at
 * kDropoutFractionCeiling.
 *
 * @param metrics Readings for the frame
 * @param nominal_burst_10bit Nominal burst peak for the source; when absent
 *        the burst reading cannot be interpreted and is ignored
 * @return Quality score in the range 0-100
 */
double compute_frame_quality_score(const FrameQualityMetrics& metrics,
                                   std::optional<double> nominal_burst_10bit);

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_DISC_MAPPER_FRAME_QUALITY_SCORE_H
