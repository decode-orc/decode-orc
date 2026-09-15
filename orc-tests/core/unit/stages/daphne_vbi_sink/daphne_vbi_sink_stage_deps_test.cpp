/*
 * File:        daphne_vbi_sink_stage_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit test(s) for Daphne VBI sink stage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Matt Ownby
 */

#include "daphne_vbi_sink_stage_deps.h"

#include <gtest/gtest.h>

#include "../../include/file_io_interface_mock.h"
#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../stage_services_mock.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::Ref;
using testing::Return;
using testing::StrictMock;

// using different namespace from module-under-test so that we can use the same
// class names in the tests as in the module-under-test
namespace orc_unit_test {
// test fixture for DaphneVBISinkStageDeps suite of tests
class DaphneVBISinkStageDeps : public ::testing::Test {
 public:
  void SetUp() override {
    pMockFileWriterUint8_ = std::make_shared<StrictMock<MockFileWriterUint8>>();

    instance_ =
        std::make_unique<orc::DaphneVBISinkStageDeps>(&mockStageServices_);
    instance_->init({}, &isProcessing_, &cancelRequested_);

    cancelRequested_.store(false);
    isProcessing_.store(true);
  }

  void TearDown() override { instance_.reset(); }

 protected:
  MockStageServices mockStageServices_;
  std::shared_ptr<StrictMock<MockFileWriterUint8>> pMockFileWriterUint8_;
  MockObservationContext mockObservationContext_;

  StrictMock<MockVideoFrameRepresentationArtifact> mockRepresentation_;

  std::atomic<bool> cancelRequested_{};
  std::atomic<bool> isProcessing_{};
  std::unique_ptr<orc::DaphneVBISinkStageDeps> instance_;
};

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(DaphneVBISinkStageDeps,
       WriteVbi_AddsExtensionAndWritesHeaderWhenSuccessful) {
  // Empty range: last < first → count() == 0 → loop doesn't execute
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{1, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(1UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));

  EXPECT_CALL(*pMockFileWriterUint8_, open("out_path.vbi"))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockFileWriterUint8_,
              write(testing::A<const std::vector<uint8_t>&>()))
      .Times(testing::AtLeast(1));

  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  const bool result = instance_->write_vbi(&mockRepresentation_, "out_path",
                                           mockObservationContext_);

  EXPECT_TRUE(result);
}

// "-" is the stdio convention: no ".vbi" is appended, unlike a real path.
TEST_F(DaphneVBISinkStageDeps, WriteVbi_PipesToStdoutSkippingExtension) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{1, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(1UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));

  EXPECT_CALL(*pMockFileWriterUint8_, open("-"))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*pMockFileWriterUint8_,
              write(testing::A<const std::vector<uint8_t>&>()))
      .Times(testing::AtLeast(1));

  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  const bool result =
      instance_->write_vbi(&mockRepresentation_, "-", mockObservationContext_);

  EXPECT_TRUE(result);
}

TEST_F(DaphneVBISinkStageDeps, WriteVbi_ReturnsFalseWhenOpenFails) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{1, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(1UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));

  EXPECT_CALL(*pMockFileWriterUint8_, open("out_path.vbi"))
      .Times(1)
      .WillOnce(Return(false));

  const bool result = instance_->write_vbi(&mockRepresentation_, "out_path",
                                           mockObservationContext_);

  EXPECT_FALSE(result);
}

TEST_F(DaphneVBISinkStageDeps,
       WriteVbi_ClosesWriterAndMarksProcessingFalseWhenCancelled) {
  // Non-empty range: FrameIDRange{0, 1} = 2 frames. Cancel is checked at the
  // top of each loop iteration before any frame data is read.
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 1}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(1UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));

  EXPECT_CALL(*pMockFileWriterUint8_, open("cancel_path.vbi"))
      .Times(1)
      .WillOnce(Return(true));

  // write_header() is called before the loop, so at least one write occurs
  EXPECT_CALL(*pMockFileWriterUint8_,
              write(testing::A<const std::vector<uint8_t>&>()))
      .Times(testing::AtLeast(1));

  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  cancelRequested_.store(true);

  const bool result = instance_->write_vbi(&mockRepresentation_, "cancel_path",
                                           mockObservationContext_);

  EXPECT_FALSE(result);
  EXPECT_FALSE(isProcessing_.load());
}
}  // namespace orc_unit_test
