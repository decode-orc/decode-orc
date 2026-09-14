/*
 * File:        nabts_sink_stage.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NABTS Sink Stage implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_sink_stage.h"

#include <orc/abi/orc_plugin_services.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>

#include "nabts_frame_slicer.h"
#include "nabts_sink_deps.h"
#include "naplps_render_grid.h"

namespace orc {

namespace {

// Upper bound on the decoding-thread parameter. Not a limit the decoder needs
// — it is bounded by the work available — but a parameter with no maximum is
// one a typo can turn into thousands of threads.
constexpr int32_t kMaxDecodeThreads = 256;

// The candidate VBI window, as 1-based UI lines. CEA-516 §1.1.1 and ITU-R
// BT.653 §2 place the 525-line teletext services on broadcast lines 10 to 21 of
// field 1 and 273 to 284 of field 2, which are field lines 10 to 21 either way.
//
// CEA-516 §1.2 also permits full-field transmission on lines 10 to 262, but the
// containers this stage is fed carry the VBI crop alone and no full-field
// sample exists to validate against, so the parameter stops at the VBI window.
constexpr int32_t kFirstAllowedUiLine = 1;
constexpr int32_t kLastAllowedUiLine = 22;

// Field lines are 0-based everywhere in recovery and 1-based everywhere the
// user sees them (see frame_numbering.h for the project-wide convention).
constexpr int32_t to_ui_line(int32_t field_line) { return field_line + 1; }
constexpr int32_t to_field_line(int32_t ui_line) { return ui_line - 1; }

// The NABTS line window, from the frame slicer rather than restated, so the
// lines the UI offers and the lines recovery probes cannot drift apart. NTSC
// stands for both 525-line television systems here: they share the window.
NabtsFrameSlicer::SystemProfile nabts_profile() {
  return NabtsFrameSlicer::profile_for(VideoSystem::NTSC);
}

// Whether a project of |project_format| can carry NABTS at all. An unknown
// format is treated as able to: a project configured before its format is known
// is configured rather than refused, and the run itself checks the source.
bool format_may_carry_nabts(VideoSystem project_format) {
  return project_format == VideoSystem::Unknown ||
         NabtsFrameSlicer::applies_to(project_format);
}

int32_t get_int32_or(const std::map<std::string, ParameterValue>& parameters,
                     const std::string& name, int32_t fallback) {
  const auto it = parameters.find(name);
  if (it == parameters.end() || !std::holds_alternative<int32_t>(it->second)) {
    return fallback;
  }
  return std::get<int32_t>(it->second);
}

bool get_bool_or(const std::map<std::string, ParameterValue>& parameters,
                 const std::string& name, bool fallback) {
  const auto it = parameters.find(name);
  if (it == parameters.end() || !std::holds_alternative<bool>(it->second)) {
    return fallback;
  }
  return std::get<bool>(it->second);
}

std::string get_string_or(
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& name, const std::string& fallback) {
  const auto it = parameters.find(name);
  if (it == parameters.end() ||
      !std::holds_alternative<std::string>(it->second)) {
    return fallback;
  }
  return std::get<std::string>(it->second);
}

}  // namespace

NabtsSinkStage::NabtsSinkStage(IStageServices* stage_services)
    : stage_services_(stage_services) {
  set_configuration_status(orc::ConfigurationStatus::Yellow);
}

NodeTypeInfo NabtsSinkStage::get_node_type_info() const {
  return NodeTypeInfo{
      NodeType::SINK,
      "nabts_sink",
      "NABTS Sink",
      "Recovers North American Basic Teletext (NABTS) from the VBI of a "
      "525-line source and exports the packet stream. Trigger to write the "
      "stream.",
      1,
      1,  // One input
      0,
      0,  // No outputs (sink)
      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> NabtsSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  // Sink stages don't produce outputs in execute(); the actual work happens
  // in trigger(). The input is cached so the preview surface has something to
  // show before the node has been triggered.
  (void)parameters;
  (void)observation_context;
  cached_input_ = nullptr;
  if (!inputs.empty()) {
    cached_input_ =
        std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  }
  return {};
}

StagePreviewCapability NabtsSinkStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_input_);
}

std::vector<ParameterDescriptor> NabtsSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

  // NABTS is defined on the 525-line NTSC signal (CEA-516 §1.1.1). A 625-line
  // project carries no service this stage can recover, so it is offered no
  // parameters to configure rather than a set that cannot be used — the same
  // way the teletext sink withholds its 625-line-only subtitle parameters.
  if (!format_may_carry_nabts(project_format)) {
    return descriptors;
  }

  const auto profile = nabts_profile();

  {
    ParameterDescriptor desc;
    desc.name = "output_path";
    desc.display_name = "Output File";
    desc.description =
        "Path to the output T33 packet stream (33-byte NABTS data packets). "
        "Leave it empty to run the recovery and read the report without "
        "writing a packet stream. \"-\" writes the stream to the CLI "
        "process's standard output instead of a file (CLI only; the GUI "
        "rejects this value) — refused together with export_records, "
        "export_captions, or write_report, since those write separate "
        "files named after this path.";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.required = false;
    desc.constraints.default_value = std::string("");
    desc.file_extension_hint = ".t33";
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "first_vbi_line";
    desc.display_name = "First VBI Line";
    desc.description =
        "First candidate field line probed for NABTS data (1-based, both "
        "fields)";
    desc.type = ParameterType::INT32;
    desc.constraints.min_value = kFirstAllowedUiLine;
    desc.constraints.max_value = kLastAllowedUiLine;
    desc.constraints.default_value = to_ui_line(profile.first_field_line);
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "last_vbi_line";
    desc.display_name = "Last VBI Line";
    desc.description =
        "Last candidate field line probed for NABTS data (1-based, both "
        "fields)";
    desc.type = ParameterType::INT32;
    desc.constraints.min_value = kFirstAllowedUiLine;
    desc.constraints.max_value = kLastAllowedUiLine;
    desc.constraints.default_value = to_ui_line(profile.last_field_line);
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "keep_empty_packets";
    desc.display_name = "Keep Empty Packets";
    desc.description =
        "Emit a whole zero packet for every candidate line with no data so "
        "packet position maps 1:1 to (frame, field, line)";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "detector";
    desc.display_name = "Bit Detector";
    desc.description =
        "How data bits are recovered from each line. Threshold slices at bit "
        "centres and suits discs and direct captures, which pass the whole "
        "data band. MLSE fits the recording's frequency response to the known "
        "start of each line and is what recovers data from tape, where the "
        "limited bandwidth smears bits into their neighbours. Automatic tries "
        "Threshold first and falls back to MLSE only where it fails";
    desc.type = ParameterType::STRING;
    desc.constraints.allowed_strings = {"Automatic", "Threshold", "MLSE"};
    desc.constraints.default_value = std::string("Automatic");
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "tolerant_framing";
    desc.display_name = "Tolerant Framing";
    desc.description =
        "Accept framing codes with one bit error (raises the false-positive "
        "rate on noisy sources, and weakens the only thing that separates "
        "NABTS from the 525-line World System Teletext service)";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "require_valid_prefix";
    desc.display_name = "Require Valid Packet Prefix";
    desc.description =
        "Drop packets whose five-byte prefix — the three packet address bytes, "
        "the continuity index and the packet structure byte — does not survive "
        "Hamming 8/4 correction. All five are error-protected, so requiring "
        "them suppresses false locks on noise very effectively; a packet whose "
        "prefix is unrecoverable cannot be placed in a data group anyway";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "pin_data_phase";
    desc.display_name = "Pin Data Phase";
    desc.description =
        "Most of the cost of reading a line is searching the whole of the "
        "standard's data-timing window for where the data burst starts. Every "
        "line of a time-base-corrected recording starts at very nearly the "
        "same place, so once enough lines have been read the search narrows to "
        "where they agreed. A narrowed search that finds nothing is repeated "
        "over the full window, so this costs a few percent on lines that carry "
        "no data and cannot lose a packet";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "learn_active_lines";
    desc.display_name = "Learn Active Lines";
    desc.description =
        "A service uses a few of the lines its standard permits, but every "
        "line of the window is read on every frame. Read them all for the "
        "first frames, then only the lines that have carried a packet. The "
        "full window is rechecked periodically, so a service that starts part "
        "way into a recording is still picked up";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "grammar_assisted_vote";
    desc.display_name = "Grammar-Assisted Record Vote";
    desc.description =
        "A record that never arrived undamaged is recovered by voting its "
        "copies together byte by byte, and some positions the copies leave "
        "level. Ask the NAPLPS grammar about those: read the whole record with "
        "each candidate in place and keep the one that leaves it best formed, "
        "or none of them where the grammar has no preference. This changes the "
        "recovered record data itself, and so the exported record files and "
        "every later reading of the record — turn it off to have a level "
        "position settled by the most recent copy instead. Presentation "
        "records only; an application record's data is not NAPLPS";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "decode_threads";
    desc.display_name = "Decoding Threads";
    desc.description =
        "Threads to recover lines on. Each line is read from its own samples "
        "alone, so frames are decoded several at a time; 0 uses one thread per "
        "processor. The recovered stream is the same whatever this is set to, "
        "so lower it only to leave the machine free for other work";
    desc.type = ParameterType::INT32;
    desc.constraints.min_value = 0;
    desc.constraints.max_value = kMaxDecodeThreads;
    desc.constraints.default_value = int32_t{0};
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "write_report";
    desc.display_name = "Write Report File";
    desc.description =
        "Write the run's diagnostic report next to the packet stream, named "
        "after it with a .txt extension (mydata.t33 gives mydata.t33.txt). The "
        "report says how many candidate lines yielded packets and which gate "
        "discarded the rest. Needs an output file to sit beside; the same "
        "report is always written to the log at debug level";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "export_records";
    desc.display_name = "Export Record Files";
    desc.description =
        "Write each teletext record the recording carried as its own file "
        "beside the packet stream, named for the channel, record address and "
        "version that identify it (mydata.t33.000-1A4-v2.rec). The file holds "
        "the record's data exactly as transmitted: NAPLPS presentation code, "
        "or "
        "application data for a record of type 2. Needs an output file to sit "
        "beside";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "export_captions";
    desc.display_name = "Export Captions (SubRip)";
    desc.description =
        "Write the recording's captioning as a SubRip subtitle file beside the "
        "packet stream (mydata.t33 gives mydata.t33.srt). The cues are the "
        "records the service marked with the caption flag (CEA-516 §5.2.7.3), "
        "in the order they were transmitted, each running until the next one "
        "replaces it; cue timing comes from the 59.94 fields per second of "
        "SMPTE 170M. A recording that carried no captioning writes no file. "
        "Needs an output file to sit beside";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> NabtsSinkStage::get_parameters() const {
  return parameters_;
}

bool NabtsSinkStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  parameters_ = params;

  const auto it = params.find("output_path");
  const bool has_path =
      (it != params.end() && std::holds_alternative<std::string>(it->second) &&
       !std::get<std::string>(it->second).empty());

  // Without an output file the stage still runs and still reports how the
  // recovery went — it just exports nothing — so an empty path is the reduced
  // behaviour Yellow stands for rather than a missing requirement.
  set_configuration_status(has_path ? orc::ConfigurationStatus::Green
                                    : orc::ConfigurationStatus::Yellow);
  return true;
}

NabtsSinkOptions NabtsSinkStage::parse_config(
    const std::map<std::string, ParameterValue>& parameters) const {
  NabtsSinkOptions options;

  // An absent or empty path is the report-only configuration: the pass runs
  // unchanged and writes no packet stream.
  const auto path_it = parameters.find("output_path");
  if (path_it != parameters.end() &&
      std::holds_alternative<std::string>(path_it->second)) {
    options.output_path = std::get<std::string>(path_it->second);
  }

  // UI lines are 1-based (frame_numbering presentation convention); the slicer
  // window is 0-based field lines.
  const auto profile = nabts_profile();
  const int32_t first_ui = get_int32_or(parameters, "first_vbi_line",
                                        to_ui_line(profile.first_field_line));
  const int32_t last_ui = get_int32_or(parameters, "last_vbi_line",
                                       to_ui_line(profile.last_field_line));
  if (first_ui < kFirstAllowedUiLine || last_ui > kLastAllowedUiLine ||
      first_ui > last_ui) {
    throw std::runtime_error("Invalid VBI line window");
  }
  options.first_field_line = to_field_line(first_ui);
  options.last_field_line = to_field_line(last_ui);

  options.keep_empty_packets =
      get_bool_or(parameters, "keep_empty_packets", false);
  options.tolerant_framing = get_bool_or(parameters, "tolerant_framing", false);
  options.require_valid_prefix =
      get_bool_or(parameters, "require_valid_prefix", true);
  options.pin_data_phase = get_bool_or(parameters, "pin_data_phase", true);
  options.learn_active_lines =
      get_bool_or(parameters, "learn_active_lines", true);
  options.grammar_assisted_vote =
      get_bool_or(parameters, "grammar_assisted_vote", true);
  options.decode_threads = std::clamp(
      get_int32_or(parameters, "decode_threads", 0), 0, kMaxDecodeThreads);

  const std::string detector =
      get_string_or(parameters, "detector", "Automatic");
  if (detector == "Automatic") {
    options.detector = TeletextDetector::kAuto;
  } else if (detector == "Threshold") {
    options.detector = TeletextDetector::kThreshold;
  } else if (detector == "MLSE") {
    options.detector = TeletextDetector::kMlse;
  } else {
    throw std::runtime_error("Unknown bit detector: " + detector);
  }

  options.write_report = get_bool_or(parameters, "write_report", false);
  options.export_records = get_bool_or(parameters, "export_records", false);
  options.export_captions = get_bool_or(parameters, "export_captions", false);

  // The report is named after the packet stream and written beside it, so it
  // has nowhere to go on a run with no output file. Refused rather than
  // dropped: an export silently not happening is the worse outcome.
  if (options.output_path.empty() && options.write_report) {
    throw std::runtime_error(
        "The report file needs an output file (it is written beside the "
        "packet stream)");
  }
  if (options.output_path.empty() && options.export_records) {
    throw std::runtime_error(
        "The record files need an output file (they are written beside the "
        "packet stream)");
  }
  if (options.output_path.empty() && options.export_captions) {
    throw std::runtime_error(
        "The caption file needs an output file (it is written beside the "
        "packet stream)");
  }

  return options;
}

bool NabtsSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  // The stage owns its decoding end to end; nothing is read from or written to
  // the observation store.
  (void)observation_context;

  trigger_status_ = "Starting NABTS recovery...";
  is_processing_.store(true);
  cancel_requested_.store(false);

  const auto fail_trigger = [this](const std::string& status) {
    trigger_status_ = status;
    is_processing_.store(false);
    return false;
  };

  try {
    if (inputs.empty()) {
      return fail_trigger("Error: No input connected");
    }

    auto representation =
        std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
    if (!representation) {
      return fail_trigger("Error: Input is not a video frame representation");
    }

    NabtsSinkOptions options;
    try {
      options = parse_config(parameters);
    } catch (const std::exception& e) {
      return fail_trigger(std::string("Error: ") + e.what());
    }

    std::shared_ptr<INabtsSinkStageDeps> deps = deps_override_;
    if (!deps) {
      deps = std::make_shared<NabtsSinkDeps>(stage_services_);
    }

    deps->init(progress_callback_, &cancel_requested_);

    const NabtsSinkResult result = deps->analyse(representation.get(), options);

    is_processing_.store(false);

    // Cached before the success check: a cancelled or partly failed run still
    // catalogued whatever it read, and the records dialog showing that is more
    // use than showing nothing.
    dataset_ = result.dataset;
    has_results_ = !dataset_.records.empty();
    invalidate_catalogue();

    // Diagnostic report of the run. Reported for a run that was cancelled
    // part-way as well as one that finished — how it was going is exactly the
    // question a cancelled run leaves — at debug level, because it is many
    // lines and the answer most runs need is the one-line status below.
    if (!result.report.empty()) {
      ORC_LOG_DEBUG("NabtsSink:\n{}", result.report);
    }

    if (!result.success) {
      trigger_status_ = "Error: " + (result.message.empty()
                                         ? std::string("NABTS recovery failed")
                                         : result.message);
      ORC_LOG_ERROR("NabtsSink: {}", trigger_status_);
      return false;
    }

    trigger_status_ = "Recovered " + std::to_string(result.packets_written) +
                      " NABTS packets (" +
                      std::to_string(result.fields_with_data) +
                      " fields with data)";
    // A report-only run has no file to name, and a status that simply omitted
    // it would read as an export that quietly failed.
    trigger_status_ += result.output_path.empty()
                           ? "; no packet stream written (no output file set)"
                           : " to " + result.output_path;
    if (result.lost_packets_estimate > 0) {
      trigger_status_ += "; " + std::to_string(result.lost_packets_estimate) +
                         " packets estimated lost";
    }
    if (!dataset_.records.empty()) {
      trigger_status_ += "; " + std::to_string(dataset_.records.size()) +
                         " records catalogued";
    }
    if (result.records_exported > 0) {
      trigger_status_ += "; " + std::to_string(result.records_exported) +
                         " record files written";
    }
    if (!result.report_path.empty()) {
      trigger_status_ += "; report to " + result.report_path;
    }
    ORC_LOG_INFO("NabtsSink: {}", trigger_status_);
    return true;

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("NabtsSink: {}", e.what());
    return fail_trigger(std::string("Error: ") + e.what());
  }
}

std::string NabtsSinkStage::get_trigger_status() const {
  return trigger_status_;
}

}  // namespace orc
