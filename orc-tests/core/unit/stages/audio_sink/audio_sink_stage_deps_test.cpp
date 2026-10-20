/*
 * File:        audio_sink_stage_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for audio sink stage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "audio_sink_stage_deps.h"

#include <gtest/gtest.h>
#include <orc/stage/audio/audio_channel_pair.h>

#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../stage_services_mock.h"

using testing::A;
using testing::Return;
using testing::StrictMock;

namespace orc_unit_test {

namespace {

orc::SourceParameters make_system_params(orc::VideoSystem system) {
  orc::SourceParameters params;
  params.system = system;
  return params;
}

std::optional<orc::AudioChannelPairDescriptor> analogue_pair_descriptor() {
  orc::AudioChannelPairDescriptor desc;
  desc.name = "Analogue";
  desc.origin = orc::AudioOrigin::ANALOGUE;
  return desc;
}

// Builds a cadence-sized frame of interleaved int32 carrier samples whose
// leading values follow |leading| and whose remainder is zero.
std::vector<int32_t> make_frame_samples(uint64_t frame_index,
                                        orc::VideoSystem system,
                                        const std::vector<int32_t>& leading) {
  std::vector<int32_t> samples(
      static_cast<size_t>(orc::audio_pairs_in_frame(frame_index, system)) * 2,
      0);
  for (size_t i = 0; i < leading.size() && i < samples.size(); ++i) {
    samples[i] = leading[i];
  }
  return samples;
}

// Reads a little-endian uint32 at the given byte offset from the header.
uint32_t header_u32_at(const std::vector<uint8_t>& bytes, size_t byte_offset) {
  return static_cast<uint32_t>(bytes[byte_offset]) |
         (static_cast<uint32_t>(bytes[byte_offset + 1]) << 8) |
         (static_cast<uint32_t>(bytes[byte_offset + 2]) << 16) |
         (static_cast<uint32_t>(bytes[byte_offset + 3]) << 24);
}

uint16_t header_u16_at(const std::vector<uint8_t>& bytes, size_t byte_offset) {
  return static_cast<uint16_t>(bytes[byte_offset] |
                               (bytes[byte_offset + 1] << 8));
}

// Reads the 24-bit signed LE sample at index |sample_index| of a payload.
int32_t s24le_at(const std::vector<uint8_t>& bytes, size_t sample_index) {
  const size_t o = sample_index * 3;
  int32_t v = static_cast<int32_t>(bytes[o]) |
              (static_cast<int32_t>(bytes[o + 1]) << 8) |
              (static_cast<int32_t>(bytes[o + 2]) << 16);
  if (v & 0x800000) {
    v -= 0x1000000;
  }
  return v;
}

constexpr size_t kWavSampleRateOffset = 24;
constexpr size_t kWavBitsPerSampleOffset = 34;
constexpr size_t kWavDataSizeOffset = 40;
constexpr size_t kBytesPerPair = 6;  // 2 × 24-bit samples

}  // namespace

class AudioSinkStageDeps : public ::testing::Test {
 public:
  void SetUp() override {
    pMockFileWriterUint8_ = std::make_shared<StrictMock<MockFileWriterUint8>>();

    instance_ = std::make_unique<orc::AudioSinkStageDeps>(&mockStageServices_);
    instance_->init({}, &isProcessing_, &cancelRequested_);

    cancelRequested_.store(false);
    isProcessing_.store(true);

    // write_audio_wav() checks this before computing the WAV header's
    // declared size (see the has_unbounded_frame_range() guard added
    // alongside the check every test here exercises the bounded/false
    // side of).
    EXPECT_CALL(mockRepresentation_, has_unbounded_frame_range())
        .WillRepeatedly(Return(false));
  }

 protected:
  void expect_writer_created_and_opened() {
    EXPECT_CALL(mockStageServices_,
                create_buffered_file_writer_uint8(4UL * 1024 * 1024))
        .Times(1)
        .WillOnce(Return(pMockFileWriterUint8_));
    EXPECT_CALL(*pMockFileWriterUint8_, open("out_path.wav"))
        .Times(1)
        .WillOnce(Return(true));
  }

  void capture_writes(std::vector<std::vector<uint8_t>>& writes, int count) {
    EXPECT_CALL(*pMockFileWriterUint8_, write(A<const std::vector<uint8_t>&>()))
        .Times(count)
        .WillRepeatedly([&writes](const std::vector<uint8_t>& data) {
          writes.push_back(data);
        });
    EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);
  }

  MockStageServices mockStageServices_;
  std::shared_ptr<StrictMock<MockFileWriterUint8>> pMockFileWriterUint8_;
  StrictMock<MockVideoFrameRepresentationArtifact> mockRepresentation_;

  std::atomic<bool> cancelRequested_{};
  std::atomic<bool> isProcessing_{};
  std::unique_ptr<orc::AudioSinkStageDeps> instance_;
};

TEST_F(AudioSinkStageDeps,
       WriteAudioWav_Declares48kHz24BitAndWritesCarrierUnconverted) {
  // One PAL frame: 1920 stereo pairs (SMPTE 272M-1994, 48000 / 25).
  constexpr uint32_t kPalPairsPerFrame = 1920;
  const std::vector<int32_t> carrier = make_frame_samples(
      0, orc::VideoSystem::PAL, {256, -512, 8388607, -8388608});

  EXPECT_CALL(mockRepresentation_, get_audio_channel_pair_descriptor(0))
      .Times(1)
      .WillOnce(Return(analogue_pair_descriptor()));
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(make_system_params(orc::VideoSystem::PAL)));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockRepresentation_, get_audio_samples(0, 0))
      .Times(1)
      .WillOnce(Return(carrier));

  expect_writer_created_and_opened();

  // One write for the 44-byte RIFF header and one for the frame payload.
  std::vector<std::vector<uint8_t>> writes;
  capture_writes(writes, 2);

  const auto result =
      instance_->write_audio_wav(&mockRepresentation_, "out_path.wav", 0);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.frames_written, kPalPairsPerFrame);
  ASSERT_EQ(writes.size(), 2U);
  ASSERT_EQ(writes[0].size(), 44U);
  // SMPTE 272M-1994 §1.2: 48 kHz for every video system.
  EXPECT_EQ(header_u32_at(writes[0], kWavSampleRateOffset), 48000U);
  // SMPTE 272M-1994 §1.3: 24-bit samples.
  EXPECT_EQ(header_u16_at(writes[0], kWavBitsPerSampleOffset), 24U);
  // Declared payload: 1920 stereo pairs × 6 bytes.
  EXPECT_EQ(header_u32_at(writes[0], kWavDataSizeOffset),
            kPalPairsPerFrame * kBytesPerPair);
  // Payload carries the int32 samples as 24-bit signed LE, unconverted.
  ASSERT_EQ(writes[1].size(),
            static_cast<size_t>(kPalPairsPerFrame) * kBytesPerPair);
  EXPECT_EQ(s24le_at(writes[1], 0), 256);
  EXPECT_EQ(s24le_at(writes[1], 1), -512);
  EXPECT_EQ(s24le_at(writes[1], 2), 8388607);
  EXPECT_EQ(s24le_at(writes[1], 3), -8388608);
  EXPECT_EQ(s24le_at(writes[1], 4), 0);
}

// "-" is passed straight through to the writer service unchanged — audio_sink
// has no extension-mangling logic to guard, unlike sinks that append their own
// extension, so this is a plain regression guard against one being added later
// without the "-" convention in mind.
TEST_F(AudioSinkStageDeps, WriteAudioWav_PipesToStdoutWithPathUnchanged) {
  constexpr uint32_t kPalPairsPerFrame = 1920;
  const std::vector<int32_t> carrier =
      make_frame_samples(0, orc::VideoSystem::PAL, {256, -512});

  EXPECT_CALL(mockRepresentation_, get_audio_channel_pair_descriptor(0))
      .Times(1)
      .WillOnce(Return(analogue_pair_descriptor()));
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(make_system_params(orc::VideoSystem::PAL)));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockRepresentation_, get_audio_samples(0, 0))
      .Times(1)
      .WillOnce(Return(carrier));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(4UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("-"))
      .Times(1)
      .WillOnce(Return(true));

  std::vector<std::vector<uint8_t>> writes;
  capture_writes(writes, 2);

  const auto result = instance_->write_audio_wav(&mockRepresentation_, "-", 0);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.frames_written, kPalPairsPerFrame);
}

TEST_F(AudioSinkStageDeps, WriteAudioWav_SilenceFramesAreSizedByNtscCadence) {
  // Two NTSC frames with no audio: silence must follow the 5-frame audio
  // frame sequence (SMPTE 272M-1994 §14.3 Table 1: frame 0 = 1602 pairs,
  // frame 1 = 1601 pairs).
  EXPECT_CALL(mockRepresentation_, get_audio_channel_pair_descriptor(0))
      .Times(1)
      .WillOnce(Return(analogue_pair_descriptor()));
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(make_system_params(orc::VideoSystem::NTSC)));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 1}));
  EXPECT_CALL(mockRepresentation_, get_audio_samples(0, 0))
      .Times(1)
      .WillOnce(Return(std::vector<int32_t>{}));
  EXPECT_CALL(mockRepresentation_, get_audio_samples(0, 1))
      .Times(1)
      .WillOnce(Return(std::vector<int32_t>{}));

  expect_writer_created_and_opened();

  // Header plus one silence write per frame.
  std::vector<std::vector<uint8_t>> writes;
  capture_writes(writes, 3);

  const auto result =
      instance_->write_audio_wav(&mockRepresentation_, "out_path.wav", 0);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.frames_written, 1602U + 1601U);
  ASSERT_EQ(writes.size(), 3U);
  EXPECT_EQ(header_u32_at(writes[0], kWavSampleRateOffset), 48000U);
  EXPECT_EQ(header_u32_at(writes[0], kWavDataSizeOffset),
            (1602U + 1601U) * kBytesPerPair);
  EXPECT_EQ(writes[1], std::vector<uint8_t>(1602 * kBytesPerPair, 0));
  EXPECT_EQ(writes[2], std::vector<uint8_t>(1601 * kBytesPerPair, 0));
}

TEST_F(AudioSinkStageDeps, WriteAudioWav_SelectedPair_ReadsThatPairOnly) {
  const std::vector<int32_t> carrier =
      make_frame_samples(0, orc::VideoSystem::PAL, {1792, -1792});

  EXPECT_CALL(mockRepresentation_, get_audio_channel_pair_descriptor(1))
      .Times(1)
      .WillOnce(Return(analogue_pair_descriptor()));
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(make_system_params(orc::VideoSystem::PAL)));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockRepresentation_, get_audio_samples(1, 0))
      .Times(1)
      .WillOnce(Return(carrier));

  expect_writer_created_and_opened();

  std::vector<std::vector<uint8_t>> writes;
  capture_writes(writes, 2);

  const auto result =
      instance_->write_audio_wav(&mockRepresentation_, "out_path.wav", 1);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.frames_written, 1920U);
  ASSERT_EQ(writes.size(), 2U);
  EXPECT_EQ(s24le_at(writes[1], 0), 1792);
  EXPECT_EQ(s24le_at(writes[1], 1), -1792);
}

TEST_F(AudioSinkStageDeps, WriteAudioWav_FailsWhenChannelPairDoesNotExist) {
  EXPECT_CALL(mockRepresentation_, get_audio_channel_pair_descriptor(3))
      .Times(1)
      .WillOnce(Return(std::nullopt));
  EXPECT_CALL(mockRepresentation_, audio_channel_pair_count())
      .Times(1)
      .WillOnce(Return(1));

  const auto result =
      instance_->write_audio_wav(&mockRepresentation_, "out_path.wav", 3);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message,
            "Audio channel pair 3 does not exist in the input (1 channel "
            "pair(s) available)");
}

TEST_F(AudioSinkStageDeps, WriteAudioWav_FailsWhenVideoSystemHasNoAudioLayout) {
  // Without a known video system there is no defined audio frame sequence,
  // so the cadence-derived payload is empty and the write fails.
  EXPECT_CALL(mockRepresentation_, get_audio_channel_pair_descriptor(0))
      .Times(1)
      .WillOnce(Return(analogue_pair_descriptor()));
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(std::nullopt));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));

  const auto result =
      instance_->write_audio_wav(&mockRepresentation_, "out_path.wav", 0);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "No audio samples found in frame range");
}

TEST_F(AudioSinkStageDeps,
       WriteAudioWav_FailsWithDiagnostic_WhenWriterServiceUnavailable) {
  orc::AudioSinkStageDeps deps_without_services(nullptr);
  deps_without_services.init({}, &isProcessing_, &cancelRequested_);

  EXPECT_CALL(mockRepresentation_, get_audio_channel_pair_descriptor(0))
      .Times(1)
      .WillOnce(Return(analogue_pair_descriptor()));
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(make_system_params(orc::VideoSystem::PAL)));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));

  const auto result = deps_without_services.write_audio_wav(
      &mockRepresentation_, "out_path.wav", 0);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "File writer service unavailable");
}

TEST_F(AudioSinkStageDeps, WriteAudioWav_Fails_WhenWriterCannotOpenFile) {
  EXPECT_CALL(mockRepresentation_, get_audio_channel_pair_descriptor(0))
      .Times(1)
      .WillOnce(Return(analogue_pair_descriptor()));
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .Times(1)
      .WillOnce(Return(make_system_params(orc::VideoSystem::PAL)));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .Times(1)
      .WillOnce(Return(orc::FrameIDRange{0, 0}));

  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(4UL * 1024 * 1024))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out_path.wav"))
      .Times(1)
      .WillOnce(Return(false));

  const auto result =
      instance_->write_audio_wav(&mockRepresentation_, "out_path.wav", 0);

  EXPECT_FALSE(result.success);
}

}  // namespace orc_unit_test
