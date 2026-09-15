/*
 * File:        nabts_sink_deps.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NabtsSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_sink_deps.h"

#include <orc/plugin/orc_stage_services.h>
#include <orc/support/logging.h>
#include <orc/support/pipe_io.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nabts_block_scanner.h"
#include "nabts_frame_slicer.h"
#include "nabts_packet.h"
#include "vbi-services/teletext_recovery_stats.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {

namespace {

constexpr size_t kWriterBufferBytes = 1UL * 1024UL * 1024UL;

// A record is at most 1904 bytes (CEA-516 §8.4.2.5), so a record file needs
// nothing like the packet stream's buffer.
constexpr size_t kRecordBufferBytes = 4UL * 1024UL;

// Progress callbacks are throttled to once every N frames (plus the final
// frame) so tight loops do not flood the UI.
constexpr uint64_t kProgressThrottleFrames = 10;

// A whole zero packet, emitted for a candidate line with no data when
// keep_empty_packets is enabled — the vhs-decode convention giving a 1:1
// packet-to-(frame, field, line) mapping.
constexpr std::array<uint8_t, kNabtsPacketBytes> kEmptyPacket{};

// Candidate lines one field can yield, which sizes the packets-per-field
// histogram behind the lost-packet estimate. One more than the widest window
// this stage will probe, so every possible count has a bucket.
constexpr size_t kMaxPacketsPerFieldTracked = 40;

// SMPTE 170M: 525-line NTSC scans 59,94 fields per second, so a frame is
// 1001/30000 of a second. Caption cue extents are frame counts, and this is
// what turns one into a SubRip timestamp.
constexpr double kNtscFramesPerSecond = 30000.0 / 1001.0;

// Format a frame index as an SRT timestamp (HH:MM:SS,mmm).
std::string srt_timestamp(uint64_t frame) {
  const double seconds_total =
      static_cast<double>(frame) / kNtscFramesPerSecond;
  const int64_t millis_total = std::llround(seconds_total * 1000.0);
  return fmt::format("{:02}:{:02}:{:02},{:03}", millis_total / 3'600'000,
                     (millis_total / 60'000) % 60, (millis_total / 1'000) % 60,
                     millis_total % 1'000);
}

// The stream's extension. The flat file is a run of whole packets with no
// header, so a reader can only tell 33-byte NABTS packets from the 34-byte
// 525-line WST ones the teletext sink writes by what the file is called. The
// convention is that sink's: 't' plus the packet length in bytes.
constexpr const char* kStreamExtension = ".t33";

std::string detector_name(TeletextDetector detector) {
  switch (detector) {
    case TeletextDetector::kThreshold:
      return "threshold";
    case TeletextDetector::kMlse:
      return "MLSE";
    case TeletextDetector::kAuto:
      break;
  }
  return "automatic (threshold, MLSE where it could not lock)";
}

/**
 * @brief Packets a recording lost, estimated from the packets-per-field spread
 *
 * A service inserting on a fixed set of VBI lines fills every one of them in
 * every field it uses, so the *usual* packets-per-field count says how many
 * lines the service is using and every field short of it is short by packets
 * the recording lost.
 *
 * The usual count is the mode of the non-zero counts rather than the maximum: a
 * single field where the slicer false-locked an extra line would otherwise
 * accuse every other field of a loss. Fields that yielded nothing at all are
 * left out — a service may insert into one field of each frame only, and
 * calling the other a total loss would put a fault on every packet. That
 * under-reports a field where every line was lost, which is the right way to be
 * wrong: the figure exists to stop claiming faults that are not there.
 */
uint64_t estimate_lost_packets(
    const std::array<uint64_t, kMaxPacketsPerFieldTracked + 1>& histogram) {
  size_t mode = 0;
  uint64_t best = 0;
  for (size_t count = 1; count < histogram.size(); ++count) {
    if (histogram[count] > best) {
      best = histogram[count];
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

std::string nabts_caption_srt(const std::vector<NabtsCaptionCue>& cues) {
  std::string document;
  size_t index = 1;
  for (const NabtsCaptionCue& cue : cues) {
    document += std::to_string(index++);
    document += '\n';
    document += srt_timestamp(cue.start_frame);
    document += " --> ";
    document += srt_timestamp(cue.end_frame);
    document += '\n';
    document += cue.text;
    document += "\n\n";
  }
  return document;
}

void NabtsSinkDeps::init(TriggerProgressCallback progress_callback,
                         std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

std::string NabtsSinkDeps::build_report(
    const NabtsSinkOptions& options, const NabtsSinkResult& result,
    uint64_t total_frames, const TeletextRecoveryStats& stats,
    const NabtsScanState& scan_state, const NabtsGroupStats& group_stats,
    const NabtsRecordStats& record_stats) const {
  std::string report = "NABTS recovery report\n";
  report += fmt::format(
      "  Output:        {}\n"
      "  Frames:        {}\n"
      "  VBI lines:     {}-{} of each field (1-based)\n"
      "  Detector:      {}\n"
      "  Packets:       {} written, {} fields carried data",
      result.output_path.empty() ? std::string("(none; no output file set)")
                                 : result.output_path,
      total_frames, options.first_field_line + 1, options.last_field_line + 1,
      detector_name(options.detector), result.packets_written,
      result.fields_with_data);
  if (options.keep_empty_packets) {
    report += " (empty candidate lines padded)";
  }
  if (result.lost_packets_estimate > 0) {
    report += fmt::format(
        "\n  Lost packets:  {} estimated from the spread of packets per field",
        result.lost_packets_estimate);
  }

  // What the pass learned about the recording, and what that saved. Reported
  // whether or not it was enabled, because the interesting case is the one
  // where the reader is wondering why a run took as long as it did.
  const TeletextPhaseHint hint = scan_state.phase().hint();
  report += "\n  Data phase:    ";
  if (!options.pin_data_phase) {
    report += "not pinned (searched in full on every line)";
  } else if (hint.valid) {
    report +=
        fmt::format("pinned to sample {:.1f} ±{:.1f} from {} locks",
                    hint.centre, hint.radius, scan_state.phase().locks_seen());
  } else {
    report += fmt::format("not pinned ({} locks seen, too few to agree)",
                          scan_state.phase().locks_seen());
  }
  report += "\n  Active lines:  ";
  if (!options.learn_active_lines) {
    report += "not learned (every candidate line read on every frame)";
  } else {
    report += fmt::format("{} candidate line reads skipped",
                          scan_state.lines().lines_skipped());
  }

  // What the vote could not settle on the evidence alone. Silent for a
  // recording whose copies never disagreed by a hair, which is a recording
  // whose records mostly arrived whole.
  const NabtsRecoverySummary& recovery = result.dataset.summary;
  if (recovery.vote_positions_contested > 0) {
    report += fmt::format(
        "\n  Record vote:   {} byte position(s) the copies could not separate",
        recovery.vote_positions_contested);
    report += options.grammar_assisted_vote
                  ? fmt::format(", {} settled by the NAPLPS grammar",
                                recovery.vote_positions_adjudicated)
                  : std::string(", the grammar not asked");
  }

  // The identities the recording named but the service never transmitted.
  // Reported whenever the pass had anything to say, including when it stood
  // aside: a reader comparing this run's record count with an earlier one's
  // needs to know which of the two is looking at a pruned catalogue.
  const std::string reconciliation =
      recovery.identity_reconciliation.summary("record");
  if (!reconciliation.empty()) {
    report += "\n  Identities:    " + reconciliation;
  }

  if (result.records_exported > 0) {
    report += fmt::format("\n  Records:       {} exported beside the stream",
                          result.records_exported);
  }
  if (options.export_captions) {
    report +=
        result.caption_path.empty()
            ? std::string(
                  "\n  Captions:      none (no record carried the "
                  "caption flag)")
            : fmt::format("\n  Captions:      {} cues to {}",
                          result.caption_cues_written, result.caption_path);
  }

  report += "\n\n";
  report += stats.summary();
  report += "\n";
  report += group_stats.summary();
  report += "\n";
  report += record_stats.summary();

  // How much of what a reader will see was recovered rather than received.
  // Quiet for a recording whose pages all arrived clean, which is the ordinary
  // case for a disc and never the case for a tape.
  const std::string lint = result.lint_totals.summary();
  if (!lint.empty()) {
    report += "\n";
    report += lint;
  }
  return report;
}

void NabtsSinkDeps::write_report(const NabtsSinkOptions& options,
                                 NabtsSinkResult& result) const {
  if (!options.write_report || result.output_path.empty() ||
      result.report.empty() || stage_services_ == nullptr) {
    return;
  }

  // Sits beside the packet stream under its full name — mydata.t33.txt — so a
  // directory of exports pairs its reports with them by sorting.
  const std::string path = result.output_path + ".txt";
  auto writer =
      stage_services_->create_buffered_file_writer_uint8(kWriterBufferBytes);
  if (!writer || !writer->open(path)) {
    ORC_LOG_WARN("NabtsSinkDeps: Could not write report to {}", path);
    return;
  }
  writer->write(reinterpret_cast<const uint8_t*>(result.report.data()),
                result.report.size());
  writer->close();
  result.report_path = path;
}

void NabtsSinkDeps::write_records(const NabtsSinkOptions& options,
                                  NabtsSinkResult& result) const {
  if (!options.export_records || result.output_path.empty() ||
      stage_services_ == nullptr) {
    return;
  }

  // Beside the packet stream, one file per record, named for the identity
  // CEA-516 §5.2.1 gives it: channel, record address and version. That is the
  // key the catalogue is ordered on, so a directory of these sorts into the
  // order the records dialog lists them in.
  const std::string base = result.output_path + ".";
  for (const NabtsCataloguedRecord& record : result.dataset.records) {
    if (record.data.empty()) {
      continue;  // A record whose every copy was header-only has nothing to
                 // write.
    }
    const std::string path =
        base + fmt::format("{:03X}-{}-v{:X}.rec", record.channel,
                           record.address_text, record.version);
    auto writer =
        stage_services_->create_buffered_file_writer_uint8(kRecordBufferBytes);
    if (!writer || !writer->open(path)) {
      ORC_LOG_WARN("NabtsSinkDeps: Could not write record to {}", path);
      continue;  // One unwritable record never fails the export.
    }
    writer->write(record.data.data(), record.data.size());
    writer->close();
    ++result.records_exported;
  }
  ORC_LOG_DEBUG("NabtsSinkDeps: Exported {} record(s)",
                result.records_exported);
}

void NabtsSinkDeps::write_captions(const NabtsSinkOptions& options,
                                   NabtsSinkResult& result) const {
  if (!options.export_captions || result.output_path.empty() ||
      stage_services_ == nullptr) {
    return;
  }

  // The cues are read off the presented pages, which recovery does not build:
  // what a page looks like depends on the receiver it is drawn for, so the
  // catalogue interprets it when it is browsed. Caption text is the one part
  // that cannot depend on the receiver — a character's code is its code at any
  // resolution — so the export interprets at the reference receiver and gets
  // the same cues the viewer will show at whatever resolution it is set to.
  //
  // Always repaired, unlike the viewer, which offers the reader the choice: a
  // subtitle file is itself a reading aid rather than a record of what was
  // transmitted, and the recording's own bytes are in the packet stream and the
  // record files either way. The report says how much repair the records took.
  std::vector<NabtsCataloguedRecord> records = result.dataset.records;
  nabts_interpret_records(records, kNaplpsGridReference, /*repair=*/true);

  // Read the same way the catalogue's caption track reads it — one shared
  // implementation, so the file and the screen cannot disagree.
  const std::vector<NabtsCaptionCue> cues = nabts_caption_cues(records);
  if (cues.empty()) {
    ORC_LOG_DEBUG(
        "NabtsSinkDeps: No caption records in the range; no cues written");
    return;
  }

  const std::string document = nabts_caption_srt(cues);

  // Beside the packet stream, named after it: mydata.t33 gives mydata.t33.srt,
  // which is the rule the report and the record files follow.
  const std::string path = result.output_path + ".srt";
  auto writer =
      stage_services_->create_buffered_file_writer_uint8(kWriterBufferBytes);
  if (!writer || !writer->open(path)) {
    ORC_LOG_WARN("NabtsSinkDeps: Could not write captions to {}", path);
    return;
  }
  writer->write(reinterpret_cast<const uint8_t*>(document.data()),
                document.size());
  writer->close();
  result.caption_path = path;
  result.caption_cues_written = cues.size();
  ORC_LOG_DEBUG("NabtsSinkDeps: Exported {} caption cue(s) to {}", cues.size(),
                path);
}

NabtsSinkResult NabtsSinkDeps::analyse(
    const VideoFrameRepresentation* representation,
    const NabtsSinkOptions& options) {
  NabtsSinkResult result;

  if (!representation) {
    result.message = "Input representation is null";
    return result;
  }

  const auto vp_opt = representation->get_video_parameters();
  if (!vp_opt.has_value() || !NabtsFrameSlicer::applies_to(vp_opt->system)) {
    result.message =
        "Input carries no NABTS service (ITU-R BT.653 System C is defined on "
        "525-line systems only; use the Teletext Sink for a 625-line source)";
    return result;
  }

  const bool export_stream = !options.output_path.empty();
  const bool piping =
      export_stream && orc::pipe_io::is_pipe_path(options.output_path);

  // A pipe carries the primary packet stream alone: the report, per-record,
  // and caption files are separate outputs derived from output_path, which
  // "-" does not name a directory to place them beside. Refused up front
  // rather than silently dropped, since dropping several files a user
  // explicitly asked for is a surprise CVBS Sink/TBC Sink's own sidecars
  // don't have to make (those are optional pipeline data, not options the
  // user turned on by name).
  if (piping && (options.export_records || options.export_captions ||
                 options.write_report)) {
    result.message =
        "Cannot pipe the NABTS stream to stdout ('-') together with "
        "export_records, export_captions, or write_report: those write "
        "separate files named after output_path, which \"-\" does not "
        "identify. Disable them, or write to a real file instead.";
    return result;
  }

  std::string output_path;
  if (export_stream) {
    output_path = options.output_path;
    if (!piping) {
      const std::string extension(kStreamExtension);
      if (output_path.length() < extension.length() ||
          output_path.compare(output_path.length() - extension.length(),
                              extension.length(), extension) != 0) {
        output_path += extension;
        ORC_LOG_DEBUG("NabtsSinkDeps: Added {} extension: {}", extension,
                      output_path);
      }
    }
    result.output_path = output_path;
  }

  const auto frame_rng = representation->frame_range();
  const uint64_t total_frames = frame_rng.count();
  if (total_frames == 0) {
    result.message = "Input has no frames";
    return result;
  }
  result.frames_analysed = total_frames;

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

  NabtsFrameSlicerOptions slicer_options;
  slicer_options.detector = options.detector;
  slicer_options.tolerant_framing = options.tolerant_framing;
  slicer_options.require_valid_prefix = options.require_valid_prefix;
  slicer_options.first_field_line = options.first_field_line;
  slicer_options.last_field_line = options.last_field_line;
  const NabtsFrameSlicer frame_slicer(slicer_options);

  // What this pass learns about the recording as it goes: where in the line its
  // data bursts start, and which candidate lines carry them at all.
  NabtsScanState scan_state(options.pin_data_phase, options.learn_active_lines);
  TeletextRecoveryStats stats;
  std::array<uint64_t, kMaxPacketsPerFieldTracked + 1> packets_per_field{};

  // Packet → data group → message → catalogue. Each stage of the chain sees the
  // stream in transmission order, which is what the continuity index and the
  // link order are read against, so the whole chain is driven from the emission
  // loop below rather than from the slicing threads.
  //
  // |catalogue_frame| is the frame the loop has reached, which a callback fired
  // from deep in the chain has no other way of knowing. Groups complete on the
  // packet that finishes them, so it is the right frame to file a record under.
  NabtsGroupAssembler groups;
  NabtsRecordAssembler records;
  NabtsRecordCatalogue catalogue;
  uint64_t catalogue_frame = 0;
  // Per-block suffix figures are a property of the group that carried them, so
  // they are totalled as the groups go past rather than kept in either
  // assembler's statistics.
  uint64_t blocks_corrected = 0;
  uint64_t blocks_damaged = 0;
  groups.set_group_callback([&](const NabtsDataGroup& group) {
    blocks_corrected += group.blocks_corrected;
    blocks_damaged += group.blocks_damaged;
    records.add_group(group);
  });
  records.set_message_callback(
      [&catalogue, &catalogue_frame](const NabtsMessage& message) {
        catalogue.merge(message, catalogue_frame);
      });

  std::vector<NabtsFieldScan> block(
      static_cast<size_t>(kNabtsMaxScanBlockFrames) * kNabtsFieldsPerFrame);
  const size_t worker_count = nabts_resolve_worker_count(
      total_frames * kNabtsFieldsPerFrame, options.decode_threads);
  ORC_LOG_DEBUG("NabtsSinkDeps: Slicing {} frames on {} thread(s)",
                total_frames, worker_count);

  const auto finish = [&](NabtsSinkResult& partial) {
    partial.lost_packets_estimate = estimate_lost_packets(packets_per_field);

    // Whatever is still in flight is reported as it stands: a recording that
    // ends part way through a group or a linked series is the normal case, and
    // the bytes that did arrive still identify their record.
    groups.flush();
    records.flush();

    // Before the vote, and so before records(): the fold moves a misread
    // identity's copies into the record it was a misreading of, and the vote
    // across those copies is held inside records().
    const VbiIdentityReconciliation reconciliation =
        catalogue.reconcile_identities();
    if (reconciliation.acted() || reconciliation.withheld) {
      ORC_LOG_INFO("NabtsSinkDeps: Identity attestation: {}",
                   reconciliation.summary("record"));
    }

    partial.dataset.records = catalogue.records(options.grammar_assisted_vote);
    NabtsRecoverySummary& summary = partial.dataset.summary;
    summary.identity_reconciliation = reconciliation;
    uint32_t records_voted = 0;
    for (const NabtsCataloguedRecord& record : partial.dataset.records) {
      summary.vote_positions_contested += record.vote_positions_contested;
      summary.vote_positions_adjudicated += record.vote_positions_adjudicated;
      records_voted += record.copies_voted > 0 ? 1 : 0;
    }
    if (records_voted > 0) {
      ORC_LOG_INFO(
          "NabtsSinkDeps: {} of {} record(s) recovered by combining copies; {} "
          "byte position(s) left level by the weights, {} settled by the "
          "NAPLPS grammar{}",
          records_voted, partial.dataset.records.size(),
          summary.vote_positions_contested, summary.vote_positions_adjudicated,
          options.grammar_assisted_vote ? "" : " (the grammar was not asked)");
    }
    summary.frames_analysed = partial.frames_analysed;
    summary.fields_with_data = partial.fields_with_data;
    summary.packets_recovered = partial.packets_written;
    summary.packets_prefix_rejected = groups.stats().prefix_failures;
    summary.lost_packets_estimate = partial.lost_packets_estimate;
    summary.groups_completed = groups.stats().groups_completed;
    summary.groups_incomplete =
        groups.stats().groups_superseded + groups.stats().groups_unfinished;
    summary.messages_complete = records.stats().messages_complete;
    summary.messages_partial = records.stats().messages_partial;
    summary.records_truncated = catalogue.truncated();
    summary.blocks_corrected = blocks_corrected;
    summary.blocks_damaged = blocks_damaged;

    // Exported before the report is built, because the report says how many
    // were written. A cancelled run exports what it catalogued, for the same
    // reason its packet stream is left as a prefix rather than deleted.
    write_records(options, partial);
    write_captions(options, partial);

    // Computed before the report and after the catalogue, because it is a
    // property of the records the run catalogued rather than of the recovery.
    partial.lint_totals = nabts_lint_records(partial.dataset.records);

    // Said out loud rather than left in the report, which is written only when
    // asked for and logged only at debug level: how much of what a reader is
    // about to browse was recovered by the grammar is worth a line of every
    // run's log.
    if (partial.lint_totals.records_linted > 0) {
      const NabtsLintTotals& lint = partial.lint_totals;
      ORC_LOG_INFO(
          "NabtsSinkDeps: NAPLPS lint over {} presentation record(s) — {} "
          "faulted before repair, {} after; {} record(s) altered and {} of "
          "them "
          "now drawing differently; {} byte(s) corrected, {} run(s) "
          "resynchronised, {} coordinate word(s) dropped, {} change(s) refused "
          "for redrawing the page; {} of {} doubtful byte(s) were known wrong, "
          "{} of those the grammar could not decide",
          lint.records_linted, lint.records_faulted, lint.records_faulted_after,
          lint.records_altered, lint.records_drawing_changed,
          lint.bytes_repaired, lint.pdis_resynchronised,
          lint.coordinate_words_dropped, lint.changes_declined_by_reach,
          lint.bytes_offered, lint.suspect_bytes, lint.bytes_ambiguous);
    }

    partial.report = build_report(options, partial, total_frames, stats,
                                  scan_state, groups.stats(), records.stats());
  };

  try {
    uint64_t frames_processed = 0;
    uint64_t block_frames = kNabtsFirstScanBlockFrames;

    for (FrameID block_first = frame_rng.first;
         block_first <= frame_rng.last;) {
      const uint64_t remaining =
          static_cast<uint64_t>(frame_rng.last - block_first) + 1;
      const uint64_t block_count = std::min(block_frames, remaining);

      const auto cancelled = [&] {
        return cancel_requested_ != nullptr && cancel_requested_->load();
      };
      const auto report_cancelled = [&]() -> NabtsSinkResult& {
        close_stream();
        result.message = "Cancelled after " + std::to_string(frames_processed) +
                         " of " + std::to_string(total_frames) + " frames";
        if (export_stream) {
          result.message += "; partial output left at " + output_path;
        }
        ORC_LOG_WARN("NabtsSinkDeps: {}", result.message);
        finish(result);
        return result;
      };

      if (cancelled()) {
        return report_cancelled();
      }

      nabts_slice_block(*representation, frame_slicer, block_first,
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
        ++frames_processed;
        if (progress_callback_ &&
            (frames_processed % kProgressThrottleFrames == 0 ||
             frames_processed == total_frames)) {
          progress_callback_(frames_processed, total_frames,
                             "Decoding NABTS frame " +
                                 std::to_string(frames_processed) + "/" +
                                 std::to_string(total_frames));
        }

        // Packet emission is strictly temporal: frame → field (1 then 2) →
        // ascending line, which is the order a receiver saw them broadcast and
        // therefore the order a data-group reassembler needs.
        catalogue_frame = static_cast<uint64_t>(block_first) + block_offset;
        for (size_t field_idx = 0; field_idx < kNabtsFieldsPerFrame;
             ++field_idx) {
          const NabtsFieldScan& scan =
              block[static_cast<size_t>(block_offset) * kNabtsFieldsPerFrame +
                    field_idx];
          const std::vector<NabtsFrameLineResult>& line_results = scan.lines;

          // The pass learns here rather than in the slicer, in its own frame
          // order, so what it knows at any point is a function of the recording
          // and not of the scheduling.
          scan_state.lines().add_skipped(scan.lines_skipped);
          for (const NabtsFrameLineResult& line : line_results) {
            scan_state.observe(field_idx, line.field_line, line.sliced);
          }

          // Walked over the configured window rather than over the results,
          // because keep_empty_packets promises a packet position per (frame,
          // field, line) and the slicer reports only the lines it could read.
          // Both are ascending, so the results are consumed in step.
          size_t field_packets = 0;
          size_t next_result = 0;
          for (int32_t field_line = options.first_field_line;
               field_line <= options.last_field_line; ++field_line) {
            const NabtsFrameLineResult* line = nullptr;
            if (next_result < line_results.size() &&
                line_results[next_result].field_line == field_line) {
              line = &line_results[next_result++];
              stats.add_line(line->field_line, line->sliced);
            }

            if (line != nullptr && line->sliced.valid) {
              ++result.packets_written;
              ++field_packets;
              emit(line->sliced.bytes.data(), kNabtsPacketBytes);
              // Same bytes, same order, into the reassembler. A padding packet
              // is deliberately not fed: it stands for a line that carried
              // nothing, and offering it as a packet would break the continuity
              // chain rather than record a gap in it.
              // The detector's own reading of each byte goes with it, to weight
              // this copy's say where the record catalogue combines repeated
              // copies. The threshold detector measures none, and says so.
              groups.add_packet(nabts_decode_packet(
                  line->sliced.bytes.data(), kNabtsPacketBytes,
                  line->sliced.has_byte_confidence
                      ? &line->sliced.byte_confidence
                      : nullptr));
            } else if (options.keep_empty_packets) {
              ++result.packets_written;
              emit(kEmptyPacket.data(), kNabtsPacketBytes);
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
      block_frames = std::min(block_frames * 2, kNabtsMaxScanBlockFrames);
    }

    close_stream();

    result.success = true;
    result.message = "Recovered " + std::to_string(result.packets_written) +
                     " NABTS packets (" +
                     std::to_string(result.fields_with_data) +
                     " fields with data)";
    result.message += export_stream
                          ? " to " + output_path
                          : "; no packet stream written (no output file set)";
    ORC_LOG_INFO("NabtsSinkDeps: {}", result.message);

    finish(result);
    write_report(options, result);
    return result;

  } catch (const std::exception& e) {
    close_stream();
    result.success = false;
    result.message = std::string("Error during NABTS recovery: ") + e.what();
    ORC_LOG_ERROR("NabtsSinkDeps: {}", result.message);
    return result;
  }
}

}  // namespace orc
