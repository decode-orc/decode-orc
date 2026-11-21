/*
 * File:        vbi_source_teletext_recovery_test.cpp
 * Module:      orc-tests
 * Purpose:     Teletext recovered from the VBI source stage's output by the
 *              teletext plugin's slicers and page decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

// The VBI Source stage's acceptance oracle is the teletext decoder: the slicer
// and page decoder, pointed at the stage's output, must recover the broadcast.
// Those decoders now live in the teletext plugin (orc-plugin_teletext), so this
// file is built only when a checkout of it is built in
// (ORC_PLUGIN_SOURCE_DIRS). The stage's own checks, which need no decoder, stay
// in vbi_source_pipeline_test.cpp and vbi_ntsc_source_test.cpp.

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "teletext_frame_slicer.h"
#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/teletext_row_squasher.h"
#include "vbi-services/teletext_slicer.h"
#include "vbi_offset_calibration.h"
#include "vbi_source_stage.h"

namespace orc {
namespace {

// ---------------------------------------------------------------------------
// 625-line WST: the bt8x8 PAL reference capture
// ---------------------------------------------------------------------------

// The reference sample: a four-hour FLAC-wrapped bt8x8 PAL capture carrying
// world system teletext. It is not checked into the repository (test-data/ is
// ignored), so every test here skips when it is absent.
const char* kReferenceCapture =
    ORC_VBI_TEST_DATA_DIR "/teletext/bt8x8 sample/0002.vbi.flac";

// The design's success criterion is that the existing teletext decoders,
// pointed at this stage's output, recover the same packets they would from a
// native decode (design §1). The decoders are the oracle here; nothing
// downstream of the stage boundary is under test.
//
// Frames examined. A handful spread across the capture rather than a prefix
// of it: the whole file is 368 007 frames and 10 GB compressed, so a full
// decode is a manual milestone check rather than an automated one — and the
// opening minute of this recording is off-air, carrying no teletext at all,
// which is exactly why the design samples a capture across its length rather
// than from its head (design §5.3.4).
constexpr uint32_t kFramesUnderTest = 8;

// Stored frame lines a 625-line WST capture carries data on: broadcast frame
// lines 7-22 and 320-335 (design §5.1).
const std::vector<size_t> kTeletextFrameLines = [] {
  std::vector<size_t> lines;
  for (size_t line = 6; line <= 21; ++line) lines.push_back(line);
  for (size_t line = 319; line <= 334; ++line) lines.push_back(line);
  return lines;
}();

bool reference_capture_available() {
  return std::filesystem::exists(kReferenceCapture);
}

std::map<std::string, ParameterValue> reference_parameters() {
  return {
      {"input_path", std::string(kReferenceCapture)},
      {"format", std::string("bt8x8 card dump, 8-bit (WST)")},
  };
}

// One packet's magazine and row, which is what says a recovered packet is a
// real teletext packet rather than noise that spelled a framing code.
struct PacketAddress {
  int magazine = 0;
  int row = 0;
};

// ETSI EN 300 706 §7.1.2: the two Hamming 8/4 protected MRAG bytes carry the
// magazine in the low three bits of the first nibble, the least significant
// row bit in its fourth, and the remaining four row bits in the second nibble.
bool decode_address(const TeletextLineResult& result, PacketAddress& out) {
  const int first = teletext_hamming84_decode(result.bytes[0]);
  const int second = teletext_hamming84_decode(result.bytes[1]);
  if (first < 0 || second < 0) return false;
  out.magazine = first & 0x7;
  out.row = ((first >> 3) & 0x1) | ((second & 0xF) << 1);
  return true;
}

// Run the stage over the reference capture and hand back its output.
std::shared_ptr<VideoFrameRepresentation> load_reference_capture(
    VBISourceStage& stage, ObservationContext& observations) {
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, reference_parameters(), observations);
  if (outputs.empty()) return nullptr;
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
}

// The acceptance oracle: the existing WST slicer, pointed at the synthesised
// frames, recovers real teletext packets.
TEST(VBISourcePipeline, TheExistingTeletextSlicerRecoversPacketsFromTheOutput) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_reference_capture(stage, observations);
  ASSERT_NE(representation, nullptr);
  ASSERT_GT(representation->frame_count(), kFramesUnderTest);

  // EBU Tech. 3280-E §1.1.1 Table 1 sample rate; the data levels are the ones
  // the stage declares it synthesised to.
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kAuto;
  const TeletextSlicer slicer(kPalSampleRate, kTeletextBitRate, options);

  const auto parameters = representation->get_video_parameters();
  ASSERT_TRUE(parameters.has_value());
  const auto black = static_cast<int16_t>(parameters->black_level);
  const auto white = static_cast<int16_t>(parameters->white_level);

  uint64_t candidate_lines = 0;
  uint64_t recovered = 0;
  std::set<int> magazines;
  std::set<int> rows;

  for (const uint64_t frame : vbi_calibration_frame_indices(
           static_cast<uint64_t>(representation->frame_count()),
           kFramesUnderTest)) {
    for (const size_t line : kTeletextFrameLines) {
      const std::vector<int16_t> samples =
          representation->get_line_samples(static_cast<FrameID>(frame), line);
      ASSERT_FALSE(samples.empty()) << "frame " << frame << " line " << line;
      ++candidate_lines;

      const TeletextLineResult result =
          slicer.slice(samples.data(), samples.size(), black, white);
      if (!result.valid) continue;
      ++recovered;

      PacketAddress address;
      ASSERT_TRUE(decode_address(result, address))
          << "frame " << frame << " line " << line;
      magazines.insert(address.magazine);
      rows.insert(address.row);
    }
  }

  // Recorded so the log carries the measurement the assertions are drawn
  // against rather than only their verdict.
  GTEST_LOG_(INFO) << recovered << " of " << candidate_lines
                   << " candidate lines yielded a packet, across "
                   << rows.size() << " row(s) and " << magazines.size()
                   << " magazine(s)";

  // A capture of a live broadcast carries teletext on most of its lines. The
  // bar is deliberately well below what a working stage produces and well
  // above what a mis-placed or mis-scaled output could reach by accident.
  EXPECT_GT(recovered, candidate_lines / 2)
      << recovered << " of " << candidate_lines << " candidate lines sliced";

  // Real teletext addresses the rows of a page in order and carries a page
  // header (row 0) somewhere in the frames examined.
  EXPECT_GT(rows.size(), 4u);
  EXPECT_NE(rows.find(0), rows.end()) << "no page header recovered";
  EXPECT_FALSE(magazines.empty());
}

// ---------------------------------------------------------------------------
// 525-line WST: the Electra .tbc VBI crop and the Keyfax bt8x8 card dump
// ---------------------------------------------------------------------------

const char* kElectraCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/TBS Electra WST 525-60/"
    "TBS_1988-01-03_Electra_teletext_NTSC_SP_vbi_only_u16-002.flac";

const char* kKeyfaxCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/bt8x8 NTSC sample/"
    "TBS_1984-02-01_Keyfax_teletext_NTSC_WST_bt8x8.vbi";

bool electra_capture_available() {
  return std::filesystem::exists(kElectraCapture);
}

std::map<std::string, ParameterValue> electra_parameters() {
  return {
      {"input_path", std::string(kElectraCapture)},
      {"format", std::string(".tbc VBI crop, 16-bit (WST)")},
  };
}

std::shared_ptr<VideoFrameRepresentation> load_electra_capture(
    VBISourceStage& stage, ObservationContext& observations) {
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, electra_parameters(), observations);
  if (outputs.empty()) return nullptr;
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
}

bool keyfax_capture_available() {
  return std::filesystem::exists(kKeyfaxCapture);
}

std::shared_ptr<VideoFrameRepresentation> load_keyfax_capture(
    VBISourceStage& stage, ObservationContext& observations) {
  const std::map<std::string, ParameterValue> parameters = {
      {"input_path", std::string(kKeyfaxCapture)},
      {"format", std::string("bt8x8 card dump, 8-bit (WST, NTSC source)")},
  };
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, parameters, observations);
  if (outputs.empty()) return nullptr;
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
}

// The recovery chain against a real 525-line capture: frame slicer -> page
// decoder. The stage supplies the frames; what is under test is that the
// 525-line service survives the whole path the teletext sink takes it
// through.

// Frames read. A page is transmitted a row at a time and this capture carries
// up to a dozen packets a field, so a few hundred frames span several carousel
// cycles — enough for whole pages to assemble.
constexpr uint32_t kPageFrames = 300;

// Where to start reading. The head of a tape is lead-in, so this is a quarter
// of the way in, as the other tests here do.
FrameID page_survey_start(uint64_t frame_count) {
  return static_cast<FrameID>(frame_count / 4);
}

struct RecoverySurvey {
  int packets = 0;
  // Packets the slicer read at the 34-byte length the 525-line service
  // transmits, rather than the 42-byte 625-line packet.
  int packets_at_525_length = 0;
  // Odd-parity display bytes per recovered packet, as a fraction of the 32 the
  // service carries (ITU-R BT.653 Table 1b §2.4.1). An undamaged packet is
  // 1,0 throughout; noise sits at 0,5.
  std::vector<double> parity_fractions;
  std::vector<TeletextPageSnapshot> pages;
};

RecoverySurvey survey_recovery(VideoFrameRepresentation& representation) {
  RecoverySurvey survey;
  TeletextFrameSlicer frame_slicer;
  TeletextPageDecoder decoder;
  // As the analysis sink reads it: with a squasher, so repeated copies of a
  // row — and of the packets that extend it — correct each other.
  TeletextRowSquasher squasher;
  decoder.set_row_squasher(&squasher);
  decoder.set_page_callback([&survey](const TeletextPageSnapshot& page) {
    survey.pages.push_back(page);
  });

  TeletextFieldScan scan;
  const FrameID first = page_survey_start(representation.frame_count());
  for (uint32_t index = 0; index < kPageFrames; ++index) {
    const FrameID frame = first + index;
    if (!representation.has_frame(frame)) break;

    for (size_t field_index = 0; field_index < 2; ++field_index) {
      const FieldID field(frame * 2 + field_index);
      // slice_field() reports in ascending field-line order, which is the
      // order a real consumer feeds and the order the packets were
      // transmitted in.
      frame_slicer.slice_field(representation, frame, field_index, scan);
      for (const auto& line : scan.lines) {
        if (!line.sliced.valid) continue;

        ++survey.packets;
        if (line.sliced.packet_bytes == kTeletext525PacketBytes) {
          ++survey.packets_at_525_length;
        }

        int odd = 0;
        for (size_t i = 2; i < line.sliced.packet_bytes; ++i) {
          odd += teletext_odd_parity_valid(line.sliced.bytes[i]) ? 1 : 0;
        }
        survey.parity_fractions.push_back(
            static_cast<double>(odd) /
            static_cast<double>(line.sliced.packet_bytes - 2));

        // A stable source id per recovered line, which is what a consumer
        // re-reading the same line has to supply for the squasher to replace
        // rather than recount it.
        decoder.process_packet(
            line.sliced.bytes, static_cast<int64_t>(field.value()),
            static_cast<int64_t>(field.value()) * 32 + line.field_line,
            line.sliced.has_byte_confidence ? &line.sliced.byte_confidence
                                            : nullptr,
            line.sliced.packet_bytes);
      }
    }
  }
  decoder.finalize(static_cast<int64_t>((first + kPageFrames) * 2));
  return survey;
}

// Printable display text of a page, one string per received row.
std::vector<std::string> page_rows(const TeletextPageSnapshot& page) {
  std::vector<std::string> rows;
  for (int row = 0; row < TeletextPageSnapshot::kRows; ++row) {
    if (!page.row_received[static_cast<size_t>(row)]) continue;
    std::string text;
    for (int column = 0; column < page.columns; ++column) {
      const auto& cell =
          page.cells[static_cast<size_t>(row)][static_cast<size_t>(column)];
      // Mosaic cells are graphics, not text, and are not what is being read.
      const bool printable = !cell.mosaic && !cell.held_mosaic &&
                             cell.character >= 0x20 && cell.character < 0x7F;
      text.push_back(printable ? static_cast<char>(cell.character) : ' ');
    }
    rows.push_back(text);
  }
  return rows;
}

// Short alphabetic words on a page. This is what separates a recovered
// broadcast from a page assembled out of false locks: noise decoded as
// characters gives long runs of one letter and very few word-shaped tokens,
// while real text gives many.
size_t word_count(const TeletextPageSnapshot& page) {
  size_t words = 0;
  for (const auto& row : page_rows(page)) {
    size_t run = 0;
    for (size_t i = 0; i <= row.size(); ++i) {
      const bool alpha = i < row.size() &&
                         std::isalpha(static_cast<unsigned char>(row[i])) != 0;
      if (alpha) {
        ++run;
      } else {
        if (run >= 2 && run <= 12) ++words;
        run = 0;
      }
    }
  }
  return words;
}

TEST(VBINTSCSource, TheObserverRecoversPacketsAtTheServicesOwnLength) {
  if (!electra_capture_available()) {
    GTEST_SKIP() << "525-line capture not present: " << kElectraCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_electra_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RecoverySurvey survey = survey_recovery(*representation);
  ASSERT_GT(survey.packets, 0)
      << "the observer recovered nothing from a capture the stage placed "
         "teletext on";

  // Every packet must be the 34-byte one: a 42-byte string here would mean the
  // observer had read this capture as the 625-line service.
  EXPECT_EQ(survey.packets_at_525_length, survey.packets);

  // The bits have to be right, not merely present. Odd parity is the standard's
  // own check on the display bytes and it is content-independent: noise sits at
  // 0,5, so a median this high says the packets carry the broadcast.
  auto fractions = survey.parity_fractions;
  std::sort(fractions.begin(), fractions.end());
  const double median = fractions[fractions.size() / 2];
  std::cout << "Electra: " << survey.packets << " packets, median parity "
            << median << ", " << survey.pages.size() << " pages\n";
  EXPECT_GT(median, 0.9);
}

// The same recovery chain against the bt8x8 card dump, which is where the
// whole of this preset's work shows up: the records were resampled from the
// card's 8 x fsc clock onto the output lattice and placed at a fitted offset,
// and if any part of that were wrong the slicer would read noise.
TEST(VBINTSCSource, TheObserverRecoversPacketsFromABt8x8CardDump) {
  if (!keyfax_capture_available()) {
    GTEST_SKIP() << "525-line bt8x8 capture not present: " << kKeyfaxCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_keyfax_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RecoverySurvey survey = survey_recovery(*representation);
  ASSERT_GT(survey.packets, 0)
      << "the observer recovered nothing from a capture the stage placed "
         "teletext on";

  // 34-byte packets throughout: a 42-byte string would mean the 625-line
  // service had been read off a 525-line capture.
  EXPECT_EQ(survey.packets_at_525_length, survey.packets);

  auto fractions = survey.parity_fractions;
  std::sort(fractions.begin(), fractions.end());
  const double median = fractions[fractions.size() / 2];
  std::cout << "Keyfax (bt8x8): " << survey.packets
            << " packets, median parity " << median << ", "
            << survey.pages.size() << " pages\n";
  EXPECT_GT(median, 0.9);
}

TEST(VBINTSCSource, ThePageDecoderAssemblesReadablePagesFromABt8x8CardDump) {
  if (!keyfax_capture_available()) {
    GTEST_SKIP() << "525-line bt8x8 capture not present: " << kKeyfaxCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_keyfax_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RecoverySurvey survey = survey_recovery(*representation);
  ASSERT_FALSE(survey.pages.empty())
      << "no page completed over " << kPageFrames << " frames";

  const TeletextPageSnapshot* best = nullptr;
  for (const auto& page : survey.pages) {
    ASSERT_EQ(page.columns, TeletextPageSnapshot::kColumns)
        << "page assembled at the wrong width";
    if (best == nullptr || word_count(page) > word_count(*best)) best = &page;
  }
  ASSERT_NE(best, nullptr);

  std::cout << "Keyfax (bt8x8): best page is " << best->magazine << std::hex
            << best->page_number << std::dec << " with " << word_count(*best)
            << " word-shaped tokens over " << survey.pages.size() << " pages\n";

  // Noise decoded as characters gives long runs of one letter and very few
  // word-shaped tokens; a recovered broadcast page gives many.
  EXPECT_GT(word_count(*best), 20u);
}

TEST(VBINTSCSource, ThePageDecoderAssemblesReadable40ColumnPages) {
  if (!electra_capture_available()) {
    GTEST_SKIP() << "525-line capture not present: " << kElectraCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_electra_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  const RecoverySurvey survey = survey_recovery(*representation);
  ASSERT_FALSE(survey.pages.empty())
      << "no page completed over " << kPageFrames << " frames";

  // 40 columns from 34-byte packets: this service sends the last 8 columns of
  // each row in row-extension packets (teletext_page_decoder.h). A page still
  // 32 wide here would mean they had stopped being recognised.
  for (const auto& page : survey.pages) {
    ASSERT_EQ(page.columns, TeletextPageSnapshot::kColumns)
        << "page assembled at the wrong width";
  }

  // Page 100 — magazine 1, page 00 — is the service's front page, so a decode
  // that addresses packets correctly finds it. Recovering it at all exercises
  // the Hamming 8/4 MRAG and page-number path on real bytes.
  const TeletextPageSnapshot* front_page = nullptr;
  for (const auto& page : survey.pages) {
    if (page.magazine != 1 || page.page_number != 0x00) continue;
    if (front_page == nullptr || word_count(page) > word_count(*front_page)) {
      front_page = &page;
    }
  }
  ASSERT_NE(front_page, nullptr) << "page 100 never completed";

  for (const auto& row : page_rows(*front_page)) {
    std::cout << "  |" << row << "|\n";
  }

  // Real text, not a page assembled out of false locks. The front page of this
  // capture carries its index (\"News index ....101\" and its neighbours) plus
  // a headline block, which is comfortably above this; a noise page scores a
  // handful.
  EXPECT_GT(word_count(*front_page), 20u);

  // The extension columns carry the page, not a blank margin: this service
  // fills its lines to the right-hand edge, so several rows must have content
  // past the 32 columns one packet brings.
  size_t rows_using_extension_columns = 0;
  for (const auto& row : page_rows(*front_page)) {
    const auto last = row.find_last_not_of(' ');
    if (last != std::string::npos &&
        static_cast<int>(last) >=
            static_cast<int>(kTeletext525PacketBytes) - 2) {
      ++rows_using_extension_columns;
    }
  }
  EXPECT_GT(rows_using_extension_columns, 3u)
      << "no row reached past column 32 — the row-extension packets are not "
         "reaching the page";

  // And they land on the right rows. A misattributed extension packet still
  // passes odd parity — it is eight valid characters in the wrong place — so
  // only content says the block mapping is right. This page's copyright line
  // spans columns 27 to 38, crossing the boundary between what the display
  // packet brought and what the extension packet did.
  bool joins_across_the_boundary = false;
  for (const auto& row : page_rows(*front_page)) {
    if (row.find("Broadcasting") != std::string::npos) {
      joins_across_the_boundary = true;
    }
  }
  EXPECT_TRUE(joins_across_the_boundary)
      << "the copyright line did not read across column 32 — the extension "
         "packets are being attributed to the wrong rows";

  // And they arrive as cleanly as the columns the display packets bring. The
  // extension columns are squashed over their own repeats like any others, so
  // damage in them is evidence of a mis-read packet rather than of the tape:
  // the extension packets each serve four rows, and one attributed to the wrong
  // block puts eight characters into four wrong places at once.
  //
  // Only the magazines that carry the service's content. This capture also
  // carries test pages — magazine 8's rolling character ramp and magazine 4's
  // "CLOCK CRACKER", whose display bytes are chosen to stress a receiver's
  // clock recovery rather than to satisfy odd parity — and neither has an
  // extension carrier, so they would only dilute both figures.
  const int kHeadColumns = static_cast<int>(kTeletext525PacketBytes) - 2;
  size_t head_cells = 0;
  size_t damaged_head_cells = 0;
  size_t extension_cells = 0;
  size_t damaged_extension_cells = 0;
  for (const auto& page : survey.pages) {
    if (page.magazine != 1 && page.magazine != 2) continue;
    for (int row = 1; row < TeletextPageSnapshot::kRows; ++row) {
      if (!page.row_received[static_cast<size_t>(row)]) continue;
      for (int column = 0; column < page.columns; ++column) {
        const bool damaged =
            page.cells[static_cast<size_t>(row)][static_cast<size_t>(column)]
                .parity_error;
        size_t& cells = column < kHeadColumns ? head_cells : extension_cells;
        size_t& damaged_cells = column < kHeadColumns ? damaged_head_cells
                                                      : damaged_extension_cells;
        ++cells;
        damaged_cells += damaged ? 1 : 0;
      }
    }
  }
  // And they reach every page, not just the one being looked at. A single
  // mis-corrected address in the carrier magazine used to disable extensions
  // for every page that followed it, which shows up here and nowhere else: a
  // page that lost them still renders, just 32 columns wide.
  size_t pages_with_extension_content = 0;
  size_t pages_with_a_carrier = 0;
  for (const auto& page : survey.pages) {
    if (page.magazine != 1 && page.magazine != 2) continue;
    ++pages_with_a_carrier;
    bool any = false;
    for (int row = 0; row < TeletextPageSnapshot::kRows && !any; ++row) {
      for (int column = kHeadColumns; column < page.columns; ++column) {
        if (page.cells[static_cast<size_t>(row)][static_cast<size_t>(column)]
                .character != 0x20) {
          any = true;
          break;
        }
      }
    }
    pages_with_extension_content += any ? 1 : 0;
  }
  std::cout << "Electra: " << pages_with_extension_content << " of "
            << pages_with_a_carrier
            << " pages in a magazine with a carrier hold extension content\n";
  ASSERT_GT(pages_with_a_carrier, 20u);
  EXPECT_GT(pages_with_extension_content, pages_with_a_carrier * 3 / 4);

  ASSERT_GT(extension_cells, 1000u);
  const double head_damage =
      static_cast<double>(damaged_head_cells) / static_cast<double>(head_cells);
  const double extension_damage = static_cast<double>(damaged_extension_cells) /
                                  static_cast<double>(extension_cells);
  std::cout << "Electra: " << damaged_head_cells << "/" << head_cells
            << " display cells damaged (" << 100.0 * head_damage << "%), "
            << damaged_extension_cells << "/" << extension_cells
            << " extension cells (" << 100.0 * extension_damage << "%)\n";

  // The bar is what an unsquashed extension column reaches on this capture,
  // which is several times this; a regression that stopped the extension
  // packets voting — or attributed them to the wrong block, which is the same
  // thing seen from the other end — lands well above it.
  EXPECT_LT(extension_damage, 0.02);
}

}  // namespace
}  // namespace orc
