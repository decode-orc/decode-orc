/*
 * File:        video_sink_decoder_schedule_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     End-to-end tests for VideoSinkStage's per-frame-range decoder
 *              schedule
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * Functional (not unit): writes real video files to disk through the real
 * RawOutputBackend, which unit tests may not touch (AGENTS.md §4.2).
 */

#include <core_observation_service.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/abi/orc_plugin_services.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "video_sink_stage.h"

namespace orc {
namespace {

constexpr size_t kFrameCount = 6;

// Codes the active picture of frame n rises by, so every frame differs.
constexpr int32_t kLumaStep = 24;

// Amplitude, in composite codes, of the 4fsc subcarrier pattern laid over
// the picture. A comb filter takes it out of luma as chroma; the mono
// decoder leaves it in luma, so the two decoders' output differs visibly.
constexpr int32_t kSubcarrierAmplitude = 60;

// An in-memory NTSC source: a flat picture per frame carrying a subcarrier
// pattern.
class PatternedNtscVfr : public orc::VideoFrameRepresentation,
                         public orc::Artifact {
 public:
  using sample_type = orc::VideoFrameRepresentation::sample_type;

  PatternedNtscVfr()
      : orc::Artifact(orc::ArtifactID("patterned_ntsc_vfr"), orc::Provenance{}),
        frame_samples_(static_cast<size_t>(orc::kNtscSamplesPerLine) *
                       static_cast<size_t>(orc::kNtscFrameLines)) {
    params_.system = orc::VideoSystem::NTSC;
    params_.frame_width_nominal = orc::kNtscSamplesPerLine;
    params_.frame_height = orc::kNtscFrameLines;
    params_.sync_tip_level = orc::kNtscSyncTip;
    params_.blanking_level = orc::kNtscBlanking;
    params_.black_level = orc::kNtscBlack;
    params_.white_level = orc::kNtscWhite;
    params_.peak_level = orc::kNtscPeak;
    params_.active_video_start = orc::kNtscActiveVideoStart;
    params_.active_video_end = orc::kNtscActiveVideoEnd;
    params_.first_active_frame_line = orc::kNtscFirstActiveFrameLine;
    params_.last_active_frame_line = orc::kNtscLastActiveFrameLine;

    // One subcarrier cycle is four samples at 4fsc.
    static constexpr int32_t kCycle[4] = {1, 0, -1, 0};
    for (size_t n = 0; n < kFrameCount; ++n) {
      std::vector<sample_type> frame(
          frame_samples_, static_cast<sample_type>(orc::kNtscBlanking));
      // A frame is 477,750 samples (≡ 2 mod 4), so one continuous subcarrier
      // alternates colour frames A and B, 180° apart.
      const int32_t sign = (n % 2 == 0) ? 1 : -1;
      add_burst(frame, static_cast<int>(n % 2));
      const int32_t level =
          orc::kNtscBlack + 200 + static_cast<int32_t>(n) * kLumaStep;
      for (int32_t line = params_.first_active_frame_line;
           line < params_.last_active_frame_line; ++line) {
        sample_type* row =
            frame.data() + static_cast<size_t>(line) *
                               static_cast<size_t>(orc::kNtscSamplesPerLine);
        for (int32_t x = params_.active_video_start;
             x < params_.active_video_end; ++x) {
          row[x] = static_cast<sample_type>(
              level + sign * kSubcarrierAmplitude * kCycle[(x + line * 2) % 4]);
        }
      }
      frames_.push_back(std::move(frame));
    }
  }

  // NTSC 3D needs both fields' colour_frame_phase ids, which the sink
  // measures from the burst; without one it falls back to the 2D result.
  // SMPTE 244M-2003 §4.1.2: at 0° SC/H, stored sample 125 of stored frame
  // line 9 of colour frame A is a +I (123°) sample, so the subcarrier phase
  // there is -33°.  The burst sits at 180° (SMPTE 170M-2004 §8) on every field
  // row from 8.
  static void add_burst(std::vector<sample_type>& frame, int colour_frame) {
    constexpr size_t spl = static_cast<size_t>(orc::kNtscSamplesPerLine);
    constexpr size_t kReference = 9 * spl + 125;
    constexpr double kPi = 3.14159265358979323846;
    const size_t f1 = static_cast<size_t>(orc::kNtscField1Lines);
    for (size_t row = 0; row < static_cast<size_t>(orc::kNtscFrameLines);
         ++row) {
      const size_t field_row = row < f1 ? row : row - f1;
      if (field_row < 8) continue;
      for (size_t x = static_cast<size_t>(orc::kNtscColourBurstStart);
           x < static_cast<size_t>(orc::kNtscColourBurstEnd); ++x) {
        const size_t s = row * spl + x;
        const double cycles =
            (static_cast<double>(s) - static_cast<double>(kReference)) / 4.0 +
            0.5 * colour_frame;
        const double omega_t = -33.0 * (kPi / 180.0) + 2.0 * kPi * cycles;
        frame[s] = static_cast<sample_type>(
            std::lround(orc::kNtscBlanking + 100.0 * std::sin(omega_t + kPi)));
      }
    }
  }

  std::string type_name() const override { return "PatternedNtscVfr"; }
  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0, kFrameCount - 1};
  }
  size_t frame_count() const override { return kFrameCount; }
  bool has_frame(orc::FrameID id) const override {
    return id <= frame_range().last;
  }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = id;
    d.system = orc::VideoSystem::NTSC;
    d.height = static_cast<size_t>(orc::kNtscFrameLines);
    d.samples_total = frame_samples_;
    d.samples_per_line_nominal = static_cast<size_t>(orc::kNtscSamplesPerLine);
    return d;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    if (id >= kFrameCount) return nullptr;
    return frames_[id].data();
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    const sample_type* ptr = get_frame(id);
    return ptr ? frames_[id] : std::vector<sample_type>{};
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    return params_;
  }

 private:
  size_t frame_samples_;
  std::vector<std::vector<sample_type>> frames_;
  orc::SourceParameters params_;
};

// The sink measures each frame's colour_frame_phase ids through the host's
// observation service. Linked rather than loaded, the stage gets no host
// services, so they are installed here as the plugin loader installs them.
void install_host_observation_service() {
  static orc::CoreObservationService observation_service;
  static orc::OrcPluginServices services{};
  services.services_size =
      static_cast<uint32_t>(sizeof(orc::OrcPluginServices));
  services.observation_service = &observation_service;
  orc::plugin::set_services(&services);
}

struct ExportResult {
  bool ok = false;
  std::string status;
  std::vector<std::string> frames;  // Raw YUV444P16 bytes of each frame
};

class VideoSinkDecoderScheduleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    install_host_observation_service();
    // One directory per test: ctest runs these as parallel processes, and a
    // shared directory would be removed under a test still writing to it.
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    out_dir_ =
        std::filesystem::temp_directory_path() /
        ("orc-video-sink-decoder-schedule-test-" + std::string(test->name()));
    std::filesystem::remove_all(out_dir_);
    std::filesystem::create_directories(out_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(out_dir_, ec);
  }

  // Export the whole source with decoder_type (and decoder_schedule when
  // given) and split the raw output into frames.
  ExportResult run_export(const std::string& decoder_type,
                          const std::string& schedule = {}) {
    const std::string out_path =
        (out_dir_ / ("out" + std::to_string(export_count_++) + ".yuv"))
            .string();
    orc::VideoSinkStage stage;
    ObservationContext observation_context;
    std::map<std::string, ParameterValue> params = {
        {"output_path", out_path},
        {"decoder_type", decoder_type},
        {"output_mode", std::string("raw")},
        {"raw_format", std::string("yuv")}};
    if (!schedule.empty()) params["decoder_schedule"] = schedule;

    ExportResult result;
    result.ok = stage.trigger({std::make_shared<PatternedNtscVfr>()}, params,
                              observation_context);
    result.status = stage.get_trigger_status();
    if (!result.ok || !std::filesystem::exists(out_path)) return result;

    std::ifstream in(out_path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    EXPECT_EQ(bytes.size() % kFrameCount, 0U);
    const size_t frame_bytes = bytes.size() / kFrameCount;
    for (size_t n = 0; n < kFrameCount; ++n) {
      result.frames.push_back(bytes.substr(n * frame_bytes, frame_bytes));
    }
    return result;
  }

  // Each frame of `scheduled` must be the same frame of `inside` for frames
  // first..last and of `outside` for the rest.
  static void expect_frames_from(const ExportResult& scheduled,
                                 const ExportResult& outside,
                                 const ExportResult& inside, size_t first,
                                 size_t last) {
    ASSERT_EQ(scheduled.frames.size(), kFrameCount) << scheduled.status;
    ASSERT_EQ(outside.frames.size(), kFrameCount) << outside.status;
    ASSERT_EQ(inside.frames.size(), kFrameCount) << inside.status;
    for (size_t n = 0; n < kFrameCount; ++n) {
      const bool in_range = n >= first && n <= last;
      ASSERT_NE(outside.frames[n], inside.frames[n])
          << "the two decoders must differ on frame " << n;
      EXPECT_EQ(scheduled.frames[n],
                in_range ? inside.frames[n] : outside.frames[n])
          << "frame " << n;
    }
  }

  std::filesystem::path out_dir_;
  int export_count_ = 0;
};

TEST_F(VideoSinkDecoderScheduleTest, DecodesEachFrameWithItsDecoder) {
  const auto colour = run_export("ntsc2d");
  const auto mono = run_export("mono");
  const auto scheduled = run_export("multiple", "ntsc2d,2-3:mono");

  EXPECT_TRUE(scheduled.ok) << scheduled.status;
  expect_frames_from(scheduled, colour, mono, 2, 3);
}

TEST_F(VideoSinkDecoderScheduleTest, GivesEachDecoderItsOwnLookAround) {
  // ntsc3d reads two frames either side; ntsc2d reads none. The 3D frames must
  // come out as they do in an all-3D export, the 2D ones as in an all-2D one.
  const auto three_d = run_export("ntsc3d");
  const auto two_d = run_export("ntsc2d");
  const auto scheduled = run_export("multiple", "ntsc3d,1-3:ntsc2d");

  EXPECT_TRUE(scheduled.ok) << scheduled.status;
  expect_frames_from(scheduled, three_d, two_d, 1, 3);
}

TEST_F(VideoSinkDecoderScheduleTest, UncoveredFrameIsRefusedUpFront) {
  const auto result = run_export("multiple", "0-2:ntsc2d");

  EXPECT_FALSE(result.ok);
  // Frame 3 (0-based) is the first without a decoder; users count from 1.
  EXPECT_THAT(result.status, testing::HasSubstr("no decoder for frame 4"));
}

TEST_F(VideoSinkDecoderScheduleTest, DecoderUnsuitedToSourceIsRefused) {
  const auto result = run_export("multiple", "ntsc2d,1-2:pal2d");

  EXPECT_FALSE(result.ok);
  EXPECT_THAT(result.status, testing::HasSubstr("pal2d"));
}

}  // namespace
}  // namespace orc
