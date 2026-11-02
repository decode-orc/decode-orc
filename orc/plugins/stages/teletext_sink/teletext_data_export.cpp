/*
 * File:        teletext_data_export.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Everything a teletext run decoded, written as the files it is
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "teletext_data_export.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace orc {
namespace {

// The sextants of a block mosaic that Unicode gives characters of their own
// outside the Symbols for Legacy Computing block, which numbers the rest in
// order of their pattern: none, the left column, the right column, all six.
constexpr uint8_t kSextantsNone = 0;
constexpr uint8_t kSextantsLeft = 0x15;
constexpr uint8_t kSextantsRight = 0x2A;
constexpr uint8_t kSextantsAll = 0x3F;
constexpr char32_t kFirstSextant = 0x1FB00;
constexpr char32_t kLeftHalfBlock = 0x258C;
constexpr char32_t kRightHalfBlock = 0x2590;
constexpr char32_t kFullBlock = 0x2588;

/// The sextants a G1 mosaic code draws (EN 300 706 §15.7.1 Table 47): bits 1
/// to 5 of the code the top-left to bottom-left cells, bit 7 the bottom-right.
uint8_t mosaic_sixels(uint8_t code) {
  return static_cast<uint8_t>((code & 0x1F) | ((code >> 1) & 0x20));
}

/// Whether a code draws a block mosaic in mosaic mode: with blast-through,
/// codes 4/0-5/F stay capitals (Table 47 NOTE 1).
bool is_mosaic_code(uint8_t code, bool blast_through) {
  return code >= 0x20 && (!blast_through || code <= 0x3F || code >= 0x60);
}

char32_t sextant_character(uint8_t sixels) {
  switch (sixels) {
    case kSextantsNone:
      return U' ';
    case kSextantsLeft:
      return kLeftHalfBlock;
    case kSextantsRight:
      return kRightHalfBlock;
    case kSextantsAll:
      return kFullBlock;
    default:
      return kFirstSextant + sixels - 1 - (sixels > kSextantsLeft ? 1 : 0) -
             (sixels > kSextantsRight ? 1 : 0);
  }
}

/// Magazine and two hexadecimal digits: "100", "1F0".
std::string page_label(int magazine, int page_number) {
  return fmt::format("{}{:02X}", magazine, page_number & 0xFF);
}

/// The sub-code's four digits S4 S3 S2 S1 as Annex A.1 writes them.
std::string subcode_label(int subcode) {
  return fmt::format("{:X}{:X}{:X}{:X}", (subcode >> 11) & 0x3,
                     (subcode >> 7) & 0xF, (subcode >> 4) & 0x7, subcode & 0xF);
}

int rows_received(const TeletextPageSnapshot& page) {
  return static_cast<int>(
      std::count(page.row_received.begin(), page.row_received.end(), true));
}

const char* plural(size_t count) { return count == 1 ? "" : "s"; }

/// A value of a tab-separated line: its tabs and line ends as spaces.
std::string tsv(std::string value) {
  std::replace_if(
      value.begin(), value.end(),
      [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
  return value;
}

void export_pages(const TeletextAnalysisDataset& dataset,
                  const VbiExportWriter& write, VbiExportTally& tally) {
  std::string index =
      "file\tpage\tsubcode\tversion\tfirst_frame\tlast_frame\ttimes_seen\t"
      "rows_received\tsubtitle\n";
  size_t pages = 0;
  size_t versions = 0;
  for (const TeletextCataloguedPage& page : dataset.pages) {
    const std::string label = page_label(page.magazine, page.page_number);
    for (const TeletextCataloguedSubPage& sub : page.subpages) {
      const std::string stem =
          fmt::format("{}/{}-{}", label, label, subcode_label(sub.subcode));
      // The versions it replaced first, oldest first, then the one on air
      // last: the order the recording showed them in.
      for (size_t v = 0; v < sub.earlier_versions.size(); ++v) {
        const TeletextPageVersion& version = sub.earlier_versions[v];
        const std::string file = fmt::format("{}.v{}.txt", stem, v + 1);
        if (vbi_export_put_text(write, tally, "pages/" + file,
                                teletext_page_text(version.page))) {
          ++versions;
          index += fmt::format(
              "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", file, label,
              subcode_label(sub.subcode), v + 1, version.first_seen_frame,
              version.last_seen_frame, version.times_seen,
              rows_received(version.page), page.subtitle ? "yes" : "no");
        }
      }
      const std::string file = stem + ".txt";
      if (vbi_export_put_text(write, tally, "pages/" + file,
                              teletext_page_text(sub.page))) {
        ++pages;
        index += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", file,
                             label, subcode_label(sub.subcode),
                             sub.earlier_versions.size() + 1,
                             sub.version_first_seen_frame, sub.last_seen_frame,
                             sub.version_times_seen, rows_received(sub.page),
                             page.subtitle ? "yes" : "no");
      }
    }
  }
  if (pages + versions == 0) {
    return;
  }
  vbi_export_put_text(write, tally, "pages/index.tsv", index);
  tally.summary.push_back(
      versions == 0
          ? fmt::format("{} page{} as text, under pages/", pages, plural(pages))
          : fmt::format("{} page{} as text, and {} earlier version{} of them, "
                        "under pages/",
                        pages, plural(pages), versions, plural(versions)));
}

void export_service_data(const TeletextAnalysisDataset& dataset,
                         const VbiExportWriter& write, VbiExportTally& tally) {
  if (dataset.service_data.empty()) {
    return;
  }
  std::string text = fmt::format(
      "Broadcast service data (packets 8/30), {} packet{} read:\n\n",
      dataset.service_data_packets, plural(dataset.service_data_packets));
  for (const std::string& line : dataset.service_data) {
    text += line + "\n";
  }
  if (vbi_export_put_text(write, tally, "service-data.txt", text)) {
    tally.summary.emplace_back(
        "what the broadcast service data said, as service-data.txt");
  }
}

void export_data_lines(const TeletextAnalysisDataset& dataset,
                       const VbiExportWriter& write, VbiExportTally& tally) {
  std::string index = "file\tformat\tchannel\tpackets\tbytes\tsummary\n";
  size_t written = 0;
  for (const TeletextDataLine& line : dataset.data_lines) {
    if (line.payload.empty()) {
      continue;
    }
    const std::string file = line.tag + ".bin";
    if (!vbi_export_put(write, tally, "data-lines/" + file, line.payload.data(),
                        line.payload.size())) {
      continue;
    }
    ++written;
    index += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\n", file, tsv(line.format),
                         line.channel, line.packets, line.payload.size(),
                         tsv(line.summary));
  }
  if (written == 0) {
    return;
  }
  vbi_export_put_text(write, tally, "data-lines/index.tsv", index);
  tally.summary.push_back(fmt::format(
      "the payloads of {} data stream{} as they arrived, under data-lines/",
      written, plural(written)));
}

constexpr VbiExportPart kExportParts[] = {
    {"report.txt",
     "The run's report: what the recording carried and how it "
     "decoded."},
    {"pages/",
     "Each sub-page as text, in UTF-8, in a folder per page: MPP-SSSS.txt "
     "the version on air last, MPP-SSSS.vN.txt each version it replaced, the "
     "oldest first. A block mosaic is the sextant character it draws. "
     "index.tsv says when each was seen and how many of its rows arrived."},
    {"service-data.txt",
     "What the broadcast service data (packets 8/30) said: the network, the "
     "initial page, the time and date, the status display and the programme "
     "labels."},
    {"data-lines/",
     "The payloads of the independent data lines and data pages, as they "
     "arrived, repeats dropped, named for their data channel and address or "
     "their page, stream and application. index.tsv says how each decoded."},
};

}  // namespace

std::string teletext_page_text(const TeletextPageSnapshot& page) {
  std::string out;
  for (size_t row = 0; row < TeletextPageSnapshot::kRows; ++row) {
    std::string line;
    if (page.row_received[row]) {
      for (size_t col = 0;
           col < static_cast<size_t>(page.columns) &&
           col < static_cast<size_t>(TeletextPageSnapshot::kColumns);
           ++col) {
        const TeletextPageCell& cell = page.cells[row][col];
        if (cell.enhanced != 0) {
          line += teletext_unicode_to_utf8(cell.enhanced);
        } else if ((cell.mosaic || cell.held_mosaic) &&
                   is_mosaic_code(cell.character, page.mosaic_blast_through)) {
          line += teletext_unicode_to_utf8(
              sextant_character(mosaic_sixels(cell.character)));
        } else {
          line += teletext_g0_to_utf8(cell.character, cell.g0_set,
                                      cell.national_option_subset);
        }
      }
    }
    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
    out += line + "\n";
  }
  return out;
}

VbiExportTally teletext_export_data(const TeletextAnalysisDataset& dataset,
                                    const std::string& report,
                                    const VbiExportWriter& write) {
  VbiExportTally tally;
  std::vector<const char*> written;
  const auto part = [&](const char* name, uint64_t files_before) {
    if (tally.files > files_before) {
      written.push_back(name);
    }
  };
  uint64_t before = tally.files;
  if (!report.empty() &&
      vbi_export_put_text(write, tally, "report.txt", report)) {
    tally.summary.emplace_back("the run's report, as report.txt");
  }
  part("report.txt", before);
  before = tally.files;
  export_pages(dataset, write, tally);
  part("pages/", before);
  before = tally.files;
  export_service_data(dataset, write, tally);
  part("service-data.txt", before);
  before = tally.files;
  export_data_lines(dataset, write, tally);
  part("data-lines/", before);

  // What the folders hold, for a reader who finds the export without the
  // program that wrote it.
  vbi_export_readme(
      write, tally,
      "What a teletext decode found, each kind of data in the format it was "
      "sent or decoded to.\n\n",
      written, kExportParts, std::size(kExportParts));
  return tally;
}

}  // namespace orc
