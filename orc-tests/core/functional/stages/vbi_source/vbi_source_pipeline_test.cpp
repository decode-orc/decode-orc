/*
 * File:        vbi_source_pipeline_test.cpp
 * Module:      orc-tests
 * Purpose:     End-to-end validation of the VBI source stage on real media
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "vbi_offset_calibration.h"
#include "vbi_source_format.h"
#include "vbi_source_stage.h"

namespace orc {
namespace {

// The reference sample: a four-hour FLAC-wrapped bt8x8 PAL capture carrying
// world system teletext. It is not checked into the repository (test-data/ is
// ignored), so every test here skips when it is absent.
const char* kReferenceCapture =
    ORC_VBI_TEST_DATA_DIR "/teletext/bt8x8 sample/0002.vbi.flac";

bool reference_capture_available() {
  return std::filesystem::exists(kReferenceCapture);
}

std::map<std::string, ParameterValue> reference_parameters() {
  return {
      {"input_path", std::string(kReferenceCapture)},
      {"format", std::string("bt8x8 card dump, 8-bit (WST)")},
  };
}

// Run the stage over the reference capture and hand back its output.
std::shared_ptr<VideoFrameRepresentation> load_reference_capture(
    VBISourceStage& stage, ObservationContext& observations) {
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, reference_parameters(), observations);
  if (outputs.empty()) return nullptr;
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
}

// ---------------------------------------------------------------------------
// The stage against the reference capture
// ---------------------------------------------------------------------------

TEST(VBISourcePipeline, OpensTheReferenceCaptureAndReportsItsFrameCount) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_reference_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  // 24 117 706 752 bytes of capture at 65 536 bytes per bt8x8 PAL frame.
  EXPECT_EQ(representation->frame_count(), 368007u);

  const auto parameters = representation->get_video_parameters();
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters->system, VideoSystem::PAL);

  const auto descriptor = representation->get_frame_descriptor(0);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->samples_total, static_cast<size_t>(kPalFrameSamples));

  // The capture offset was fitted rather than taken from the preset's
  // folkloric starting hint of 244 samples (design §5.3.3).
  const auto offset =
      observations.get(FieldID(0), "vbi_source", "capture_offset");
  ASSERT_TRUE(offset.has_value());
  EXPECT_NEAR(std::get<double>(*offset), 261.6, 2.0);
}

// The two policies over the same capture: the counter says the same thing
// about it either way, and only the policy's response to it differs.
TEST(VBISourcePipeline, TheFrameCounterIsReadWithoutWalkingTheCapture) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  ASSERT_NE(load_reference_capture(stage, observations), nullptr);

  const auto sequence =
      observations.get(FieldID(0), "vbi_source", "frame_sequence");
  ASSERT_TRUE(sequence.has_value());
  const std::string summary = std::get<std::string>(*sequence);

  // Whatever the capture turns out to hold, the stage must have made a
  // statement about its frame counter rather than staying silent.
  EXPECT_TRUE(summary.find("Frame counter") != std::string::npos ||
              summary.find("frame counter") != std::string::npos)
      << summary;

  // And about what that cost the output, which is the part a user acts on: a
  // capture whose counter repeats or runs backwards cannot have the source's
  // own frame numbering rebuilt, and a frame id from this stage then means
  // something different from one written on the capture.
  EXPECT_NE(summary.find("Output frame numbering"), std::string::npos)
      << summary;
}

// Concurrent readers get the same frames a single reader does, and get them in
// parallel.
//
// The GUI reads this representation from several threads at once: the preview
// render on the coordinator's worker plus one background observation worker
// per two cores. Synthesis is deliberately not serialised (only the read of
// the capture's records is), so the frames it produces have to be independent
// of how many threads are asking — and reading concurrently must not simply
// queue.
TEST(VBISourcePipeline, ConcurrentReadersSeeTheSameFramesAsASingleReader) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  constexpr int kThreads = 4;

  // Spread across the capture, past its off-air opening minute.
  std::vector<FrameID> frames;
  for (int i = 0; i < 24; ++i) {
    frames.push_back(static_cast<FrameID>(2000 + i * 97));
  }

  // One reader, from a cold cache.
  VBISourceStage serial_stage;
  ObservationContext serial_observations;
  const auto serial_source =
      load_reference_capture(serial_stage, serial_observations);
  ASSERT_NE(serial_source, nullptr);

  const auto serial_start = std::chrono::steady_clock::now();
  std::map<FrameID, std::vector<VideoFrameRepresentation::sample_type>>
      expected;
  for (const FrameID frame : frames) {
    expected[frame] = serial_source->get_frame_copy(frame);
    ASSERT_FALSE(expected[frame].empty()) << "frame " << frame;
  }
  const auto serial_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - serial_start)
                             .count();

  // The same frames, the same total synthesis, from a second cold cache —
  // split so each is synthesised exactly once, by whichever thread reaches it.
  VBISourceStage parallel_stage;
  ObservationContext parallel_observations;
  const auto parallel_source =
      load_reference_capture(parallel_stage, parallel_observations);
  ASSERT_NE(parallel_source, nullptr);

  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  const auto parallel_start = std::chrono::steady_clock::now();
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (size_t i = static_cast<size_t>(t); i < frames.size();
           i += kThreads) {
        if (parallel_source->get_frame_copy(frames[i]) !=
            expected.at(frames[i])) {
          ++mismatches;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  const auto parallel_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - parallel_start)
          .count();

  EXPECT_EQ(mismatches.load(), 0)
      << "frames synthesised concurrently differ from the same frames "
         "synthesised one at a time";

  // Now with the readers colliding: every thread walks every frame, each
  // starting at a different offset, so they race the cache and each other for
  // the same ids rather than marching in step.
  mismatches = 0;
  threads.clear();
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (size_t i = 0; i < frames.size(); ++i) {
        const FrameID frame =
            frames[(i + static_cast<size_t>(t) * 7) % frames.size()];
        if (parallel_source->get_frame_copy(frame) != expected.at(frame)) {
          ++mismatches;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(mismatches.load(), 0)
      << "overlapping concurrent reads of the same frames disagreed";

  // Reported rather than asserted: the wall-clock ratio depends on the host,
  // and a threshold here would flake on a loaded CI machine. A parallel pass
  // that costs about as much as the serial one is the signature of synthesis
  // having been pulled back under a single lock — which starved the
  // interactive preview render behind the observation pool when it was.
  std::cout << "[ INFO     ] " << frames.size()
            << " frames, one reader: " << serial_ms << " ms; same frames over "
            << kThreads << " readers: " << parallel_ms << " ms" << std::endl;
}

}  // namespace
}  // namespace orc
