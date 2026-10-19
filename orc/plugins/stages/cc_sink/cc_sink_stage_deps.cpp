/*
 * File:        cc_sink_stage_deps.cpp
 * Module:      orc-core
 * Purpose:     CCSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "cc_sink_stage_deps.h"

#include <orc/stage/common_types.h>
#include <orc/support/eia608_line21_pairs.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orc {
namespace {

// The services the all-services export decodes, in the order the report
// lists them.
constexpr std::array<EIA608Service, 8> kAllServices = {
    EIA608Service::CC1, EIA608Service::CC2, EIA608Service::T1,
    EIA608Service::T2,  EIA608Service::CC3, EIA608Service::CC4,
    EIA608Service::T3,  EIA608Service::T4};

// Where the all-services export writes the report, in its folder.
constexpr const char* kAllServicesReportName = "line21.report.txt";

/// The XDS listing's choice of what to write: a station sends each packet
/// over and over, and a value is written when it changes, which is what a
/// reader wants of a listing hours long.
class XdsChanges {
 public:
  /// Whether |packet|'s value differs from the last of its type; if so, its
  /// type's name and the value.
  bool changed(const CCXdsPacket& packet, std::string& name,
               std::string& value) {
    value = cc_xds_value_text(packet);
    std::string& last = last_value_[{packet.xds_class, packet.type}];
    if (value == last) {
      return false;
    }
    last = value;
    name = cc_xds_type_name(packet.xds_class, packet.type);
    if (name.empty()) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "Class %02Xh type %02Xh",
                    packet.xds_class, packet.type);
      name = buffer;
    }
    return true;
  }

 private:
  std::map<std::pair<uint8_t, uint8_t>, std::string> last_value_;
};

/// Frame rate of the video system, exactly. NTSC line 21 timing is derived
/// from 30000/1001 and not from its rounded "29.97" spelling; at an hour the
/// difference is a few frames.
double frame_rate_hz(VideoFormat format) {
  return (format == VideoFormat::PAL) ? 25.0 : (30000.0 / 1001.0);
}

/// SubRip timestamp: HH:MM:SS,mmm
std::string srt_timestamp(double seconds) {
  if (seconds < 0.0) {
    seconds = 0.0;
  }
  const int64_t total_ms = static_cast<int64_t>(std::llround(seconds * 1000.0));
  const int64_t ms = total_ms % 1000;
  const int64_t total_s = total_ms / 1000;
  const int64_t ss = total_s % 60;
  const int64_t mm = (total_s / 60) % 60;
  const int64_t hh = total_s / 3600;

  std::ostringstream oss;
  oss << std::setfill('0') << std::setw(2) << hh << ':' << std::setw(2) << mm
      << ':' << std::setw(2) << ss << ',' << std::setw(3) << ms;
  return oss.str();
}

/// Escape the five characters that cannot appear literally in HTML text.
std::string html_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

}  // namespace

void CCSinkStageDeps::init(TriggerProgressCallback progress_callback,
                           std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

CCExportResult CCSinkStageDeps::export_cc(
    VideoFrameRepresentation* representation,
    IObservationContext& observation_context, CCExportOptions options) {
  if (!representation) {
    return {false, "Input representation is null", 0};
  }

  if (options.output_path.empty()) {
    return {false, "output_path parameter is required", 0};
  }

  const auto frame_rng = representation->frame_range();
  if (frame_rng.count() == 0) {
    return {false, "Input has no frames", 0};
  }

  auto descriptor = representation->get_frame_descriptor(frame_rng.first);
  if (!descriptor.has_value()) {
    return {false, "Cannot determine video format", 0};
  }
  const VideoFormat video_format = video_format_from_system(descriptor->system);

  // Obtain a host-owned "closed_caption" observer session. The handle is reused
  // across every frame so the export runs the same standard observer the host
  // uses. A null service (older host) leaves the handle null; the export then
  // falls back to whatever observations already exist in the context.
  std::unique_ptr<IObserverHandle> cc_observer;
  if (observation_service_) {
    cc_observer = observation_service_->create_observer("closed_caption");
  }
  if (!cc_observer) {
    logger_.warn(
        "CCSinkDeps: observation service unavailable; closed caption data will "
        "be read from the context only");
  }

  int32_t cc_frames_exported = 0;
  bool success = false;

  // The report, and the XDS it summarises, cover the whole of line 21
  // whatever service the export is of.
  report_ = CCLine21Report{};
  xds_decoder_ = CCXdsDecoder{};
  xds_decoder_.set_packet_callback([this](const CCXdsPacket& packet) {
    report_.add_xds_packet(packet);
    if (xds_listener_) {
      xds_listener_(packet);
    }
  });

  if (options.all_services) {
    logger_.info("CCSinkDeps: Exporting every line 21 service into {}",
                 options.output_path);
    success =
        export_all(representation, options, video_format, observation_context,
                   cc_observer.get(), cc_frames_exported);
  } else if (options.export_xds) {
    logger_.info(
        "CCSinkDeps: Exporting XDS to {}: {}",
        options.export_format == CCExportFormat::HTML ? "HTML" : "plain text",
        options.output_path);
    success =
        export_xds(representation, options, video_format, observation_context,
                   cc_observer.get(), cc_frames_exported);
  } else if (options.export_format == CCExportFormat::SCC) {
    logger_.info("CCSinkDeps: Exporting {} to SCC format: {}",
                 cc_service_label(options.service), options.output_path);
    success =
        export_scc(representation, options, video_format, observation_context,
                   cc_observer.get(), cc_frames_exported);
  } else {
    logger_.info("CCSinkDeps: Exporting {} to {}: {}",
                 cc_service_label(options.service),
                 options.export_format == CCExportFormat::SRT    ? "SubRip"
                 : options.export_format == CCExportFormat::HTML ? "HTML"
                                                                 : "plain text",
                 options.output_path);
    success = export_decoded(representation, options, video_format,
                             observation_context, cc_observer.get(),
                             cc_frames_exported);
  }

  if (!success) {
    return {false, "Failed to export closed captions", cc_frames_exported, {}};
  }

  report_.finish();
  report_.set_xds_counts(xds_decoder_.packets_good(),
                         xds_decoder_.packets_bad());
  const std::string report = report_.text(frame_rng.count());
  logger_.info("CCSinkDeps: {}", report);
  if (options.write_report) {
    // Beside the output under its full name: captions.scc gives
    // captions.scc.report.txt. The all-services export puts it in its
    // folder, beside the services' files.
    const std::string report_path =
        options.all_services ? (std::filesystem::path(options.output_path) /
                                kAllServicesReportName)
                                   .string()
                             : options.output_path + ".report.txt";
    std::ofstream file(report_path);
    if (file.is_open()) {
      file << report;
    } else {
      logger_.warn("CCSinkDeps: Could not write the report to {}", report_path);
    }
  }

  return {true, "Exported " + std::to_string(cc_frames_exported) + " CC frames",
          cc_frames_exported, report};
}

std::string CCSinkStageDeps::generate_timestamp(int32_t field_index,
                                                VideoFormat format) const {
  double frame_index = static_cast<double>(
      (field_index - 1) / 2);  // NOLINT(bugprone-integer-division)

  const double frames_per_second = frame_rate_hz(format);
  const double frames_per_minute = frames_per_second * 60.0;
  const double frames_per_hour = frames_per_minute * 60.0;

  const int32_t hh = static_cast<int32_t>(frame_index / frames_per_hour);
  frame_index -= static_cast<double>(hh) * frames_per_hour;
  const int32_t mm = static_cast<int32_t>(frame_index / frames_per_minute);
  frame_index -= static_cast<double>(mm) * frames_per_minute;
  const int32_t ss = static_cast<int32_t>(frame_index / frames_per_second);
  frame_index -= static_cast<double>(ss) * frames_per_second;
  const int32_t ff = static_cast<int32_t>(frame_index);

  std::ostringstream oss;
  oss << std::setfill('0') << std::setw(2) << hh << ":" << std::setfill('0')
      << std::setw(2) << mm << ":" << std::setfill('0') << std::setw(2) << ss
      << ":" << std::setfill('0') << std::setw(2) << ff;

  return oss.str();
}

uint8_t CCSinkStageDeps::apply_odd_parity(uint8_t byte) const {
  uint8_t val = byte & 0x7F;
  int count = 0;
  uint8_t tmp = val;
  while (tmp) {
    count += static_cast<int>(tmp & 1U);
    tmp >>= 1;
  }
  if (count % 2 == 0) {
    val |= 0x80;
  }
  return val;
}

bool CCSinkStageDeps::is_control_code(uint8_t byte) const {
  return byte >= 0x10 && byte <= 0x1F;
}

bool CCSinkStageDeps::for_each_caption_pair(
    const VideoFrameRepresentation* representation,
    IObservationContext& observation_context, IObserverHandle* cc_observer,
    const std::function<void(const CaptionPair&)>& fn) {
  const auto frame_rng = representation->frame_range();
  const uint64_t total_frames = frame_rng.count();

  // Every pair of the run, kept until the field order is known. (A run of 2^32
  // frames would be two years of video.)
  EIA608Line21Run run;

  for (FrameID frame_id = frame_rng.first; frame_id <= frame_rng.last;
       ++frame_id) {
    if (cancel_requested_ && cancel_requested_->load()) {
      logger_.warn("CCSinkDeps: Cancelled during closed caption export");
      return false;
    }
    if (progress_callback_) {
      const uint64_t done = frame_id - frame_rng.first + 1;
      progress_callback_(done, total_frames, "Processing closed captions...");
    }

    // Run the standard closed_caption observer for this frame so its results
    // land in the context (fields frame_id*2 and frame_id*2 + 1). When the
    // service is unavailable, read whatever is already present.
    //
    // Phase 5.3: when the host has pre-loaded this frame's observation from
    // the provenance-keyed store, skip re-running the observer.
    // closed_caption is stateful, so the host only pre-loads when the whole
    // range is covered (all-or-nothing); a covered frame here therefore means
    // the entire run is covered and the stream's continuity is preserved.
    const bool frame_covered =
        observation_context.has(FieldID(frame_id * 2), "closed_caption",
                                "present") ||
        observation_context.has(FieldID(frame_id * 2 + 1), "closed_caption",
                                "present");
    if (cc_observer && representation->has_frame(frame_id) && !frame_covered) {
      cc_observer->process_frame(*representation, frame_id,
                                 observation_context);
    }

    const auto frame_offset = static_cast<uint32_t>(frame_id - frame_rng.first);
    for (int field_idx = 0; field_idx < 2; ++field_idx) {
      const FieldID field_id(frame_id * 2 + static_cast<uint64_t>(field_idx));
      run.add(frame_offset, field_idx,
              eia608_field_reading(observation_context, field_id),
              eia608_other_line_reading(observation_context, field_id));
    }

    if (cc_observer) {
      observation_context.clear_field(FieldID(frame_id * 2));
      observation_context.clear_field(FieldID(frame_id * 2 + 1));
    }
  }

  // A capture whose first stored field is television field 2 is read the
  // other way round, so that its CC1 comes out as CC1.
  const bool reversed = run.fields_reversed();
  report_.set_fields_reversed(reversed);
  for (int stored = 0; stored < 2; ++stored) {
    const int field = reversed ? 1 - stored : stored;
    report_.add_lines(field, run.lines(stored), run.carried(stored));
    for (const int line : {22, 20}) {
      report_.add_neighbour_line(field, line, run.taken_from(stored, line));
    }
  }
  if (reversed) {
    logger_.info(
        "CCSinkDeps: the capture's first stored field is television field 2; "
        "its fields are read the other way round");
  }

  run.for_each([&](const EIA608Line21Run::Pair& read_pair) {
    CaptionPair pair;
    pair.field_in_frame = read_pair.television_field;
    pair.field_index = (frame_rng.first + read_pair.frame_offset) * 2 +
                       static_cast<uint64_t>(read_pair.stored_field);
    pair.data0 = read_pair.data0;
    pair.data1 = read_pair.data1;

    // Taken only when the pair is one EIA-608 defines for its field — a
    // receiver acts on nothing else — and the field carries line 21 data
    // around it.
    const bool taken = read_pair.taken();
    if (taken && pair.field_in_frame == 1) {
      pair.xds = xds_decoder_.accept(pair.data0, pair.data1, pair.field_index);
    }

    report_.add_pair(read_pair, pair.xds);
    if (taken) {
      fn(pair);
    }
  });

  return true;
}

bool CCSinkStageDeps::export_scc(const VideoFrameRepresentation* representation,
                                 const CCExportOptions& options,
                                 VideoFormat format,
                                 IObservationContext& observation_context,
                                 IObserverHandle* cc_observer,
                                 int32_t& cc_frames_exported) {
  try {
    std::ofstream file(options.output_path);
    if (!file.is_open()) {
      logger_.error("CCSinkDeps: Failed to open output file: {}",
                    options.output_path);
      return false;
    }

    file << "Scenarist_SCC V1.0";

    // The byte pairs are written exactly as transmitted, including the channel
    // bits of the selected service and the duplicate copy of every control
    // pair: an SCC file is a record of the line 21 stream, and the tools that
    // read one de-duplicate for themselves.
    EIA608ServiceDemux demux(options.service,
                             /*suppress_repeated_controls=*/false);

    bool caption_in_progress = false;
    std::string debug_caption;

    const bool completed = for_each_caption_pair(
        representation, observation_context, cc_observer,
        [&](const CaptionPair& pair) {
          if (pair.xds) {
            return;  // the Extended Data Service's, not a caption's
          }
          const bool mine =
              demux.accept(pair.field_in_frame, pair.data0, pair.data1);

          // A field whose pair went to one of the other three services (or to
          // none, being null padding) breaks the run: what follows is written
          // as a fresh timestamped block rather than being run on to the bytes
          // before it.
          if (!mine || (pair.data0 == 0 && pair.data1 == 0)) {
            if (caption_in_progress) {
              debug_caption += "]";
              logger_.debug("CCSinkDeps: {}", debug_caption);
            }
            caption_in_progress = false;
            return;
          }

          if (!caption_in_progress) {
            const std::string timestamp = generate_timestamp(
                static_cast<int32_t>(pair.field_index + 1), format);
            file << "\n\n" << timestamp << "\t";

            debug_caption = "Caption at " + timestamp + " : [";
            caption_in_progress = true;
          }

          const uint8_t scc0 = apply_odd_parity(pair.data0);
          const uint8_t scc1 = apply_odd_parity(pair.data1);
          file << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(scc0) << std::setfill('0') << std::setw(2)
               << static_cast<int>(scc1) << " ";

          if (is_control_code(pair.data0)) {
            debug_caption += " ";
          } else {
            const char chars[3] = {static_cast<char>(pair.data0),
                                   static_cast<char>(pair.data1), 0};
            debug_caption += std::string(chars);
          }

          cc_frames_exported++;
        });

    if (!completed) {
      return false;
    }

    file << "\n\n";
    file.close();

    logger_.info("CCSinkDeps: Exported {} SCC caption fields",
                 cc_frames_exported);
    return true;

  } catch (const std::exception& e) {
    logger_.error("CCSinkDeps: Error exporting SCC: {}", e.what());
    return false;
  }
}

bool CCSinkStageDeps::export_xds(const VideoFrameRepresentation* representation,
                                 const CCExportOptions& options,
                                 VideoFormat format,
                                 IObservationContext& observation_context,
                                 IObserverHandle* cc_observer,
                                 int32_t& cc_frames_exported) {
  try {
    std::ofstream file(options.output_path);
    if (!file.is_open()) {
      logger_.error("CCSinkDeps: Failed to open output file: {}",
                    options.output_path);
      return false;
    }
    const bool html = options.export_format == CCExportFormat::HTML;
    if (html) {
      file << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
           << "<meta charset=\"utf-8\">\n"
           << "<title>Extended Data Service</title>\n"
           << "<style>\n"
           << "body { font-family: sans-serif; margin: 2em; }\n"
           << "table { border-collapse: collapse; }\n"
           << "td { vertical-align: top; padding: 0.15em 1em 0.15em 0; }\n"
           << "td.time { font-family: monospace; white-space: nowrap;"
           << " color: #666; }\n"
           << "</style>\n</head>\n<body>\n"
           << "<h1>Extended Data Service</h1>\n<table>\n";
    }

    XdsChanges changes;
    xds_listener_ = [&](const CCXdsPacket& packet) {
      std::string name;
      std::string value;
      if (!changes.changed(packet, name, value)) {
        return;
      }
      const std::string time = generate_timestamp(
          static_cast<int32_t>(packet.field_index + 1), format);
      if (html) {
        file << "<tr><td class=\"time\">" << time << "</td><td>"
             << html_escape(name) << "</td><td>" << html_escape(value)
             << "</td></tr>\n";
      } else {
        file << time << "  " << name << ": " << value << "\n";
      }
      ++cc_frames_exported;
    };

    const bool completed =
        for_each_caption_pair(representation, observation_context, cc_observer,
                              [](const CaptionPair& /*pair*/) {});
    xds_listener_ = nullptr;
    if (!completed) {
      return false;
    }
    if (html) {
      file << "</table>\n</body>\n</html>\n";
    }
    file.close();
    logger_.info("CCSinkDeps: Exported {} XDS value(s)", cc_frames_exported);
    return true;
  } catch (const std::exception& e) {
    xds_listener_ = nullptr;
    logger_.error("CCSinkDeps: Error exporting XDS: {}", e.what());
    return false;
  }
}

bool CCSinkStageDeps::export_decoded(
    const VideoFrameRepresentation* representation,
    const CCExportOptions& options, VideoFormat format,
    IObservationContext& observation_context, IObserverHandle* cc_observer,
    int32_t& cc_frames_exported) {
  try {
    std::ofstream file(options.output_path);
    if (!file.is_open()) {
      logger_.error("CCSinkDeps: Failed to open output file: {}",
                    options.output_path);
      return false;
    }

    eia608_decoder_ = EIA608Decoder{};

    // Decoding acts on the stream, so here the duplicate copy of each control
    // pair must be dropped: acted on twice, a roll-up scrolls two lines and a
    // caption is displayed twice.
    EIA608ServiceDemux demux(options.service,
                             /*suppress_repeated_controls=*/true);

    const double frames_per_second = frame_rate_hz(format);
    double last_timestamp = 0.0;

    const bool completed = for_each_caption_pair(
        representation, observation_context, cc_observer,
        [&](const CaptionPair& pair) {
          if (pair.xds ||
              !demux.accept(pair.field_in_frame, pair.data0, pair.data1)) {
            return;
          }

          // Field index to seconds: two fields per frame.
          const double timestamp =
              static_cast<double>(pair.field_index) / (2.0 * frames_per_second);
          last_timestamp = timestamp;
          eia608_decoder_.process_bytes(timestamp, pair.data0, pair.data1);
          cc_frames_exported++;
        });

    if (!completed) {
      return false;
    }

    // Close whatever was still on screen when the run ended — without this the
    // last caption of every recording is dropped.
    const auto frame_rng = representation->frame_range();
    const double end_time =
        std::max(last_timestamp,
                 static_cast<double>(frame_rng.last + 1) / frames_per_second);
    const std::vector<CaptionCue> cues = eia608_decoder_.finalize(end_time);
    logger_.info("CCSinkDeps: Extracted {} caption cues for {}", cues.size(),
                 cc_service_label(options.service));

    write_cues(file, cues, options.export_format, options.service, format);
    file.close();
    return true;

  } catch (const std::exception& e) {
    logger_.error("CCSinkDeps: Error exporting closed captions: {}", e.what());
    return false;
  }
}

void CCSinkStageDeps::write_cues(std::ostream& out,
                                 const std::vector<CaptionCue>& cues,
                                 CCExportFormat export_format,
                                 EIA608Service service,
                                 VideoFormat format) const {
  const double frames_per_second = frame_rate_hz(format);
  switch (export_format) {
    case CCExportFormat::SRT: {
      size_t index = 1;
      for (const auto& cue : cues) {
        out << index++ << "\n"
            << srt_timestamp(cue.start_time) << " --> "
            << srt_timestamp(cue.end_time) << "\n"
            << cue.text << "\n\n";
      }
      break;
    }

    case CCExportFormat::HTML: {
      // Monospaced, and every line inside a <pre>: a text service positions
      // its text by column, so a page of listings or scores only reads
      // correctly while the columns still line up.
      out << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
          << "<meta charset=\"utf-8\">\n"
          << "<title>Closed captions — " << cc_service_label(service)
          << "</title>\n"
          << "<style>\n"
          << "body { font-family: sans-serif; margin: 2em; }\n"
          << "table { border-collapse: collapse; }\n"
          << "td { vertical-align: top; padding: 0.15em 1em 0.15em 0; }\n"
          << "td.time { font-family: monospace; white-space: nowrap;"
          << " color: #666; }\n"
          << "pre { font-family: monospace; margin: 0; }\n"
          << "</style>\n</head>\n<body>\n"
          << "<h1>Closed captions — " << cc_service_label(service) << "</h1>\n"
          << "<table>\n";
      for (const auto& cue : cues) {
        out << "<tr><td class=\"time\">" << srt_timestamp(cue.start_time)
            << "</td><td><pre>" << html_escape(cue.text)
            << "</pre></td></tr>\n";
      }
      out << "</table>\n</body>\n</html>\n";
      break;
    }

    case CCExportFormat::PLAIN_TEXT:
    default: {
      for (const auto& cue : cues) {
        const int frame_number =
            static_cast<int>(cue.start_time * frames_per_second * 2.0);
        // The decoder's text is UTF-8 — the music note, the accented letters
        // — in rows joined by newlines (eia608_cells_to_utf8()).
        out << "\n[" << generate_timestamp(frame_number, format) << "]\n"
            << cue.text << "\n";
      }
      break;
    }
  }
}

bool CCSinkStageDeps::export_all(const VideoFrameRepresentation* representation,
                                 const CCExportOptions& options,
                                 VideoFormat format,
                                 IObservationContext& observation_context,
                                 IObserverHandle* cc_observer,
                                 int32_t& cc_frames_exported) {
  try {
    const std::filesystem::path folder(options.output_path);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
      logger_.error("CCSinkDeps: Failed to create the output folder {}: {}",
                    options.output_path, error.message());
      return false;
    }

    // Each service has a demultiplexer and a decoder of its own, fed every
    // pair just as the one-service export feeds its single pair of them, so
    // each file is the one that export would write: one pass instead of nine.
    struct ServiceStream {
      EIA608Service service;
      EIA608ServiceDemux demux;
      EIA608Decoder decoder;
    };
    std::vector<ServiceStream> streams;
    streams.reserve(kAllServices.size());
    for (const EIA608Service service : kAllServices) {
      streams.push_back(
          {service,
           EIA608ServiceDemux(service,
                              /*suppress_repeated_controls=*/true),
           EIA608Decoder{}});
    }

    std::string xds_listing;
    XdsChanges changes;
    xds_listener_ = [&](const CCXdsPacket& packet) {
      std::string name;
      std::string value;
      if (changes.changed(packet, name, value)) {
        xds_listing +=
            generate_timestamp(static_cast<int32_t>(packet.field_index + 1),
                               format) +
            "  " + name + ": " + value + "\n";
      }
    };

    const double frames_per_second = frame_rate_hz(format);
    const bool completed = for_each_caption_pair(
        representation, observation_context, cc_observer,
        [&](const CaptionPair& pair) {
          if (pair.xds) {
            return;
          }
          // Field index to seconds: two fields per frame.
          const double timestamp =
              static_cast<double>(pair.field_index) / (2.0 * frames_per_second);
          for (ServiceStream& stream : streams) {
            if (stream.demux.accept(pair.field_in_frame, pair.data0,
                                    pair.data1)) {
              stream.decoder.process_bytes(timestamp, pair.data0, pair.data1);
              ++cc_frames_exported;
            }
          }
        });
    xds_listener_ = nullptr;
    if (!completed) {
      return false;
    }

    // Whatever was still on screen when the run ended closes with it.
    const auto frame_rng = representation->frame_range();
    const double end_time =
        static_cast<double>(frame_rng.last + 1) / frames_per_second;
    for (ServiceStream& stream : streams) {
      const std::vector<CaptionCue> cues = stream.decoder.finalize(end_time);
      logger_.info("CCSinkDeps: Extracted {} caption cues for {}", cues.size(),
                   cc_service_label(stream.service));
      if (cues.empty()) {
        continue;
      }
      const bool text = eia608_service_is_text(stream.service);
      const std::filesystem::path path =
          folder / (std::string(cc_service_label(stream.service)) +
                    (text ? ".txt" : ".srt"));
      std::ofstream file(path);
      if (!file.is_open()) {
        logger_.error("CCSinkDeps: Failed to open output file: {}",
                      path.string());
        return false;
      }
      write_cues(file, cues,
                 text ? CCExportFormat::PLAIN_TEXT : CCExportFormat::SRT,
                 stream.service, format);
    }

    if (!xds_listing.empty()) {
      const std::filesystem::path path = folder / "XDS.txt";
      std::ofstream file(path);
      if (!file.is_open()) {
        logger_.error("CCSinkDeps: Failed to open output file: {}",
                      path.string());
        return false;
      }
      file << xds_listing;
    }
    return true;
  } catch (const std::exception& e) {
    xds_listener_ = nullptr;
    logger_.error("CCSinkDeps: Error exporting closed captions: {}", e.what());
    return false;
  }
}
}  // namespace orc
