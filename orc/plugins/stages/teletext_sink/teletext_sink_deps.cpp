/*
 * File:        teletext_sink_deps.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     TeletextSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_sink_deps.h"

#include <orc/plugin/orc_stage_services.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/logging.h>
#include <orc/support/pipe_io.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "teletext_block_scanner.h"
#include "teletext_frame_slicer.h"
#include "teletext_page_catalogue.h"
#include "teletext_squash_stats.h"
#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/teletext_recovery_stats.h"
#include "vbi-services/teletext_row_squasher.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {

namespace {

constexpr size_t kWriterBufferBytes = 1UL * 1024UL * 1024UL;

// Page runs the squasher retains for the rewrite pass (see where it is used).
// A run costs its received rows: at most 24 × 16 copies × ~210 bytes, but a
// run created by an erase-on-every-transmission service holds one copy per row
// and costs around 5 kB. The bound is the point past which a recording's older
// runs start going uncorrected rather than a memory ceiling being enforced.
constexpr size_t kSquashPageRunBound = 4096;

// Progress callbacks are throttled to once every N frames (plus the final
// frame) so tight loops do not flood the UI.
constexpr uint64_t kProgressThrottleFrames = 10;

// A whole zero packet, emitted for a candidate line with no data when
// keep_empty_packets is enabled — the vhs-decode convention giving a 1:1
// packet-to-(frame, field, line) mapping. Only the service's packet length is
// written from it.
constexpr std::array<uint8_t, kTeletextPacketBytes> kEmptyPacket{};

// ITU-R BT.1700 Annex 1 Part B Table 1 item 2: 625-line PAL scans 50 fields
// per second; subtitle cue timing derives from the field index (the same
// field-number/field-rate derivation as the closed-caption sink).
constexpr double kPalFieldsPerSecond = 50.0;

// Candidate lines one field can yield, which sizes the packets-per-field
// histogram behind the lost-packet estimate. One more than the widest window
// this stage will probe, so every possible count has a bucket.
constexpr size_t kMaxPacketsPerFieldTracked = 40;

// Format a field index as an SRT timestamp (HH:MM:SS,mmm).
std::string srt_timestamp(int64_t field_index) {
  const double seconds_total =
      static_cast<double>(field_index) / kPalFieldsPerSecond;
  const int64_t millis_total = std::llround(seconds_total * 1000.0);
  const int64_t hours = millis_total / 3'600'000;
  const int64_t minutes = (millis_total / 60'000) % 60;
  const int64_t seconds = (millis_total / 1'000) % 60;
  const int64_t millis = millis_total % 1'000;
  return fmt::format("{:02}:{:02}:{:02},{:03}", hours, minutes, seconds,
                     millis);
}

// One emitted packet, held so squashing can rewrite it once every copy of
// every row has been seen. |empty| marks a keep_empty_packets placeholder.
//
// The recovered stream of a long source is tens of millions of these (600k
// frames × up to 34 candidate lines), so the entry is kept lean: the per-byte
// confidence — absent entirely for threshold-detected packets, which is every
// packet of a disc or direct capture — lives in a side pool rather than as a
// 168-byte float array inside every entry.
struct StreamEntry {
  std::array<uint8_t, kTeletextPacketBytes> bytes;
  int64_t field_index;
  bool empty;
  // How sure the recovery chain was of each byte, and whether it could say.
  // Held with the packet because the rewrite pass re-feeds every copy under
  // its original identity: a re-feed that dropped the confidence would replace
  // a weighted copy with an unweighted one. |confidence_slot| indexes the
  // confidence pool when |has_confidence| is set.
  bool has_confidence;
  uint32_t confidence_slot;
};

// Pooled per-byte confidence, quantised to 8 bits (value × 255). The direct
// slicer path loses at most 1/255 of a vote weight per byte.
using QuantizedPacketConfidence = std::array<uint8_t, kTeletextPacketBytes>;

// Display row a packet is addressed to (X/1 to X/24, ETSI EN 300 706 §9.3.2),
// or 0 when the MRAG does not decode or the packet is not a displayable row.
// The character figures are counted over these: they are the packets whose
// data bytes carry byte-wise odd parity and are shown on screen.
int display_row_of(const std::array<uint8_t, kTeletextPacketBytes>& packet) {
  const int mrag_low = teletext_hamming84_decode(packet[0]);
  const int mrag_high = teletext_hamming84_decode(packet[1]);
  if (mrag_low < 0 || mrag_high < 0) {
    return 0;
  }
  const int row = ((mrag_low >> 3) & 0x01) | ((mrag_high << 1) & 0x1E);
  return (row >= 1 && row < TeletextPageSnapshot::kRows) ? row : 0;
}

// The display bytes of a packet (§9.3.2: the payload after the 2 MRAG bytes),
// in a 40-byte row buffer. A 525-line packet carries 32 of them and the rest
// stay zero — see TeletextSquashStats::add_row()'s column count.
TeletextRowBytes display_bytes_of(
    const std::array<uint8_t, kTeletextPacketBytes>& packet) {
  TeletextRowBytes row{};
  std::copy(packet.begin() + 2, packet.begin() + 2 + kTeletextRowBytes,
            row.begin());
  return row;
}

// Name a detector for the report, in the wording the parameter uses.
const char* detector_name(TeletextDetector detector) {
  switch (detector) {
    case TeletextDetector::kThreshold:
      return "Threshold";
    case TeletextDetector::kMlse:
      return "MLSE";
    case TeletextDetector::kAuto:
      return "Automatic";
  }
  return "Automatic";
}

// Render the decoder's cues as a SubRip document.
std::string format_srt(const std::vector<TeletextSubtitleCue>& cues) {
  std::string srt;
  size_t index = 1;
  for (const auto& cue : cues) {
    srt += std::to_string(index++);
    srt += '\n';
    srt += srt_timestamp(cue.start_field_index);
    srt += " --> ";
    srt += srt_timestamp(cue.end_field_index);
    srt += '\n';
    srt += cue.text;
    srt += "\n\n";
  }
  return srt;
}

/**
 * @brief Packets a recording lost, estimated from the packets-per-field spread
 *
 * A service part-way through a page fills every VBI line it is inserting on,
 * in every field it uses, so the *usual* packets-per-field count says how many
 * lines the service is using and every field short of it is short by packets
 * the recording lost.
 *
 * The usual count is taken as the mode of the non-zero counts rather than the
 * maximum: a single field where the slicer false-locked an extra line would
 * otherwise accuse every other field of a loss. Fields that yielded nothing at
 * all are left out — plenty of services insert into one field of each frame
 * only, and calling the other one a total loss would put a fault on every
 * page. That under-reports a field where every line was lost, which is the
 * right way to be wrong: the point of the figure is to stop claiming faults
 * that are not there.
 */
uint64_t estimate_lost_packets(
    const std::array<uint64_t, kMaxPacketsPerFieldTracked + 1>& histogram) {
  size_t mode = 0;
  uint64_t mode_fields = 0;
  for (size_t count = 1; count <= kMaxPacketsPerFieldTracked; ++count) {
    if (histogram[count] > mode_fields) {
      mode_fields = histogram[count];
      mode = count;
    }
  }
  if (mode == 0) {
    return 0;
  }

  uint64_t lost = 0;
  for (size_t count = 1; count < mode; ++count) {
    lost += histogram[count] * static_cast<uint64_t>(mode - count);
  }
  return lost;
}

}  // namespace

std::string TeletextSinkDeps::build_report(
    const TeletextSinkOptions& options, const TeletextSinkResult& result,
    uint64_t total_frames, const TeletextRecoveryStats& stats,
    const TeletextSquashStats& squash_stats,
    const TeletextScanState& scan_state) {
  // The headline first: the one figure a reader wants before deciding whether
  // any of the detail below is worth their time.
  std::string report = "Teletext analysis report\n";
  const std::string loss = squash_stats.character_loss_summary();
  if (!loss.empty()) {
    report += "  " + loss + "\n\n";
  }
  report += fmt::format(
      "  Output:        {}\n"
      "  Frames:        {}\n"
      "  VBI lines:     {}-{} of each field (1-based)\n"
      "  Detector:      {}\n"
      // Which alphabet the catalogued pages were read in. Worth stating even
      // at its default: it cannot be recovered from the packet stream, which
      // is the transmitted bytes and carries no character set, and a reader
      // comparing pages against a photograph of the same service has no other
      // way to know what was assumed. Qualified because a service that
      // designates its own set (a packet X/28 or M/29) overrides this. The
      // second set is named alongside it when there is one, because a reader
      // seeing two alphabets on one page has the same question about the
      // second as about the first.
      "  Character set: {}{} (unless a page designates its own)\n"
      "  Packets:       {} written, {} fields carried data",
      result.output_path.empty() ? std::string("(none; pages browsed only)")
                                 : result.output_path,
      total_frames, options.first_field_line + 1, options.last_field_line + 1,
      detector_name(options.detector), to_string(options.character_set),
      options.second_character_set.has_value()
          ? fmt::format(", switching to {}",
                        to_string(options.second_character_set->g0_set))
          : std::string(),
      result.packets_written, result.fields_with_data);
  if (options.keep_empty_packets) {
    report += " (empty candidate lines padded)";
  }
  if (result.bytes_repaired > 0) {
    report += fmt::format("\n  Parity repair: {} damaged display bytes mended",
                          result.bytes_repaired);
  }
  if (!result.subtitle_path.empty()) {
    report += fmt::format("\n  Subtitles:     {} cues from page {} to {}",
                          result.subtitle_cues_written, options.subtitle_page,
                          result.subtitle_path);
  }
  report += fmt::format("\n  Pages:         {} catalogued{}",
                        result.dataset.pages.size(),
                        result.dataset.summary.pages_truncated
                            ? " (page cap reached; oldest pages dropped)"
                            : "");
  if (result.dataset.summary.lost_packets_estimate > 0) {
    report += fmt::format(
        "\n  Lost packets:  {} estimated from the spread of "
        "packets per field",
        result.dataset.summary.lost_packets_estimate);
  }

  // The page numbers the recording named but the service never transmitted.
  // Reported whenever the pass had anything to say, including when it stood
  // aside: a reader comparing this run's page count with an earlier one's needs
  // to know which of the two is looking at a pruned catalogue.
  const std::string reconciliation =
      result.dataset.summary.identity_reconciliation.summary("page");
  if (!reconciliation.empty()) {
    report += "\n  Identities:    " + reconciliation;
  }

  // What the pass learned about the recording, and what that saved. Both are
  // reported whether or not they were enabled, because the interesting case is
  // the one where the reader is wondering why a run took as long as it did.
  const TeletextPhaseHint hint = scan_state.phase().hint();
  report += "\n  Data phase:    ";
  if (!options.pin_data_phase) {
    report += "not pinned (searched in full on every line)";
  } else if (hint.valid) {
    report +=
        fmt::format("pinned to sample {:.1f} ±{:.1f} from {} locks",
                    hint.centre, hint.radius, scan_state.phase().locks_seen());
  } else {
    report += fmt::format(
        "not pinned; {} locks were too few or too scattered to agree",
        scan_state.phase().locks_seen());
  }
  report += "\n  Line probing:  ";
  if (!options.learn_active_lines) {
    report += "every candidate line of every frame";
  } else {
    report += fmt::format("{} candidate lines skipped as never carrying data",
                          scan_state.lines().lines_skipped());
  }

  if (stats.lines_seen() > 0) {
    report += "\n\n" + stats.summary();
  }

  report += "\n\n";
  report += options.squash_repeated_rows
                ? squash_stats.summary()
                : "Teletext squashing: disabled; packets written as recovered";
  return report;
}

void TeletextSinkDeps::write_report(const TeletextSinkOptions& options,
                                    TeletextSinkResult& result) const {
  // No packet stream means nothing for the report to sit beside; the stage
  // refuses the combination up front, and the report still reaches the log.
  if (!options.write_report || stage_services_ == nullptr ||
      result.output_path.empty()) {
    return;
  }
  // Sits beside the packet stream under its full name — mydata.t42.txt — so
  // it cannot collide with the .srt, or with a second export writing a
  // differently-named stream into the same directory.
  const std::string path = result.output_path + ".txt";
  std::shared_ptr<IFileWriterUint8> report_writer =
      stage_services_->create_buffered_file_writer_uint8(kWriterBufferBytes);
  if (!report_writer || !report_writer->open(path)) {
    ORC_LOG_WARN("TeletextSinkDeps: could not open report file: {}", path);
    return;
  }
  const std::string text = result.report + "\n";
  report_writer->write(reinterpret_cast<const uint8_t*>(text.data()),
                       text.size());
  report_writer->close();
  result.report_path = path;
}

void TeletextSinkDeps::init(TriggerProgressCallback progress_callback,
                            std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

TeletextSinkResult TeletextSinkDeps::analyse(
    const VideoFrameRepresentation* representation,
    const TeletextSinkOptions& options) {
  TeletextSinkResult result;

  if (!representation) {
    result.message = "Input representation is null";
    return result;
  }

  const auto vp_opt = representation->get_video_parameters();
  if (!vp_opt.has_value() || !TeletextFrameSlicer::applies_to(vp_opt->system)) {
    result.message =
        "Input carries no World System Teletext service (ITU-R BT.653 System "
        "B is defined on PAL, NTSC and PAL-M only)";
    return result;
  }
  const auto& vp = vp_opt.value();

  // The service decides the packet length, and with it the stream's extension:
  // the flat file is a run of whole packets with no header, so a reader can
  // only tell 42-byte packets from 34-byte ones by what the file is called.
  const TeletextFrameSlicer::SystemProfile profile =
      TeletextFrameSlicer::profile_for(vp.system);
  const size_t packet_bytes = teletext_packet_bytes(profile.teletext_system);
  const size_t display_bytes = packet_bytes - 2;
  const std::string extension =
      (packet_bytes == kTeletextPacketBytes) ? ".t42" : ".t34";

  // No output path is the browse-only run: everything below happens as it
  // would, and only the writing is skipped. The page catalogue is a product of
  // the pass in its own right, so a caller that wants the pages and no file
  // pays for the decode and nothing else.
  const bool export_stream = !options.output_path.empty();
  const bool piping =
      export_stream && orc::pipe_io::is_pipe_path(options.output_path);
  std::string output_path;
  if (export_stream) {
    output_path = options.output_path;
    if (!piping &&
        (output_path.length() < extension.length() ||
         output_path.compare(output_path.length() - extension.length(),
                             extension.length(), extension) != 0)) {
      output_path += extension;
      ORC_LOG_DEBUG("TeletextSinkDeps: Added {} extension: {}", extension,
                    output_path);
    }
    result.output_path = output_path;
  }

  const auto frame_rng = representation->frame_range();
  const uint64_t total_frames = frame_rng.count();
  if (total_frames == 0) {
    result.message = "Input has no frames";
    return result;
  }
  result.dataset.summary.frames_analysed = total_frames;

  // The subtitle document is named after the packet stream and written beside
  // it, so it has nowhere to go on a browse-only run.
  if (options.export_subtitles && !export_stream) {
    result.message =
        "Subtitle export needs an output file (the cues are written beside the "
        "packet stream)";
    return result;
  }

  // A pipe carries the primary packet stream alone: the report and subtitle
  // files are separate outputs named after output_path, which "-" does not
  // identify a location for. The stage's parse_config() already refuses this
  // combination before trigger(); repeated here since analyse() is reachable
  // directly through the deps interface (e.g. tests) without going through it.
  if (piping && (options.export_subtitles || options.write_report)) {
    result.message =
        "Cannot pipe the teletext stream to stdout ('-') together with "
        "export_subtitles or write_report: those write separate files named "
        "after output_path, which \"-\" does not identify. Disable them, or "
        "write to a real file instead.";
    return result;
  }

  std::shared_ptr<IFileWriterUint8> writer;
  if (export_stream) {
    if (stage_services_) {
      writer = stage_services_->create_buffered_file_writer_uint8(
          kWriterBufferBytes);
    }
    if (!writer) {
      result.message = "Failed to create output writer service";
      return result;
    }
    if (!writer->open(output_path)) {
      result.message = "Failed to open output file: " + output_path;
      return result;
    }
  }

  // The packet stream's only two operations, no-ops when it is not exported.
  const auto emit = [&writer](const uint8_t* data, size_t count) {
    if (writer) {
      writer->write(data, count);
    }
  };
  const auto close_stream = [&writer]() {
    if (writer) {
      writer->close();
    }
  };

  TeletextFrameSlicerOptions slicer_options;
  slicer_options.detector = options.detector;
  slicer_options.parity_repair = options.parity_repair;
  slicer_options.tolerant_framing = options.tolerant_framing;
  slicer_options.require_valid_mrag = options.require_valid_mrag;
  slicer_options.first_field_line = options.first_field_line;
  slicer_options.last_field_line = options.last_field_line;
  const TeletextFrameSlicer frame_slicer(slicer_options);

  // What this pass learns about the recording as it goes: where in the line
  // its data bursts start, and which candidate lines carry them at all. Owned
  // here because the frame slicer is const and shareable and this is neither.
  TeletextScanState scan_state(options.pin_data_phase,
                               options.learn_active_lines);

  // Recovery diagnostics over the whole run, from the full line results.
  TeletextRecoveryStats stats;
  // What combining repeated rows changed, accumulated in the rewrite pass.
  TeletextSquashStats squash_stats;
  // Packets recovered per field, bucketed by count — the lost-packet estimate
  // reads the shape of this rather than holding one entry per field, so a
  // ten-hour capture costs the same as a ten-second one.
  std::array<uint64_t, kMaxPacketsPerFieldTracked + 1> packets_per_field{};

  // Squashing needs every copy of a row before it can combine them, so the
  // recovered stream is held and rewritten in a second pass. Without it the
  // stream is written straight out and pages assemble inline.
  const bool squash = options.squash_repeated_rows;
  // The rewrite pass asks about runs of a page that were transmitted long
  // before the packet it is rewriting, so the squasher's page bound has to
  // hold every run of the recording rather than the recently-used working set
  // a live previewer needs. Erases multiply runs (see TeletextPageKey), so a
  // carousel that erases as it re-sends can reach several thousand. A run
  // evicted before the rewrite reaches it simply goes uncorrected — the vote
  // falls back to the single copy re-fed for it — so the cost of the bound is
  // lost correction, never wrong output. Proportionate to the packet stream
  // this pass already holds in memory.
  TeletextRowSquasher::Options squasher_options;
  squasher_options.max_pages = kSquashPageRunBound;
  TeletextRowSquasher squasher(squasher_options);
  std::vector<StreamEntry> stream;
  std::vector<QuantizedPacketConfidence> confidence_pool;
  std::optional<TeletextPageDecoder> squash_pass;
  if (squash) {
    squash_pass.emplace();
    squash_pass->set_row_squasher(&squasher);
  }

  // Every packet the run emits is fed, in emission order, to one decoder: it
  // catalogues the pages for the viewer, attributes the rows the rewrite pass
  // corrects, and — when asked — watches the subtitle page. One decoder for
  // all three because they want the same stream in the same order, and a
  // second one would re-derive page assembly from scratch to no purpose.
  TeletextPageCatalogue catalogue;
  TeletextPageDecoder output_decoder;
  // Only the decoder whose pages are rendered needs the fallback alphabet: the
  // squash pass exists to attribute rows to pages, which the character set has
  // no part in.
  output_decoder.set_default_g0_set(options.character_set);
  output_decoder.set_default_second_g0_set(options.second_character_set);
  if (squash) {
    output_decoder.set_row_squasher(&squasher);
  }
  output_decoder.set_page_callback([&](const TeletextPageSnapshot& snapshot) {
    // Snapshot field indices are relative to the start of the export range.
    const uint64_t frame_id =
        frame_rng.first + static_cast<uint64_t>(std::max<int64_t>(
                              0, snapshot.header_field_index)) /
                              2;
    catalogue.merge(snapshot, frame_id);
  });

  // Subtitle export: the SubRip timing derives from the 50 fields/s of a
  // 625-line service, so a 525-line source is refused rather than given cues
  // that drift by a fifth.
  const bool export_subtitles =
      options.export_subtitles && packet_bytes == kTeletextPacketBytes;
  if (options.export_subtitles && !export_subtitles) {
    close_stream();
    result.message =
        "Subtitle export is 625-line only (the cue timing assumes 50 fields "
        "per second)";
    return result;
  }
  if (export_subtitles &&
      !output_decoder.set_subtitle_page(options.subtitle_page)) {
    close_stream();
    result.message = "Invalid subtitle page: " + options.subtitle_page;
    return result;
  }

  // Data levels come from the source inside the frame slicer; the geometry
  // here is only what the loop needs to report progress.
  const auto finish_dataset = [&](TeletextSinkResult& partial) {
    // Before pages(): a page number the recording never once received as
    // transmitted is a mis-corrected copy of one it did, not a page of its own.
    const VbiIdentityReconciliation reconciliation =
        catalogue.reconcile_identities();
    if (reconciliation.acted() || reconciliation.withheld) {
      ORC_LOG_INFO("TeletextSinkDeps: Identity attestation: {}",
                   reconciliation.summary("page"));
    }
    partial.dataset.summary.identity_reconciliation = reconciliation;

    partial.dataset.pages = catalogue.pages();
    // Per-page loss, which only the row copies can give: see
    // teletext_subpage_lost_packets(). Without the squasher there are no
    // repeats to compare, so the question goes unanswered rather than answered
    // wrongly.
    if (squash) {
      for (auto& page : partial.dataset.pages) {
        for (auto& subpage : page.subpages) {
          subpage.lost_packets = teletext_subpage_lost_packets(subpage);
        }
      }
    }
    partial.dataset.summary.pages_truncated = catalogue.truncated();
    // The alphabet the pages above were read in, carried to the viewer so the
    // reader is told rather than left to judge it from the glyphs.
    partial.dataset.summary.character_set = options.character_set;
    partial.dataset.summary.second_character_set = options.second_character_set;
    partial.dataset.summary.packets_recovered = partial.packets_written;
    partial.dataset.summary.fields_with_data = partial.fields_with_data;
    partial.dataset.summary.packets_corrected = partial.packets_corrected;
    partial.dataset.summary.bytes_repaired = partial.bytes_repaired;
    partial.dataset.summary.characters_written = squash_stats.bytes_total();
    partial.dataset.summary.characters_damaged =
        squash_stats.parity_failures_after();
    partial.dataset.summary.lost_packets_estimate =
        estimate_lost_packets(packets_per_field);
    // A cancelled run still recovered whatever it got to, and that is exactly
    // when a reader wants to know how it was going. The report file is not
    // written for one: it describes a run that did not finish, and the option
    // asks for a record of the export beside the export.
    partial.report = build_report(options, partial, total_frames, stats,
                                  squash_stats, scan_state);
  };

  // Slicing is nearly all of what a pass costs and each line is recovered from
  // its own samples alone, so frames are sliced several at a time; emission is
  // strictly ordered and stateful, so it stays on this thread. Frames are
  // taken a block at a time (see kFirstScanBlockFrames): the whole block is
  // sliced against one frozen snapshot of what the pass has learned, then
  // emitted in order, which is what advances the learning for the next block.
  // A worker never reads the live state, so the packets a recording yields do
  // not depend on how the blocks were scheduled — the output is identical at
  // any thread count.
  std::vector<TeletextFieldScan> block(
      static_cast<size_t>(kMaxScanBlockFrames) * kFieldsPerFrame);
  const size_t worker_count = resolve_worker_count(
      total_frames * kFieldsPerFrame, options.decode_threads);
  ORC_LOG_DEBUG("TeletextSinkDeps: Slicing {} frames on {} thread(s)",
                total_frames, worker_count);

  try {
    uint64_t frames_processed = 0;
    uint64_t block_frames = kFirstScanBlockFrames;

    for (FrameID block_first = frame_rng.first;
         block_first <= frame_rng.last;) {
      const uint64_t remaining =
          static_cast<uint64_t>(frame_rng.last - block_first) + 1;
      const uint64_t block_count = std::min(block_frames, remaining);

      const auto cancelled = [&] {
        return cancel_requested_ != nullptr && cancel_requested_->load();
      };
      const auto report_cancelled = [&]() -> TeletextSinkResult& {
        close_stream();
        result.message = "Cancelled after " + std::to_string(frames_processed) +
                         " of " + std::to_string(total_frames) + " frames";
        if (export_stream) {
          result.message += "; partial output left at " + output_path;
        }
        ORC_LOG_WARN("TeletextSinkDeps: {}", result.message);
        finish_dataset(result);
        return result;
      };

      if (cancelled()) {
        return report_cancelled();
      }

      slice_block(*representation, frame_slicer, block_first,
                  static_cast<uint64_t>(block_first - frame_rng.first),
                  block_count, scan_state.snapshot(), worker_count,
                  cancel_requested_, block);

      // A cancelled block was abandoned part way through, so none of it is
      // emitted: the stream stays a prefix of the run rather than gaining a
      // hole where the workers stopped taking jobs.
      if (cancelled()) {
        return report_cancelled();
      }

      for (uint64_t block_offset = 0; block_offset < block_count;
           ++block_offset) {
        const FrameID frame_id =
            block_first + static_cast<FrameID>(block_offset);

        ++frames_processed;
        if (progress_callback_ &&
            (frames_processed % kProgressThrottleFrames == 0 ||
             frames_processed == total_frames)) {
          progress_callback_(frames_processed, total_frames,
                             "Decoding teletext frame " +
                                 std::to_string(frames_processed) + "/" +
                                 std::to_string(total_frames));
        }

        // Packet emission is strictly temporal: frame → field (1 then 2) →
        // ascending line — the order carousel-reassembling consumers expect.
        for (size_t field_idx = 0; field_idx < kFieldsPerFrame; ++field_idx) {
          const TeletextFieldScan& scan =
              block[static_cast<size_t>(block_offset) * kFieldsPerFrame +
                    field_idx];
          const std::vector<TeletextFrameLineResult>& line_results = scan.lines;

          // The pass learns here rather than in the slicer, in its own frame
          // order, so what it knows at any point is a function of the
          // recording and not of the scheduling.
          scan_state.lines().add_skipped(scan.lines_skipped);
          for (const TeletextFrameLineResult& line : line_results) {
            scan_state.observe(field_idx, line.field_line, line.sliced);
          }

          // Cue and squash timing is relative to the start of the export range.
          const int64_t relative_field_index =
              static_cast<int64_t>(frame_id - frame_rng.first) * 2 +
              static_cast<int64_t>(field_idx);

          // Walked over the configured window rather than over the results,
          // because keep_empty_packets promises a packet position per (frame,
          // field, line) and the slicer reports only the lines it could read.
          // Both are ascending, so the results are consumed in step.
          size_t field_packets = 0;
          size_t next_result = 0;
          for (int32_t field_line = options.first_field_line;
               field_line <= options.last_field_line; ++field_line) {
            const TeletextFrameLineResult* line = nullptr;
            if (next_result < line_results.size() &&
                line_results[next_result].field_line == field_line) {
              line = &line_results[next_result++];
              stats.add_line(line->field_line, line->sliced);
            }

            std::optional<std::array<uint8_t, kTeletextPacketBytes>> packet;
            bool has_confidence = false;
            TeletextPacketConfidence confidence{};
            if (line != nullptr && line->sliced.valid) {
              packet = line->sliced.bytes;
              has_confidence = line->sliced.has_byte_confidence;
              confidence = line->sliced.byte_confidence;
              result.bytes_repaired +=
                  static_cast<uint64_t>(line->sliced.repaired_bytes);
            }

            if (packet.has_value()) {
              ++result.packets_written;
              ++field_packets;
              if (squash) {
                // Quantise the confidence for the pool, then vote on the
                // dequantised values in both passes so the rewrite re-feeds
                // exactly the weights this pass used.
                uint32_t confidence_slot = 0;
                if (has_confidence) {
                  QuantizedPacketConfidence quantized;
                  for (size_t i = 0; i < kTeletextPacketBytes; ++i) {
                    quantized[i] = static_cast<uint8_t>(std::lround(
                        std::clamp(confidence[i], 0.0F, 1.0F) * 255.0F));
                    confidence[i] =
                        static_cast<float>(quantized[i]) * (1.0F / 255.0F);
                  }
                  confidence_slot =
                      static_cast<uint32_t>(confidence_pool.size());
                  confidence_pool.push_back(quantized);
                }
                // The stream index is the copy's identity for the squasher, so
                // the rewrite pass can re-feed it without counting it twice.
                squash_pass->process_packet(
                    *packet, relative_field_index,
                    static_cast<int64_t>(stream.size()),
                    has_confidence ? &confidence : nullptr, packet_bytes);
                stream.push_back(StreamEntry{*packet, relative_field_index,
                                             false, has_confidence,
                                             confidence_slot});
              } else {
                const TeletextPacketConfidence* weights =
                    has_confidence ? &confidence : nullptr;
                emit(packet->data(), packet_bytes);
                if (display_row_of(*packet) != 0) {
                  squash_stats.add_written_row(display_bytes_of(*packet),
                                               display_bytes);
                }
                output_decoder.process_packet(*packet, relative_field_index,
                                              TeletextPageDecoder::kAutoSource,
                                              weights, packet_bytes);
              }
            } else if (options.keep_empty_packets) {
              ++result.packets_written;
              if (squash) {
                stream.push_back(StreamEntry{kEmptyPacket, relative_field_index,
                                             true, false, 0});
              } else {
                emit(kEmptyPacket.data(), packet_bytes);
              }
            }
          }

          if (field_packets > 0) {
            ++result.fields_with_data;
          }
          ++packets_per_field[std::min(field_packets,
                                       kMaxPacketsPerFieldTracked)];
        }
      }

      block_first += static_cast<FrameID>(block_count);
      // Blocks grow as the pass settles: see kFirstScanBlockFrames.
      block_frames = std::min(block_frames * 2, kMaxScanBlockFrames);
    }

    // Rewrite pass: now that every copy of every row has been seen, emit the
    // stream with each row replaced by the combination of its copies. Packet
    // order, count and timing are unchanged — only damaged display bytes
    // move. Headers and enhancement packets pass through untouched (their
    // display bytes carry a clock that differs between transmissions).
    if (squash) {
      for (size_t index = 0; index < stream.size(); ++index) {
        if (cancel_requested_ && cancel_requested_->load()) {
          close_stream();
          result.message = "Cancelled while combining repeated rows";
          if (export_stream) {
            result.message += "; partial output left at " + output_path;
          }
          ORC_LOG_WARN("TeletextSinkDeps: {}", result.message);
          squash_stats.set_page_runs(squasher.page_count());
          finish_dataset(result);
          return result;
        }
        if (progress_callback_ &&
            (index % (kProgressThrottleFrames * 100) == 0 ||
             index + 1 == stream.size())) {
          progress_callback_(index + 1, stream.size(),
                             "Combining repeated teletext rows " +
                                 std::to_string(index + 1) + "/" +
                                 std::to_string(stream.size()));
        }

        const StreamEntry& entry = stream[index];
        if (entry.empty) {
          emit(kEmptyPacket.data(), packet_bytes);
          continue;
        }

        auto out = entry.bytes;
        TeletextPacketConfidence entry_confidence{};
        if (entry.has_confidence) {
          const QuantizedPacketConfidence& quantized =
              confidence_pool[entry.confidence_slot];
          for (size_t i = 0; i < kTeletextPacketBytes; ++i) {
            entry_confidence[i] =
                static_cast<float>(quantized[i]) * (1.0F / 255.0F);
          }
        }
        // The decoder re-derives which page each row belongs to. It shares the
        // squasher, and each copy is re-fed under its original stream index,
        // so the table it consults is the one built above, unchanged.
        output_decoder.process_packet(
            entry.bytes, entry.field_index, static_cast<int64_t>(index),
            entry.has_confidence ? &entry_confidence : nullptr, packet_bytes);
        const auto& attribution = output_decoder.last_row_attribution();
        if (attribution.has_value()) {
          const int row = output_decoder.last_row_number();
          const auto squashed = squasher.squashed_row(*attribution, row);
          if (squashed.has_value()) {
            squash_stats.add_row(display_bytes_of(entry.bytes), *squashed,
                                 squasher.copy_count(*attribution, row),
                                 display_bytes);
            // Only the display bytes this packet carries are rewritten: on a
            // 525-line service the remaining columns of the row arrive in a
            // separate extension packet and belong to that packet's bytes.
            if (!std::equal(squashed->begin(),
                            squashed->begin() + display_bytes,
                            out.begin() + 2)) {
              std::copy(squashed->begin(), squashed->begin() + display_bytes,
                        out.begin() + 2);
              ++result.packets_corrected;
            }
          }
        } else if (display_row_of(entry.bytes) != 0) {
          // A display row that belonged to no open page is written as it was
          // recovered. It carries characters the reader will see, so its
          // damage belongs in the loss figure even though no vote reached it.
          squash_stats.add_written_row(display_bytes_of(entry.bytes),
                                       display_bytes);
        }
        emit(out.data(), packet_bytes);
      }
      squash_stats.set_page_runs(squasher.page_count());
    }

    close_stream();

    // Close the pages still open at the end of the range, and any cue still on
    // screen, so the last transmission of the recording is catalogued too.
    output_decoder.finalize(static_cast<int64_t>(total_frames) * 2);

    if (export_subtitles) {
      const auto& cues = output_decoder.subtitle_cues();

      // The stream extension was applied above; the SubRip document sits next
      // to the packet stream.
      std::string subtitle_path =
          output_path.substr(0, output_path.length() - extension.length()) +
          ".srt";
      std::shared_ptr<IFileWriterUint8> subtitle_writer =
          stage_services_->create_buffered_file_writer_uint8(
              kWriterBufferBytes);
      if (!subtitle_writer || !subtitle_writer->open(subtitle_path)) {
        result.message =
            "Exported " + std::to_string(result.packets_written) +
            " teletext packets to " + output_path +
            " but failed to open subtitle output: " + subtitle_path;
        ORC_LOG_ERROR("TeletextSinkDeps: {}", result.message);
        return result;
      }
      const std::string srt = format_srt(cues);
      subtitle_writer->write(reinterpret_cast<const uint8_t*>(srt.data()),
                             srt.size());
      subtitle_writer->close();
      result.subtitle_path = subtitle_path;
      result.subtitle_cues_written = cues.size();
    }

    result.characters_written = squash_stats.bytes_total();
    result.characters_damaged = squash_stats.parity_failures_after();

    result.success = true;
    result.message = "Recovered " + std::to_string(result.packets_written) +
                     " teletext packets (" +
                     std::to_string(result.fields_with_data) +
                     " fields with data)";
    result.message += export_stream
                          ? " to " + output_path
                          : "; no packet stream written (no output file set)";
    if (!result.subtitle_path.empty()) {
      result.message += "; " + std::to_string(result.subtitle_cues_written) +
                        " subtitle cues to " + result.subtitle_path;
    }
    ORC_LOG_INFO("TeletextSinkDeps: {}", result.message);

    // Both consumers of the pass are filled from the same counters, so the
    // viewer and the file exports can never disagree about the run.
    finish_dataset(result);
    write_report(options, result);
    return result;

  } catch (const std::exception& e) {
    close_stream();
    result.success = false;
    result.message = std::string("Error during teletext analysis: ") + e.what();
    ORC_LOG_ERROR("TeletextSinkDeps: {}", result.message);
    return result;
  }
}

}  // namespace orc
