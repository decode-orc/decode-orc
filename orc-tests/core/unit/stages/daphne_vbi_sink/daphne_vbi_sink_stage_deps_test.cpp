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
#include <orc/stage/observation/observation_context.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "../../include/file_io_interface_mock.h"
#include "../../include/observation_context_interface_mock.h"
#include "../../include/observation_service_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../stage_services_mock.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::ByMove;
using testing::Invoke;
using testing::NiceMock;
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

    instance_ = std::make_unique<orc::DaphneVBISinkStageDeps>(
        &mockStageServices_, &mockObservationService_);
    instance_->init({}, &isProcessing_, &cancelRequested_);

    cancelRequested_.store(false);
    isProcessing_.store(true);

    // write_vbi() checks this before its per-frame loop (see the
    // has_unbounded_frame_range() guard added alongside it); every test
    // here exercises the bounded/false side of that check.
    EXPECT_CALL(mockRepresentation_, has_unbounded_frame_range())
        .WillRepeatedly(Return(false));
  }

  void TearDown() override { instance_.reset(); }

 protected:
  MockStageServices mockStageServices_;
  std::shared_ptr<StrictMock<MockFileWriterUint8>> pMockFileWriterUint8_;
  MockObservationContext mockObservationContext_;
  NiceMock<MockObservationService> mockObservationService_;

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

// Three frames give six records after the header. Frame 1 carries a white
// flag and three decoded lines (one unparsable); frame 2 is missing from the
// input, so its records are written empty to keep later fields aligned.
TEST_F(DaphneVBISinkStageDeps,
       WriteVbi_WritesOneRecordPerFieldFromObservations) {
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillOnce(Return(orc::FrameIDRange{0, 2}));
  EXPECT_CALL(mockRepresentation_, has_frame(_))
      .WillRepeatedly(Invoke([](orc::FrameID fid) { return fid != 2; }));

  auto biphase = std::make_unique<StrictMock<MockObserverHandle>>();
  EXPECT_CALL(*biphase, process_frame(Ref(mockRepresentation_), _, _))
      .Times(2)
      .WillRepeatedly(
          Invoke([](const orc::VideoFrameRepresentation&, orc::FrameID fid,
                    orc::IObservationContext& context) {
            if (fid != 1) return;
            const orc::FieldID field(fid * 2);
            context.set(field, "biphase", "vbi_line_16", int32_t{0x8BA123});
            context.set(field, "biphase", "vbi_line_17", int32_t{-1});
            context.set(field, "biphase", "vbi_line_18", int32_t{0xF81234});
          }));
  auto white_flag = std::make_unique<StrictMock<MockObserverHandle>>();
  EXPECT_CALL(*white_flag, process_frame(Ref(mockRepresentation_), _, _))
      .Times(2)
      .WillRepeatedly(
          Invoke([](const orc::VideoFrameRepresentation&, orc::FrameID fid,
                    orc::IObservationContext& context) {
            if (fid == 1)
              context.set(orc::FieldID(fid * 2), "white_flag", "present", true);
          }));
  EXPECT_CALL(mockObservationService_, create_observer("biphase"))
      .WillOnce(Return(ByMove(std::move(biphase))));
  EXPECT_CALL(mockObservationService_, create_observer("white_flag"))
      .WillOnce(Return(ByMove(std::move(white_flag))));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(1UL * 1024 * 1024))
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("records.vbi"))
      .WillOnce(Return(true));

  std::vector<uint8_t> written;
  EXPECT_CALL(*pMockFileWriterUint8_,
              write(testing::A<const std::vector<uint8_t>&>()))
      .WillRepeatedly(Invoke([&written](const std::vector<uint8_t>& data) {
        written.insert(written.end(), data.begin(), data.end());
      }));
  EXPECT_CALL(*pMockFileWriterUint8_, write(testing::A<const uint8_t*>(), _))
      .WillRepeatedly(Invoke([&written](const uint8_t* data, size_t count) {
        written.insert(written.end(), data, data + count);
      }));
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  orc::ObservationContext context;
  ASSERT_TRUE(instance_->write_vbi(&mockRepresentation_, "records", context));

  constexpr size_t kHeader = 4;
  constexpr size_t kRecord = 10;
  ASSERT_EQ(written.size(), kHeader + kRecord * 6);
  EXPECT_EQ(std::vector<uint8_t>(written.begin(), written.begin() + kHeader),
            (std::vector<uint8_t>{'1', 'V', 'B', 'I'}));

  const auto record = [&written](size_t field) {
    const auto begin = written.begin() +
                       static_cast<std::ptrdiff_t>(kHeader + kRecord * field);
    return std::vector<uint8_t>(begin, begin + kRecord);
  };
  const std::vector<uint8_t> empty(kRecord, 0);
  EXPECT_EQ(record(0), empty);
  EXPECT_EQ(record(1), empty);
  EXPECT_EQ(record(2), (std::vector<uint8_t>{0x01, 0x8B, 0xA1, 0x23, 0x7F, 0xFF,
                                             0xFF, 0xF8, 0x12, 0x34}));
  EXPECT_EQ(record(3), empty);
  EXPECT_EQ(record(4), empty);
  EXPECT_EQ(record(5), empty);
}

// Without an observation service the file still holds one record per field.
TEST_F(DaphneVBISinkStageDeps, WriteVbi_WritesEmptyRecordsWithoutService) {
  instance_ = std::make_unique<orc::DaphneVBISinkStageDeps>(&mockStageServices_,
                                                            nullptr);
  instance_->init({}, &isProcessing_, &cancelRequested_);

  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillOnce(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockRepresentation_, has_frame(0)).WillOnce(Return(true));
  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(1UL * 1024 * 1024))
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("plain.vbi")).WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_,
              write(testing::A<const std::vector<uint8_t>&>()))
      .Times(1);
  EXPECT_CALL(*pMockFileWriterUint8_, write(testing::A<const uint8_t*>(), 10))
      .Times(2);
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);

  orc::ObservationContext context;
  EXPECT_TRUE(instance_->write_vbi(&mockRepresentation_, "plain", context));
}
}  // namespace orc_unit_test
