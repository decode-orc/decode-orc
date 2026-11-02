/*
 * File:        nabts_data_export.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Everything a NABTS run decoded, written as the files it is
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_data_export.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nabts_network_messages.h"
#include "nabts_record_catalogue.h"
#include "nabts_web_mirror.h"
#include "naplps_render_grid.h"

namespace orc {
namespace {

constexpr const char* kRecordExtension = ".rec";
// A carried file's time as the service gave it, digits only, is a date and a
// time of day: YYYYMMDD then hhmmss, a 'T' between them.
constexpr size_t kDateTimeDigits = 14;
constexpr size_t kDateDigits = 8;

bool put(const NabtsExportWriter& write, NabtsExportTally& tally,
         const std::string& path, const uint8_t* data, size_t size) {
  return vbi_export_put(write, tally, path, data, size);
}

bool put_text(const NabtsExportWriter& write, NabtsExportTally& tally,
              const std::string& path, const std::string& text) {
  return vbi_export_put_text(write, tally, path, text);
}

/// A value of a tab-separated line: its tabs and line ends as spaces.
std::string tsv(std::string value) {
  std::replace_if(
      value.begin(), value.end(),
      [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
  return value;
}

/// Bytes a parallel arrival mask says no copy delivered.
size_t bytes_lost(const std::vector<uint8_t>& present) {
  return static_cast<size_t>(std::count(present.begin(), present.end(), 0));
}

const char* plural(size_t count) { return count == 1 ? "" : "s"; }

/// The classification flags (CEA-516 §5.2.7.2) a record set, by name.
std::string record_flags(const NabtsCataloguedRecord& record) {
  const std::pair<bool, const char*> named[] = {
      {record.caption, "caption"},
      {record.cyclic_marker, "cyclic marker"},
      {record.priority, "priority"},
      {record.alarm, "alarm"},
      {record.update, "update"},
      {record.support_record, "support record"},
      {record.support_needed, "support needed"},
      {record.index, "index"},
      {record.more, "more"},
      {record.auto_acquire, "auto-acquire"},
  };
  std::string out;
  for (const auto& [set, name] : named) {
    if (set) {
      out += out.empty() ? "" : ", ";
      out += name;
    }
  }
  return out;
}

void export_records(const NabtsAnalysisDataset& dataset,
                    const NabtsExportWriter& write, NabtsExportTally& tally) {
  // A folder per data channel, each record named for the identity CEA-516
  // §5.2.1 gives it, which is the key the catalogue orders on: a folder lists
  // in the order the browser does.
  std::string index =
      "file\tchannel\taddress\tversion\tcontent\trecord_type\tflags\t"
      "first_frame\tlast_frame\ttimes_seen\tcopies_voted\tcomplete\tbytes\t"
      "bytes_lost\n";
  size_t written = 0;
  for (const NabtsCataloguedRecord& record : dataset.records) {
    if (record.data.empty()) {
      continue;  // every copy was header-only: nothing to write
    }
    const std::string file =
        fmt::format("{:03X}/{}{}", record.channel, nabts_record_key(record),
                    kRecordExtension);
    if (!put(write, tally, "records/" + file, record.data.data(),
             record.data.size())) {
      continue;
    }
    ++written;
    index += fmt::format(
        "{}\t{:03X}\t{}\t{:X}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", file,
        record.channel, tsv(record.address_text), record.version & 0xFU,
        record.content + 1, record.record_type, record_flags(record),
        record.first_seen_frame, record.last_seen_frame, record.times_seen,
        record.copies_voted, record.complete ? "yes" : "no", record.data.size(),
        bytes_lost(record.data_present));
  }
  if (written == 0) {
    return;
  }
  put_text(write, tally, "records/index.tsv", index);
  tally.summary.push_back(
      fmt::format("{} record{} as their copies voted them, under records/",
                  written, plural(written)));
}

void export_captions(const NabtsAnalysisDataset& dataset,
                     const NabtsExportWriter& write, NabtsExportTally& tally) {
  if (dataset.captions.empty()) {
    return;
  }
  // Read as the caption track of the catalogue reads them, and as the
  // trigger's own subtitle file does: repaired and at the reference receiver,
  // a character's code being its code at any resolution.
  std::vector<NabtsCataloguedRecord> captions = dataset.captions;
  nabts_interpret_records(captions, kNaplpsGridReference, /*repair=*/true);
  const std::vector<NabtsCaptionCue> cues = nabts_caption_cues(captions);
  size_t cues_written = 0;
  if (!cues.empty() && put_text(write, tally, "captions/captions.srt",
                                nabts_caption_srt(cues))) {
    cues_written = cues.size();
  }
  // Each caption's record as it arrived, numbered in order: a caption's
  // version names it only while it is on air (§7.3.10.1), so the key alone
  // would put later captions over earlier ones.
  size_t records = 0;
  for (size_t i = 0; i < dataset.captions.size(); ++i) {
    const NabtsCataloguedRecord& caption = dataset.captions[i];
    if (caption.data.empty()) {
      continue;
    }
    const std::string path =
        fmt::format("captions/records/{:04}-{}{}", i + 1,
                    nabts_record_key(caption), kRecordExtension);
    if (put(write, tally, path, caption.data.data(), caption.data.size())) {
      ++records;
    }
  }
  if (cues_written + records == 0) {
    return;
  }
  tally.summary.push_back(fmt::format(
      "{} caption{} as subtitles in captions/captions.srt, and {} caption "
      "record{} under captions/records/",
      cues_written, plural(cues_written), records, plural(records)));
}

void export_datacasts(const NabtsAnalysisDataset& dataset,
                      const NabtsExportWriter& write, NabtsExportTally& tally) {
  std::string index = "file\tchannel\traw\tpayload_bytes\tsummary\n";
  size_t written = 0;
  for (const NabtsDatacastListing& listing : dataset.datacasts) {
    if (listing.payload.empty()) {
      continue;
    }
    const std::string base = fmt::format("{:03X}", listing.channel);
    if (!put(write, tally, "datacasts/" + base + ".bin", listing.payload.data(),
             listing.payload.size())) {
      continue;
    }
    ++written;
    if (!listing.text.empty()) {
      put_text(write, tally, "datacasts/" + base + ".txt", listing.text);
    }
    if (!listing.carried.empty()) {
      put_text(write, tally, "datacasts/" + base + ".carried.txt",
               listing.carried);
    }
    if (!listing.table_rows.empty()) {
      put_text(write, tally, "datacasts/" + base + ".tsv",
               nabts_table_tsv(listing.table_columns, listing.table_rows));
    }
    index += fmt::format("{}.bin\t{:03X}\t{}\t{}\t{}\n", base, listing.channel,
                         listing.raw ? "yes" : "no", listing.payload.size(),
                         tsv(listing.summary));
  }
  if (written == 0) {
    return;
  }
  put_text(write, tally, "datacasts/index.tsv", index);
  tally.summary.push_back(fmt::format(
      "{} datacast payload{} as decoded, with the text they hold, under "
      "datacasts/",
      written, plural(written)));
}

void export_carried_files(const NabtsAnalysisDataset& dataset,
                          const NabtsExportWriter& write,
                          NabtsExportTally& tally) {
  // A framed service's files go to the web folder instead, as the sites they
  // came from.
  std::string index =
      "file\taddresses\ttransfer\tname\ttime\tformat\tbytes\tbytes_lost\t"
      "copies\tchecksum\tchecksum_holds\tdeciphered\tjoined_parts\tcut_short\n";
  std::set<std::string> used;
  size_t written = 0;
  for (const NabtsCarriedFile& file : dataset.carried_files) {
    if (nabts_mirror_path(file)) {
      continue;
    }
    const NabtsCarriedFileName name = nabts_carried_file_name(file);
    const std::string folder = nabts_carried_file_addresses(file) + "/";
    std::string relative = folder + name.base + "." + name.extension;
    for (unsigned n = 2; !used.insert(relative).second; ++n) {
      relative =
          fmt::format("{}{}-{}.{}", folder, name.base, n, name.extension);
    }
    if (!put(write, tally, "files/" + relative, file.data.data(),
             file.data.size())) {
      continue;
    }
    ++written;
    if (!file.contents.empty()) {
      put_text(write, tally, "files/" + relative + ".contents.txt",
               file.contents);
    }
    if (!file.response_head.empty()) {
      put_text(write, tally, "files/" + relative + ".response.txt",
               file.response_head);
    }
    // The service's check of the file and whether it held, where it gave one.
    std::string checksum;
    std::string holds;
    if (file.checksum.has_value()) {
      checksum = fmt::format("{:08X}", file.checksum.value_or(0));
    }
    if (file.checksum_holds.has_value()) {
      holds = file.checksum_holds.value_or(false) ? "yes" : "no";
    }
    index += fmt::format(
        "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", relative,
        nabts_carried_file_addresses(file), tsv(file.transfer), tsv(file.name),
        tsv(file.time), file.format, file.data.size(), file.bytes_lost(),
        file.copies, checksum, holds, file.deciphered ? "yes" : "no",
        file.joined_parts, file.cut_short ? "yes" : "no");
  }
  if (written == 0) {
    return;
  }
  put_text(write, tally, "files/index.tsv", index);
  tally.summary.push_back(
      fmt::format("{} file{} the datacasts carried, as sent, under files/",
                  written, plural(written)));
}

void export_framed_services(const NabtsAnalysisDataset& dataset,
                            const NabtsExportWriter& write,
                            NabtsExportTally& tally) {
  size_t written = 0;
  for (const NabtsFramedService& service : dataset.framed_services) {
    if (service.stream.empty() || service.channels.empty()) {
      continue;
    }
    const std::string base =
        fmt::format("frames/{:03X}-{:03X}.{:08x}", service.channels.front(),
                    service.channels.back(), service.identifier);
    if (!put(write, tally, base + ".frames", service.stream.data(),
             service.stream.size())) {
      continue;
    }
    ++written;
    std::string text = service.summary + "\n";
    if (!service.catalogue.empty()) {
      text +=
          "\nWhat the catalogue of services says of it: " + service.catalogue +
          "\n";
    }
    if (!service.headers.empty()) {
      text +=
          "\nThe headers of its first frames, the pad off:\n" + service.headers;
    }
    put_text(write, tally, base + ".txt", text);
  }
  if (written == 0) {
    return;
  }
  tally.summary.push_back(fmt::format(
      "the frames of {} framed service{}, the pad off and in the order their "
      "count gives, under frames/",
      written, plural(written)));
}

void export_web_mirror(const NabtsAnalysisDataset& dataset,
                       const NabtsExportWriter& write,
                       NabtsExportTally& tally) {
  const NabtsWebMirror mirror = nabts_web_mirror(dataset.carried_files);
  if (mirror.entries.empty()) {
    return;  // no framed service sent a file: no site, and no index of one
  }
  size_t written = 0;
  for (const NabtsMirrorFile& file : mirror.files) {
    written += put(write, tally, "web/" + file.path, file.data.data(),
                   file.data.size())
                   ? 1
                   : 0;
  }
  if (written == 0) {
    return;
  }
  tally.summary.push_back(fmt::format(
      "{} web address{} on {} host{}, {} file{} in all, as the sites they "
      "came from, under web/ (open web/index.html)",
      mirror.entries.size(), mirror.entries.size() == 1 ? "" : "es",
      mirror.hosts, plural(mirror.hosts), written, plural(written)));
}

void export_private_data(const NabtsAnalysisDataset& dataset,
                         const NabtsExportWriter& write,
                         NabtsExportTally& tally) {
  size_t written = 0;
  for (const NabtsPrivateData& data : dataset.private_data) {
    std::string text = fmt::format(
        "Data groups of type {} on {:03X}: {} group{}, {} packet{}, {} of the "
        "groups read as text",
        data.type, data.channel, data.groups, plural(data.groups), data.packets,
        plural(data.packets), data.text_groups);
    if (data.not_shown > 0) {
      text += fmt::format("; the last {} not kept", data.not_shown);
    }
    text += "\n\n" + data.shown;
    written += put_text(write, tally,
                        fmt::format("private/{:03X}-type{}.txt", data.channel,
                                    data.type),
                        text)
                   ? 1
                   : 0;
  }
  if (written == 0) {
    return;
  }
  tally.summary.push_back(fmt::format(
      "the data groups of a type the standard reserves on {} channel{}, as "
      "they arrived, under private/",
      written, plural(written)));
}

/// What each folder or file of the export holds, for its README.
constexpr VbiExportPart kExportParts[] = {
    {"report.txt",
     "The run's report: what the recording carried and how it "
     "decoded."},
    {"records/",
     "Each record (CEA-516) as its copies voted it, in a folder per data "
     "channel, named for its identity: channel, record address and version "
     "(CHN-ADDRESS-vV.rec, a further -N where one identity carried more than "
     "one content). A presentation record holds NAPLPS (ANSI X3.110) code. "
     "index.tsv lists them with their flags, when they were seen and the "
     "bytes no copy delivered."},
    {"captions/",
     "The caption service: captions.srt, the captions as subtitles; records/, "
     "each caption's record in the order the captions arrived."},
    {"datacasts/",
     "Each datacast as decoded, by packet address: CHN.bin its payload, "
     "CHN.txt the text it holds, CHN.carried.txt what it carries where its "
     "framing is known (a framed service's frames, a clock's readings), and "
     "CHN.tsv a table it holds (an automation playlist's events). index.tsv "
     "says how each decoded."},
    {"files/",
     "The files the datacasts carried, as sent, in a folder per packet "
     "address: NAME-TIME.EXT, the extension what the bytes are. Beside a file "
     "whose format is known, NAME.contents.txt is what was read of it (an "
     "archive's members, a table's records). index.tsv gives each file's "
     "copies, the bytes no copy delivered and whether its check held."},
    {"frames/",
     "Each framed datacast service's frames, the pad off, in the order their "
     "count gives (FIRST-LAST.ID.frames, the packet addresses that carried "
     "it and its identifier), and how they read (.txt)."},
    {"web/",
     "The web pages and files framed services sent, laid out as the sites "
     "they came from, links pointed at one another. Open index.html; "
     "manifest.tsv lists every version of every address."},
    {"messages/",
     "Items of a network's own data services for its stations, read out of "
     "private-use data groups: wire stories, station messages, alerts, "
     "pages. A folder per channel and record address; each item's .txt gives "
     "its header, heading fields, text and trailer, .block.bin a binary "
     "heading block and .naplps a page's code. index.tsv lists them."},
    {"private/",
     "Data groups of types the standard reserves, as they arrived, a file "
     "per channel and type: text where they read as text, otherwise their "
     "bytes."},
    {"fragments.txt",
     "Runs of bytes recovered that no record could be placed in."},
};

}  // namespace

NabtsCarriedFileName nabts_carried_file_name(const NabtsCarriedFile& file) {
  NabtsCarriedFileName out;
  std::string stem = file.name;
  for (char& c : stem) {
    const auto byte = static_cast<unsigned char>(c);
    if (std::isalnum(byte) == 0 && c != '.' && c != '-' && c != '_') {
      c = '_';
    }
  }
  const size_t dot = stem.rfind('.');
  if (dot != std::string::npos) {
    out.extension = stem.substr(dot + 1);
    stem.resize(dot);
  }
  for (char& c : out.extension) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (file.format == "gif" || file.format == "png") {
    out.extension = file.format;
  } else if (file.format == "jpeg") {
    out.extension = "jpg";
  } else if (out.extension.empty()) {
    out.extension = file.format == "html"   ? "htm"
                    : file.format == "text" ? "txt"
                                            : "bin";
  }
  std::string when;
  for (const char c : file.time) {
    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      when.push_back(c);
    }
  }
  if (when.size() == kDateTimeDigits) {
    when.insert(kDateDigits, "T");
  }
  out.base = when.empty() ? stem : stem + "-" + when;
  return out;
}

std::string nabts_table_tsv(const std::vector<std::string>& columns,
                            const std::vector<std::vector<std::string>>& rows) {
  const auto line = [](const std::vector<std::string>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
      out += (i == 0 ? "" : "\t") + tsv(values[i]);
    }
    return out + "\n";
  };
  std::string out = line(columns);
  for (const std::vector<std::string>& row : rows) {
    out += line(row);
  }
  return out;
}

std::string nabts_carried_file_addresses(const NabtsCarriedFile& file) {
  return file.last_channel > file.channel
             ? fmt::format("{:03X}-{:03X}", file.channel, file.last_channel)
             : fmt::format("{:03X}", file.channel);
}

size_t nabts_export_network_messages(
    const std::vector<NabtsNetworkMessage>& messages,
    const NabtsExportWriter& write, NabtsExportTally& tally) {
  std::string index =
      "file\tchannel\trecord_address\tkind\tsequence\tfirst_frame\tlast_frame\t"
      "form\tlost_bytes\tdamaged\tcomplete\ttitle\n";
  size_t written = 0;
  for (size_t i = 0; i < messages.size(); ++i) {
    const NabtsNetworkMessage& message = messages[i];
    const std::string base = fmt::format(
        "{:03X}-{}/{}", message.channel & 0xFFFU, message.record_address,
        nabts_network_message_file_stem(message, i));
    if (!put_text(write, tally, base + ".txt",
                  nabts_network_message_document(message))) {
      continue;
    }
    ++written;
    if (!message.frame.empty()) {
      put(write, tally, base + ".block.bin", message.frame.data(),
          message.frame.size());
    }
    if (!message.code.empty()) {
      put(write, tally, base + ".naplps", message.code.data(),
          message.code.size());
    }
    index += fmt::format(
        "{}.txt\t{:03X}\t{}\t{}\t{:X}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", base,
        message.channel & 0xFFFU, message.record_address, message.kind,
        message.sequence, message.first_frame, message.last_frame, message.form,
        message.lost_bytes, message.damaged_bytes,
        message.complete ? "yes" : "no", tsv(message.title));
  }
  if (written > 0) {
    put_text(write, tally, "index.tsv", index);
  }
  return written;
}

NabtsExportTally nabts_export_data(const NabtsAnalysisDataset& dataset,
                                   const std::string& report,
                                   const NabtsExportWriter& write) {
  NabtsExportTally tally;
  std::vector<const char*> written;
  const auto part = [&](const char* name, size_t files_before) {
    if (tally.files > files_before) {
      written.push_back(name);
    }
  };

  size_t before = tally.files;
  if (!report.empty() && put_text(write, tally, "report.txt", report)) {
    tally.summary.emplace_back("the run's report, as report.txt");
  }
  part("report.txt", before);
  before = tally.files;
  export_records(dataset, write, tally);
  part("records/", before);
  before = tally.files;
  export_captions(dataset, write, tally);
  part("captions/", before);
  before = tally.files;
  export_datacasts(dataset, write, tally);
  part("datacasts/", before);
  before = tally.files;
  export_carried_files(dataset, write, tally);
  part("files/", before);
  before = tally.files;
  export_framed_services(dataset, write, tally);
  part("frames/", before);
  before = tally.files;
  export_web_mirror(dataset, write, tally);
  part("web/", before);
  before = tally.files;
  if (!dataset.network_messages.empty()) {
    NabtsExportTally messages;
    const size_t items = nabts_export_network_messages(
        dataset.network_messages,
        [&write](const std::string& path, const uint8_t* data, size_t size) {
          return write("messages/" + path, data, size);
        },
        messages);
    tally.files += messages.files;
    for (const std::string& failure : messages.failures) {
      tally.failures.push_back("messages/" + failure);
    }
    if (items > 0) {
      tally.summary.push_back(fmt::format(
          "{} item{} of the network's own data services, under messages/",
          items, plural(items)));
    }
  }
  part("messages/", before);
  before = tally.files;
  export_private_data(dataset, write, tally);
  part("private/", before);
  before = tally.files;
  if (!dataset.unplaced_runs.empty() &&
      put_text(write, tally, "fragments.txt", dataset.unplaced_runs)) {
    tally.summary.emplace_back(
        "the runs of bytes no record could be placed in, as fragments.txt");
  }
  part("fragments.txt", before);

  // What the folders hold, for a reader who finds the export without the
  // program that wrote it.
  vbi_export_readme(
      write, tally,
      "What a NABTS decode found, each kind of data in the format it was sent "
      "or decoded to.\n\n",
      written, kExportParts, std::size(kExportParts));
  return tally;
}

}  // namespace orc
