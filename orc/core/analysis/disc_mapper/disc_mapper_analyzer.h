/*
 * File:        disc_mapper_analyzer.h
 * Module:      orc-core/analysis
 * Purpose:     Frame mapping analyzer (disc mapper implementation)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/stage/field_id.h>
#include <orc/stage/video_frame_representation.h>

#include <string>
#include <vector>

namespace orc {

// Forward declaration
class AnalysisProgress;

/**
 * @brief Result of disc mapping analysis
 */
struct FieldMappingDecision {
  std::string mapping_spec;
  bool success = false;
  std::string rationale;
  std::vector<std::string> warnings;
  bool is_cav = false;
  bool is_pal = false;

  struct Stats {
    size_t total_fields = 0;
    size_t removed_lead_in_out = 0;
    /// Lead-in frame retained at the head of the output (option-gated)
    bool lead_in_included = false;
    /// Lead-out frame retained at the tail of the output (option-gated)
    bool lead_out_included = false;
    size_t removed_invalid_phase = 0;
    size_t removed_duplicates = 0;
    size_t removed_unmappable = 0;
    size_t corrected_vbi_errors = 0;
    size_t pulldown_frames = 0;
    size_t padding_frames = 0;
    size_t gaps_padded = 0;
    /// Frames carrying at least one burst/SNR quality reading
    size_t frames_with_quality = 0;
    /// Picture numbers that had more than one candidate frame
    size_t duplicate_groups = 0;
    /// Duplicate groups whose winner was chosen on signal quality alone
    size_t duplicates_decided_by_quality = 0;
  } stats;
};

/**
 * @brief Frame mapping analyzer
 *
 * Maps decoded fields onto a coherent frame sequence using the VBI data
 * populated in the observation context. The analysis runs a six-stage
 * pipeline:
 *   1. Per-field VBI normalization (with sequence-based resolution of CAV
 *      VBI line disagreements).
 *   2. Field pairing into candidate frames.
 *   3. Frame validation and filtering (lead-in/out, phase, unmappable).
 *      Optionally one lead-in and one lead-out frame are held back from the
 *      filter and re-attached to the ends of the output in stage 5.
 *   4. Deduplication by picture number, picking the best copy of each disc
 *      picture from the colour-burst level and white/black SNR readings
 *      published by the quality observers.
 *   5. Sort by picture number and gap detection.
 *   6. Mapping-specification generation with range notation.
 * Returns a FieldMappingDecision describing the resulting mapping, the
 * per-stage statistics, and any warnings.
 */
class DiscMapperAnalyzer {
 public:
  /**
   * @brief Configuration options for disc mapping analysis
   */
  struct Options {
    bool delete_unmappable_frames;  ///< Remove frames that can't be mapped
    bool strict_pulldown_checking;  ///< Enforce strict pulldown patterns
    bool reverse_field_order;       ///< Reverse first/second field order
    bool pad_gaps;                  ///< Insert padding for missing frames
    /// Keep one lead-in and one lead-out frame (when the capture contains
    /// them) at the ends of the mapped output instead of discarding them.
    bool include_lead_in_out;

    // Default constructor with sensible defaults
    Options()
        : delete_unmappable_frames(false),
          strict_pulldown_checking(true),
          reverse_field_order(false),
          pad_gaps(true),
          include_lead_in_out(false) {}
  };

  DiscMapperAnalyzer() = default;
  ~DiscMapperAnalyzer() = default;

  /**
   * @brief Analyze disc mapping using VBI data from the observation context.
   *
   * @param source The video field representation
   * @param observation_context Observation context containing VBI data from
   * observers
   * @param options Analysis options
   * @param progress Optional progress callback
   * @return Frame mapping decision
   */
  FieldMappingDecision analyze(
      const VideoFrameRepresentation& source,
      const class ObservationContext& observation_context,
      const Options& options = Options{},
      class AnalysisProgress* progress = nullptr);
};

}  // namespace orc
