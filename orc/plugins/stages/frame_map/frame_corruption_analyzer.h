/*
 * File:        frame_corruption_analyzer.h
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     Frame corruption pattern generator for testing disc mapper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief Frame corruption pattern generator
 *
 * Generates frame mapping range specifications that simulate laserdisc
 * player corruption patterns (skips, repeats, gaps). Used for testing
 * the disc mapper and frame correction algorithms.
 *
 * This analyzer creates range specifications compatible with FrameMapStage,
 * allowing corruption to be applied within the DAG chain rather than
 * requiring separate corrupted source captures.
 */
class FrameCorruptionAnalyzer {
 public:
  /**
   * @brief Predefined corruption patterns
   */
  enum class Pattern {
    SIMPLE_SKIP,    // Skip 5 frames every 100 frames
    SIMPLE_REPEAT,  // Repeat 3 frames every 50 frames
    SKIP_WITH_GAP,  // Skip 10 frames, insert 5 gap markers every 200 frames
    HEAVY_SKIP,     // Skip 15 frames every 100 frames (severe damage)
    HEAVY_REPEAT,   // Repeat 5 frames every 30 frames (severe sticking)
    MIXED_LIGHT,    // Light mix of skips and repeats
    MIXED_HEAVY     // Heavy mix of skips, repeats, and gap markers
  };

  /**
   * @brief Corruption pattern configuration
   */
  struct PatternConfig {
    std::string name;
    std::string description;
    uint32_t skip_frames;       // Number of frames to skip
    uint32_t repeat_frames;     // Number of times to repeat a frame
    uint32_t gap_marker_count;  // Number of gap markers to insert
    double corruption_rate;     // Probability of corruption event (0.0-1.0)
  };

  /**
   * @brief Corruption event record
   */
  struct CorruptionEvent {
    enum Type { SKIP, REPEAT, GAP };
    Type type;
    uint64_t start_frame;
    uint64_t end_frame;
    uint32_t count;

    std::string to_string() const;
  };

  /**
   * @brief Analysis result containing corruption specification
   */
  struct Result {
    std::string mapping_spec;             ///< Frame mapping range specification
    bool success = false;                 ///< True if generation succeeded
    std::string rationale;                ///< Description of pattern applied
    std::vector<CorruptionEvent> events;  ///< List of corruption events

    struct Stats {
      uint64_t normal_frames = 0;
      uint64_t repeated_frames = 0;
      uint64_t skipped_frames = 0;
      uint64_t gap_markers = 0;
      uint64_t total_output_frames = 0;
    } stats;
  };

  /**
   * @brief Construct analyzer with frame count and pattern
   * @param total_frames Total number of input frames
   * @param pattern Corruption pattern to apply
   * @param seed Random seed (0 = random device)
   */
  FrameCorruptionAnalyzer(uint64_t total_frames, Pattern pattern,
                          uint32_t seed = 0);

  /**
   * @brief Construct analyzer with custom pattern config
   */
  FrameCorruptionAnalyzer(uint64_t total_frames, const PatternConfig& config,
                          uint32_t seed = 0);

  /**
   * @brief Generate corruption pattern
   * @return Result containing mapping specification and statistics
   */
  Result analyze();

  /**
   * @brief Get predefined pattern configuration
   */
  static PatternConfig get_pattern_config(Pattern pattern);

  /**
   * @brief Get all available pattern configs with descriptions
   */
  static std::vector<PatternConfig> get_all_patterns();

 private:
  uint64_t total_frames_;
  PatternConfig config_;
  std::mt19937 rng_;

  // Temporary storage for events/stats during generation
  std::vector<CorruptionEvent> events_;
  Result::Stats stats_;

  /**
   * @brief Build frame mapping with corruption applied
   * @return Vector where each element is an input frame ID
   *         (0xFFFFFFFF = gap marker)
   */
  std::vector<uint64_t> build_mapping();

  /**
   * @brief Convert frame mapping to range specification string
   */
  std::string mapping_to_ranges(const std::vector<uint64_t>& mapping);
};

}  // namespace orc
