/*
 * File:        frame_map_range_tool.cpp
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     The Frame Map's Range Finder: the frames between two picture
 *              numbers or CLV time codes, as the Frame Map's ranges
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "frame_map_range_tool.h"

#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>
#include <orc/support/observers/biphase_observer.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "frame_map_range_search.h"

namespace orc::frame_map_range_tool {

namespace {

using frame_map_range_contract::kEndAddress;
using frame_map_range_contract::kStartAddress;

StageAnalysisReport failed(std::string why) {
  StageAnalysisReport report;
  report.status = StageAnalysisReport::Status::Failed;
  report.summary = std::move(why);
  return report;
}

StageAnalysisReport cancelled() {
  StageAnalysisReport report;
  report.status = StageAnalysisReport::Status::Cancelled;
  return report;
}

/// The job's progress: a status line and how far through it the run is.
class Progress {
 public:
  explicit Progress(IToolJobContext& context) : context_(context) {}
  void status(const std::string& text) {
    status_ = text;
    context_.report_progress(fraction_, status_);
  }
  void percent(int value) {
    fraction_ = value / 100.0;
    context_.report_progress(fraction_, status_);
  }

 private:
  IToolJobContext& context_;
  double fraction_ = 0.0;
  std::string status_;
};

std::string text_parameter(const std::map<std::string, ParameterValue>& params,
                           const char* name) {
  const auto it = params.find(name);
  if (it != params.end()) {
    if (const auto* value = std::get_if<std::string>(&it->second)) {
      return *value;
    }
  }
  return {};
}

std::string trim_copy(const std::string& input) {
  size_t start = 0;
  while (start < input.size() &&
         std::isspace(static_cast<unsigned char>(input[start]))) {
    start++;
  }
  size_t end = input.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    end--;
  }
  return input.substr(start, end - start);
}

bool parse_int32(const std::string& input, int32_t& value) {
  try {
    size_t pos = 0;
    int64_t parsed = std::stoll(input, &pos, 10);
    if (pos != input.size()) {
      return false;
    }
    if (parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max()) {
      return false;
    }
    value = static_cast<int32_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

// Read picture number from one field slot in the observation context.
std::optional<int32_t> get_picture_number_from_vbi(
    const ObservationContext& observation_context, FieldID field_id,
    bool is_pal) {
  auto picture_num = observation_context.get(field_id, "vbi", "picture_number");
  if (picture_num && std::holds_alternative<int32_t>(*picture_num)) {
    return std::get<int32_t>(*picture_num);
  }

  auto hours_opt =
      observation_context.get(field_id, "vbi", "clv_timecode_hours");
  auto minutes_opt =
      observation_context.get(field_id, "vbi", "clv_timecode_minutes");
  auto seconds_opt =
      observation_context.get(field_id, "vbi", "clv_timecode_seconds");
  auto picture_opt =
      observation_context.get(field_id, "vbi", "clv_timecode_picture");

  if (hours_opt && minutes_opt && seconds_opt && picture_opt) {
    if (std::holds_alternative<int32_t>(*hours_opt) &&
        std::holds_alternative<int32_t>(*minutes_opt) &&
        std::holds_alternative<int32_t>(*seconds_opt) &&
        std::holds_alternative<int32_t>(*picture_opt)) {
      int32_t hours = std::get<int32_t>(*hours_opt);
      int32_t minutes = std::get<int32_t>(*minutes_opt);
      int32_t seconds = std::get<int32_t>(*seconds_opt);
      int32_t picture = std::get<int32_t>(*picture_opt);
      int32_t fps = is_pal ? 25 : 30;
      int64_t frame_index = static_cast<int64_t>(hours) * 3600LL * fps +
                            static_cast<int64_t>(minutes) * 60LL * fps +
                            static_cast<int64_t>(seconds) * fps +
                            static_cast<int64_t>(picture);
      int64_t picture_number = frame_index + 1;  // 0:0:0.0 = picture number 1
      if (picture_number > 0 &&
          picture_number <= std::numeric_limits<int32_t>::max()) {
        return static_cast<int32_t>(picture_number);
      }
    }
  }

  return std::nullopt;
}

// BiphaseObserver stores VBI per-field at FieldID(frame_id*2 + field_idx).
// Check both fields and return the first valid picture number found.
std::optional<int32_t> get_picture_number_from_frame(
    const ObservationContext& obs_context, FrameID frame_id, bool is_pal) {
  for (uint64_t field_idx = 0; field_idx < 2; ++field_idx) {
    auto pn = get_picture_number_from_vbi(
        obs_context, FieldID(frame_id * 2 + field_idx), is_pal);
    if (pn) return pn;
  }
  return std::nullopt;
}

}  // namespace

std::vector<ParameterDescriptor> parameters() {
  ParameterDescriptor start_addr;
  start_addr.name = kStartAddress;
  start_addr.display_name = "Start Address";
  start_addr.description =
      "Start picture number (e.g., '12345') or CLV timecode (e.g., '0:0:0.0').";
  start_addr.type = ParameterType::STRING;
  start_addr.constraints.default_value = std::string("");
  start_addr.constraints.required = true;

  ParameterDescriptor end_addr;
  end_addr.name = kEndAddress;
  end_addr.display_name = "End Address";
  end_addr.description =
      "End picture number (e.g., '12350') or CLV timecode (e.g., '0:0:0.5').";
  end_addr.type = ParameterType::STRING;
  end_addr.constraints.default_value = std::string("");
  end_addr.constraints.required = true;

  return {start_addr, end_addr};
}

ParsedAddress parse_address(const std::string& input, bool is_pal) {
  ParsedAddress result;
  std::string trimmed = trim_copy(input);
  if (trimmed.empty()) {
    result.error = "Address is empty";
    return result;
  }

  if (trimmed.find(':') == std::string::npos) {
    int32_t pn = 0;
    if (!parse_int32(trimmed, pn) || pn <= 0) {
      result.error = "Invalid picture number: " + trimmed;
      return result;
    }
    result.ok = true;
    result.is_timecode = false;
    result.picture_number = pn;
    result.normalized = trimmed;
    return result;
  }

  std::vector<std::string> parts;
  std::stringstream ss(trimmed);
  std::string part;
  while (std::getline(ss, part, ':')) {
    parts.push_back(trim_copy(part));
  }

  if (parts.size() < 3 || parts.size() > 4) {
    result.error = "Invalid timecode format: " + trimmed;
    return result;
  }

  int32_t hours = 0;
  int32_t minutes = 0;
  int32_t seconds = 0;
  int32_t pictures = 0;

  if (!parse_int32(parts[0], hours) || !parse_int32(parts[1], minutes)) {
    result.error = "Invalid timecode format: " + trimmed;
    return result;
  }

  if (parts.size() == 4) {
    if (!parse_int32(parts[2], seconds) || !parse_int32(parts[3], pictures)) {
      result.error = "Invalid timecode format: " + trimmed;
      return result;
    }
  } else {
    const std::string& sec_part = parts[2];
    size_t dot_pos = sec_part.find('.');
    if (dot_pos == std::string::npos) {
      dot_pos = sec_part.find(';');
    }
    if (dot_pos == std::string::npos) {
      result.error = "Timecode must include picture component (e.g., 0:0:0.0)";
      return result;
    }
    std::string seconds_str = trim_copy(sec_part.substr(0, dot_pos));
    std::string pictures_str = trim_copy(sec_part.substr(dot_pos + 1));
    if (!parse_int32(seconds_str, seconds) ||
        !parse_int32(pictures_str, pictures)) {
      result.error = "Invalid timecode format: " + trimmed;
      return result;
    }
  }

  int32_t fps = is_pal ? 25 : 30;
  int64_t frame_index = static_cast<int64_t>(hours) * 3600LL * fps +
                        static_cast<int64_t>(minutes) * 60LL * fps +
                        static_cast<int64_t>(seconds) * fps +
                        static_cast<int64_t>(pictures);
  int64_t picture_number = frame_index + 1;  // 0:0:0.0 = picture number 1
  if (picture_number <= 0 ||
      picture_number > std::numeric_limits<int32_t>::max()) {
    result.error = "Timecode out of range: " + trimmed;
    return result;
  }

  std::ostringstream normalized;
  normalized << hours << ":" << minutes << ":" << seconds << "." << pictures;
  result.ok = true;
  result.is_timecode = true;
  result.picture_number = static_cast<int32_t>(picture_number);
  result.normalized = normalized.str();
  return result;
}

StageAnalysisReport run(const std::map<std::string, ParameterValue>& params,
                        IToolJobContext& context) {
  Progress progress(context);
  progress.status("Initializing frame map range analysis...");
  progress.percent(0);

  const std::string start_input = text_parameter(params, kStartAddress);
  const std::string end_input = text_parameter(params, kEndAddress);
  if (start_input.empty() || end_input.empty()) {
    return failed("Start and end addresses are required.");
  }

  const auto& inputs = context.inputs();
  if (inputs.empty() || !inputs[0]) {
    return failed("Frame map node has no input connected");
  }
  auto source = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
  if (!source) {
    return failed("Input node did not produce VideoFrameRepresentation");
  }

  const auto frame_range = source->frame_range();
  const size_t total_frames = frame_range.count();
  if (total_frames == 0) {
    return failed("No frames found in source");
  }

  bool is_pal = false;
  if (const auto first = source->get_frame_descriptor(frame_range.first)) {
    is_pal = (first->system == VideoSystem::PAL);
  }

  const ParsedAddress start_addr = parse_address(start_input, is_pal);
  if (!start_addr.ok) {
    return failed("Start address error: " + start_addr.error);
  }
  const ParsedAddress end_addr = parse_address(end_input, is_pal);
  if (!end_addr.ok) {
    return failed("End address error: " + end_addr.error);
  }

  progress.status("Locating start address...");
  progress.percent(30);

  BiphaseObserver biphase_observer;
  ObservationContext observations;

  // Memoized probe: each frame is biphase-decoded at most once.
  std::unordered_map<int64_t, std::optional<int32_t>> picture_cache;
  auto probe = [&](int64_t fid) -> std::optional<int32_t> {
    const auto it = picture_cache.find(fid);
    if (it != picture_cache.end()) {
      return it->second;
    }
    const auto frame_id = static_cast<FrameID>(fid);
    biphase_observer.process_frame(*source, frame_id, observations);
    auto pn = get_picture_number_from_frame(observations, frame_id, is_pal);
    picture_cache.emplace(fid, pn);
    return pn;
  };
  auto cancel_check = [&context]() { return context.is_cancelled(); };

  const auto src_first = static_cast<int64_t>(frame_range.first);
  const auto src_last = static_cast<int64_t>(frame_range.last);

  using frame_map_range::SearchStatus;

  // Binary chop over the monotonic picture numbers: O(log n) frame decodes
  // instead of a sequential hunt.
  const auto start_outcome = frame_map_range::find_first_frame_with_picture(
      src_first, src_last, start_addr.picture_number, probe, cancel_check);
  if (start_outcome.status == SearchStatus::Cancelled) {
    return cancelled();
  }

  bool start_found = start_outcome.status == SearchStatus::Found;
  FrameID start_frame = start_found ? static_cast<FrameID>(start_outcome.frame)
                                    : frame_range.first;
  if (start_found) {
    ORC_LOG_DEBUG("Start position found at frame {}: picture number {}",
                  start_frame, start_addr.picture_number);
  }

  // Fallback: a sequential scan handles non-monotonic picture numbers (the
  // player skipped backwards during capture, say).
  if (!start_found) {
    ORC_LOG_WARN(
        "Start not found by binary search, falling back to sequential scan");
    progress.status("Start not found by binary search, scanning...");

    for (int64_t fid = src_first; fid <= src_last; ++fid) {
      if (cancel_check()) {
        return cancelled();
      }
      const auto pn = probe(fid);
      if (pn && *pn == start_addr.picture_number) {
        start_found = true;
        start_frame = static_cast<FrameID>(fid);
        ORC_LOG_DEBUG(
            "Start position found at frame {} (full scan): picture number {}",
            fid, *pn);
        break;
      }
      if (fid % 2500 == 0) {
        progress.percent(
            40 + static_cast<int>(20.0 * static_cast<double>(fid - src_first) /
                                  static_cast<double>(total_frames)));
      }
    }
  }

  if (!start_found) {
    return failed("Start address not found in source.");
  }

  progress.status("Locating end address...");
  progress.percent(70);
  if (context.is_cancelled()) {
    return cancelled();
  }

  bool end_found = false;
  FrameID end_frame = start_frame;

  if (start_addr.picture_number == end_addr.picture_number) {
    end_found = true;
    ORC_LOG_DEBUG("End position (same as start) at frame {}: picture number {}",
                  end_frame, start_addr.picture_number);
  } else {
    // The end address may repeat across consecutive frames (player
    // hesitation); take the last matching frame.
    const auto end_outcome = frame_map_range::find_last_frame_with_picture(
        static_cast<int64_t>(start_frame), src_last, end_addr.picture_number,
        probe, cancel_check);
    if (end_outcome.status == SearchStatus::Cancelled) {
      return cancelled();
    }
    if (end_outcome.status == SearchStatus::Found) {
      end_found = true;
      end_frame = static_cast<FrameID>(end_outcome.frame);
      ORC_LOG_DEBUG("End position found at frame {}: picture number {}",
                    end_frame, end_addr.picture_number);
    }

    // Fallback: scan forward from the start (non-monotonic picture numbers).
    if (!end_found) {
      ORC_LOG_WARN(
          "End not found by binary search, falling back to scan from start "
          "position");
      progress.status("End not found by binary search, scanning from start...");

      for (int64_t fid = static_cast<int64_t>(start_frame) + 1; fid <= src_last;
           ++fid) {
        if (cancel_check()) {
          return cancelled();
        }
        const auto pn = probe(fid);
        if (pn && *pn == end_addr.picture_number) {
          end_found = true;
          end_frame = static_cast<FrameID>(fid);
        } else if (end_found && pn && *pn != end_addr.picture_number) {
          break;
        }
        if (fid % 2500 == 0) {
          const int64_t frames_from_start =
              fid - static_cast<int64_t>(start_frame);
          const int64_t total_to_scan =
              src_last - static_cast<int64_t>(start_frame);
          progress.percent(
              70 +
              static_cast<int>(
                  20.0 * static_cast<double>(frames_from_start) /
                  static_cast<double>(std::max<int64_t>(1, total_to_scan))));
        }
      }

      if (end_found) {
        ORC_LOG_DEBUG(
            "End position found at frame {} (full scan): picture number {}",
            end_frame, end_addr.picture_number);
      }
    }
  }

  progress.percent(90);
  if (context.is_cancelled()) {
    return cancelled();
  }

  if (!end_found) {
    return failed("End address not found after start address.");
  }
  if (start_frame > end_frame) {
    return failed("Computed frame range is invalid (start after end).");
  }

  std::ostringstream range_spec;
  range_spec << start_frame << "-" << end_frame;

  // Presentation is 1-based (matching the preview and the Frame Map stage
  // parameter dialog); the applied range spec stays 0-based.
  std::ostringstream summary;
  summary << "Frame range located successfully.\n\n";
  summary << "Start address: " << start_input << " (picture number "
          << start_addr.picture_number << ")\n";
  summary << "End address: " << end_input << " (picture number "
          << end_addr.picture_number << ")\n\n";
  summary << "Frame range: " << (start_frame + 1) << "-" << (end_frame + 1)
          << "\n\n";
  summary << "Click 'Apply to Stage' to update the Frame Map stage.";

  StageAnalysisReport report;
  report.status = StageAnalysisReport::Status::Success;
  report.summary = summary.str();
  report.parameter_changes["ranges"] = range_spec.str();

  progress.status("Analysis complete");
  progress.percent(100);
  return report;
}

}  // namespace orc::frame_map_range_tool
