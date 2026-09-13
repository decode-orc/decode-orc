/*
 * File:        preview_audio_controller_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 2 coverage of the audio-mastered preview playback session
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_audio_controller.h"

#include <gtest/gtest.h>
#include <orc/stage/audio/audio_channel_pair.h>

#include <QSignalSpy>
#include <memory>

#include "mocks/fake_audio_output.h"
#include "mocks/fake_audio_stream_reader.h"
#include "preview_audio_chase.h"

namespace {

using orc::gui::PreviewAudioController;
using orc::gui::preview_audio::kFeedChunkFrames;
using orc::gui::test::FakeAudioOutput;
using orc::gui::test::FakeAudioStreamReader;

constexpr uint32_t kRate = orc::kAudioSampleRateHz;
constexpr uint64_t kPalPairsPerFrame = 1920;

// A range long enough that the feeder never reaches the end mid-test.
constexpr orc::FrameIDRange kLongRange{0, 999};

class PreviewAudioControllerTest : public ::testing::Test {
 protected:
  void build(orc::FrameIDRange range = kLongRange,
             orc::VideoSystem system = orc::VideoSystem::PAL,
             size_t device_capacity_pairs = 9600) {
    controller_ = std::make_unique<PreviewAudioController>();
    auto output = std::make_unique<FakeAudioOutput>(device_capacity_pairs);
    output_ = output.get();
    controller_->setAudioOutput(std::move(output));
    reader_ = std::make_shared<FakeAudioStreamReader>(range, system);
    controller_->setReader(reader_);
  }

  // Play |pairs| of queued audio, topping the device back up as a running
  // feed timer would. Mirrors steady-state playback.
  void playPairsWithFeed(uint64_t pairs) {
    constexpr uint64_t kStep = 480;  // 10 ms
    for (uint64_t done = 0; done < pairs; done += kStep) {
      output_->playPairs(std::min(kStep, pairs - done));
      controller_->feed();
    }
  }

  std::unique_ptr<PreviewAudioController> controller_;
  FakeAudioOutput* output_ = nullptr;
  std::shared_ptr<FakeAudioStreamReader> reader_;
};

// ---------------------------------------------------------------------------
// Session start
// ---------------------------------------------------------------------------

TEST_F(PreviewAudioControllerTest, Start_Fails_WhenNoAudioOutputIsInstalled) {
  PreviewAudioController controller;
  controller.setReader(std::make_shared<FakeAudioStreamReader>(
      kLongRange, orc::VideoSystem::PAL));

  EXPECT_FALSE(controller.hasAudioOutput());
  EXPECT_FALSE(controller.start(0));
  EXPECT_FALSE(controller.isPlaying());
}

TEST_F(PreviewAudioControllerTest, Start_Fails_WhenNoReaderIsInstalled) {
  PreviewAudioController controller;
  controller.setAudioOutput(std::make_unique<FakeAudioOutput>());

  EXPECT_FALSE(controller.hasReader());
  EXPECT_FALSE(controller.start(0));
  EXPECT_FALSE(controller.isPlaying());
}

TEST_F(PreviewAudioControllerTest, Start_Fails_WhenTheFrameIsOutsideTheRange) {
  build(orc::FrameIDRange{10, 20});

  EXPECT_FALSE(controller_->start(9));
  EXPECT_FALSE(controller_->start(21));
  EXPECT_EQ(output_->startCalls(), 0);
  EXPECT_TRUE(controller_->start(10));
}

TEST_F(PreviewAudioControllerTest, Start_Fails_WhenTheDeviceRefusesToOpen) {
  build();
  output_->setStartSucceeds(false);

  EXPECT_FALSE(controller_->start(0));
  EXPECT_FALSE(controller_->isPlaying());
}

TEST_F(PreviewAudioControllerTest,
       Start_OpensTheDeviceAtTheStereoPipelineFormat) {
  build();
  ASSERT_TRUE(controller_->start(0));

  EXPECT_TRUE(output_->isStarted());
  EXPECT_EQ(output_->requestedSampleRateHz(), kRate);
  EXPECT_EQ(output_->requestedChannels(), 2u);
}

// ---------------------------------------------------------------------------
// Feeding
// ---------------------------------------------------------------------------

TEST_F(PreviewAudioControllerTest, Start_FillsTheDeviceBeforeTheFirstTick) {
  build();
  ASSERT_TRUE(controller_->start(0));

  // Playback must begin immediately, not one feed interval later.
  EXPECT_EQ(output_->queuedPairs(), 9600u);
}

TEST_F(PreviewAudioControllerTest,
       Feed_ReadsNoFurtherAhead_ThanOneChunkPastAFullDevice) {
  build();
  ASSERT_TRUE(controller_->start(0));
  const uint64_t after_start = output_->writtenPairs();

  controller_->feed();
  controller_->feed();

  // Nothing has played, so the device is still full: the feeder must not have
  // read more of the stream into its own buffer.
  EXPECT_EQ(output_->writtenPairs(), after_start);
  EXPECT_EQ(output_->queuedPairs(), 9600u);
}

TEST_F(PreviewAudioControllerTest, Feed_TopsTheDeviceBackUp_AsAudioPlays) {
  build();
  ASSERT_TRUE(controller_->start(0));

  output_->playPairs(4800);
  ASSERT_EQ(output_->queuedPairs(), 4800u);
  controller_->feed();

  EXPECT_EQ(output_->queuedPairs(), 9600u);
}

TEST_F(PreviewAudioControllerTest, Feed_ReadsInChunks_FromTheStartFrame) {
  build();
  reader_->clearReadRequests();
  ASSERT_TRUE(controller_->start(4));

  ASSERT_FALSE(reader_->readRequests().empty());
  EXPECT_EQ(reader_->readRequests().front().first, 4u);
  for (const auto& request : reader_->readRequests()) {
    EXPECT_EQ(request.second, kFeedChunkFrames);
  }
}

TEST_F(PreviewAudioControllerTest,
       Feed_DeliversAContiguousStream_FromTheStartFrame) {
  build();
  ASSERT_TRUE(controller_->start(3));
  playPairsWithFeed(5 * kPalPairsPerFrame);

  // Every fake sample carries its absolute stream position, so a gap or a
  // repeat anywhere in the feed path shows up here.
  const auto& written = output_->written();
  ASSERT_GT(written.size(), 0u);
  const uint64_t first_position = reader_->pairPositionForFrame(3);
  for (size_t pair = 0; pair < written.size() / 2; ++pair) {
    ASSERT_FLOAT_EQ(written[pair * 2],
                    static_cast<float>(first_position + pair))
        << "at pair " << pair;
  }
}

// ---------------------------------------------------------------------------
// Video chase
// ---------------------------------------------------------------------------

TEST_F(PreviewAudioControllerTest, TargetFrame_IsTheStartFrame_BeforeAnyAudio) {
  build();
  ASSERT_TRUE(controller_->start(12));

  EXPECT_EQ(controller_->targetFrame(), 12u);
  EXPECT_EQ(controller_->targetPreviewIndex(), 12u);
}

TEST_F(PreviewAudioControllerTest, TargetFrame_IsTheStartFrame_WhenStopped) {
  build();
  ASSERT_TRUE(controller_->start(12));
  playPairsWithFeed(3 * kPalPairsPerFrame);
  controller_->stop();

  EXPECT_EQ(controller_->targetFrame(), 12u);
}

TEST_F(PreviewAudioControllerTest, TargetFrame_FollowsThePalCadence) {
  build();
  ASSERT_TRUE(controller_->start(0));

  for (uint64_t frame = 0; frame < 6; ++frame) {
    EXPECT_EQ(controller_->targetFrame(), frame);
    playPairsWithFeed(kPalPairsPerFrame);
  }
  EXPECT_EQ(controller_->targetFrame(), 6u);
}

TEST_F(PreviewAudioControllerTest, TargetFrame_FollowsTheNtscFiveFrameCadence) {
  build(kLongRange, orc::VideoSystem::NTSC);
  ASSERT_TRUE(controller_->start(0));

  // SMPTE 272M-1994 §14.3 Table 1: 1602/1601/1602/1601/1602 over five frames.
  const uint64_t kSequence[] = {1602, 1601, 1602, 1601, 1602};
  for (uint64_t frame = 0; frame < 10; ++frame) {
    EXPECT_EQ(controller_->targetFrame(), frame) << "frame " << frame;
    playPairsWithFeed(kSequence[frame % 5]);
  }
  EXPECT_EQ(controller_->targetFrame(), 10u);
}

TEST_F(PreviewAudioControllerTest,
       TargetFrame_StaysOnAFrame_UntilItsLastPairHasPlayed) {
  build(kLongRange, orc::VideoSystem::NTSC);
  ASSERT_TRUE(controller_->start(0));

  // One pair short of the frame boundary must still show frame 0.
  playPairsWithFeed(1601);
  EXPECT_EQ(controller_->targetFrame(), 0u);
  playPairsWithFeed(1);
  EXPECT_EQ(controller_->targetFrame(), 1u);
}

TEST_F(PreviewAudioControllerTest,
       TargetPreviewIndex_AdvancesTwoPerFrame_ForFieldIndexedOutputs) {
  build();
  controller_->setItemsPerFrame(2);
  ASSERT_TRUE(controller_->start(5));

  EXPECT_EQ(controller_->targetPreviewIndex(), 10u);
  playPairsWithFeed(3 * kPalPairsPerFrame);
  EXPECT_EQ(controller_->targetFrame(), 8u);
  EXPECT_EQ(controller_->targetPreviewIndex(), 16u);
}

TEST_F(PreviewAudioControllerTest, TargetFrame_IsClampedToTheReaderRange) {
  build(orc::FrameIDRange{0, 2});
  ASSERT_TRUE(controller_->start(0));

  // Play well past the end of the material.
  playPairsWithFeed(10 * kPalPairsPerFrame);
  EXPECT_LE(controller_->targetFrame(), 2u);
}

// ---------------------------------------------------------------------------
// Seek
// ---------------------------------------------------------------------------

TEST_F(PreviewAudioControllerTest, Seek_RestartsTheDevice_WhilePlaying) {
  build();
  ASSERT_TRUE(controller_->start(0));
  playPairsWithFeed(2 * kPalPairsPerFrame);
  ASSERT_EQ(output_->startCalls(), 1);

  controller_->seek(40);

  EXPECT_TRUE(controller_->isPlaying());
  EXPECT_EQ(output_->startCalls(), 2);
  EXPECT_GE(output_->stopCalls(), 1);
  EXPECT_EQ(controller_->startFrame(), 40u);
  EXPECT_EQ(controller_->targetFrame(), 40u);
}

TEST_F(PreviewAudioControllerTest, Seek_RebasesTheClock_SoTheChaseResumes) {
  build();
  ASSERT_TRUE(controller_->start(0));
  playPairsWithFeed(2 * kPalPairsPerFrame);

  controller_->seek(40);
  playPairsWithFeed(3 * kPalPairsPerFrame);

  EXPECT_EQ(controller_->targetFrame(), 43u);
}

TEST_F(PreviewAudioControllerTest, Seek_OnlyRecordsThePosition_WhenNotPlaying) {
  build();

  controller_->seek(40);

  EXPECT_FALSE(controller_->isPlaying());
  EXPECT_EQ(output_->startCalls(), 0);
  EXPECT_EQ(controller_->startFrame(), 40u);
  EXPECT_EQ(controller_->targetFrame(), 40u);
}

// ---------------------------------------------------------------------------
// Drain and underrun
// ---------------------------------------------------------------------------

TEST_F(PreviewAudioControllerTest, Feed_ReportsFinished_WhenTheRangeDrains) {
  build(orc::FrameIDRange{0, 1});
  QSignalSpy finished(controller_.get(), &PreviewAudioController::finished);
  ASSERT_TRUE(controller_->start(0));

  // Two PAL frames fit in the device buffer, so one drain ends the session.
  output_->playAllQueued();
  controller_->feed();

  EXPECT_EQ(finished.count(), 1);
  EXPECT_FALSE(controller_->isPlaying());
  EXPECT_FALSE(output_->isStarted());
}

TEST_F(PreviewAudioControllerTest,
       Feed_DoesNotReportFinished_WhileFramesRemain) {
  build(orc::FrameIDRange{0, 199});
  QSignalSpy finished(controller_.get(), &PreviewAudioController::finished);
  ASSERT_TRUE(controller_->start(0));

  output_->playAllQueued();
  controller_->feed();

  EXPECT_EQ(finished.count(), 0);
  EXPECT_TRUE(controller_->isPlaying());
}

TEST_F(PreviewAudioControllerTest,
       Feed_ReportsAnUnderrun_WhenTheDeviceRunsDryMidStream) {
  build(orc::FrameIDRange{0, 199});
  QSignalSpy underrun(controller_.get(),
                      &PreviewAudioController::underrunDetected);
  ASSERT_TRUE(controller_->start(0));

  output_->playAllQueued();
  controller_->feed();

  EXPECT_EQ(underrun.count(), 1);
  EXPECT_EQ(controller_->underrunCount(), 1u);
}

TEST_F(PreviewAudioControllerTest, Feed_RefillsAndResumes_AfterAnUnderrun) {
  build(orc::FrameIDRange{0, 199});
  ASSERT_TRUE(controller_->start(0));
  const uint64_t starved_target = 9600 / kPalPairsPerFrame;

  output_->playAllQueued();
  controller_->feed();
  // The clock stalls with the device, so the chase stalls rather than drifts.
  EXPECT_EQ(controller_->targetFrame(), starved_target);
  EXPECT_GT(output_->queuedPairs(), 0u);

  // The stream stays contiguous across the gap, so the chase picks up exactly
  // where it stopped.
  playPairsWithFeed(2 * kPalPairsPerFrame);
  EXPECT_EQ(controller_->targetFrame(), starved_target + 2);
  EXPECT_EQ(controller_->underrunCount(), 1u);
}

TEST_F(PreviewAudioControllerTest,
       Feed_ReportsAnUnderrun_OncePerStarvationNotPerTick) {
  build(orc::FrameIDRange{0, 199});
  QSignalSpy underrun(controller_.get(),
                      &PreviewAudioController::underrunDetected);
  ASSERT_TRUE(controller_->start(0));

  output_->playAllQueued();
  controller_->feed();
  ASSERT_EQ(underrun.count(), 1);

  // Ticks where the device kept up must stay silent.
  output_->playPairs(480);
  controller_->feed();
  output_->playPairs(480);
  controller_->feed();
  EXPECT_EQ(underrun.count(), 1);

  // A fresh starvation is reported again.
  output_->playAllQueued();
  controller_->feed();
  EXPECT_EQ(underrun.count(), 2);
}

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------

TEST_F(PreviewAudioControllerTest, Stop_ClosesTheDevice_AndKeepsTheReader) {
  build();
  ASSERT_TRUE(controller_->start(0));

  controller_->stop();

  EXPECT_FALSE(controller_->isPlaying());
  EXPECT_FALSE(output_->isStarted());
  EXPECT_TRUE(controller_->hasReader());
}

TEST_F(PreviewAudioControllerTest, SetReader_StopsPlayback_OnANodeOrDagChange) {
  build();
  ASSERT_TRUE(controller_->start(0));

  controller_->setReader(std::make_shared<FakeAudioStreamReader>(
      kLongRange, orc::VideoSystem::PAL));

  EXPECT_FALSE(controller_->isPlaying());
  EXPECT_FALSE(output_->isStarted());
}

TEST_F(PreviewAudioControllerTest,
       SetReader_ClearsPlayback_WhenTheAudioGoesAway) {
  build();
  ASSERT_TRUE(controller_->start(0));

  controller_->setReader(nullptr);

  EXPECT_FALSE(controller_->isPlaying());
  EXPECT_FALSE(controller_->hasReader());
  EXPECT_FALSE(controller_->start(0));
}

TEST_F(PreviewAudioControllerTest,
       SetReader_ClampsTheStartFrame_ToTheNewRange) {
  build();
  ASSERT_TRUE(controller_->start(500));
  controller_->stop();

  controller_->setReader(std::make_shared<FakeAudioStreamReader>(
      orc::FrameIDRange{0, 9}, orc::VideoSystem::PAL));

  EXPECT_EQ(controller_->startFrame(), 0u);
}

TEST_F(PreviewAudioControllerTest, SetAudioOutput_StopsPlayback) {
  build();
  ASSERT_TRUE(controller_->start(0));
  // The controller owns its output, so installing a replacement destroys the
  // device being replaced. The count has to be held apart from it to be read
  // once the handover is done.
  const std::shared_ptr<const int> previous_stops = output_->stopCallsHandle();

  controller_->setAudioOutput(std::make_unique<FakeAudioOutput>());

  EXPECT_FALSE(controller_->isPlaying());
  EXPECT_EQ(*previous_stops, 1);
}

// ---------------------------------------------------------------------------
// Volume and mute
// ---------------------------------------------------------------------------

TEST_F(PreviewAudioControllerTest, SetVolume_ForwardsASquaredTaper) {
  build();

  controller_->setVolume(0.5);

  EXPECT_DOUBLE_EQ(controller_->volume(), 0.5);
  EXPECT_DOUBLE_EQ(output_->volume(), 0.25);
}

TEST_F(PreviewAudioControllerTest, SetVolume_ClampsToTheControlRange) {
  build();

  controller_->setVolume(1.5);
  EXPECT_DOUBLE_EQ(controller_->volume(), 1.0);
  EXPECT_DOUBLE_EQ(output_->volume(), 1.0);

  controller_->setVolume(-0.5);
  EXPECT_DOUBLE_EQ(controller_->volume(), 0.0);
  EXPECT_DOUBLE_EQ(output_->volume(), 0.0);
}

TEST_F(PreviewAudioControllerTest,
       SetMuted_SilencesTheDevice_AndUnmuteRestores) {
  build();
  controller_->setVolume(0.5);

  controller_->setMuted(true);
  EXPECT_TRUE(controller_->isMuted());
  EXPECT_DOUBLE_EQ(output_->volume(), 0.0);

  controller_->setMuted(false);
  EXPECT_FALSE(controller_->isMuted());
  EXPECT_DOUBLE_EQ(output_->volume(), 0.25);
}

TEST_F(PreviewAudioControllerTest, SetMuted_LeavesTheClockAndChaseRunning) {
  build();
  ASSERT_TRUE(controller_->start(0));
  controller_->setMuted(true);

  playPairsWithFeed(3 * kPalPairsPerFrame);

  EXPECT_TRUE(controller_->isPlaying());
  EXPECT_EQ(controller_->targetFrame(), 3u);
}

TEST_F(PreviewAudioControllerTest, Volume_IsRetainedAcrossSessions) {
  build();
  controller_->setVolume(0.5);
  controller_->setMuted(true);

  ASSERT_TRUE(controller_->start(0));

  EXPECT_DOUBLE_EQ(output_->volume(), 0.0);
  controller_->setMuted(false);
  EXPECT_DOUBLE_EQ(output_->volume(), 0.25);
}

}  // namespace
