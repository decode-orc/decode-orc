/*
 * File:        teletext_sink_stage.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Teletext Sink Stage implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_sink_stage.h"

#include <orc/abi/orc_plugin_services.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>

#include "teletext_frame_slicer.h"
#include "teletext_sink_deps.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

// Upper bound on the decoding-thread parameter. Not a limit the decoder needs
// — it is bounded by the work available — but a parameter with no maximum is
// one a typo can turn into thousands of threads.
constexpr int32_t kMaxDecodeThreads = 256;

// The second_character_set choice that leaves a page in one alphabet
// throughout, so the ESC control character has nothing to switch to. Spelled
// out rather than left as an empty string because it is a decision the reader
// makes, not a field they failed to fill in.
constexpr const char* kNoSecondCharacterSet = "None";

// The widest window either service uses bounds the parameters, so a project
// whose format is not yet known can still be configured.
constexpr int32_t kFirstAllowedUiLine = 1;
constexpr int32_t kLastAllowedUiLine = 22;

// The service a project's format carries, and the candidate VBI window that
// goes with it — taken from TeletextFrameSlicer rather than restated, so the
// lines the UI offers and the lines recovery probes cannot drift apart.
//
// An unknown format is described as the 625-line service: it is what the stage
// was written for, and its window is a superset of the 525-line one, so a
// project configured before its format is known is configured widely rather
// than narrowly.
TeletextFrameSlicer::SystemProfile profile_for_project(
    VideoSystem project_format) {
  const auto profile = TeletextFrameSlicer::profile_for(project_format);
  return profile.carries_teletext
             ? profile
             : TeletextFrameSlicer::profile_for(VideoSystem::PAL);
}

// Field lines are 0-based everywhere in recovery and 1-based everywhere the
// user sees them (see frame_numbering.h for the project-wide convention).
constexpr int32_t to_ui_line(int32_t field_line) { return field_line + 1; }
constexpr int32_t to_field_line(int32_t ui_line) { return ui_line - 1; }

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

TeletextSinkStage::TeletextSinkStage(IStageServices* stage_services)
    : stage_services_(stage_services) {
  set_configuration_status(orc::ConfigurationStatus::Yellow);
}

NodeTypeInfo TeletextSinkStage::get_node_type_info() const {
  return NodeTypeInfo{
      NodeType::SINK,
      "teletext_sink",
      "Teletext Sink",
      "Recovers teletext from the VBI, exports the packet stream and browses "
      "the pages. Trigger to write the stream and update the page catalogue.",
      1,
      1,  // One input
      0,
      0,  // No outputs (sink)
      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> TeletextSinkStage::execute(
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

StagePreviewCapability TeletextSinkStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_input_);
}

std::vector<ParameterDescriptor> TeletextSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)source_type;
  const auto profile = profile_for_project(project_format);
  const bool line_525 = (profile.teletext_system == TeletextSystem::kWst525);
  std::vector<ParameterDescriptor> descriptors;

  {
    ParameterDescriptor desc;
    desc.name = "output_path";
    desc.display_name = "Output File";
    // Optional: the run's other product is the page catalogue the viewer
    // shows, and browsing a recording's teletext is a reason to trigger the
    // stage on its own. Left empty, the pass runs exactly as it would and
    // simply writes no stream.
    desc.description =
        std::string(line_525 ? "Path to the output T34 packet stream (34-byte "
                               "525-line packets)"
                             : "Path to the output T42 packet stream (42-byte "
                               "625-line packets)") +
        ". Leave it empty to decode and browse the pages without writing a "
        "packet stream";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.required = false;
    desc.constraints.default_value = std::string("");
    desc.file_extension_hint = line_525 ? ".t34" : ".t42";
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "first_vbi_line";
    desc.display_name = "First VBI Line";
    desc.description =
        "First candidate field line probed for teletext (1-based, both "
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
        "Last candidate field line probed for teletext (1-based, both fields)";
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
        "start of each line and is what recovers teletext from tape, where "
        "the limited bandwidth smears bits into their neighbours. Automatic "
        "tries Threshold first and falls back to MLSE where it fails to lock "
        "or locks with too small an eye margin to trust";
    desc.type = ParameterType::STRING;
    desc.constraints.allowed_strings = {"Automatic", "Threshold", "MLSE"};
    desc.constraints.default_value = std::string("Automatic");
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "character_set";
    desc.display_name = "Character Set";
    desc.description =
        "Which alphabet the page codes are read in when the service does not "
        "say. A service that says so is always believed: if it transmits a "
        "packet X/28/0 or M/29/0 designating its character set, that is what "
        "is used and this setting is ignored. Level 1 services — which is "
        "most of what survives on tape — transmit neither, and the standard "
        "leaves the choice to \"a local Code of Practice\": the region the "
        "receiver was sold in. Nothing in the recording can settle it, so it "
        "has to be set here. Latin covers western Europe and is what every "
        "page was shown as before; pick a Cyrillic option for a Russian, "
        "Bulgarian, Ukrainian or ex-Yugoslav service, which would otherwise "
        "read as transliterated nonsense. The national option sub-set within "
        "Latin still comes from each page's own header bits";
    desc.type = ParameterType::STRING;
    desc.constraints.allowed_strings = {to_string(TeletextG0Set::Latin),
                                        to_string(TeletextG0Set::Cyrillic2),
                                        to_string(TeletextG0Set::Cyrillic3),
                                        to_string(TeletextG0Set::Cyrillic1)};
    desc.constraints.default_value = to_string(TeletextG0Set::Latin);
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "second_character_set";
    desc.display_name = "Second Character Set";
    desc.description =
        "The alphabet the ESC control character switches into, for services "
        "that mix two on one page. A Russian or Ukrainian service writes "
        "foreign names and titles in Latin letters in among the Cyrillic, and "
        "marks each switch with a control code (ETSI EN 300 706 §12.2 Table 26 "
        "code 1/B) that toggles the rest of the display row into the other "
        "alphabet. As with the character set above, a service that designates "
        "its own pair is always believed and this setting is ignored — "
        "including when it says it uses only one, which switches the control "
        "code off. None, the default, means the same: pages are read in one "
        "alphabet throughout and the code does nothing, which is how every "
        "page was read before. Set this only where a recording really does mix "
        "alphabets, since a wrong pairing turns the switched runs into "
        "nonsense "
        "rather than leaving them alone";
    desc.type = ParameterType::STRING;
    desc.constraints.allowed_strings = {kNoSecondCharacterSet,
                                        to_string(TeletextG0Set::Latin),
                                        to_string(TeletextG0Set::Cyrillic2),
                                        to_string(TeletextG0Set::Cyrillic3),
                                        to_string(TeletextG0Set::Cyrillic1)};
    desc.constraints.default_value = std::string(kNoSecondCharacterSet);
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "tolerant_framing";
    desc.display_name = "Tolerant Framing";
    desc.description =
        "Accept framing codes with one bit error (raises the false-positive "
        "rate on noisy sources)";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "require_valid_mrag";
    desc.display_name = "Require Valid MRAG";
    desc.description =
        "Drop packets whose magazine/row address fails Hamming 8/4 "
        "correction (suppresses false locks on noise)";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "repair_damaged_bytes";
    desc.display_name = "Repair Damaged Bytes";
    desc.description =
        "Every display byte carries a parity bit, so a byte that fails its "
        "parity check is known to be damaged. Restore it by flipping the bit "
        "the MLSE detector came closest to reading the other way. Recovers "
        "characters a difficult tape would otherwise lose, at the cost of the "
        "repaired bytes becoming indistinguishable from undamaged ones — a "
        "repair that guessed wrong is no longer marked as damage. Applies to "
        "the MLSE detector only, so a disc or direct capture is unaffected";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    desc.constraints.depends_on =
        ParameterDependency{"detector", {"Automatic", "MLSE"}};
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
    desc.name = "squash_repeated_rows";
    desc.display_name = "Combine Repeated Rows";
    desc.description =
        "Teletext pages are transmitted on a loop, so a recording holds "
        "several copies of every row damaged in different places. Combine "
        "them byte by byte, preferring values that pass their parity check, "
        "and write the combined rows. Needs a second pass over the recovered "
        "packets, held in memory";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "write_report";
    desc.display_name = "Write Report File";
    desc.description =
        std::string(
            "Write the run's diagnostic report next to the packet stream, "
            "named after it with a .txt extension (") +
        (line_525 ? "mydata.t34 gives mydata.t34.txt"
                  : "mydata.t42 gives mydata.t42.txt") +
        "). The report says how many candidate lines yielded packets, how the "
        "odd-parity failures of the recovered packets are spread across the "
        "display-byte positions, and what combining repeated rows changed. "
        "Needs an output file to sit beside; the same report is always "
        "written to the log at debug level";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  // Subtitle export is 625-line only: the cue timing derives from the 50
  // fields per second of ITU-R BT.1700 Annex 1 Part B Table 1 item 2.
  if (!line_525) {
    {
      ParameterDescriptor desc;
      desc.name = "export_subtitles";
      desc.display_name = "Export Subtitles";
      desc.description =
          "Decode the subtitle page (C6-flagged, conventionally 888) and "
          "write timed subtitle cues next to the T42 output. Needs an output "
          "file to sit beside";
      desc.type = ParameterType::BOOL;
      desc.constraints.default_value = false;
      descriptors.push_back(desc);
    }

    {
      ParameterDescriptor desc;
      desc.name = "subtitle_page";
      desc.display_name = "Subtitle Page";
      desc.description =
          "Teletext page carrying the subtitles: magazine digit (1-8) "
          "followed by two hexadecimal page digits, e.g. 888";
      desc.type = ParameterType::STRING;
      desc.constraints.default_value = std::string("888");
      desc.constraints.depends_on =
          ParameterDependency{"export_subtitles", {"true"}};
      descriptors.push_back(desc);
    }

    {
      ParameterDescriptor desc;
      desc.name = "subtitle_format";
      desc.display_name = "Subtitle Format";
      desc.description =
          "Subtitle output format (SubRip .srt; colour and positioning are "
          "dropped at this level)";
      desc.type = ParameterType::STRING;
      desc.constraints.allowed_strings = {"SRT"};
      desc.constraints.default_value = std::string("SRT");
      desc.constraints.depends_on =
          ParameterDependency{"export_subtitles", {"true"}};
      descriptors.push_back(desc);
    }
  }

  return descriptors;
}

std::map<std::string, ParameterValue> TeletextSinkStage::get_parameters()
    const {
  return parameters_;
}

bool TeletextSinkStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  parameters_ = params;

  const auto it = params.find("output_path");
  const bool has_path =
      (it != params.end() && std::holds_alternative<std::string>(it->second) &&
       !std::get<std::string>(it->second).empty());

  // Without an output file the stage still runs and still fills the page
  // viewer — it just exports nothing — so an empty path is the reduced
  // behaviour Yellow stands for rather than a missing requirement.
  set_configuration_status(has_path ? orc::ConfigurationStatus::Green
                                    : orc::ConfigurationStatus::Yellow);
  return true;
}

TeletextSinkOptions TeletextSinkStage::parse_config(
    const std::map<std::string, ParameterValue>& parameters) const {
  TeletextSinkOptions options;

  // An absent or empty path is the browse-only configuration: the pass runs
  // unchanged and writes no packet stream.
  const auto path_it = parameters.find("output_path");
  if (path_it != parameters.end() &&
      std::holds_alternative<std::string>(path_it->second)) {
    options.output_path = std::get<std::string>(path_it->second);
  }

  // UI lines are 1-based (frame_numbering presentation convention); the slicer
  // window is 0-based field lines. A parameter set that names neither falls
  // back to the 625-line window, as an unset project format does.
  const auto fallback = profile_for_project(VideoSystem::Unknown);
  const int32_t first_ui = get_int32_or(parameters, "first_vbi_line",
                                        to_ui_line(fallback.first_field_line));
  const int32_t last_ui = get_int32_or(parameters, "last_vbi_line",
                                       to_ui_line(fallback.last_field_line));
  if (first_ui < kFirstAllowedUiLine || last_ui > kLastAllowedUiLine ||
      first_ui > last_ui) {
    throw std::runtime_error("Invalid VBI line window");
  }
  options.first_field_line = to_field_line(first_ui);
  options.last_field_line = to_field_line(last_ui);

  options.keep_empty_packets =
      get_bool_or(parameters, "keep_empty_packets", false);
  options.tolerant_framing = get_bool_or(parameters, "tolerant_framing", false);
  options.require_valid_mrag =
      get_bool_or(parameters, "require_valid_mrag", true);
  options.parity_repair = get_bool_or(parameters, "repair_damaged_bytes", true);
  options.pin_data_phase = get_bool_or(parameters, "pin_data_phase", true);
  options.learn_active_lines =
      get_bool_or(parameters, "learn_active_lines", true);
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

  const std::string character_set = get_string_or(
      parameters, "character_set", to_string(TeletextG0Set::Latin));
  if (const auto g0_set = teletext_g0_set_from_string(character_set)) {
    options.character_set = *g0_set;
  } else {
    throw std::runtime_error("Unknown character set: " + character_set);
  }

  // The second set is optional in a way the first is not: a page always has one
  // alphabet, and only some services have two.
  const std::string second_character_set =
      get_string_or(parameters, "second_character_set", kNoSecondCharacterSet);
  if (second_character_set == kNoSecondCharacterSet) {
    options.second_character_set.reset();
  } else if (const auto second =
                 teletext_g0_set_from_string(second_character_set)) {
    // The national option sub-set of a second Latin set is designated with the
    // set itself rather than by the page header (EN 300 706 §15.3), and there
    // is no designation to read here — so it is English, which is what the
    // sub-set of an unqualified "Latin" has always meant on this surface.
    options.second_character_set = TeletextG0Designation{*second, 0};
  } else {
    throw std::runtime_error("Unknown second character set: " +
                             second_character_set);
  }

  options.squash_repeated_rows =
      get_bool_or(parameters, "squash_repeated_rows", true);
  options.write_report = get_bool_or(parameters, "write_report", false);
  options.export_subtitles = get_bool_or(parameters, "export_subtitles", false);
  if (options.export_subtitles) {
    options.subtitle_page = get_string_or(parameters, "subtitle_page", "888");
    if (!TeletextPageDecoder::parse_page_number(options.subtitle_page)
             .has_value()) {
      throw std::runtime_error(
          "Invalid subtitle page \"" + options.subtitle_page +
          "\" (expected magazine digit 1-8 plus two hex digits, e.g. 888)");
    }
    const std::string format =
        get_string_or(parameters, "subtitle_format", "SRT");
    if (format != "SRT") {
      throw std::runtime_error("Unsupported subtitle format: " + format);
    }
  }

  // Both file-side extras are named after the packet stream and written beside
  // it, so neither has anywhere to go on a browse-only run. Refused rather
  // than dropped: an export silently not happening is the worse outcome.
  if (options.output_path.empty()) {
    if (options.export_subtitles) {
      throw std::runtime_error(
          "Subtitle export needs an output file (the cues are written beside "
          "the packet stream)");
    }
    if (options.write_report) {
      throw std::runtime_error(
          "The report file needs an output file (it is written beside the "
          "packet stream)");
    }
  }

  return options;
}

bool TeletextSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  // The stage owns its decoding end to end; nothing is read from or written to
  // the observation store.
  (void)observation_context;

  trigger_status_ = "Starting teletext analysis...";
  is_processing_.store(true);
  cancel_requested_.store(false);
  has_results_ = false;
  dataset_ = TeletextAnalysisDataset{};
  invalidate_catalogue();

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

    TeletextSinkOptions options;
    try {
      options = parse_config(parameters);
    } catch (const std::exception& e) {
      return fail_trigger(std::string("Error: ") + e.what());
    }

    std::shared_ptr<ITeletextSinkStageDeps> deps = deps_override_;
    if (!deps) {
      deps = std::make_shared<TeletextSinkDeps>(stage_services_);
    }

    deps->init(progress_callback_, &cancel_requested_);

    const TeletextSinkResult result =
        deps->analyse(representation.get(), options);

    is_processing_.store(false);

    // The catalogue is kept whether or not the run finished: a cancelled run
    // still recovered the pages it got to, and the viewer is the reason the
    // user triggered it.
    dataset_ = result.dataset;
    has_results_ = result.success;
    invalidate_catalogue();

    // Diagnostic report of the run: recovery profile plus what combining
    // repeated rows changed. Reported for a run that was cancelled part-way as
    // well as one that finished — how it was going is exactly the question a
    // cancelled run leaves — at debug level, because it is many lines and the
    // answer most runs need is the one-line status below.
    if (!result.report.empty()) {
      ORC_LOG_DEBUG("TeletextSink:\n{}", result.report);
    }

    if (!result.success) {
      trigger_status_ =
          "Error: " + (result.message.empty()
                           ? std::string("Teletext analysis failed")
                           : result.message);
      ORC_LOG_ERROR("TeletextSink: {}", trigger_status_);
      return false;
    }

    trigger_status_ = "Recovered " + std::to_string(result.packets_written) +
                      " teletext packets (" +
                      std::to_string(result.fields_with_data) +
                      " fields with data)";
    // A browse-only run has no file to name, and a status that simply omitted
    // it would read as an export that quietly failed.
    trigger_status_ += result.output_path.empty()
                           ? "; no packet stream written (no output file set)"
                           : " to " + result.output_path;
    trigger_status_ += "; " + std::to_string(result.dataset.pages.size()) +
                       (result.dataset.pages.size() == 1 ? " page" : " pages");
    // A page number transmitted as a sequence of sub-pages is one page here
    // but several pages to read, so the count only means something with the
    // sub-pages named beside it.
    std::size_t subpages = 0;
    for (const auto& page : result.dataset.pages) {
      subpages += page.subpages.size();
    }
    if (subpages > result.dataset.pages.size()) {
      trigger_status_ += " (" + std::to_string(subpages) + " sub-pages)";
    }
    if (result.bytes_repaired > 0) {
      trigger_status_ += "; repaired " + std::to_string(result.bytes_repaired) +
                         " damaged bytes";
    }
    if (result.packets_corrected > 0) {
      trigger_status_ += "; combined repeated rows corrected " +
                         std::to_string(result.packets_corrected) + " packets";
    }
    // The result in one figure, on the same terms as the report's headline:
    // characters that still fail their parity check are characters the reader
    // will see damaged.
    if (result.characters_written > 0) {
      char loss[32];
      std::snprintf(loss, sizeof(loss), "%.2f",
                    100.0 * static_cast<double>(result.characters_damaged) /
                        static_cast<double>(result.characters_written));
      trigger_status_ += "; data loss " + std::string(loss) + "% (" +
                         std::to_string(result.characters_damaged) + " of " +
                         std::to_string(result.characters_written) +
                         " characters damaged)";
    }
    if (!result.subtitle_path.empty()) {
      trigger_status_ += "; " + std::to_string(result.subtitle_cues_written) +
                         " subtitle cues to " + result.subtitle_path;
    }
    if (!result.report_path.empty()) {
      trigger_status_ += "; report to " + result.report_path;
    }
    ORC_LOG_INFO("TeletextSink: {}", trigger_status_);
    return true;

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("TeletextSink: {}", e.what());
    return fail_trigger(std::string("Error: ") + e.what());
  }
}

std::string TeletextSinkStage::get_trigger_status() const {
  return trigger_status_;
}

}  // namespace orc
