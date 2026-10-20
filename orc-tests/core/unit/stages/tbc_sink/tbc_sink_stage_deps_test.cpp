/*
 * File:        tbc_sink_stage_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for TBCSinkStageDeps
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tbc_sink_stage_deps.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../../include/file_io_interface_mock.h"
#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../stage_services_mock.h"
#include "tbc_metadata_writer_interface_mock.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::Ref;
using testing::Return;
using testing::StrictMock;

namespace orc_unit_test {
// test fixture for TBCSinkStageDeps suite of tests
class TBCSinkStageDepsTest : public ::testing::Test {
 public:
  void SetUp() override {
    pMockFileWriterUint16_ =
        std::make_shared<StrictMock<MockFileWriterUint16>>();
    pMockMetadataWriter_ =
        std::make_shared<StrictMock<MockTBCMetadataWriter>>();

    instance_ = std::make_unique<orc::TBCSinkStageDeps>(&mockStageServices_,
                                                        pMockMetadataWriter_);
    instance_->init({}, &isProcessing_, &cancelRequested_);

    // No audio by default; the sidecar tests below use their own fixture.
    // has_efm() is not mocked, so it keeps the base class's false.
    EXPECT_CALL(mockRepresentation_, audio_channel_pair_count())
        .WillRepeatedly(Return(0));

    cancelRequested_.store(false);
    isProcessing_.store(true);
  }

  void TearDown() override { instance_.reset(); }

 protected:
  MockStageServices mockStageServices_;
  std::shared_ptr<StrictMock<MockFileWriterUint16>> pMockFileWriterUint16_;
  std::shared_ptr<StrictMock<MockTBCMetadataWriter>> pMockMetadataWriter_;
  MockObservationContext mockObservationContext_;

  StrictMock<MockVideoFrameRepresentationArtifact> mockRepresentation_;

  std::atomic<bool> cancelRequested_{};
  std::atomic<bool> isProcessing_{};
  std::unique_ptr<orc::TBCSinkStageDeps> instance_;
};

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TBCSinkStageDepsTest, WriteTbc_AddsExtensionAndSucceedsWithEmptyRange) {
  // Empty range: last < first → count() == 0 → loop doesn't execute
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{1, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint16(16UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint16_));

  EXPECT_CALL(*pMockFileWriterUint16_, open("out_path.tbc"))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, open("out_path.tbc.db"))
      .Times(1)
      .WillOnce(Return(true));

  orc::SourceParameters video_params;
  video_params.system = orc::VideoSystem::PAL;
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(video_params));

  EXPECT_CALL(*pMockMetadataWriter_, write_video_parameters(_))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, begin_transaction())
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, commit_transaction())
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, close()).Times(1);

  EXPECT_CALL(*pMockFileWriterUint16_, close()).Times(1);

  const bool result = instance_->write_tbc_and_metadata(
      &mockRepresentation_, "out_path", 0, mockObservationContext_);

  EXPECT_TRUE(result);
}

// The "-" stdio convention (CVBS Sink, TBC Stream Source): the primary .tbc
// payload streams straight to stdout with no extension appended and no
// .db metadata sidecar opened or written — a pipe can carry only the one
// stream. begin/commit_transaction and close() are still called on the
// (never-opened) metadata writer, matching TBCMetadataWriter's real
// no-op-when-closed behaviour; they are stubbed here so the StrictMock does
// not flag them as unexpected.
TEST_F(TBCSinkStageDepsTest,
       WriteTbc_PipesToStdoutSkippingExtensionAndMetadataDb) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{1, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint16(16UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint16_));

  // "-" unchanged: no ".tbc" appended the way a real path would get.
  EXPECT_CALL(*pMockFileWriterUint16_, open("-"))
      .Times(1)
      .WillOnce(Return(true));

  orc::SourceParameters video_params;
  video_params.system = orc::VideoSystem::PAL;
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(video_params));

  // Never opened, never written to: no EXPECT_CALL for open() or
  // write_video_parameters() on pMockMetadataWriter_ — a StrictMock fails the
  // test outright if either happens.
  EXPECT_CALL(*pMockMetadataWriter_, begin_transaction())
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockMetadataWriter_, commit_transaction())
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockMetadataWriter_, close()).Times(1);

  EXPECT_CALL(*pMockFileWriterUint16_, close()).Times(1);

  const bool result = instance_->write_tbc_and_metadata(
      &mockRepresentation_, "-", 0, mockObservationContext_);

  EXPECT_TRUE(result);
}

TEST_F(TBCSinkStageDepsTest, WriteTbc_ReturnsFalseWhenTbcFileOpenFails) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{1, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint16(16UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint16_));

  EXPECT_CALL(*pMockFileWriterUint16_, open("out_path.tbc"))
      .Times(1)
      .WillOnce(Return(false));

  const bool result = instance_->write_tbc_and_metadata(
      &mockRepresentation_, "out_path", 0, mockObservationContext_);

  EXPECT_FALSE(result);
}

TEST_F(TBCSinkStageDepsTest, WriteTbc_ReturnsFalseWhenMetadataDbOpenFails) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{1, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint16(16UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint16_));

  EXPECT_CALL(*pMockFileWriterUint16_, open("out_path.tbc"))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, open("out_path.tbc.db"))
      .Times(1)
      .WillOnce(Return(false));

  EXPECT_CALL(*pMockFileWriterUint16_, close()).Times(1);

  const bool result = instance_->write_tbc_and_metadata(
      &mockRepresentation_, "out_path", 0, mockObservationContext_);

  EXPECT_FALSE(result);
}

TEST_F(TBCSinkStageDepsTest,
       WriteTbc_ReturnsFalseWhenGetVideoParametersReturnsNullopt) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{1, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint16(16UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint16_));

  EXPECT_CALL(*pMockFileWriterUint16_, open("out_path.tbc"))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, open("out_path.tbc.db"))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(std::nullopt));

  EXPECT_CALL(*pMockMetadataWriter_, close()).Times(1);
  EXPECT_CALL(*pMockFileWriterUint16_, close()).Times(1);

  const bool result = instance_->write_tbc_and_metadata(
      &mockRepresentation_, "out_path", 0, mockObservationContext_);

  EXPECT_FALSE(result);
}

TEST_F(TBCSinkStageDepsTest,
       WriteTbc_ReturnsFalseWhenWriteVideoParametersFails) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{1, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint16(16UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint16_));

  EXPECT_CALL(*pMockFileWriterUint16_, open("out_path.tbc"))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, open("out_path.tbc.db"))
      .Times(1)
      .WillOnce(Return(true));

  orc::SourceParameters video_params;
  video_params.system = orc::VideoSystem::PAL;
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(video_params));

  EXPECT_CALL(*pMockMetadataWriter_, write_video_parameters(_))
      .Times(1)
      .WillOnce(Return(false));

  EXPECT_CALL(*pMockMetadataWriter_, close()).Times(1);
  EXPECT_CALL(*pMockFileWriterUint16_, close()).Times(1);

  const bool result = instance_->write_tbc_and_metadata(
      &mockRepresentation_, "out_path", 0, mockObservationContext_);

  EXPECT_FALSE(result);
}

TEST_F(TBCSinkStageDepsTest,
       WriteTbc_PaddingFrameWritesPadMarkedFieldMetadata) {
  // A padding frame is written as two blanking-level fields whose metadata
  // carries pad=true, so the padding identity round-trips through a
  // re-imported TBC (issue #77).
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint16(16UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint16_));

  EXPECT_CALL(*pMockFileWriterUint16_, open("pad_path.tbc"))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, open("pad_path.tbc.db"))
      .Times(1)
      .WillOnce(Return(true));

  orc::SourceParameters video_params;
  video_params.system = orc::VideoSystem::PAL;
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(video_params));

  EXPECT_CALL(*pMockMetadataWriter_, write_video_parameters(_))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, begin_transaction())
      .Times(1)
      .WillOnce(Return(true));

  orc::FrameDescriptor padding_desc;
  padding_desc.is_padding_frame = true;
  EXPECT_CALL(mockRepresentation_, get_frame_descriptor(orc::FrameID(0)))
      .Times(1)
      .WillOnce(Return(padding_desc));

  // Two blanking-level fields are written for the padding frame.
  EXPECT_CALL(*pMockFileWriterUint16_,
              write(testing::A<const std::vector<uint16_t>&>()))
      .Times(2);

  std::vector<orc::FieldMetadata> written;
  EXPECT_CALL(*pMockMetadataWriter_, write_field_metadata(_))
      .Times(2)
      .WillRepeatedly([&written](const orc::FieldMetadata& fm) {
        written.push_back(fm);
        return true;
      });

  EXPECT_CALL(*pMockMetadataWriter_, commit_transaction())
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockMetadataWriter_, close()).Times(1);
  EXPECT_CALL(*pMockFileWriterUint16_, close()).Times(1);

  const bool result = instance_->write_tbc_and_metadata(
      &mockRepresentation_, "pad_path", 0, mockObservationContext_);

  EXPECT_TRUE(result);
  ASSERT_EQ(written.size(), 2u);
  EXPECT_EQ(written[0].is_first_field, std::optional<bool>(true));
  EXPECT_EQ(written[1].is_first_field, std::optional<bool>(false));
  EXPECT_EQ(written[0].is_pad, std::optional<bool>(true));
  EXPECT_EQ(written[1].is_pad, std::optional<bool>(true));
}

TEST_F(TBCSinkStageDepsTest,
       WriteTbc_ClosesFilesAndMarksProcessingFalseWhenCancelled) {
  // Non-empty range: FrameIDRange{0, 1} = 2 frames. Cancel is checked at the
  // top of each iteration, so the first frame (0) triggers cancel before any
  // frame data is read.
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 1}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint16(16UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint16_));

  EXPECT_CALL(*pMockFileWriterUint16_, open("cancel_path.tbc"))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, open("cancel_path.tbc.db"))
      .Times(1)
      .WillOnce(Return(true));

  orc::SourceParameters video_params;
  video_params.system = orc::VideoSystem::PAL;
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(video_params));

  EXPECT_CALL(*pMockMetadataWriter_, write_video_parameters(_))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockMetadataWriter_, begin_transaction())
      .Times(1)
      .WillOnce(Return(true));

  // On cancel: commit + close metadata, close tbc
  EXPECT_CALL(*pMockMetadataWriter_, commit_transaction())
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockMetadataWriter_, close()).Times(1);
  EXPECT_CALL(*pMockFileWriterUint16_, close()).Times(1);

  cancelRequested_.store(true);

  const bool result = instance_->write_tbc_and_metadata(
      &mockRepresentation_, "cancel_path", 0, mockObservationContext_);

  EXPECT_FALSE(result);
  EXPECT_FALSE(isProcessing_.load());
}

////////////////////////////////////////////////////////////////////////////////////////////
// Field ordering (issue #257)
////////////////////////////////////////////////////////////////////////////////////////////

namespace {

// Fixture for the single-frame export tests: drives one NTSC frame whose every
// sample on VFR line L carries the value L, so the field buffers handed to the
// TBC writer say exactly which VFR lines they were built from.
class TBCSinkFieldOrderTest : public TBCSinkStageDepsTest {
 protected:
  // Sets up one non-padding NTSC frame and captures the two field buffers
  // written for it into written_fields_ / written_meta_.
  void ExportOneNtscFrame(std::vector<orc::DropoutRun> dropout_hints = {}) {
    EXPECT_CALL(mockRepresentation_, frame_range())
        .WillOnce(Return(orc::FrameIDRange{0, 0}));
    EXPECT_CALL(mockStageServices_,
                create_buffered_file_writer_uint16(16UL * 1024 * 1024))
        .WillOnce(Return(pMockFileWriterUint16_));
    EXPECT_CALL(*pMockFileWriterUint16_, open("order.tbc"))
        .WillOnce(Return(true));
    EXPECT_CALL(*pMockMetadataWriter_, open("order.tbc.db"))
        .WillOnce(Return(true));

    orc::SourceParameters video_params;
    video_params.system = orc::VideoSystem::NTSC;
    EXPECT_CALL(mockRepresentation_, get_video_parameters())
        .WillOnce(Return(video_params));
    EXPECT_CALL(*pMockMetadataWriter_, write_video_parameters(_))
        .WillOnce(Return(true));
    EXPECT_CALL(*pMockMetadataWriter_, begin_transaction())
        .WillOnce(Return(true));

    orc::FrameDescriptor desc;
    desc.is_padding_frame = false;
    EXPECT_CALL(mockRepresentation_, get_frame_descriptor(orc::FrameID(0)))
        .WillOnce(Return(desc));
    EXPECT_CALL(mockRepresentation_, get_dropout_hints(orc::FrameID(0)))
        .WillOnce(Return(dropout_hints));

    // Every sample on VFR line L is the CVBS value L.
    EXPECT_CALL(mockRepresentation_, get_line(orc::FrameID(0), _))
        .WillRepeatedly([this](orc::FrameID, size_t line) {
          line_buffer_.assign(static_cast<size_t>(orc::kNtscSamplesPerLine),
                              static_cast<int16_t>(line));
          return line_buffer_.data();
        });

    EXPECT_CALL(*pMockFileWriterUint16_,
                write(testing::A<const std::vector<uint16_t>&>()))
        .Times(2)
        .WillRepeatedly([this](const std::vector<uint16_t>& data) {
          written_fields_.push_back(data);
        });
    EXPECT_CALL(*pMockMetadataWriter_, write_field_metadata(_))
        .Times(2)
        .WillRepeatedly([this](const orc::FieldMetadata& fm) {
          written_meta_.push_back(fm);
          return true;
        });
    EXPECT_CALL(*pMockMetadataWriter_, write_dropout(_, _))
        .WillRepeatedly(
            [this](orc::FieldID field_id, const orc::DropoutInfo& di) {
              written_dropouts_.push_back({field_id, di});
              return true;
            });

    EXPECT_CALL(*pMockMetadataWriter_, commit_transaction())
        .WillOnce(Return(true));
    EXPECT_CALL(*pMockMetadataWriter_, close()).Times(1);
    EXPECT_CALL(*pMockFileWriterUint16_, close()).Times(1);

    ASSERT_TRUE(instance_->write_tbc_and_metadata(&mockRepresentation_, "order",
                                                  0, mockObservationContext_));
  }

  // The VFR line index recorded in field |field| at stored line |line|,
  // undoing the x64 widening the export applies to every sample.
  int32_t SourceLineOf(size_t field, size_t line) const {
    return static_cast<int32_t>(written_fields_.at(field).at(
               line * static_cast<size_t>(orc::kNtscSamplesPerLine))) /
           64;
  }

  std::vector<int16_t> line_buffer_;
  std::vector<std::vector<uint16_t>> written_fields_;
  std::vector<orc::FieldMetadata> written_meta_;
  std::vector<std::pair<orc::FieldID, orc::DropoutInfo>> written_dropouts_;
};

}  // namespace

// ld-decode writes the isFirstField=true field first and that field is the top
// spatial field, so VFR field 1 must become TBC field 1. Emitting them the
// other way round transposed every field pair on export (issue #257).
TEST_F(TBCSinkFieldOrderTest, WriteTbc_FirstFieldIsTheTopVfrField) {
  ASSERT_NO_FATAL_FAILURE(ExportOneNtscFrame());

  ASSERT_EQ(written_meta_.size(), 2u);
  EXPECT_EQ(written_meta_[0].is_first_field, std::optional<bool>(true));
  EXPECT_EQ(written_meta_[1].is_first_field, std::optional<bool>(false));

  ASSERT_EQ(written_fields_.size(), 2u);
  const size_t stored_lines = orc::calculate_padded_field_height(
      orc::VideoSystem::NTSC);  // 263 lines per stored field
  const size_t stored_samples =
      stored_lines * static_cast<size_t>(orc::kNtscSamplesPerLine);
  EXPECT_EQ(written_fields_[0].size(), stored_samples);
  EXPECT_EQ(written_fields_[1].size(), stored_samples);

  // TBC field 1 = VFR lines [0, 263); TBC field 2 = VFR lines [263, 525).
  EXPECT_EQ(SourceLineOf(0, 0), 0);
  EXPECT_EQ(SourceLineOf(0, 1), 1);
  EXPECT_EQ(SourceLineOf(0, orc::kNtscField1Lines - 1),
            orc::kNtscField1Lines - 1);
  EXPECT_EQ(SourceLineOf(1, 0), orc::kNtscField1Lines);
  EXPECT_EQ(SourceLineOf(1, orc::kNtscFrameLines - orc::kNtscField1Lines - 1),
            orc::kNtscFrameLines - 1);
}

// The 525-line systems have one fewer line in the bottom field; ld-decode
// stores both fields at the same height, so only the second field is padded.
TEST_F(TBCSinkFieldOrderTest, WriteTbc_PadsOnlyTheShorterSecondField) {
  ASSERT_NO_FATAL_FAILURE(ExportOneNtscFrame());

  ASSERT_EQ(written_fields_.size(), 2u);
  const size_t last_line =
      orc::calculate_padded_field_height(orc::VideoSystem::NTSC) - 1;
  // Field 1 carries real picture on every stored line.
  EXPECT_EQ(SourceLineOf(0, last_line), orc::kNtscField1Lines - 1);
  // Field 2's final stored line is blanking-level padding.
  EXPECT_EQ(written_fields_[1].at(
                last_line * static_cast<size_t>(orc::kNtscSamplesPerLine)),
            static_cast<uint16_t>(orc::kTbcNtscBlanking));
}

// Samples must survive the export untouched: the ld-decode 16-bit domain is
// CVBS_U10_4FSC x 64, so the widening is the only change (issue #257).
TEST_F(TBCSinkFieldOrderTest, WriteTbc_ScalesSamplesByExactly64) {
  ASSERT_NO_FATAL_FAILURE(ExportOneNtscFrame());

  ASSERT_EQ(written_fields_.size(), 2u);
  for (size_t line = 0; line < static_cast<size_t>(orc::kNtscField1Lines);
       ++line) {
    EXPECT_EQ(written_fields_[0].at(
                  line * static_cast<size_t>(orc::kNtscSamplesPerLine)),
              static_cast<uint16_t>(line * 64))
        << "line " << line;
  }
}

// Dropouts follow the same field mapping as the samples they mark.
TEST_F(TBCSinkFieldOrderTest, WriteTbc_DropoutsFollowTheSameFieldMapping) {
  // One run wholly inside VFR line 10 (top field, TBC field 1) and one wholly
  // inside VFR line 300 (bottom field, TBC field 2).
  const auto spl = static_cast<uint64_t>(orc::kNtscSamplesPerLine);
  std::vector<orc::DropoutRun> hints;
  orc::DropoutRun top;
  top.frame_id = orc::FrameID(0);
  top.sample_start = 10 * spl + 100;
  top.sample_count = 50;
  hints.push_back(top);
  orc::DropoutRun bottom;
  bottom.frame_id = orc::FrameID(0);
  bottom.sample_start = 300 * spl + 200;
  bottom.sample_count = 30;
  hints.push_back(bottom);

  ASSERT_NO_FATAL_FAILURE(ExportOneNtscFrame(hints));

  ASSERT_EQ(written_dropouts_.size(), 2u);
  // Exported field 0 is TBC field 1 and carries the top-field dropout at its
  // field-local line 10.
  EXPECT_EQ(written_dropouts_[0].first, orc::FieldID(0));
  EXPECT_EQ(written_dropouts_[0].second.line, 10u);
  EXPECT_EQ(written_dropouts_[0].second.start_sample, 100u);
  EXPECT_EQ(written_dropouts_[0].second.end_sample, 150u);
  // Exported field 1 is TBC field 2; VFR line 300 is its field-local line 37.
  EXPECT_EQ(written_dropouts_[1].first, orc::FieldID(1));
  EXPECT_EQ(written_dropouts_[1].second.line,
            static_cast<uint32_t>(300 - orc::kNtscField1Lines));
  EXPECT_EQ(written_dropouts_[1].second.start_sample, 200u);
  EXPECT_EQ(written_dropouts_[1].second.end_sample, 230u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// .pcm and .efm sidecars
////////////////////////////////////////////////////////////////////////////////////////////

namespace {

// has_efm() and prime_audio_decode() are plain virtuals on
// VideoFrameRepresentation rather than mocked members of the shared artifact
// mock, so the sidecar tests extend it locally.
class MockVfrWithSidecars : public MockVideoFrameRepresentationArtifact {
 public:
  MOCK_METHOD(bool, has_efm, (), (const, override));
  MOCK_METHOD(void, prime_audio_decode, (const orc::AudioDecodeProgressFn&),
              (const, override));
};

// Drives a one-frame PAL export whose input carries both sidecars, capturing
// what each writer received.
class TBCSinkSidecarTest : public ::testing::Test {
 public:
  void SetUp() override {
    pMockTbcWriter_ = std::make_shared<StrictMock<MockFileWriterUint16>>();
    pMockByteWriter_ = std::make_shared<StrictMock<MockFileWriterUint8>>();
    pMockMetadataWriter_ =
        std::make_shared<StrictMock<MockTBCMetadataWriter>>();

    instance_ = std::make_unique<orc::TBCSinkStageDeps>(&mockStageServices_,
                                                        pMockMetadataWriter_);
    instance_->init({}, &isProcessing_, &cancelRequested_);
    cancelRequested_.store(false);
    isProcessing_.store(true);
  }

 protected:
  // |audio_pairs| is how many channel pairs the input reports; |efm| adds an
  // EFM payload. Returns after a successful export.
  void ExportOnePalFrame(size_t audio_pairs, bool efm,
                         size_t requested_pair = 0) {
    EXPECT_CALL(mockRepresentation_, frame_range())
        .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
    EXPECT_CALL(mockRepresentation_, audio_channel_pair_count())
        .WillRepeatedly(Return(audio_pairs));
    EXPECT_CALL(mockRepresentation_, has_efm()).WillRepeatedly(Return(efm));
    EXPECT_CALL(mockRepresentation_, prime_audio_decode(_))
        .Times(audio_pairs > 0 ? 1 : 0);

    EXPECT_CALL(mockStageServices_,
                create_buffered_file_writer_uint16(16UL * 1024 * 1024))
        .WillOnce(Return(pMockTbcWriter_));
    EXPECT_CALL(*pMockTbcWriter_, open("sc.tbc")).WillOnce(Return(true));
    EXPECT_CALL(*pMockMetadataWriter_, open("sc.tbc.db"))
        .WillOnce(Return(true));

    orc::SourceParameters video_params;
    video_params.system = orc::VideoSystem::PAL;
    EXPECT_CALL(mockRepresentation_, get_video_parameters())
        .WillOnce(Return(video_params));
    EXPECT_CALL(*pMockMetadataWriter_, write_video_parameters(_))
        .WillOnce(Return(true));
    EXPECT_CALL(*pMockMetadataWriter_, write_pcm_audio_parameters(_))
        .Times(audio_pairs > 0 ? 1 : 0)
        .WillRepeatedly([this](const orc::PcmAudioParameters& p) {
          pcm_params_ = p;
          return true;
        });
    EXPECT_CALL(*pMockMetadataWriter_, begin_transaction())
        .WillOnce(Return(true));

    // Byte writers: the .efm (during the loop) and the .pcm (after it). Both
    // come from the same uint8 factory, so the paths distinguish them.
    const int byte_writers = (efm ? 1 : 0) + (audio_pairs > 0 ? 1 : 0);
    EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
        .Times(byte_writers)
        .WillRepeatedly(Return(pMockByteWriter_));
    EXPECT_CALL(*pMockByteWriter_, open(_))
        .Times(byte_writers)
        .WillRepeatedly([this](const std::string& path) {
          opened_byte_paths_.push_back(path);
          return true;
        });
    EXPECT_CALL(*pMockByteWriter_,
                write(testing::A<const std::vector<uint8_t>&>()))
        .WillRepeatedly([this](const std::vector<uint8_t>& data) {
          if (opened_byte_paths_.empty()) return;
          byte_writes_[opened_byte_paths_.back()].insert(
              byte_writes_[opened_byte_paths_.back()].end(), data.begin(),
              data.end());
        });
    EXPECT_CALL(*pMockByteWriter_, close()).Times(byte_writers);

    orc::FrameDescriptor desc;
    desc.is_padding_frame = false;
    EXPECT_CALL(mockRepresentation_, get_frame_descriptor(orc::FrameID(0)))
        .WillOnce(Return(desc));
    EXPECT_CALL(mockRepresentation_, get_dropout_hints(orc::FrameID(0)))
        .WillOnce(Return(std::vector<orc::DropoutRun>{}));
    EXPECT_CALL(mockRepresentation_, get_line(orc::FrameID(0), _))
        .WillRepeatedly([this](orc::FrameID, size_t) {
          line_buffer_.assign(
              static_cast<size_t>(orc::kPalSamplesPerLineNominal), 0);
          return line_buffer_.data();
        });
    EXPECT_CALL(*pMockTbcWriter_,
                write(testing::A<const std::vector<uint16_t>&>()))
        .Times(2);

    if (audio_pairs > 0) {
      // A full PAL frame of audio: 1920 stereo pairs, every sample distinct
      // per requested channel pair so the test can tell which pair was taken.
      EXPECT_CALL(mockRepresentation_, get_audio_samples(_, orc::FrameID(0)))
          .WillRepeatedly([](size_t pair, orc::FrameID) {
            std::vector<int32_t> samples(1920 * 2);
            for (size_t i = 0; i < samples.size(); ++i) {
              samples[i] = static_cast<int32_t>((pair + 1) * 256);
            }
            return samples;
          });
    }
    if (efm) {
      EXPECT_CALL(mockRepresentation_, get_efm_samples(orc::FrameID(0)))
          .WillOnce(Return(std::vector<uint8_t>{3, 4, 5, 6, 7}));
    }

    EXPECT_CALL(*pMockMetadataWriter_, write_field_metadata(_))
        .Times(2)
        .WillRepeatedly([this](const orc::FieldMetadata& fm) {
          written_meta_.push_back(fm);
          return true;
        });
    EXPECT_CALL(*pMockMetadataWriter_, commit_transaction())
        .WillOnce(Return(true));
    EXPECT_CALL(*pMockMetadataWriter_, close()).Times(1);
    EXPECT_CALL(*pMockTbcWriter_, close()).Times(1);

    ASSERT_TRUE(instance_->write_tbc_and_metadata(
        &mockRepresentation_, "sc", requested_pair, mockObservationContext_));
  }

  MockStageServices mockStageServices_;
  std::shared_ptr<StrictMock<MockFileWriterUint16>> pMockTbcWriter_;
  std::shared_ptr<StrictMock<MockFileWriterUint8>> pMockByteWriter_;
  std::shared_ptr<StrictMock<MockTBCMetadataWriter>> pMockMetadataWriter_;
  MockObservationContext mockObservationContext_;
  StrictMock<MockVfrWithSidecars> mockRepresentation_;

  std::vector<int16_t> line_buffer_;
  std::vector<std::string> opened_byte_paths_;
  std::map<std::string, std::vector<uint8_t>> byte_writes_;
  std::vector<orc::FieldMetadata> written_meta_;
  orc::PcmAudioParameters pcm_params_;

  std::atomic<bool> cancelRequested_{};
  std::atomic<bool> isProcessing_{};
  std::unique_ptr<orc::TBCSinkStageDeps> instance_;
};

}  // namespace

// ld-decode names the sidecars off the base with the .tbc replaced, which is
// the layout tbc_source auto-detects.
TEST_F(TBCSinkSidecarTest, WriteTbc_UsesLdDecodeSidecarPaths) {
  ASSERT_NO_FATAL_FAILURE(ExportOnePalFrame(1, true));

  EXPECT_NE(
      std::find(opened_byte_paths_.begin(), opened_byte_paths_.end(), "sc.efm"),
      opened_byte_paths_.end());
  EXPECT_NE(
      std::find(opened_byte_paths_.begin(), opened_byte_paths_.end(), "sc.pcm"),
      opened_byte_paths_.end());
}

// An input with no audio and no EFM writes neither sidecar and records no
// pcm_audio_parameters row.
TEST_F(TBCSinkSidecarTest, WriteTbc_WritesNoSidecarsWhenTheInputHasNone) {
  ASSERT_NO_FATAL_FAILURE(ExportOnePalFrame(0, false));
  EXPECT_TRUE(opened_byte_paths_.empty());
}

// The EFM sidecar is one byte per T-value, and the per-field counts are what
// let tbc_source find a frame's payload again.
TEST_F(TBCSinkSidecarTest, WriteTbc_EfmSidecarIsRawTValuesWithFieldCounts) {
  ASSERT_NO_FATAL_FAILURE(ExportOnePalFrame(0, true));

  const auto& efm = byte_writes_["sc.efm"];
  EXPECT_EQ(efm, (std::vector<uint8_t>{3, 4, 5, 6, 7}));

  ASSERT_EQ(written_meta_.size(), 2u);
  // Five bytes split across the frame's two fields, odd byte to the first.
  EXPECT_EQ(written_meta_[0].efm_t_values, std::optional<int32_t>(3));
  EXPECT_EQ(written_meta_[1].efm_t_values, std::optional<int32_t>(2));
}

// PAL: 1920 pipeline pairs per frame at 48 kHz become 1764 at 44.1 kHz, split
// evenly across the frame's two fields.
TEST_F(TBCSinkSidecarTest, WriteTbc_PcmSidecarIsS16leAt44100) {
  ASSERT_NO_FATAL_FAILURE(ExportOnePalFrame(1, false));

  EXPECT_EQ(pcm_params_.bits, 16);
  EXPECT_TRUE(pcm_params_.is_signed);
  EXPECT_TRUE(pcm_params_.is_little_endian);
  EXPECT_DOUBLE_EQ(pcm_params_.sample_rate, 44100.0);

  // 4 bytes per stereo pair.
  EXPECT_EQ(byte_writes_["sc.pcm"].size(), 1764u * 4u);

  ASSERT_EQ(written_meta_.size(), 2u);
  EXPECT_EQ(written_meta_[0].audio_samples, std::optional<int32_t>(882));
  EXPECT_EQ(written_meta_[1].audio_samples, std::optional<int32_t>(882));
}

// The parameter picks the pair; the default is the lowest.
TEST_F(TBCSinkSidecarTest, WriteTbc_ExportsTheRequestedChannelPair) {
  ASSERT_NO_FATAL_FAILURE(ExportOnePalFrame(3, false, 2));

  // Pair 2's samples are (2 + 1) * 256 in the 24-bit carrier, which narrows to
  // 3 in the 16-bit sidecar; a steady level survives the resample unchanged.
  const auto& pcm = byte_writes_["sc.pcm"];
  ASSERT_GE(pcm.size(), 4u);
  const int16_t mid = static_cast<int16_t>(
      static_cast<uint16_t>(pcm[pcm.size() / 2 & ~size_t{1}]) |
      (static_cast<uint16_t>(pcm[(pcm.size() / 2 & ~size_t{1}) + 1]) << 8));
  EXPECT_EQ(mid, 3);
}

// A pair the input does not carry falls back to the lowest one rather than
// exporting silence.
TEST_F(TBCSinkSidecarTest, WriteTbc_FallsBackToPairZeroWhenOutOfRange) {
  ASSERT_NO_FATAL_FAILURE(ExportOnePalFrame(1, false, 5));

  const auto& pcm = byte_writes_["sc.pcm"];
  ASSERT_GE(pcm.size(), 4u);
  const size_t at = pcm.size() / 2 & ~size_t{1};
  const int16_t mid =
      static_cast<int16_t>(static_cast<uint16_t>(pcm[at]) |
                           (static_cast<uint16_t>(pcm[at + 1]) << 8));
  EXPECT_EQ(mid, 1);  // pair 0 → (0 + 1) * 256 → 1
}

}  // namespace orc_unit_test
