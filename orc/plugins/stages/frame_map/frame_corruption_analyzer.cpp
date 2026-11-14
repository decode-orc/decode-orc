/*
 * File:        frame_corruption_analyzer.cpp
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     Frame corruption pattern generator implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "frame_corruption_analyzer.h"

#include <algorithm>
#include <sstream>

namespace orc {

// Pattern configurations
FrameCorruptionAnalyzer::PatternConfig
FrameCorruptionAnalyzer::get_pattern_config(Pattern pattern) {
  switch (pattern) {
    case Pattern::SIMPLE_SKIP:
      return {"simple-skip", "Skip 5 frames every 100 frames", 5, 0, 0, 0.01};

    case Pattern::SIMPLE_REPEAT:
      return {
          "simple-repeat", "Repeat 3 frames every 50 frames", 0, 3, 0, 0.02};

    case Pattern::SKIP_WITH_GAP:
      return {"skip-with-gap",
              "Skip 10 frames and insert 5 gap markers every 200 frames",
              10,
              0,
              5,
              0.005};

    case Pattern::HEAVY_SKIP:
      return {"heavy-skip",
              "Skip 15 frames every 100 frames (severe damage)",
              15,
              0,
              0,
              0.01};

    case Pattern::HEAVY_REPEAT:
      return {"heavy-repeat",
              "Repeat 5 frames every 30 frames (severe sticking)",
              0,
              5,
              0,
              0.033};

    case Pattern::MIXED_LIGHT:
      return {"mixed-light", "Light mix of skips and repeats", 3, 2, 0, 0.02};

    case Pattern::MIXED_HEAVY:
      return {"mixed-heavy",
              "Heavy mix of skips, repeats, and gap markers",
              10,
              5,
              3,
              0.05};
  }

  return {"unknown", "Unknown pattern", 0, 0, 0, 0.0};
}

std::vector<FrameCorruptionAnalyzer::PatternConfig>
FrameCorruptionAnalyzer::get_all_patterns() {
  return {get_pattern_config(Pattern::SIMPLE_SKIP),
          get_pattern_config(Pattern::SIMPLE_REPEAT),
          get_pattern_config(Pattern::SKIP_WITH_GAP),
          get_pattern_config(Pattern::HEAVY_SKIP),
          get_pattern_config(Pattern::HEAVY_REPEAT),
          get_pattern_config(Pattern::MIXED_LIGHT),
          get_pattern_config(Pattern::MIXED_HEAVY)};
}

// Constructor implementations
FrameCorruptionAnalyzer::FrameCorruptionAnalyzer(uint64_t total_frames,
                                                 Pattern pattern, uint32_t seed)
    : total_frames_(total_frames),
      config_(get_pattern_config(pattern)),
      rng_(seed == 0 ? std::random_device{}() : seed) {}

FrameCorruptionAnalyzer::FrameCorruptionAnalyzer(uint64_t total_frames,
                                                 const PatternConfig& config,
                                                 uint32_t seed)
    : total_frames_(total_frames),
      config_(config),
      rng_(seed == 0 ? std::random_device{}() : seed) {}

// Analyze and generate corruption
FrameCorruptionAnalyzer::Result FrameCorruptionAnalyzer::analyze() {
  Result result;

  // Clear previous state
  events_.clear();
  stats_ = {};

  auto mapping = build_mapping();
  result.mapping_spec = mapping_to_ranges(mapping);
  result.success = !result.mapping_spec.empty();
  result.rationale =
      "Applied pattern: " + config_.name + " - " + config_.description;

  // Copy events and stats to result
  result.events = events_;
  result.stats = stats_;

  return result;
}

// Build frame mapping with corruption
std::vector<uint64_t> FrameCorruptionAnalyzer::build_mapping() {
  std::vector<uint64_t> mapping;
  mapping.reserve(total_frames_ * 2);  // Reserve extra space for repeats

  std::uniform_real_distribution<double> dist(0.0, 1.0);

  for (uint64_t i = 0; i < total_frames_; ++i) {
    // Check if we should corrupt this frame
    bool should_corrupt = (dist(rng_) < config_.corruption_rate);

    if (should_corrupt) {
      // Skip frames
      if (config_.skip_frames > 0 && i + config_.skip_frames < total_frames_) {
        uint64_t skip_end = i + config_.skip_frames - 1;
        events_.push_back(
            {CorruptionEvent::SKIP, i, skip_end, config_.skip_frames});
        stats_.skipped_frames += config_.skip_frames;

        // Advance past skipped frames
        i = skip_end;
        continue;
      }
      // Repeat frame
      else if (config_.repeat_frames > 0) {
        events_.push_back(
            {CorruptionEvent::REPEAT, i, i, config_.repeat_frames});

        // Add frame multiple times
        for (uint32_t r = 0; r < config_.repeat_frames; ++r) {
          mapping.push_back(i);
        }
        stats_.repeated_frames += config_.repeat_frames;
        continue;  // Don't add normal frame below
      }
      // Gap markers
      else if (config_.gap_marker_count > 0) {
        events_.push_back(
            {CorruptionEvent::GAP, i, i, config_.gap_marker_count});

        // Insert gap markers (0xFFFFFFFF)
        for (uint32_t g = 0; g < config_.gap_marker_count; ++g) {
          mapping.push_back(0xFFFFFFFFULL);
        }
        stats_.gap_markers += config_.gap_marker_count;
        // Fall through to add normal frame
      }
    }

    // Add normal frame
    mapping.push_back(i);
    stats_.normal_frames++;
  }

  stats_.total_output_frames = mapping.size();
  return mapping;
}

// Convert mapping to range specification
std::string FrameCorruptionAnalyzer::mapping_to_ranges(
    const std::vector<uint64_t>& mapping) {
  if (mapping.empty()) {
    return "";
  }

  std::ostringstream oss;
  bool first = true;

  size_t i = 0;
  while (i < mapping.size()) {
    if (!first) {
      oss << ",";
    }
    first = false;

    uint64_t current = mapping[i];

    // Handle gap markers
    if (current == 0xFFFFFFFFULL) {
      oss << "4294967295";  // 0xFFFFFFFF as decimal
      i++;
      continue;
    }

    // Find consecutive range
    uint64_t range_start = current;
    uint64_t range_end = current;

    while (i + 1 < mapping.size() && mapping[i + 1] == range_end + 1 &&
           mapping[i + 1] != 0xFFFFFFFFULL) {
      range_end++;
      i++;
    }

    // Output range
    if (range_start == range_end) {
      oss << range_start;
    } else {
      oss << range_start << "-" << range_end;
    }

    i++;
  }

  return oss.str();
}

// Event to string. Frame numbers are presented 1-based, matching the preview
// (internal frame indices are 0-based).
std::string FrameCorruptionAnalyzer::CorruptionEvent::to_string() const {
  std::ostringstream oss;

  switch (type) {
    case SKIP:
      if (start_frame == end_frame) {
        oss << "SKIP: Frame " << (start_frame + 1) << " (" << count << " frame"
            << (count > 1 ? "s)" : ")");
      } else {
        oss << "SKIP: Frames " << (start_frame + 1) << "-" << (end_frame + 1)
            << " (" << count << " frames)";
      }
      break;

    case REPEAT:
      oss << "REPEAT: Frame " << (start_frame + 1) << " (" << count
          << " times)";
      break;

    case GAP:
      oss << "GAP: " << count << " gap marker" << (count > 1 ? "s" : "")
          << " at frame " << (start_frame + 1);
      break;
  }

  return oss.str();
}

}  // namespace orc
