/*
 * File:        teletext_sink_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the teletext sink stage dependencies
 *
 * Covers: service gating, packet bytes and temporal ordering on both the
 * 625-line (.t42) and 525-line (.t34) services, keep-empty padding, row
 * squashing, the page catalogue and recovery summary, subtitle export, cancel,
 * progress throttling, the report, and the writer failure paths. Every line of
 * video is synthesised in memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_sink_deps.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../common/vbi-services/teletext_line_synthesizer.h"
#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../stage_services_mock.h"
#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/teletext_slicer.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::StrictMock;

// using different namespace from module-under-test so that we can use the same
// class names in the tests as in the module-under-test
namespace orc_unit_test {

namespace {

orc::SourceParameters make_pal_params() {
  orc::SourceParameters p{};
  p.system = orc::VideoSystem::PAL;
  p.frame_width_nominal = orc::kPalSamplesPerLineNominal;
  p.frame_height = orc::kPalFrameLines;
  p.black_level = orc::kPalBlack;
  p.white_level = orc::kPalWhite;
  return p;
}

orc::SourceParameters make_ntsc_params() {
  orc::SourceParameters p{};
  p.system = orc::VideoSystem::NTSC;
  p.frame_width_nominal = orc::kNtscSamplesPerLine;
  p.frame_height = orc::kNtscFrameLines;
  p.black_level = orc::kNtscBlack;
  p.white_level = orc::kNtscWhite;
  return p;
}

orc::SourceParameters make_unknown_params() {
  orc::SourceParameters p{};
  p.system = orc::VideoSystem::Unknown;
  return p;
}

// A distinguishable MRAG-valid, parity-coded payload per variant. Parity
// coding matters because the MLSE fallback of the automatic detector applies a
// parity plausibility gate to rows 0-25.
std::array<uint8_t, orc::kTeletextPacketBytes> make_payload(uint8_t variant) {
  auto payload = orc::tests::make_parity_coded_payload();
  payload[2] = orc::teletext_odd_parity_encode(variant);
  return payload;
}

// X/0 page header for transmission magazine 0 (displayed magazine 8,
// page 8<page_number:2x>), 32 space header characters.
std::array<uint8_t, orc::kTeletextPacketBytes> make_header_packet(
    int page_number, bool subtitle, bool erase) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  const auto mrag = orc::tests::make_mrag(0, 0);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  packet[2] =
      orc::teletext_hamming84_encode(static_cast<uint8_t>(page_number & 0xF));
  packet[3] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>((page_number >> 4) & 0xF));
  packet[4] = orc::teletext_hamming84_encode(0);
  packet[5] = orc::teletext_hamming84_encode(erase ? 0x8 : 0x0);  // C4
  packet[6] = orc::teletext_hamming84_encode(0);
  packet[7] = orc::teletext_hamming84_encode(subtitle ? 0x8 : 0x0);  // C6
  packet[8] = orc::teletext_hamming84_encode(0);
  packet[9] = orc::teletext_hamming84_encode(0);
  for (size_t i = 0; i < 32; ++i) {
    packet[10 + i] = orc::teletext_odd_parity_encode(' ');
  }
  return packet;
}

// Displayable row for transmission magazine 0 with boxed subtitle text
// (Start Box ×2 ... End Box, the C5/C6 convention).
std::array<uint8_t, orc::kTeletextPacketBytes> make_subtitle_row_packet(
    int row, const std::string& text) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  const auto mrag = orc::tests::make_mrag(0, row);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  std::string bytes;
  bytes.push_back(0x0B);
  bytes.push_back(0x0B);
  bytes += text;
  bytes.push_back(0x0A);
  for (size_t i = 0; i < 40; ++i) {
    const char c = i < bytes.size() ? bytes[i] : ' ';
    packet[2 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

// Displayable row for transmission magazine 0 carrying plain text.
std::array<uint8_t, orc::kTeletextPacketBytes> make_row_packet(
    int row, const std::string& text) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  const auto mrag = orc::tests::make_mrag(0, row);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  for (size_t i = 0; i < 40; ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    packet[2 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

}  // namespace

class TeletextSinkDeps : public ::testing::Test {
 public:
  void SetUp() override {
    pMockFileWriterUint8_ = std::make_shared<StrictMock<MockFileWriterUint8>>();
    cancelRequested_.store(false);
  }

 protected:
  // Wire the writer factory and capture every byte written, in order.
  void expect_writer(const std::string& expected_path) {
    EXPECT_CALL(mockStageServices_,
                create_buffered_file_writer_uint8(1UL * 1024 * 1024))
        .Times(1)
        .WillOnce(Return(pMockFileWriterUint8_));
    EXPECT_CALL(*pMockFileWriterUint8_, open(expected_path))
        .Times(1)
        .WillOnce(Return(true));
    EXPECT_CALL(*pMockFileWriterUint8_, write(_, _))
        .WillRepeatedly(Invoke([this](const uint8_t* data, size_t count) {
          written_.insert(written_.end(), data, data + count);
        }));
    EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);
  }

  // Serve the synthesised lines the test placed with put_line(); every other
  // line of the frame reads as absent, which is what an empty VBI line looks
  // like to the frame slicer.
  void serve_lines(const orc::SourceParameters& params) {
    EXPECT_CALL(mockRepresentation_, get_video_parameters())
        .WillRepeatedly(Return(params));
    EXPECT_CALL(mockRepresentation_, get_line(_, _))
        .WillRepeatedly(
            Invoke([this](orc::FrameID frame, size_t line) -> const int16_t* {
              const auto it = lines_.find({frame, line});
              return it == lines_.end() ? nullptr : it->second.data();
            }));
  }

  // Place a synthesised line at a frame-flat line index.
  void put_line(orc::FrameID frame, size_t flat_line,
                std::vector<int16_t> samples) {
    lines_[{frame, flat_line}] = std::move(samples);
  }

  // Frame-flat line of a 0-based field line, for the given system.
  static size_t flat_line(const orc::SourceParameters& params, size_t field_idx,
                          int32_t field_line) {
    return (field_idx == 0 ? 0u : orc::field1_lines(params.system)) +
           static_cast<size_t>(field_line);
  }

  orc::TeletextSinkDeps make_deps() {
    orc::TeletextSinkDeps deps(&mockStageServices_);
    deps.init({}, &cancelRequested_);
    return deps;
  }

  // Default options: one candidate line per field, so a test places exactly
  // the lines it means to be read.
  static orc::TeletextSinkOptions single_line_options(int32_t field_line) {
    orc::TeletextSinkOptions options;
    options.output_path = "out";
    options.first_field_line = field_line;
    options.last_field_line = field_line;
    options.detector = orc::TeletextDetector::kThreshold;
    return options;
  }

  MockStageServices mockStageServices_;
  std::shared_ptr<StrictMock<MockFileWriterUint8>> pMockFileWriterUint8_;
  NiceMock<MockVideoFrameRepresentationArtifact> mockRepresentation_;
  std::atomic<bool> cancelRequested_{};
  std::vector<uint8_t> written_;
  std::map<std::pair<orc::FrameID, size_t>, std::vector<int16_t>> lines_;
};

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkDeps, Analyse_FailsWhenSystemCarriesNoWstService) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_unknown_params()));

  auto deps = make_deps();
  auto options = single_line_options(7);

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("no World System Teletext service"),
            std::string::npos)
      << result.message;
}

TEST_F(TeletextSinkDeps, Analyse_FailsWhenInputHasNoFrames) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  // Empty range: last < first → count() == 0
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{1, 0}));

  auto deps = make_deps();
  auto options = single_line_options(7);

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Input has no frames");
}

TEST_F(TeletextSinkDeps, Analyse_FailsWhenWriterServiceUnavailable) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));

  orc::TeletextSinkDeps deps(nullptr);
  deps.init({}, &cancelRequested_);
  auto options = single_line_options(7);

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Failed to create output writer service");
}

TEST_F(TeletextSinkDeps, Analyse_FailsWhenOpenFails) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out.t42"))
      .Times(1)
      .WillOnce(Return(false));

  auto deps = make_deps();
  auto options = single_line_options(7);  // extension appended before open()

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Failed to open output file: out.t42");
  EXPECT_EQ(result.output_path, "out.t42");
}

////////////////////////////////////////////////////////////////////////////////////////////
// The "-" stdio convention
////////////////////////////////////////////////////////////////////////////////////////////

// "-" is passed straight through to the writer with no extension appended,
// unlike a real path.
TEST_F(TeletextSinkDeps, Analyse_PipesToStdoutWithPathUnchanged) {
  const auto params = make_pal_params();
  const auto payload = make_payload(0x11);
  put_line(0, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(payload));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  expect_writer("-");

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.output_path = "-";
  options.squash_repeated_rows = false;

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.output_path, "-");
  EXPECT_EQ(written_, std::vector<uint8_t>(payload.begin(), payload.end()));
}

// A pipe can carry only the primary packet stream; the report and subtitle
// exports are separate files named after output_path, which "-" does not
// identify a location for. Refused outright rather than silently dropped.
TEST_F(TeletextSinkDeps, Analyse_RefusesPipingTogetherWithReportOrSubtitles) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(0);

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.output_path = "-";
  options.write_report = true;

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("Cannot pipe the teletext stream to stdout"),
            std::string::npos)
      << result.message;
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkDeps, Analyse_WritesPacketsInTemporalOrder) {
  const auto params = make_pal_params();
  const auto payload_a = make_payload(0x11);  // frame 0, field 1, line 7
  const auto payload_b = make_payload(0x23);  // frame 0, field 2, line 8
  const auto payload_c = make_payload(0x35);  // frame 1, field 1, line 8

  put_line(0, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(payload_a));
  put_line(0, flat_line(params, 1, 8),
           orc::tests::synthesize_teletext_line(payload_b));
  put_line(1, flat_line(params, 0, 8),
           orc::tests::synthesize_teletext_line(payload_c));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 1}));
  expect_writer("out.t42");

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.last_field_line = 8;
  options.squash_repeated_rows = false;

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.packets_written, 3u);
  EXPECT_EQ(result.fields_with_data, 3u);

  // frame → field → ascending line, and whole packets only.
  std::vector<uint8_t> expected;
  for (const auto* payload : {&payload_a, &payload_b, &payload_c}) {
    expected.insert(expected.end(), payload->begin(), payload->end());
  }
  EXPECT_EQ(written_, expected);
}

// The 525-line service transmits 34-byte packets, so the stream is named .t34
// and carries exactly those bytes — the eight the 625-line packet buffer holds
// past them were never transmitted and must not reach the file.
TEST_F(TeletextSinkDeps, Analyse_Writes525LinePacketsAsT34) {
  const auto params = make_ntsc_params();
  const auto payload = orc::tests::make_525_test_payload();
  const auto synth = orc::tests::ntsc_wst_synth_options();

  put_line(0, flat_line(params, 0, 10),
           orc::tests::synthesize_teletext_line(payload, synth,
                                                orc::kTeletext525PacketBytes));
  put_line(0, flat_line(params, 1, 10),
           orc::tests::synthesize_teletext_line(payload, synth,
                                                orc::kTeletext525PacketBytes));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  expect_writer("out.t34");

  auto deps = make_deps();
  auto options = single_line_options(10);
  options.squash_repeated_rows = false;

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.output_path, "out.t34");
  EXPECT_EQ(result.packets_written, 2u);
  ASSERT_EQ(written_.size(), 2 * orc::kTeletext525PacketBytes);
  for (size_t copy = 0; copy < 2; ++copy) {
    for (size_t i = 0; i < orc::kTeletext525PacketBytes; ++i) {
      EXPECT_EQ(written_[copy * orc::kTeletext525PacketBytes + i], payload[i])
          << "copy " << copy << " byte " << i;
    }
  }
}

// The cue timing assumes 50 fields per second, which a 525-line service is
// not; refusing beats writing cues that drift by a fifth.
TEST_F(TeletextSinkDeps, Analyse_RefusesSubtitleExportOn525Lines) {
  const auto params = make_ntsc_params();
  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out.t34"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  auto deps = make_deps();
  auto options = single_line_options(10);
  options.export_subtitles = true;

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("625-line only"), std::string::npos)
      << result.message;
}

TEST_F(TeletextSinkDeps, Analyse_KeepEmptyPacketsPadsAllCandidateLines) {
  const auto params = make_pal_params();
  const auto payload = make_payload(0x11);
  put_line(0, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(payload));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  expect_writer("out.t42");

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.last_field_line = 8;
  options.keep_empty_packets = true;
  options.squash_repeated_rows = false;

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  // Two candidate lines in each of two fields, one of which carried data.
  EXPECT_EQ(result.packets_written, 4u);
  ASSERT_EQ(written_.size(), 4 * orc::kTeletextPacketBytes);
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), written_.begin()));
  for (size_t i = orc::kTeletextPacketBytes; i < written_.size(); ++i) {
    EXPECT_EQ(written_[i], 0) << "byte " << i;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkDeps, Analyse_SquashingRepairsADamagedRowByte) {
  const auto params = make_pal_params();
  const auto header = make_header_packet(0x00, /*subtitle=*/false,
                                         /*erase=*/false);
  const auto good_row = make_row_packet(1, "HELLO");
  auto damaged_row = good_row;
  damaged_row[2] = 0x00;  // fails odd parity, so the vote must reject it

  // Three transmissions of the same page; the middle one is damaged, so the
  // parity-first vote takes the byte the other two agree on.
  for (orc::FrameID frame = 0; frame <= 2; ++frame) {
    put_line(frame, flat_line(params, 0, 7),
             orc::tests::synthesize_teletext_line(header));
    put_line(frame, flat_line(params, 1, 7),
             orc::tests::synthesize_teletext_line(frame == 1 ? damaged_row
                                                             : good_row));
  }

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 2}));
  expect_writer("out.t42");

  auto deps = make_deps();
  auto options = single_line_options(7);

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.packets_written, 6u);
  EXPECT_GE(result.packets_corrected, 1u);

  // The damaged copy is the second row packet written: header, row, header,
  // row, header, row.
  ASSERT_EQ(written_.size(), 6 * orc::kTeletextPacketBytes);
  EXPECT_EQ(written_[3 * orc::kTeletextPacketBytes + 2], good_row[2]);
}

// A header with C4 set says the page's content is being replaced, so copies
// either side of it are copies of different pages and must not be combined.
TEST_F(TeletextSinkDeps, Analyse_SquashingSurvivesErasePageHeaders) {
  const auto params = make_pal_params();
  const auto header = make_header_packet(0x00, /*subtitle=*/false,
                                         /*erase=*/true);
  const auto row_a = make_row_packet(1, "FIRST");
  const auto row_b = make_row_packet(1, "SECOND");

  put_line(0, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(header));
  put_line(0, flat_line(params, 1, 7),
           orc::tests::synthesize_teletext_line(row_a));
  put_line(1, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(header));
  put_line(1, flat_line(params, 1, 7),
           orc::tests::synthesize_teletext_line(row_b));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 1}));
  expect_writer("out.t42");

  auto deps = make_deps();
  auto options = single_line_options(7);

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  ASSERT_EQ(written_.size(), 4 * orc::kTeletextPacketBytes);
  // Neither run's row is blended into the other's.
  EXPECT_TRUE(std::equal(row_a.begin(), row_a.end(),
                         written_.begin() + orc::kTeletextPacketBytes));
  EXPECT_TRUE(std::equal(row_b.begin(), row_b.end(),
                         written_.begin() + 3 * orc::kTeletextPacketBytes));
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkDeps, Analyse_CataloguesEveryPageTheRangeCarried) {
  const auto params = make_pal_params();
  const auto header_100 = make_header_packet(0x00, /*subtitle=*/false,
                                             /*erase=*/false);
  const auto header_188 = make_header_packet(0x88, /*subtitle=*/true,
                                             /*erase=*/false);
  const auto row = make_row_packet(1, "HELLO");

  // Two appearances of page 800 with page 888 between them, as a carousel
  // sends them. Back-to-back headers for the same page would be one rolling
  // transmission (ETSI EN 300 706 §9.3.1.4), not two appearances.
  put_line(0, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(header_100));
  put_line(0, flat_line(params, 1, 7),
           orc::tests::synthesize_teletext_line(row));
  put_line(1, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(header_188));
  put_line(1, flat_line(params, 1, 7),
           orc::tests::synthesize_teletext_line(row));
  put_line(2, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(header_100));
  put_line(2, flat_line(params, 1, 7),
           orc::tests::synthesize_teletext_line(row));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 2}));
  expect_writer("out.t42");

  auto deps = make_deps();
  auto options = single_line_options(7);

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  ASSERT_EQ(result.dataset.pages.size(), 2u);

  const auto& page_800 = result.dataset.pages[0];
  EXPECT_EQ(page_800.magazine, 8);
  EXPECT_EQ(page_800.page_number, 0x00);
  EXPECT_EQ(page_800.times_seen, 2u);
  EXPECT_EQ(page_800.first_seen_frame, 0u);
  EXPECT_EQ(page_800.last_seen_frame, 2u);
  EXPECT_FALSE(page_800.subtitle);
  ASSERT_EQ(page_800.subpages.size(), 1u);
  EXPECT_TRUE(page_800.subpages[0].page.row_received[1]);

  const auto& page_888 = result.dataset.pages[1];
  EXPECT_EQ(page_888.page_number, 0x88);
  EXPECT_EQ(page_888.times_seen, 1u);
  EXPECT_TRUE(page_888.subtitle);

  EXPECT_EQ(result.dataset.summary.frames_analysed, 3u);
  EXPECT_EQ(result.dataset.summary.packets_recovered, result.packets_written);
  EXPECT_EQ(result.dataset.summary.fields_with_data, result.fields_with_data);
  EXPECT_FALSE(result.dataset.summary.pages_truncated);
}

// No output path is the browse-only run: the pass still recovers and
// catalogues, and no writer is asked for at all.
TEST_F(TeletextSinkDeps, Analyse_CataloguesWithoutWritingWhenNoPath) {
  const auto params = make_pal_params();
  const auto header = make_header_packet(0x00, /*subtitle=*/false,
                                         /*erase=*/false);
  const auto row = make_row_packet(1, "HELLO");

  put_line(0, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(header));
  put_line(0, flat_line(params, 1, 7),
           orc::tests::synthesize_teletext_line(row));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(0);

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.output_path.clear();

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_TRUE(result.output_path.empty());
  EXPECT_TRUE(written_.empty());
  EXPECT_EQ(result.packets_written, 2u);
  ASSERT_EQ(result.dataset.pages.size(), 1u);
  EXPECT_EQ(result.dataset.pages[0].page_number, 0x00);
  ASSERT_EQ(result.dataset.pages[0].subpages.size(), 1u);
  EXPECT_TRUE(result.dataset.pages[0].subpages[0].page.row_received[1]);
  EXPECT_NE(result.message.find("no packet stream written"), std::string::npos)
      << result.message;
  EXPECT_NE(result.report.find("(none; pages browsed only)"), std::string::npos)
      << result.report;
}

// The character set has to survive the whole way from the option to the page
// the viewer draws. Every link in that chain was tested apart from this one —
// the pass itself — and a set that stops here is invisible: the packets are
// right, the page is right, and only the alphabet it is read in is wrong.
TEST_F(TeletextSinkDeps,
       Analyse_CataloguedPagesCarryTheConfiguredCharacterSet) {
  const auto params = make_pal_params();
  const auto header = make_header_packet(0x00, /*subtitle=*/false,
                                         /*erase=*/false);
  const auto row = make_row_packet(1, "Wtornik");

  put_line(0, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(header));
  put_line(0, flat_line(params, 1, 7),
           orc::tests::synthesize_teletext_line(row));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.output_path.clear();
  options.character_set = orc::TeletextG0Set::Cyrillic2;

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  ASSERT_EQ(result.dataset.pages.size(), 1u);
  ASSERT_EQ(result.dataset.pages[0].subpages.size(), 1u);
  const orc::TeletextPageSnapshot& page =
      result.dataset.pages[0].subpages[0].page;
  EXPECT_EQ(page.g0_set, orc::TeletextG0Set::Cyrillic2);

  // And it reads as Cyrillic, which is the only thing a user can see.
  std::string text;
  for (int column = 0; column < 7; ++column) {
    text += orc::teletext_g0_to_utf8(
        page.cells[1][column].character, page.cells[1][column].g0_set,
        page.cells[1][column].national_option_subset);
  }
  EXPECT_EQ(text, "Вторник");
}

// The same chain for the second set, which has one extra link in it: the pass
// has to hand the pairing to the decoder as well as the alphabet, and a row's
// ESC codes only do anything once it has.
TEST_F(TeletextSinkDeps,
       Analyse_CataloguedPagesFollowTheConfiguredSecondCharacterSet) {
  const auto params = make_pal_params();
  const auto header = make_header_packet(0x00, /*subtitle=*/false,
                                         /*erase=*/false);
  // ESC (§12.2 Table 26 code 1/B) switches "BBC" into the second set and the
  // second switches back, so the row mixes both alphabets.
  const auto row =
      make_row_packet(1, std::string("W\x1B") + "BBC\x1B" + "tornik");

  put_line(0, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(header));
  put_line(0, flat_line(params, 1, 7),
           orc::tests::synthesize_teletext_line(row));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.output_path.clear();
  options.character_set = orc::TeletextG0Set::Cyrillic2;
  options.second_character_set =
      orc::TeletextG0Designation{orc::TeletextG0Set::Latin, 0};

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  ASSERT_EQ(result.dataset.pages.size(), 1u);
  ASSERT_EQ(result.dataset.pages[0].subpages.size(), 1u);
  const orc::TeletextPageSnapshot& page =
      result.dataset.pages[0].subpages[0].page;
  ASSERT_TRUE(page.second_g0_set.has_value());

  std::string text;
  for (int column = 0; column < 12; ++column) {
    text += orc::teletext_g0_to_utf8(
        page.cells[1][column].character, page.cells[1][column].g0_set,
        page.cells[1][column].national_option_subset);
  }
  // Read in one set throughout this would have been "В ББЦ торник".
  EXPECT_EQ(text, "В BBC торник");
  EXPECT_NE(result.report.find("switching to Latin"), std::string::npos)
      << result.report;
}

// The cues are named after the packet stream and written beside it, so a
// browse-only run has nowhere to put them: refused, not silently dropped.
TEST_F(TeletextSinkDeps, Analyse_RefusesSubtitleExportWithoutPath) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(0);

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.output_path.clear();
  options.export_subtitles = true;

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("Subtitle export needs an output file"),
            std::string::npos)
      << result.message;
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkDeps, Analyse_CancelAbortsWithTruthfulStatus) {
  const auto params = make_pal_params();
  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 9}));

  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out.t42"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  cancelRequested_.store(true);

  auto deps = make_deps();
  auto options = single_line_options(7);

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message,
            "Cancelled after 0 of 10 frames; partial output left at out.t42");
  // A cancelled run still says how it was going.
  EXPECT_FALSE(result.report.empty());
}

TEST_F(TeletextSinkDeps, Analyse_ThrottlesProgressReporting) {
  const auto params = make_pal_params();
  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 24}));
  expect_writer("out.t42");

  std::vector<uint64_t> progress_at;
  orc::TeletextSinkDeps deps(&mockStageServices_);
  deps.init(
      [&progress_at](uint64_t current, uint64_t, const std::string&) {
        progress_at.push_back(current);
      },
      &cancelRequested_);

  auto options = single_line_options(7);
  options.squash_repeated_rows = false;

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  // Every tenth frame plus the last: 10, 20, 25.
  EXPECT_EQ(progress_at, (std::vector<uint64_t>{10, 20, 25}));
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkDeps, Analyse_WritesSubtitleSrtWhenEnabled) {
  const auto params = make_pal_params();
  const auto header_on = make_header_packet(0x88, /*subtitle=*/true,
                                            /*erase=*/false);
  const auto header_off = make_header_packet(0x88, /*subtitle=*/false,
                                             /*erase=*/true);
  const auto row = make_subtitle_row_packet(20, "HELLO");

  // A subtitle transmission, then a clear.
  put_line(0, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(header_on));
  put_line(0, flat_line(params, 1, 7),
           orc::tests::synthesize_teletext_line(row));
  put_line(4, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(header_off));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 5}));

  auto subtitle_writer = std::make_shared<StrictMock<MockFileWriterUint8>>();
  std::string srt;
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(2)
      .WillOnce(Return(pMockFileWriterUint8_))
      .WillOnce(Return(subtitle_writer));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out.t42"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_, write(_, _)).WillRepeatedly(Return());
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);
  EXPECT_CALL(*subtitle_writer, open("out.srt"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*subtitle_writer, write(_, _))
      .WillRepeatedly(Invoke([&srt](const uint8_t* data, size_t count) {
        srt.append(reinterpret_cast<const char*>(data), count);
      }));
  EXPECT_CALL(*subtitle_writer, close()).Times(1);

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.export_subtitles = true;
  options.subtitle_page = "888";

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.subtitle_path, "out.srt");
  EXPECT_EQ(result.subtitle_cues_written, 1u);
  EXPECT_NE(srt.find("HELLO"), std::string::npos) << srt;
  EXPECT_NE(srt.find(" --> "), std::string::npos) << srt;
}

TEST_F(TeletextSinkDeps, Analyse_RejectsMalformedSubtitlePage) {
  const auto params = make_pal_params();
  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out.t42"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.export_subtitles = true;
  options.subtitle_page = "zzz";

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Invalid subtitle page: zzz");
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkDeps, Analyse_ReportProfilesWhatSquashingChanged) {
  const auto params = make_pal_params();
  const auto header = make_header_packet(0x00, false, false);
  const auto row = make_row_packet(1, "HELLO");

  for (orc::FrameID frame = 0; frame <= 1; ++frame) {
    put_line(frame, flat_line(params, 0, 7),
             orc::tests::synthesize_teletext_line(header));
    put_line(frame, flat_line(params, 1, 7),
             orc::tests::synthesize_teletext_line(row));
  }

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 1}));
  expect_writer("out.t42");

  auto deps = make_deps();
  auto options = single_line_options(7);

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_NE(result.report.find("Teletext analysis report"), std::string::npos)
      << result.report;
  EXPECT_NE(result.report.find("Teletext squashing:"), std::string::npos)
      << result.report;
  EXPECT_NE(result.report.find("Pages:"), std::string::npos) << result.report;
  EXPECT_TRUE(result.report_path.empty());
}

TEST_F(TeletextSinkDeps, Analyse_SquashingDisabledSaysSoInTheReport) {
  const auto params = make_pal_params();
  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  expect_writer("out.t42");

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.squash_repeated_rows = false;

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_NE(result.report.find("Teletext squashing: disabled"),
            std::string::npos)
      << result.report;
}

TEST_F(TeletextSinkDeps, Analyse_WritesTheReportBesideThePacketStream) {
  const auto params = make_pal_params();
  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));

  auto report_writer = std::make_shared<StrictMock<MockFileWriterUint8>>();
  std::string report_text;
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(2)
      .WillOnce(Return(pMockFileWriterUint8_))
      .WillOnce(Return(report_writer));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out.t42"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);
  EXPECT_CALL(*report_writer, open("out.t42.txt"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*report_writer, write(_, _))
      .WillRepeatedly(Invoke([&report_text](const uint8_t* data, size_t count) {
        report_text.append(reinterpret_cast<const char*>(data), count);
      }));
  EXPECT_CALL(*report_writer, close()).Times(1);

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.write_report = true;

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.report_path, "out.t42.txt");
  EXPECT_NE(report_text.find("Teletext analysis report"), std::string::npos)
      << report_text;
}

// The packet stream is the product; a report that could not be written is a
// warning, not a failed run.
TEST_F(TeletextSinkDeps, Analyse_ReportWriteFailureDoesNotFailTheRun) {
  const auto params = make_pal_params();
  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));

  auto report_writer = std::make_shared<StrictMock<MockFileWriterUint8>>();
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(2)
      .WillOnce(Return(pMockFileWriterUint8_))
      .WillOnce(Return(report_writer));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out.t42"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);
  EXPECT_CALL(*report_writer, open("out.t42.txt"))
      .Times(1)
      .WillOnce(Return(false));

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.write_report = true;

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.report_path.empty());
  EXPECT_FALSE(result.report.empty());
}

TEST_F(TeletextSinkDeps, Analyse_ReportsARecoveryProfile) {
  const auto params = make_pal_params();
  const auto payload = make_payload(0x11);
  put_line(0, flat_line(params, 0, 7),
           orc::tests::synthesize_teletext_line(payload));
  // The same line of field 2 is blank rather than absent, so it reaches the
  // slicer and is rejected by the amplitude gate.
  put_line(
      0, flat_line(params, 1, 7),
      std::vector<int16_t>(static_cast<size_t>(orc::kPalSamplesPerLineNominal),
                           static_cast<int16_t>(orc::kPalBlack)));

  serve_lines(params);
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  expect_writer("out.t42");

  auto deps = make_deps();
  auto options = single_line_options(7);
  options.squash_repeated_rows = false;

  const auto result = deps.analyse(&mockRepresentation_, options);

  ASSERT_TRUE(result.success) << result.message;
  ASSERT_FALSE(result.report.empty());
  EXPECT_NE(result.report.find("2 candidate lines"), std::string::npos)
      << result.report;
  EXPECT_NE(result.report.find("1 packets recovered"), std::string::npos)
      << result.report;
  EXPECT_NE(result.report.find("amplitude gate 1"), std::string::npos)
      << result.report;
}

}  // namespace orc_unit_test
