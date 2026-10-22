/*
 * File:        stack_alignment_roundtrip_test.cpp
 * Module:      orc-core-functional-tests
 * Purpose:     PAL captures of unequal length through cvbs_source,
 *              source_align (leading and trailing padding), stacker and
 *              cvbs_sink: the export must keep every picture and every frame
 *              must start on a line boundary
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cvbs_sink_stage_deps.h"
#include "cvbs_source_stage.h"
#include "source_align_stage.h"
#include "stacker_stage.h"

namespace orc_functional_test {

namespace {

using sample_type = orc::VideoFrameRepresentation::sample_type;

constexpr size_t kWidth = 1135;
constexpr size_t kHeight = 625;
constexpr uint64_t kFrameBytes = 1418758;  // 709,379 int16 samples

// Every line opens with a sync-level run, so a frame that starts anywhere but
// on a line boundary is visible in the raw file.
constexpr sample_type kSync = 64;
constexpr size_t kSyncSamples = 84;

// The disc picture both captures carry: the value of sample n of line |line|
// of picture |picture|. Broadband after the sync run, so the stacker's grid
// measurement has a single clear peak at zero.
sample_type picture_sample(uint64_t picture, size_t line, size_t n) {
  if (n < kSyncSamples) return kSync;
  uint64_t h = picture * 0x9e3779b97f4a7c15ULL +
               static_cast<uint64_t>(line) * 6364136223846793005ULL +
               static_cast<uint64_t>(n) * 1442695040888963407ULL;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 29;
  return static_cast<sample_type>(300 + static_cast<int>(h % 512));
}

std::vector<sample_type> picture_frame(uint64_t picture) {
  const size_t total =
      orc::frame_line_sample_offset(orc::VideoSystem::PAL, kWidth, kHeight);
  std::vector<sample_type> frame(total);
  for (size_t line = 0; line < kHeight; ++line) {
    const size_t start =
        orc::frame_line_sample_offset(orc::VideoSystem::PAL, kWidth, line);
    const size_t count =
        orc::frame_line_sample_count(orc::VideoSystem::PAL, kWidth, line);
    for (size_t n = 0; n < count; ++n) {
      frame[start + n] = picture_sample(picture, line, n);
    }
  }
  return frame;
}

// A capture of disc pictures [first_picture, first_picture + frames).
class CaptureVFR : public orc::VideoFrameRepresentation, public orc::Artifact {
 public:
  CaptureVFR(uint64_t first_picture, size_t frames)
      : orc::Artifact(orc::ArtifactID("capture_vfr"), orc::Provenance{}) {
    for (size_t f = 0; f < frames; ++f) {
      frames_.push_back(picture_frame(first_picture + f));
    }
  }

  std::string type_name() const override { return "CaptureVFR"; }

  orc::FrameIDRange frame_range() const override {
    return {0, static_cast<orc::FrameID>(frames_.size() - 1)};
  }
  size_t frame_count() const override { return frames_.size(); }
  bool has_frame(orc::FrameID id) const override { return id < frames_.size(); }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = orc::VideoSystem::PAL;
    desc.height = kHeight;
    desc.samples_total = frames_[id].size();
    desc.samples_per_line_nominal = kWidth;
    return desc;
  }

  const sample_type* get_frame(orc::FrameID id) const override {
    return has_frame(id) ? frames_[id].data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return has_frame(id) ? frames_[id] : std::vector<sample_type>{};
  }

 private:
  std::vector<std::vector<sample_type>> frames_;
};

void write_cvbs(const orc::VideoFrameRepresentation& vfr,
                const std::string& base) {
  std::atomic<bool> cancel{false};
  orc::CVBSSinkStageDeps sink_deps;
  sink_deps.init({}, &cancel);
  orc::CVBSSinkWriteConfig config;
  config.output_base_path = base;
  const auto result = sink_deps.write_cvbs(&vfr, config);
  ASSERT_TRUE(result.success) << result.status_message;
  EXPECT_EQ(result.frames_written, vfr.frame_count());
}

std::shared_ptr<orc::VideoFrameRepresentation> read_cvbs(
    orc::PALCVBSSourceStage& source, const std::string& base) {
  const std::map<std::string, orc::ParameterValue> parameters{
      {"input_path", base + ".cvbs"}};
  EXPECT_TRUE(source.set_parameters(parameters));
  orc::ObservationContext observation_context;
  const auto outputs = source.execute({}, parameters, observation_context);
  EXPECT_EQ(outputs.size(), 1u);
  return outputs.empty()
             ? nullptr
             : std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(
                   outputs[0]);
}

std::vector<sample_type> read_file_samples(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  const auto bytes = std::filesystem::file_size(path);
  std::vector<sample_type> samples(bytes / sizeof(sample_type));
  ifs.read(reinterpret_cast<char*>(samples.data()),
           static_cast<std::streamsize>(samples.size() * sizeof(sample_type)));
  return samples;
}

// Frames [first_frame, first_frame + pictures.size()) of a flat PAL file
// start on a line boundary and carry |pictures| in order.
void expect_pictures(const std::vector<sample_type>& samples,
                     size_t first_frame, const std::vector<uint64_t>& pictures) {
  const size_t frame_samples = kFrameBytes / sizeof(sample_type);
  ASSERT_GE(samples.size(), (first_frame + pictures.size()) * frame_samples);
  for (size_t i = 0; i < pictures.size(); ++i) {
    const size_t frame_base = (first_frame + i) * frame_samples;
    const auto expected = picture_frame(pictures[i]);
    size_t misplaced_lines = 0;
    size_t wrong_samples = 0;
    for (size_t line = 0; line < kHeight; ++line) {
      const size_t start =
          orc::frame_line_sample_offset(orc::VideoSystem::PAL, kWidth, line);
      if (samples[frame_base + start] != kSync ||
          samples[frame_base + start + kSyncSamples] == kSync) {
        ++misplaced_lines;
      }
    }
    for (size_t n = 0; n < frame_samples; ++n) {
      if (samples[frame_base + n] != expected[n]) ++wrong_samples;
    }
    EXPECT_EQ(misplaced_lines, 0u) << "frame " << first_frame + i;
    EXPECT_EQ(wrong_samples, 0u) << "frame " << first_frame + i;
  }
}

}  // namespace

// Capture A holds pictures 0-7 and capture B pictures 3-8. Aligned with
// "1+0, 2+3", B is padded at the head and A at the tail, and the stack holds
// pictures 0-8. Short PAL padding frames used to shift every later frame of
// an export by 4 samples, and the stack used to end with input 1 and lose
// picture 8.
TEST(StackAlignmentRoundTrip, PAL_PaddedStackExportsWholeFrames) {
  const std::filesystem::path dir =
      std::filesystem::path(::testing::TempDir()) / "orc_stack_alignment";
  std::filesystem::create_directories(dir);
  const std::string base_a = (dir / "capture_a").string();
  const std::string base_b = (dir / "capture_b").string();
  const std::string base_out = (dir / "stacked").string();

  write_cvbs(CaptureVFR(0, 8), base_a);
  write_cvbs(CaptureVFR(3, 6), base_b);

  orc::PALCVBSSourceStage source_a;
  orc::PALCVBSSourceStage source_b;
  const auto read_a = read_cvbs(source_a, base_a);
  const auto read_b = read_cvbs(source_b, base_b);
  ASSERT_NE(read_a, nullptr);
  ASSERT_NE(read_b, nullptr);

  orc::SourceAlignStage align;
  ASSERT_TRUE(
      align.set_parameters({{"alignmentMode", std::string("pad_for_alignment")},
                            {"alignmentMap", std::string("1+0, 2+3")}}));
  orc::ObservationContext ctx;
  const auto aligned = align.execute(
      {std::dynamic_pointer_cast<orc::Artifact>(read_a),
       std::dynamic_pointer_cast<orc::Artifact>(read_b)},
      {}, ctx);
  ASSERT_EQ(aligned.size(), 2u);

  std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>> inputs;
  for (const auto& a : aligned) {
    auto vfr = std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(a);
    ASSERT_NE(vfr, nullptr);
    EXPECT_EQ(vfr->frame_count(), 9u);
    inputs.push_back(vfr);
  }

  // The aligned capture B on its own: three leading padding frames, then
  // pictures 3-8. Padding written short shifts every frame after it.
  const std::string base_padded = (dir / "aligned_b").string();
  write_cvbs(*inputs[1], base_padded);
  const std::string padded_path = base_padded + ".cvbs";
  ASSERT_EQ(std::filesystem::file_size(padded_path), 9u * kFrameBytes);
  expect_pictures(read_file_samples(padded_path), 3, {3, 4, 5, 6, 7, 8});

  orc::StackerStage stacker;
  ASSERT_TRUE(stacker.set_parameters({{"mode", std::string("Mean")}}));
  const auto stacked = stacker.process(inputs);
  ASSERT_NE(stacked, nullptr);
  ASSERT_EQ(stacked->frame_count(), 9u);

  write_cvbs(*stacked, base_out);

  const std::string out_path = base_out + ".cvbs";
  ASSERT_EQ(std::filesystem::file_size(out_path), 9u * kFrameBytes);

  expect_pictures(read_file_samples(out_path), 0,
                  {0, 1, 2, 3, 4, 5, 6, 7, 8});

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace orc_functional_test
