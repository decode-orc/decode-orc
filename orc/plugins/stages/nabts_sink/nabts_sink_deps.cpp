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
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "nabts_block_scanner.h"
#include "nabts_datacast.h"
#include "nabts_datacast_files.h"
#include "nabts_frame_slicer.h"
#include "nabts_framed_service.h"
#include "nabts_packet.h"
#include "nabts_salvage.h"
#include "nabts_scan_state.h"
#include "nabts_service_census.h"
#include "nabts_web_mirror.h"
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

// The stream's extension. The flat file is a run of whole packets with no
// header, so a reader can only tell 33-byte NABTS packets from the 34-byte
// 525-line WST ones the teletext sink writes by what the file is called. The
// convention is that sink's: 't' plus the packet length in bytes.
constexpr const char* kStreamExtension = ".t33";

// What export_records names each record's file with, after the stream's own
// name and the record's key: mydata.t33.000-1A4-v2.rec.
constexpr const char* kRecordExtension = ".rec";

std::string detector_name(TeletextDetector detector) {
  switch (detector) {
    case TeletextDetector::kThreshold:
      return "threshold";
    case TeletextDetector::kMlse:
      return "MLSE";
    case TeletextDetector::kAuto:
      break;
  }
  return "automatic (threshold, MLSE where it could not lock or read a clear "
         "eye)";
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

/// A record file's name as export_records builds it: the packet stream's file
/// name, the record's key (nabts_record_key()) and the extension.
struct RecordFileName {
  std::string stream;
  std::string key;
};

/// |file_name| read as a record file's name, or empty when it is not one: the
/// key holds no dot, and the stream is a packet stream's file name, which
/// export_records always gives the .t33 extension.
std::optional<RecordFileName> record_file_name(const std::string& file_name) {
  const std::filesystem::path name(file_name);
  if (name.extension() != kRecordExtension) {
    return std::nullopt;
  }
  const std::string base = name.stem().string();
  const size_t dot = base.rfind('.');
  if (dot == std::string::npos || dot + 1 == base.size() ||
      std::filesystem::path(base.substr(0, dot)).extension() !=
          kStreamExtension) {
    return std::nullopt;
  }
  return RecordFileName{base.substr(0, dot), base.substr(dot + 1)};
}

/// The packet a line read as. The detector's own reading of each byte goes
/// with it, to weight this copy's say where the record catalogue combines
/// repeated copies.
NabtsPacket packet_from_line(const TeletextLineResult& sliced) {
  NabtsPacket packet = nabts_decode_packet(
      sliced.bytes.data(), kNabtsPacketBytes,
      sliced.has_byte_confidence ? &sliced.byte_confidence : nullptr);
  packet.read_under_check = sliced.checked_bits > 0;
  // The bytes the detector's wide reading changed pass their parity because
  // they were read to (NabtsPacket::reread), and a block it changed is one read
  // under its check. An address it changed was read to as well, not received
  // as sent.
  if (sliced.wide_bytes != 0) {
    packet.reread =
        static_cast<uint32_t>((sliced.wide_bytes >> kNabtsPrefixBytes) &
                              ((uint64_t{1} << kNabtsMaxDataBlockBytes) - 1));
    packet.read_under_check = true;
    if ((sliced.wide_bytes & 0x7U) != 0) {
      packet.address_attested = false;
    }
  }
  return packet;
}

/**
 * @brief Packet → data group → message → catalogue, in transmission order
 *
 * Each stage of the chain sees the stream in transmission order, which is what
 * the continuity index and the link order are read against, so the whole chain
 * is driven from the pass's emission loop rather than from the slicing
 * threads. Every packet the reassembly is fed also goes to the census of what
 * the addresses carry, which names the services no record will come from, and
 * every group to the log of text sent in groups of the provider's own type.
 *
 * The chain keeps the group assembler's line clock as it will read it — every
 * packet fed and every empty line reported — and where each channel's last
 * packet was, so that a loss the recording routinely suffers on lines the pass
 * does not read can be told to the group before the packet that shows it (see
 * NabtsRoutineLosses).
 *
 * Not copyable: the assemblers call back into it.
 */
class NabtsPacketChain {
 public:
  explicit NabtsPacketChain(const NabtsSinkOptions& options)
      : first_field_line_(options.first_field_line),
        last_field_line_(options.last_field_line) {
    groups_.set_group_callback([this](const NabtsDataGroup& group) {
      blocks_corrected_ += group.blocks_corrected;
      blocks_damaged_ += group.blocks_damaged;
      // A group the record layer did not read is another application's.
      if (!records_.add_group(group)) {
        private_text_.add_group(group, frame_);
      }
    });
    records_.set_message_callback([this](const NabtsMessage& message) {
      catalogue_.merge(message, frame_);
    });
    groups_.set_loose_run_callback(
        [this](const NabtsLooseRun& run) { salvage_.add(run, frame_); });
  }
  NabtsPacketChain(const NabtsPacketChain&) = delete;
  NabtsPacketChain& operator=(const NabtsPacketChain&) = delete;

  /// The frame the emission has reached, which a callback fired from deep in
  /// the chain has no other way of knowing. Groups complete on the packet that
  /// finishes them, so it is the right frame to file a record under.
  void set_frame(uint64_t frame) { frame_ = frame; }

  /// |packet|, read at |here| of the configured window, |raw| its bytes as
  /// received; |routine| is what the recording routinely loses where.
  void add_packet(const NabtsPacket& packet, const uint8_t* raw,
                  NabtsRoutineLosses::Position here,
                  NabtsRoutineLosses& routine) {
    if (packet.valid) {
      const auto last = last_packet_of_.find(packet.channel);
      if (last != last_packet_of_.end()) {
        // Lines the group has been told of since the channel's last packet,
        // and the loss this recording routinely shows between the two
        // positions: where that loss is the larger, it fell on lines the pass
        // does not read, and the group is told of as many before the packet
        // arrives.
        const uint64_t counted = line_clock_ - last->second.clock;
        const uint64_t routine_loss = std::min<uint64_t>(
            routine.routine_loss(last->second.position, here),
            window_lines_between(last->second.position, here));
        if (counted < routine_loss) {
          routine.note_told(last->second.position, here);
        }
        for (uint64_t told = counted; told < routine_loss; ++told) {
          groups_.add_empty_line();
          ++line_clock_;
        }
        routine.observe(
            last->second.position, here,
            static_cast<uint8_t>(
                (packet.continuity + 16U - last->second.index) % 16U));
      }
    }
    groups_.add_packet(packet);
    census_.add_packet(packet, raw);
    datacast_.add_packet(packet, raw, static_cast<uint32_t>(frame_));
    ++line_clock_;
    if (packet.valid) {
      last_packet_of_[packet.channel] =
          LastPacket{here, packet.continuity, line_clock_};
    }
  }

  /// A line the slicer read and got nothing from, which |carries_data| where
  /// a packet may have been lost on it (NabtsGroupAssembler::add_empty_line()).
  void add_empty_line(bool carries_data) {
    groups_.add_empty_line(carries_data);
    ++line_clock_;
  }

  /// What the chain recovered, into |dataset|'s records, captions and summary.
  /// Whatever is still in flight is reported as it stands: a recording that
  /// ends part way through a group or a linked series is the normal case, and
  /// the bytes that did arrive still identify their record.
  void finish(NabtsAnalysisDataset& dataset) {
    groups_.flush();
    records_.flush();

    // Before the vote, and so before records(): the fold moves a misread
    // identity's copies into the record it was a misreading of, and the vote
    // across those copies is held inside records().
    const VbiIdentityReconciliation reconciliation =
        catalogue_.reconcile_identities();
    if (reconciliation.acted() || reconciliation.withheld) {
      ORC_LOG_INFO("NabtsSinkDeps: Identity attestation: {}",
                   reconciliation.summary("record"));
    }

    dataset.records = catalogue_.records();
    dataset.captions = catalogue_.captions();
    NabtsRecoverySummary& summary = dataset.summary;
    // The groups whose synchronizing packet was lost, after the vote: a run of
    // their packets fills only what no copy of its record delivered, and the
    // pages none of the records holds are listed after them (NabtsSalvage).
    const NabtsSalvageStats salvaged = salvage_.apply(dataset.records);
    if (salvaged.runs_held > 0 || salvaged.runs_refused > 0) {
      summary.salvage = salvaged.summary();
      ORC_LOG_INFO("NabtsSinkDeps: Salvage: {}", summary.salvage);
    }
    summary.captions_dropped = catalogue_.captions_dropped();
    summary.identity_reconciliation = reconciliation;
    uint32_t records_voted = 0;
    for (const NabtsCataloguedRecord& record : dataset.records) {
      records_voted += record.copies_voted > 0 ? 1 : 0;
    }
    if (records_voted > 0) {
      ORC_LOG_INFO(
          "NabtsSinkDeps: {} of {} record(s) recovered by combining copies",
          records_voted, dataset.records.size());
    }
    summary.groups_completed = groups_.stats().groups_completed;
    summary.groups_incomplete =
        groups_.stats().groups_superseded + groups_.stats().groups_unfinished;
    summary.messages_complete = records_.stats().messages_complete;
    summary.messages_partial = records_.stats().messages_partial;
    for (const auto& [key, count] : records_.stats().attested_foreign_groups) {
      summary.foreign_groups.push_back({key.first, key.second, count});
    }
    summary.records_truncated = catalogue_.truncated();
    summary.other_services = census_.services();
    datacasts_ = datacast_.finish();
    size_t carried_bytes = 0;
    for (const NabtsDatacastChannel& datacast : datacasts_) {
      summary.datacasts.push_back(datacast.summary());
      ORC_LOG_INFO("NabtsSinkDeps: Datacast {}", summary.datacasts.back());
      // What the payload holds that can be read without knowing its
      // service, and the files it carried in a framing that is known
      // (nabts_datacast_files()), for the viewer and the exports.
      NabtsDatacastListing listing;
      listing.channel = datacast.channel;
      listing.raw = datacast.raw;
      listing.summary = summary.datacasts.back();
      listing.payload_bytes = datacast.payload.size();
      listing.text = nabts_datacast_text(datacast.payload);
      dataset.datacasts.push_back(std::move(listing));
      for (NabtsCarriedFile& file : nabts_datacast_files(datacast)) {
        if (dataset.carried_files.size() >= kNabtsCarriedFilesMax ||
            carried_bytes + file.data.size() > kNabtsCarriedBytesMax) {
          break;
        }
        carried_bytes += file.data.size();
        dataset.carried_files.push_back(std::move(file));
      }
    }
    if (!dataset.carried_files.empty()) {
      ORC_LOG_INFO("NabtsSinkDeps: {} file(s) taken out of the datacasts",
                   dataset.carried_files.size());
    }
    // The services that send counted frames across a family of addresses,
    // read as one each, and the files their frames held; the listing of
    // each address a service used says so.
    NabtsFramedDatacasts framed = nabts_framed_services(datacasts_);
    dataset.framed_services = std::move(framed.services);
    for (NabtsCarriedFile& file : framed.files) {
      if (dataset.carried_files.size() >= kNabtsCarriedFilesMax ||
          carried_bytes + file.data.size() > kNabtsCarriedBytesMax) {
        break;
      }
      carried_bytes += file.data.size();
      dataset.carried_files.push_back(std::move(file));
    }
    for (const NabtsFramedService& service : dataset.framed_services) {
      ORC_LOG_INFO("NabtsSinkDeps: Framed service {:08x}: {}",
                   service.identifier, service.summary);
      for (NabtsDatacastListing& listing : dataset.datacasts) {
        if (std::find(service.channels.begin(), service.channels.end(),
                      listing.channel) != service.channels.end()) {
          listing.summary += fmt::format(
              "; frames of service {:08x}, read with the other addresses that "
              "carry it",
              service.identifier);
        }
      }
    }
    for (NabtsOtherService& service : private_text_.services()) {
      summary.other_services.push_back(std::move(service));
    }
    dataset.private_data = private_text_.channels();
    for (const NabtsOtherService& service : summary.other_services) {
      ORC_LOG_INFO("NabtsSinkDeps: Address {} is not teletext: {} packets, {}",
                   service.channels, service.packets, service.description);
    }
    // Per-block suffix figures are a property of the group that carried them,
    // so they are totalled as the groups go past rather than kept in either
    // assembler's statistics.
    summary.blocks_corrected = blocks_corrected_;
    summary.blocks_damaged = blocks_damaged_;
  }

  const NabtsGroupStats& group_stats() const { return groups_.stats(); }
  const NabtsRecordStats& record_stats() const { return records_.stats(); }
  /// The text of the provider's own groups (NabtsPrivateTextLog::document()).
  std::string private_text() const { return private_text_.document(); }
  /// The FEC datacasts decoded at finish(), payloads and all.
  const std::vector<NabtsDatacastChannel>& datacasts() const {
    return datacasts_;
  }
  /// The runs salvage placed nowhere at finish() (NabtsSalvage::unplaced()).
  const std::vector<NabtsHeldRun>& unplaced_runs() const {
    return salvage_.unplaced();
  }

 private:
  struct LastPacket {
    NabtsRoutineLosses::Position position;
    uint8_t index = 0;
    uint64_t clock = 0;
  };

  /// The lines of the configured window strictly between two packet
  /// positions, the second no more than a frame after the first: no more
  /// packets than that can have been lost between them.
  uint64_t window_lines_between(NabtsRoutineLosses::Position from,
                                NabtsRoutineLosses::Position to) const {
    const int64_t per_field = last_field_line_ - first_field_line_ + 1;
    const int64_t a = static_cast<int64_t>(from.field) * per_field +
                      (from.line - first_field_line_);
    int64_t b = static_cast<int64_t>(to.field) * per_field +
                (to.line - first_field_line_);
    if (b <= a) {
      b += static_cast<int64_t>(kNabtsFieldsPerFrame) * per_field;
    }
    return static_cast<uint64_t>(b - a - 1);
  }

  const int32_t first_field_line_;
  const int32_t last_field_line_;
  NabtsGroupAssembler groups_;
  NabtsRecordAssembler records_;
  NabtsRecordCatalogue catalogue_;
  NabtsServiceCensus census_;
  NabtsPrivateTextLog private_text_;
  NabtsSalvage salvage_;
  NabtsDatacastDecoder datacast_;
  std::vector<NabtsDatacastChannel> datacasts_;
  uint64_t frame_ = 0;
  uint64_t line_clock_ = 0;
  std::map<uint16_t, LastPacket> last_packet_of_;
  uint64_t blocks_corrected_ = 0;
  uint64_t blocks_damaged_ = 0;
};

/// The pass learns from |block| — its first |frame_count| frames — in its own
/// frame order, so what it knows at any point is a function of the recording
/// and not of the scheduling.
void learn_from_block(NabtsScanState& scan_state,
                      const std::vector<NabtsFieldScan>& block,
                      uint64_t frame_count) {
  for (uint64_t block_offset = 0; block_offset < frame_count; ++block_offset) {
    for (size_t field_idx = 0; field_idx < kNabtsFieldsPerFrame; ++field_idx) {
      const NabtsFieldScan& scan =
          block[static_cast<size_t>(block_offset) * kNabtsFieldsPerFrame +
                field_idx];
      scan_state.lines().add_skipped(scan.lines_skipped);
      for (const NabtsFrameLineResult& line : scan.lines) {
        scan_state.observe(field_idx, line.field_line, line.sliced,
                           line.clock_scores ? &*line.clock_scores : nullptr);
      }
    }
  }
}

}  // namespace

std::string nabts_caption_srt(const std::vector<NabtsCaptionCue>& cues) {
  std::string document;
  size_t index = 1;
  for (const NabtsCaptionCue& cue : cues) {
    document += std::to_string(index++);
    document += '\n';
    document += nabts_caption_time(cue.start_frame, ',');
    document += " --> ";
    document += nabts_caption_time(cue.end_frame, ',');
    document += '\n';
    document += cue.text;
    document += "\n\n";
  }
  return document;
}

std::string nabts_reference_source(const std::string& folder_name,
                                   const std::string& stream_file_name) {
  // A run's stream is out.t33, in a folder named for the recording.
  return std::filesystem::path(stream_file_name).stem() == "out" &&
                 !folder_name.empty()
             ? folder_name
             : stream_file_name;
}

std::optional<NabtsReferenceRecord> nabts_reference_record_file(
    const std::string& folder_name, const std::string& file_name,
    std::vector<uint8_t> data) {
  const std::optional<RecordFileName> name = record_file_name(file_name);
  if (!name || data.empty()) {
    return std::nullopt;
  }
  NabtsReferenceRecord record;
  record.source = nabts_reference_source(folder_name, name->stream);
  record.key = name->key;
  record.data = std::move(data);
  return record;
}

void NabtsSinkDeps::init(TriggerProgressCallback progress_callback,
                         std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

namespace {

/// One line on the files the datacasts carried: per channel, how many of
/// each kind and by which framing, and how many of their bytes no copy
/// delivered.
std::string carried_files_summary(const std::vector<NabtsCarriedFile>& files) {
  // By the address that carried them, or the family a framed service spread
  // them over.
  const auto where = [](const NabtsCarriedFile& file) {
    return file.last_channel > file.channel
               ? fmt::format("{:03X}-{:03X}", file.channel, file.last_channel)
               : fmt::format("{:03X}", file.channel);
  };
  std::map<std::string, std::map<std::string, size_t>> kinds;
  std::map<std::string, std::string> transfer;
  std::map<std::string, size_t> deciphered;
  std::map<std::string, std::pair<size_t, size_t>> checked;
  std::map<std::string, size_t> cut_short;
  size_t lost = 0;
  for (const NabtsCarriedFile& file : files) {
    const std::string channel = where(file);
    ++kinds[channel][file.format];
    transfer[channel] = file.transfer;
    deciphered[channel] += file.deciphered ? 1 : 0;
    if (file.checksum_holds.has_value()) {
      ++(*file.checksum_holds ? checked[channel].first
                              : checked[channel].second);
    }
    cut_short[channel] += file.cut_short ? 1 : 0;
    lost += file.bytes_lost();
  }
  std::string out;
  for (const auto& [channel, counts] : kinds) {
    std::string parts;
    for (const auto& [format, count] : counts) {
      parts += fmt::format("{}{} {}", parts.empty() ? "" : ", ", count, format);
    }
    out += fmt::format("{}{}: {} by {}", out.empty() ? "" : "; ", channel,
                       parts, transfer[channel]);
    if (deciphered[channel] > 0) {
      out +=
          fmt::format(", {} read through its byte table", deciphered[channel]);
    }
    // The sums the carousel's directory gives, where a file arrived whole,
    // or the checks the containers a framed service sent keep of what they
    // hold.
    const auto [holds, fails] = checked[channel];
    if (holds + fails > 0 && transfer[channel] == "file carousel") {
      out += fmt::format(", {} of {} whole file(s) adding up to their sum",
                         holds, holds + fails);
    } else if (holds + fails > 0) {
      out += fmt::format(
          ", {} of {} unpacked from their container with its check holding",
          holds, holds + fails);
    }
    if (cut_short[channel] > 0) {
      out += fmt::format(
          ", {} unpacked only as far as their container's "
          "bytes went",
          cut_short[channel]);
    }
  }
  if (lost > 0) {
    out += fmt::format("; {} byte(s) no copy delivered", lost);
  }
  return out;
}

}  // namespace

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
  if (result.packets_refused_unprotected > 0) {
    report += fmt::format(
        "; {} refused, every prefix byte corrected (a test-signal line read "
        "as data)",
        result.packets_refused_unprotected);
  }
  if (result.packets_refused_other_service > 0) {
    report += fmt::format(
        "; {} refused from lines that carry no NABTS: the other teletext "
        "service (WST), whose framing code they fit as well, or another "
        "signal, their prefixes seldom whole",
        result.packets_refused_other_service);
  }
  if (result.lost_packets_estimate > 0) {
    report += fmt::format(
        "\n  Lost packets:  {} estimated from the spread of packets per field",
        result.lost_packets_estimate);
  }
  // How many of the packets arrived with their prefix whole: what tells NABTS
  // from another signal behind the same run-in and framing code.
  if (result.packets_written > 0) {
    report += fmt::format(
        "\n  Prefixes:      {} of the packets written arrived with every "
        "prefix byte whole ({:.1f} %)",
        result.packets_prefix_whole,
        100.0 * static_cast<double>(result.packets_prefix_whole) /
            static_cast<double>(result.packets_written));
  }
  if (result.dataset.summary.not_nabts) {
    report +=
        "\n  Not NABTS:     every recording that carries NABTS gives 70 % or "
        "more: these lines carry another signal behind NABTS's run-in and "
        "framing code, so nothing is catalogued from them";
  }

  // What the pass learned about the recording, and what that saved. Reported
  // on every run, because the interesting case is the one where the reader is
  // wondering why a run took as long as it did.
  const TeletextPhaseHint hint = scan_state.phase().hint();
  report += "\n  Data phase:    ";
  if (hint.valid) {
    report +=
        fmt::format("pinned to sample {:.1f} ±{:.1f} from {} locks",
                    hint.centre, hint.radius, scan_state.phase().locks_seen());
  } else {
    report += fmt::format("not pinned ({} locks seen, too few to agree)",
                          scan_state.phase().locks_seen());
  }
  report += "\n  Bit clock:     " + (options.calibrate_bit_clock
                                         ? scan_state.clock().summary()
                                         : std::string("as specified"));
  report += fmt::format("\n  Active lines:  {} candidate line reads skipped",
                        scan_state.lines().lines_skipped());
  // Where the continuity index showed packets routinely lost on lines the pass
  // does not read, which the data groups were told of. Lines are numbered as
  // the per-line table below numbers them.
  std::string routine;
  for (const NabtsRoutineLosses::Routine& loss :
       scan_state.routine_losses().told()) {
    routine +=
        fmt::format("{}{} between line {} (field {}) and line {} (field {})",
                    routine.empty() ? "" : ", ", loss.loss, loss.from.line,
                    loss.from.field + 1, loss.to.line, loss.to.field + 1);
  }
  if (!routine.empty()) {
    report += "; packets routinely lost on lines not read: " + routine;
  }

  // The identities the recording named but the service never transmitted.
  // Reported whenever the pass had anything to say, including when it stood
  // aside: a reader comparing this run's record count with an earlier one's
  // needs to know which of the two is looking at a pruned catalogue.
  const std::string reconciliation =
      result.dataset.summary.identity_reconciliation.summary("record");
  if (!reconciliation.empty() && !result.dataset.summary.not_nabts) {
    report += "\n  Identities:    " + reconciliation;
  }
  if (!result.dataset.summary.salvage.empty() &&
      !result.dataset.summary.not_nabts) {
    report += "\n  Salvage:       " + result.dataset.summary.salvage;
  }
  if (!result.fragments_path.empty()) {
    report += fmt::format(
        "\n  Fragments:     {} run(s) placed in no record or page, written "
        "as text beside the stream as .fragments.txt",
        result.fragments_exported);
  }

  if (result.records_exported > 0) {
    report += fmt::format("\n  Records:       {} exported beside the stream",
                          result.records_exported);
  }
  // The caption service is kept as it arrived rather than catalogued, so how
  // many captions it held is its own figure (CEA-516 §7.3.10).
  if (!result.dataset.captions.empty()) {
    report += fmt::format("\n  Caption service: {} caption(s) as they arrived",
                          result.dataset.captions.size());
    if (result.dataset.summary.captions_dropped > 0) {
      report += fmt::format(", {} more not kept past the bound",
                            result.dataset.summary.captions_dropped);
    }
  }
  if (!options.reference_records.empty()) {
    report += fmt::format(
        "\n  References:    {} record(s) from {} stream(s) for reconstruction",
        result.dataset.references.size(), result.reference_streams);
    if (result.reference_paths_unreadable > 0) {
      report += fmt::format(", {} path(s) unreadable",
                            result.reference_paths_unreadable);
    }
  }
  if (!result.private_text_path.empty()) {
    report += fmt::format("\n  Private text:  written to {}",
                          result.private_text_path);
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

  // What the addresses that are not teletext carried, ahead of the counters:
  // a recording whose NABTS is all datacast otherwise reads as one whose
  // recovery failed.
  const std::string other_services =
      nabts_other_services_summary(result.dataset.summary.other_services);
  if (!other_services.empty()) {
    report += "\n\n";
    report += other_services;
  }
  if (!result.dataset.summary.datacasts.empty()) {
    report += "\n\nDatacasts decoded (RFC 2728 forward error correction)";
    for (const std::string& line : result.dataset.summary.datacasts) {
      report += "\n  " + line;
    }
    if (result.datacasts_exported > 0) {
      report += fmt::format(
          "\n  Payloads:      {} written beside the stream as "
          ".datacast-<channel>.bin, {} with the text they hold as .txt",
          result.datacasts_exported, result.datacast_texts_exported);
    }
    if (!result.dataset.carried_files.empty()) {
      report += "\n  Files:         " +
                carried_files_summary(result.dataset.carried_files);
      if (result.carried_files_exported > 0) {
        report += fmt::format(
            "; {} written beside the stream as "
            ".datacast-<channel>.<name>-<time>",
            result.carried_files_exported);
      }
    }
    if (result.mirror_files_exported > 0) {
      report += fmt::format(
          "\n  Mirror:        the framed services' {} addresses on {} hosts, "
          "{} of them sent again changed{}, laid out as the web sites they "
          "came from in {} files under .mirror/ beside the stream (index.html "
          "lists them)",
          result.mirror_addresses, result.mirror_hosts, result.mirror_replaced,
          result.mirror_misread > 0
              ? fmt::format(", {} placed back in the folder a misread took "
                            "them out of",
                            result.mirror_misread)
              : std::string(),
          result.mirror_files_exported);
    }
    for (const NabtsFramedService& service : result.dataset.framed_services) {
      report += fmt::format("\n  Framed:        service {:08x}, {}",
                            service.identifier, service.summary);
    }
    if (result.framed_services_exported > 0) {
      report += fmt::format(
          "\n  Frames:        {} service stream(s) written beside the stream "
          "as .datacast-<first>-<last>.<service>.frames, the pad taken off, "
          "in the order their count gives",
          result.framed_services_exported);
    }
  }

  report += "\n\n";
  report += stats.summary();
  report += "\n";
  report += group_stats.summary();
  report += "\n";
  report += record_stats.summary();
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
  if (write_file(path, reinterpret_cast<const uint8_t*>(result.report.data()),
                 result.report.size(), kWriterBufferBytes, "report")) {
    result.report_path = path;
  }
}

bool NabtsSinkDeps::write_file(const std::string& path, const uint8_t* data,
                               size_t size, size_t buffer_bytes,
                               const char* what) const {
  auto writer =
      stage_services_->create_buffered_file_writer_uint8(buffer_bytes);
  if (!writer || !writer->open(path)) {
    ORC_LOG_WARN("NabtsSinkDeps: Could not write {} to {}", what, path);
    return false;
  }
  writer->write(data, size);
  writer->close();
  return true;
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
    const std::string path = base + nabts_record_key(record) + kRecordExtension;
    // One unwritable record never fails the export.
    if (write_file(path, record.data.data(), record.data.size(),
                   kRecordBufferBytes, "record")) {
      ++result.records_exported;
    }
  }
  ORC_LOG_DEBUG("NabtsSinkDeps: Exported {} record(s)",
                result.records_exported);
}

void NabtsSinkDeps::write_datacasts(
    const NabtsSinkOptions& options,
    const std::vector<NabtsDatacastChannel>& datacasts,
    NabtsSinkResult& result) const {
  if (!options.export_records || result.output_path.empty() ||
      stage_services_ == nullptr) {
    return;
  }
  // Beside the packet stream, one file per channel: mydata.t33 gives
  // mydata.t33.datacast-242.bin. What the payload means is its service's own,
  // so it is written as decoded; the text it holds — bulletins, listings,
  // markup — is also written on its own, a run of at least
  // kNabtsDatacastTextRun printable characters to a line, where there is any.
  for (const NabtsDatacastChannel& datacast : datacasts) {
    if (datacast.payload.empty()) {
      continue;
    }
    const std::string base =
        fmt::format("{}.datacast-{:03X}", result.output_path, datacast.channel);
    if (write_file(base + ".bin", datacast.payload.data(),
                   datacast.payload.size(), kRecordBufferBytes, "datacast")) {
      ++result.datacasts_exported;
    }
    const std::string text = nabts_datacast_text(datacast.payload);
    if (!text.empty() &&
        write_file(base + ".txt", reinterpret_cast<const uint8_t*>(text.data()),
                   text.size(), kRecordBufferBytes, "datacast text")) {
      ++result.datacast_texts_exported;
    }
  }
}

void NabtsSinkDeps::write_carried_files(const NabtsSinkOptions& options,
                                        NabtsSinkResult& result) const {
  if (!options.export_records || result.output_path.empty() ||
      stage_services_ == nullptr) {
    return;
  }
  // Beside the packet stream, flat like the records: mydata.t33 gives
  // mydata.t33.datacast-243.035J0034-20030519T062710.htm, the address, the
  // file's own name and its time, the extension what its bytes are. A
  // character of the name no file system takes is written as '_', and a
  // second file of the same name and time takes a number. A byte no copy
  // delivered is written as zero; the report says how many there were. A
  // framed service's files go to the mirror instead, below.
  std::set<std::string> written;
  for (const NabtsCarriedFile& file : result.dataset.carried_files) {
    if (nabts_mirror_path(file)) {
      continue;
    }
    std::string stem = file.name;
    for (char& c : stem) {
      const auto byte = static_cast<unsigned char>(c);
      if (std::isalnum(byte) == 0 && c != '.' && c != '-' && c != '_') {
        c = '_';
      }
    }
    std::string extension;
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) {
      extension = stem.substr(dot + 1);
      stem.resize(dot);
    }
    for (char& c : extension) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (file.format == "gif" || file.format == "png") {
      extension = file.format;
    } else if (file.format == "jpeg") {
      extension = "jpg";
    } else if (extension.empty()) {
      extension = file.format == "html"   ? "htm"
                  : file.format == "text" ? "txt"
                                          : "bin";
    }
    std::string when;
    for (const char c : file.time) {
      if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
        when.push_back(c);
      }
    }
    if (when.size() == 14) {
      when.insert(8, "T");
    }
    const std::string addresses =
        file.last_channel > file.channel
            ? fmt::format("{:03X}-{:03X}", file.channel, file.last_channel)
            : fmt::format("{:03X}", file.channel);
    const std::string base =
        fmt::format("{}.datacast-{}.{}{}{}", result.output_path, addresses,
                    stem, when.empty() ? "" : "-", when);
    std::string path = base + "." + extension;
    for (unsigned n = 2; !written.insert(path).second; ++n) {
      path = fmt::format("{}-{}.{}", base, n, extension);
    }
    if (write_file(path, file.data.data(), file.data.size(), kRecordBufferBytes,
                   "datacast file")) {
      ++result.carried_files_exported;
    }
  }
  // Each framed service's frames, the pad taken off, in the order their count
  // gives, the addresses that carried it and its name in the file's:
  // mydata.t33.datacast-0F0-0FF.95a17491.frames.
  for (const NabtsFramedService& service : result.dataset.framed_services) {
    if (service.stream.empty() || service.channels.empty()) {
      continue;
    }
    const std::string path = fmt::format(
        "{}.datacast-{:03X}-{:03X}.{:08x}.frames", result.output_path,
        service.channels.front(), service.channels.back(), service.identifier);
    if (write_file(path, service.stream.data(), service.stream.size(),
                   kRecordBufferBytes, "framed service")) {
      ++result.framed_services_exported;
    }
  }
  // The framed services' files as the web sites they came from, under
  // mydata.t33.mirror/: host/path, every version kept (nabts_web_mirror()).
  const NabtsWebMirror mirror = nabts_web_mirror(result.dataset.carried_files);
  if (mirror.entries.empty()) {
    return;
  }
  const std::filesystem::path root(result.output_path + ".mirror");
  for (const NabtsMirrorFile& file : mirror.files) {
    const std::filesystem::path path = root / file.path;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (write_file(path.string(), file.data.data(), file.data.size(),
                   kRecordBufferBytes, "mirror file")) {
      ++result.mirror_files_exported;
    }
  }
  result.mirror_addresses = mirror.entries.size();
  result.mirror_hosts = mirror.hosts;
  result.mirror_replaced = mirror.replaced;
  result.mirror_misread = mirror.misread;
}

void NabtsSinkDeps::write_fragments(const NabtsSinkOptions& options,
                                    const std::vector<NabtsHeldRun>& runs,
                                    NabtsSinkResult& result) const {
  if (!options.export_records || result.output_path.empty() ||
      stage_services_ == nullptr || runs.empty()) {
    return;
  }
  // Beside the packet stream: mydata.t33 gives mydata.t33.fragments.txt.
  const std::string text = nabts_unplaced_runs_text(runs);
  const std::string path = result.output_path + ".fragments.txt";
  if (write_file(path, reinterpret_cast<const uint8_t*>(text.data()),
                 text.size(), kRecordBufferBytes, "fragments")) {
    result.fragments_exported = runs.size();
    result.fragments_path = path;
  }
}

void NabtsSinkDeps::load_references(const NabtsSinkOptions& options,
                                    NabtsSinkResult& result) const {
  if (options.reference_records.empty()) {
    return;
  }
  namespace fs = std::filesystem;

  // What the entries name, in name order within a folder so a run is
  // repeatable: packet streams to replay, and the record files export_records
  // wrote. A stream's record files are what its own run's vote made of it,
  // with the line each copy came off and how sure the detector was of each
  // byte behind them — neither of which a replayed stream carries — and they
  // are read a file a record rather than reassembled, which on a long
  // recording is minutes against milliseconds. So a folder stands for the
  // record files in it, and for each packet stream in it whose records were
  // not exported beside it.
  std::vector<fs::path> streams;
  std::vector<fs::path> record_files;
  for (const std::string& entry : options.reference_records) {
    std::error_code error;
    const fs::path path(entry);
    if (fs::is_directory(path, error)) {
      std::vector<fs::path> found_streams;
      std::vector<fs::path> found_records;
      for (const auto& file : fs::directory_iterator(path, error)) {
        if (file.path().extension() == kStreamExtension) {
          found_streams.push_back(file.path());
        } else if (file.path().extension() == kRecordExtension) {
          found_records.push_back(file.path());
        }
      }
      std::sort(found_streams.begin(), found_streams.end());
      std::sort(found_records.begin(), found_records.end());
      for (const fs::path& stream : found_streams) {
        const std::string prefix = stream.filename().string() + ".";
        const bool exported = std::any_of(
            found_records.begin(), found_records.end(),
            [&prefix](const fs::path& record) {
              return record.filename().string().rfind(prefix, 0) == 0;
            });
        if (!exported) {
          streams.push_back(stream);
        }
      }
      record_files.insert(record_files.end(), found_records.begin(),
                          found_records.end());
    } else if (fs::is_regular_file(path, error)) {
      (path.extension() == kRecordExtension ? record_files : streams)
          .push_back(path);
    } else {
      ORC_LOG_WARN("NabtsSinkDeps: Reference recording '{}' not found", entry);
      ++result.reference_paths_unreadable;
    }
  }

  // Never this run's own stream, nor the records it exported a moment ago,
  // whose bytes are drawn on anyway.
  std::error_code own_error;
  const fs::path own_path =
      result.output_path.empty()
          ? fs::path()
          : fs::weakly_canonical(fs::path(result.output_path), own_error);
  const auto own_stream = [&own_path, &own_error](const fs::path& stream) {
    std::error_code error;
    const fs::path path = fs::weakly_canonical(stream, error);
    return !own_path.empty() && !own_error && !error && path == own_path;
  };

  for (const fs::path& path : streams) {
    if (cancel_requested_ != nullptr && cancel_requested_->load()) {
      return;
    }
    if (own_stream(path)) {
      continue;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      ORC_LOG_WARN("NabtsSinkDeps: Could not read reference recording {}",
                   path.string());
      ++result.reference_paths_unreadable;
      continue;
    }
    const std::vector<uint8_t> stream{std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>()};
    // Named for the reader: a stream a run wrote as out.t33 is known by the
    // folder it is in.
    const std::string source = nabts_reference_source(
        path.parent_path().filename().string(), path.filename().string());
    size_t loaded = 0;
    for (NabtsCataloguedRecord& record : nabts_catalogue_stream(stream)) {
      if (!nabts_type_is_presentation(record.record_type) ||
          record.data.empty()) {
        continue;
      }
      result.dataset.references.push_back(NabtsReferenceRecord{
          source, nabts_record_key(record), std::move(record.data),
          std::move(record.data_present)});
      ++loaded;
    }
    ++result.reference_streams;
    ORC_LOG_INFO("NabtsSinkDeps: Reference recording {}: {} record(s)", source,
                 loaded);
  }

  // The record files, a stream's at a time. A record file does not say what
  // type of record it holds, so an application record's data (§7.2) is read
  // as well as the pages'.
  fs::path previous_stream;
  std::string source;
  size_t loaded = 0;
  const auto close_stream = [&] {
    if (loaded > 0) {
      ++result.reference_streams;
      ORC_LOG_INFO(
          "NabtsSinkDeps: Reference recording {}: {} record(s) from its record "
          "files",
          source, loaded);
    }
    loaded = 0;
  };
  for (const fs::path& path : record_files) {
    if (cancel_requested_ != nullptr && cancel_requested_->load()) {
      return;
    }
    const std::optional<RecordFileName> name =
        record_file_name(path.filename().string());
    const fs::path stream = path.parent_path() / (name ? name->stream : "");
    if (!name || own_stream(stream)) {
      continue;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      ORC_LOG_WARN("NabtsSinkDeps: Could not read reference record {}",
                   path.string());
      ++result.reference_paths_unreadable;
      continue;
    }
    std::optional<NabtsReferenceRecord> record = nabts_reference_record_file(
        path.parent_path().filename().string(), path.filename().string(),
        std::vector<uint8_t>{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()});
    if (!record) {
      continue;  // An empty file: a record with no data is never exported.
    }
    if (stream != previous_stream) {
      close_stream();
      previous_stream = stream;
      source = record->source;
    }
    result.dataset.references.push_back(std::move(*record));
    ++loaded;
  }
  close_stream();
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
  // The captions are the caption service as it arrived, one entry per caption
  // (NabtsAnalysisDataset::captions), not the version-keyed catalogue: a
  // caption's version names it only while it is on air (§7.3.10.1).
  std::vector<NabtsCataloguedRecord> captions = result.dataset.captions;
  nabts_interpret_records(captions, kNaplpsGridReference);

  // Read the same way the catalogue's caption track reads it — one shared
  // implementation, so the file and the screen cannot disagree.
  const std::vector<NabtsCaptionCue> cues = nabts_caption_cues(captions);
  if (cues.empty()) {
    ORC_LOG_DEBUG(
        "NabtsSinkDeps: No caption records in the range; no cues written");
    return;
  }

  const std::string document = nabts_caption_srt(cues);

  // Beside the packet stream, named after it: mydata.t33 gives mydata.t33.srt,
  // which is the rule the report and the record files follow.
  const std::string path = result.output_path + ".srt";
  if (!write_file(path, reinterpret_cast<const uint8_t*>(document.data()),
                  document.size(), kWriterBufferBytes, "captions")) {
    return;
  }
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
        "Input carries no NABTS service (CEA-516 defines NABTS on the "
        "525-line signal and no 625-line System C service is known; use the "
        "Teletext Sink for a 625-line source)";
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

  // The block-based scanner below (nabts_slice_block) has no per-frame
  // "source has genuinely ended" signal reachable from this loop, unlike
  // the sinks with a simple sequential per-frame fetch — and a piped,
  // unbounded source (frame_count left at 0) reports a huge placeholder
  // frame_range() rather than its real length, not known until it ends.
  // Refuse cleanly rather than scan blocks toward that placeholder forever.
  if (representation->has_unbounded_frame_range()) {
    result.message =
        "Input source has an unbounded frame range (a piped/live source "
        "left frame_count at 0) — this sink's block-based scanner needs a "
        "known length; set an explicit frame_count on the source.";
    return result;
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
  slicer_options.first_field_line = options.first_field_line;
  slicer_options.last_field_line = options.last_field_line;
  const NabtsFrameSlicer frame_slicer(slicer_options);

  // What this pass learns about the recording as it goes: where in the line its
  // data bursts start, and which candidate lines carry them at all.
  NabtsScanState scan_state(options.calibrate_bit_clock);
  TeletextRecoveryStats stats;
  std::array<uint64_t, kMaxPacketsPerFieldTracked + 1> packets_per_field{};

  NabtsPacketChain chain(options);
  // Which lines carry the other 525-line service, judged as the emission
  // reaches them (NabtsServiceLines).
  NabtsServiceLines service_lines;

  // Two blocks are held, the one being emitted and the one being sliced while
  // it is (see NabtsBlockSlicer), and they take turns. They are never swapped:
  // the slicer writes into the one it was handed while the other is emitted.
  std::array<std::vector<NabtsFieldScan>, 2> blocks;
  for (std::vector<NabtsFieldScan>& buffer : blocks) {
    buffer.resize(static_cast<size_t>(kNabtsMaxScanBlockFrames) *
                  kNabtsFieldsPerFrame);
  }
  size_t emitting = 0;
  const size_t worker_count = nabts_resolve_worker_count(
      total_frames * kNabtsFieldsPerFrame, options.decode_threads);
  ORC_LOG_DEBUG("NabtsSinkDeps: Slicing {} frames on {} thread(s)",
                total_frames, worker_count);
  // Declared after everything its threads read, so that it is destroyed — and
  // its threads joined — before any of it.
  NabtsBlockSlicer block_slicer(worker_count);

  const auto finish = [&](NabtsSinkResult& partial) {
    partial.lost_packets_estimate = estimate_lost_packets(packets_per_field);
    chain.finish(partial.dataset);
    NabtsRecoverySummary& summary = partial.dataset.summary;
    summary.frames_analysed = partial.frames_analysed;
    summary.packets_recovered = partial.packets_written;
    summary.packets_refused_unprotected = partial.packets_refused_unprotected;
    summary.packets_refused_other_service =
        partial.packets_refused_other_service;
    summary.packets_prefix_whole = partial.packets_prefix_whole;
    summary.lost_packets_estimate = partial.lost_packets_estimate;
    // Lines that carry another signal behind NABTS's run-in and framing code
    // give packets only where Hamming correction let their noise through, and
    // whatever the catalogue made of those is noise too: nothing is listed or
    // exported from them (nabts_lines_carry_no_nabts()).
    if (nabts_lines_carry_no_nabts(partial.packets_written,
                                   partial.packets_prefix_whole)) {
      summary.not_nabts = true;
      partial.dataset.records.clear();
      partial.dataset.captions.clear();
      partial.dataset.datacasts.clear();
      partial.dataset.carried_files.clear();
      partial.dataset.framed_services.clear();
      partial.dataset.private_data.clear();
      ORC_LOG_INFO(
          "NabtsSinkDeps: {} of {} packet prefixes arrived whole: these lines "
          "carry another signal, not NABTS, and nothing is catalogued",
          partial.packets_prefix_whole, partial.packets_written);
    }

    // Exported before the report is built, because the report says how many
    // were written. A cancelled run exports what it catalogued, for the same
    // reason its packet stream is left as a prefix rather than deleted.
    ORC_LOG_DEBUG("NabtsSinkDeps: Lines of the other teletext service: {}",
                  service_lines.summary());
    write_records(options, partial);
    write_captions(options, partial);
    // Lines that carry no NABTS have no datacasts or fragments either: what
    // they yield is the other signal's bytes read as packets.
    if (!summary.not_nabts) {
      write_datacasts(options, chain.datacasts(), partial);
      write_carried_files(options, partial);
      write_fragments(options, chain.unplaced_runs(), partial);
    } else {
      summary.datacasts.clear();
    }
    // The text of the provider's own groups goes beside the report, which
    // names the channels that carried it: mydata.t33 gives
    // mydata.t33.other.txt.
    const std::string other_text = chain.private_text();
    if (options.write_report && !partial.output_path.empty() &&
        !other_text.empty() && stage_services_ != nullptr) {
      const std::string path = partial.output_path + ".other.txt";
      if (write_file(path, reinterpret_cast<const uint8_t*>(other_text.data()),
                     other_text.size(), kWriterBufferBytes, "private text")) {
        partial.private_text_path = path;
      }
    }
    load_references(options, partial);

    partial.report =
        build_report(options, partial, total_frames, stats, scan_state,
                     chain.group_stats(), chain.record_stats());
  };

  try {
    uint64_t frames_processed = 0;
    uint64_t block_frames = kNabtsFirstScanBlockFrames;

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

    FrameID block_first = frame_rng.first;
    uint64_t block_count = std::min(block_frames, total_frames);
    block_slicer.start(*representation, frame_slicer, block_first, 0,
                       block_count, scan_state.snapshot(), cancel_requested_,
                       blocks[emitting]);

    for (;;) {
      block_slicer.finish();

      // A cancelled block was abandoned part way through, so none of it is
      // emitted: the stream stays a prefix of the run rather than gaining a
      // hole where the workers stopped taking jobs.
      if (cancelled()) {
        return report_cancelled();
      }

      const std::vector<NabtsFieldScan>& block = blocks[emitting];

      // The pass learns here rather than in the slicer, in its own frame
      // order, so what it knows at any point is a function of the recording
      // and not of the scheduling. It learns from the whole block before the
      // next one starts, because the next one is sliced against what this one
      // taught; emission reads nothing the learning writes.
      learn_from_block(scan_state, block, block_count);

      // The next block is sliced while this one is emitted: emission is
      // ordered and stays on this thread, and the workers would otherwise
      // stand idle for the whole of it.
      const uint64_t frames_done =
          static_cast<uint64_t>(block_first - frame_rng.first) + block_count;
      const bool more_blocks = frames_done < total_frames;
      const FrameID next_first =
          block_first + static_cast<FrameID>(block_count);
      uint64_t next_count = 0;
      bool next_started = false;
      if (more_blocks) {
        block_frames = std::min(block_frames * 2, kNabtsMaxScanBlockFrames);
        next_count = std::min(block_frames, total_frames - frames_done);
        if (!cancelled()) {
          block_slicer.start(*representation, frame_slicer, next_first,
                             frames_done, next_count, scan_state.snapshot(),
                             cancel_requested_, blocks[1 - emitting]);
          next_started = true;
        }
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
        chain.set_frame(static_cast<uint64_t>(block_first) + block_offset);
        for (size_t field_idx = 0; field_idx < kNabtsFieldsPerFrame;
             ++field_idx) {
          const NabtsFieldScan& scan =
              block[static_cast<size_t>(block_offset) * kNabtsFieldsPerFrame +
                    field_idx];
          const std::vector<NabtsFrameLineResult>& line_results = scan.lines;

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

            // A test-signal line read as data is refused here rather than by
            // the slicer, so the scan state still learns from the line as it
            // did, and the line counts as read and empty for the continuity
            // bound. Refused earlier, it moved the data phase the pass learns
            // and re-read real packets on tapes with a test signal in the
            // data lines, a net loss; refused here, the records come out
            // nearer the other recordings of the same day.
            const bool unprotected =
                line != nullptr && line->sliced.valid &&
                nabts_prefix_every_byte_corrected(line->sliced.bytes.data());
            if (unprotected) {
              ++result.packets_refused_unprotected;
            }
            // A line of the other 525-line service, read as NABTS inside the
            // detector's framing margin, is refused the same way and for the
            // same reason (NabtsServiceLines).
            const bool other_service =
                line != nullptr && line->sliced.valid && !unprotected &&
                service_lines.refuse(
                    field_idx, field_line,
                    line->sliced.other_framing_fits_better,
                    nabts_prefix_every_byte_whole(line->sliced.bytes.data()));
            if (other_service) {
              ++result.packets_refused_other_service;
            }
            if (line != nullptr && line->sliced.valid && !unprotected &&
                !other_service) {
              ++result.packets_written;
              if (nabts_prefix_every_byte_whole(line->sliced.bytes.data())) {
                ++result.packets_prefix_whole;
              }
              ++field_packets;
              emit(line->sliced.bytes.data(), kNabtsPacketBytes);
              // Same bytes, same order, into the reassembler. A padding packet
              // is deliberately not fed: it stands for a line that carried
              // nothing, and offering it as a packet would break the continuity
              // chain rather than record a gap in it.
              chain.add_packet(
                  packet_from_line(line->sliced), line->sliced.bytes.data(),
                  NabtsRoutineLosses::Position{field_idx, field_line},
                  scan_state.routine_losses());
            } else {
              if (line != nullptr) {
                // A line the slicer read and got nothing from is one a packet
                // may have been lost on, which is what bounds how many a gap in
                // the continuity index can stand for. A line it never read is
                // one the pass has learnt carries nothing, unless the
                // continuity index has shown packets routinely lost there
                // (above). A line read only on the pass's periodic look at the
                // whole window, never yet seen to carry data, lost no group's
                // packet, and the groups are told so.
                const NabtsLineMask& alive = scan_state.lines().alive();
                chain.add_empty_line(
                    nabts_line_tracked(field_idx, field_line) &&
                    alive[field_idx][static_cast<size_t>(field_line)]);
              }
              if (options.keep_empty_packets) {
                ++result.packets_written;
                emit(kEmptyPacket.data(), kNabtsPacketBytes);
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

      if (!more_blocks) {
        break;
      }
      if (!next_started) {
        return report_cancelled();
      }
      block_first = next_first;
      block_count = next_count;
      emitting = 1 - emitting;
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
