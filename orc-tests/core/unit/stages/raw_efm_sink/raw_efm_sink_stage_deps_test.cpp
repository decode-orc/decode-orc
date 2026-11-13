/*
 * File:        raw_efm_sink_stage_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for raw EFM sink stage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../stage_services_mock.h"
#include "efm_sink_stage_deps.h"

using testing::Return;
using testing::StrictMock;

namespace orc_unit_test {

class RawEFMSinkStageDeps : public ::testing::Test {
 public:
  void SetUp() override {
    pMockFileWriterUint8_ = std::make_shared<StrictMock<MockFileWriterUint8>>();

    instance_ = std::make_unique<orc::RawEFMSinkStageDeps>(&mockStageServices_);
    instance_->init({}, &cancelRequested_);

    cancelRequested_.store(false);
  }

 protected:
  MockStageServices mockStageServices_;
  std::shared_ptr<StrictMock<MockFileWriterUint8>> pMockFileWriterUint8_;
  StrictMock<MockVideoFrameRepresentationArtifact> mockRepresentation_;

  std::atomic<bool> cancelRequested_{};
  std::unique_ptr<orc::RawEFMSinkStageDeps> instance_;
};

TEST_F(RawEFMSinkStageDeps,
       WriteRawEfm_WritesTValuesThroughUint8WriterService) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockRepresentation_, get_efm_sample_count(0))
      .Times(1)
      .WillOnce(Return(3));
  EXPECT_CALL(mockRepresentation_, get_efm_samples(0))
      .Times(1)
      .WillOnce(Return(std::vector<uint8_t>{3, 7, 11}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(4UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out_path.efm"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_, write(std::vector<uint8_t>{3, 7, 11}))
      .Times(1);
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  const auto result =
      instance_->write_raw_efm(&mockRepresentation_, "out_path.efm",
                               /*include_confidence=*/true);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.tvalues_written, 3U);
}

TEST_F(RawEFMSinkStageDeps,
       WriteRawEfm_FailsWithDiagnostic_WhenWriterServiceUnavailable) {
  orc::RawEFMSinkStageDeps deps_without_services(nullptr);
  deps_without_services.init({}, &cancelRequested_);

  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockRepresentation_, get_efm_sample_count(0))
      .Times(1)
      .WillOnce(Return(3));

  const auto result = deps_without_services.write_raw_efm(
      &mockRepresentation_, "out_path.efm", /*include_confidence=*/true);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.status_message, "Error: File writer service unavailable");
}

// The pipeline byte packs the t-value into the low nibble and the producer's
// doubt into the high one. The lossless export writes the byte through
// untouched.
TEST_F(RawEFMSinkStageDeps, WriteRawEfm_WritesPackedBytes_WhenConfidenceKept) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockRepresentation_, get_efm_sample_count(0))
      .Times(1)
      .WillOnce(Return(3));
  EXPECT_CALL(mockRepresentation_, get_efm_samples(0))
      .Times(1)
      .WillOnce(Return(std::vector<uint8_t>{0x03, 0xF7, 0x8B}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(4UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out_path.efm"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_,
              write(std::vector<uint8_t>{0x03, 0xF7, 0x8B}))
      .Times(1);
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  const auto result =
      instance_->write_raw_efm(&mockRepresentation_, "out_path.efm",
                               /*include_confidence=*/true);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.tvalues_written, 3U);
}

// With the confidence nibble excluded the file is the bare t-value stream that
// tools predating the confidence field expect.
TEST_F(RawEFMSinkStageDeps,
       WriteRawEfm_StripsDoubtNibble_WhenConfidenceExcluded) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockRepresentation_, get_efm_sample_count(0))
      .Times(1)
      .WillOnce(Return(3));
  EXPECT_CALL(mockRepresentation_, get_efm_samples(0))
      .Times(1)
      .WillOnce(Return(std::vector<uint8_t>{0x03, 0xF7, 0x8B}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(4UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out_path.efm"))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_, write(std::vector<uint8_t>{3, 7, 11}))
      .Times(1);
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  const auto result =
      instance_->write_raw_efm(&mockRepresentation_, "out_path.efm",
                               /*include_confidence=*/false);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.tvalues_written, 3U);
}

TEST_F(RawEFMSinkStageDeps, WriteRawEfm_Fails_WhenWriterCannotOpenFile) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockRepresentation_, get_efm_sample_count(0))
      .Times(1)
      .WillOnce(Return(3));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(4UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out_path.efm"))
      .Times(1)
      .WillOnce(Return(false));

  const auto result =
      instance_->write_raw_efm(&mockRepresentation_, "out_path.efm",
                               /*include_confidence=*/true);

  EXPECT_FALSE(result.success);
}

}  // namespace orc_unit_test
