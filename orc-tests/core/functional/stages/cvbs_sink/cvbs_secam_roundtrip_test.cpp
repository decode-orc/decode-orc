/*
 * File:        cvbs_secam_roundtrip_test.cpp
 * Module:      orc-core-functional-tests
 * Purpose:     SECAM through CVBS Sink and back through SECAM CVBS Source:
 *              the .meta declares the SECAM preset as the specification
 *              requires, and the samples come back bit-exact
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * Functional (not unit): writes and reads real files in a temporary
 * directory, which unit tests may not touch (AGENTS.md §4.2).
 */

#include <core_observation_service.h>
#include <gtest/gtest.h>
#include <orc/abi/orc_plugin_services.h>
#include <orc/stage/artifact.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>
#include <sqlite3.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cvbs_sink_stage_deps.h"
#include "cvbs_source_stage.h"

namespace orc_functional_test {

namespace {

using orc::VideoSystem;
using sample_type = orc::VideoFrameRepresentation::sample_type;

constexpr size_t kWidth = orc::kSecamSamplesPerLineNominal;
constexpr size_t kHeight = orc::kSecamFrameLines;

// One SECAM frame at |sequence_frame| (1 or 2) of the 2-frame sequence:
// blanking everywhere, a picture ramp, and on every line the undeviated
// back-porch reference at fOR on D'R lines and fOB on D'B lines. In frame 1
// the even stored frame lines carry D'R (CVBS spec v1.8.1, Preset SECAM).
// That reference is all the colour-frame observer needs to place the frame.
std::vector<sample_type> secam_frame(int sequence_frame, uint64_t picture) {
  const size_t total =
      orc::frame_line_sample_offset(VideoSystem::SECAM, kWidth, kHeight);
  std::vector<sample_type> frame(total, orc::kSecamBlanking);
  for (size_t line = 0; line < kHeight; ++line) {
    const size_t start =
        orc::frame_line_sample_offset(VideoSystem::SECAM, kWidth, line);
    const size_t count =
        orc::frame_line_sample_count(VideoSystem::SECAM, kWidth, line);
    const bool dr = ((line % 2 == 0) == (sequence_frame == 1));
    const double f = dr ? orc::kSecamFor : orc::kSecamFob;
    for (size_t n = orc::kSecamSubcarrierReferenceStart;
         n < static_cast<size_t>(orc::kSecamSubcarrierReferenceEnd); ++n) {
      frame[start + n] = static_cast<sample_type>(
          std::lround(orc::kSecamBlanking +
                      80.0 * std::sin(2.0 * M_PI * f * static_cast<double>(n) /
                                      orc::kSecamSampleRate)));
    }
    for (size_t n = 200; n < count; ++n) {
      frame[start + n] =
          static_cast<sample_type>(300 + ((picture * 7 + line * 3 + n) % 500));
    }
  }
  return frame;
}

orc::SourceParameters secam_params() {
  orc::SourceParameters p;
  p.system = VideoSystem::SECAM;
  p.frame_width_nominal = static_cast<int32_t>(kWidth);
  p.frame_height = static_cast<int32_t>(kHeight);
  p.sync_tip_level = orc::kSecamSyncTip;
  p.blanking_level = orc::kSecamBlanking;
  p.black_level = orc::kSecamBlack;
  p.white_level = orc::kSecamWhite;
  p.peak_level = orc::kSecamPeak;
  p.is_subcarrier_locked = false;
  return p;
}

// SECAM frames at the given sequence positions.
class SecamVFR : public orc::VideoFrameRepresentation, public orc::Artifact {
 public:
  explicit SecamVFR(const std::vector<int>& sequence)
      : orc::Artifact(orc::ArtifactID("secam_vfr"), orc::Provenance{}) {
    for (size_t f = 0; f < sequence.size(); ++f) {
      frames_.push_back(secam_frame(sequence[f], f));
    }
  }

  std::string type_name() const override { return "SecamVFR"; }

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
    desc.system = VideoSystem::SECAM;
    desc.height = kHeight;
    desc.samples_total = frames_[id].size();
    desc.samples_per_line_nominal = kWidth;
    return desc;
  }

  std::optional<orc::SourceParameters> get_video_parameters() const override {
    return secam_params();
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

struct MetaRow {
  int user_version = -1;
  std::string preset;
  std::string signal_state_preset;
  std::optional<bool> sequence_continuous;
};

MetaRow read_meta(const std::string& path) {
  MetaRow row;
  sqlite3* db = nullptr;
  EXPECT_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr),
            SQLITE_OK);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr) ==
          SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW) {
    row.user_version = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT preset, signal_state_preset, "
                         "sequence_continuous FROM cvbs_file",
                         -1, &stmt, nullptr) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW) {
    row.preset = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    row.signal_state_preset =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
      row.sequence_continuous = sqlite3_column_int(stmt, 2) != 0;
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return row;
}

std::shared_ptr<orc::VideoFrameRepresentation> read_cvbs(
    const std::string& base) {
  orc::SECAMCVBSSourceStage source;
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

// The sink measures each frame's sequence position through the host's
// observation service. Linked rather than loaded, the stages get no host
// services, so they are installed here as the plugin loader installs them.
void install_host_observation_service() {
  static orc::CoreObservationService observation_service;
  static orc::OrcPluginServices services{};
  services.services_size =
      static_cast<uint32_t>(sizeof(orc::OrcPluginServices));
  services.observation_service = &observation_service;
  orc::plugin::set_services(&services);
}

std::filesystem::path temp_dir() {
  const auto dir =
      std::filesystem::path(::testing::TempDir()) / "orc_cvbs_secam_roundtrip";
  std::filesystem::create_directories(dir);
  return dir;
}

}  // namespace

// A continuous 1, 2, 1, 2 sequence: preset SECAM at user_version 12, never
// locked, continuous, and SECAM CVBS Source reads back the same samples.
TEST(CVBSSecamRoundTrip, ContinuousSequenceIsDeclaredAndReadBackBitExact) {
  install_host_observation_service();
  const std::string base = (temp_dir() / "continuous").string();
  const SecamVFR input({1, 2, 1, 2});
  write_cvbs(input, base);

  const MetaRow meta = read_meta(base + ".meta");
  EXPECT_EQ(meta.user_version, 12);
  EXPECT_EQ(meta.preset, "SECAM");
  EXPECT_EQ(meta.signal_state_preset, "STANDARD_STABLE_UNLOCKED");
  EXPECT_EQ(meta.sequence_continuous, std::optional<bool>(true));

  const auto output = read_cvbs(base);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->frame_count(), input.frame_count());
  const auto params = output->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  EXPECT_EQ(params->system, VideoSystem::SECAM);
  EXPECT_FALSE(params->is_subcarrier_locked);
  for (orc::FrameID f = 0; f < input.frame_count(); ++f) {
    EXPECT_EQ(output->get_frame_copy(f), input.get_frame_copy(f))
        << "frame " << f;
  }
}

// A repeated frame breaks the D'R/D'B alternation: still SECAM and unlocked
// (the preset is never downgraded), with sequence_continuous = FALSE.
TEST(CVBSSecamRoundTrip, RepeatedFrameIsDeclaredDiscontinuous) {
  install_host_observation_service();
  const std::string base = (temp_dir() / "repeat").string();
  write_cvbs(SecamVFR({1, 2, 2, 1}), base);

  const MetaRow meta = read_meta(base + ".meta");
  EXPECT_EQ(meta.user_version, 12);
  EXPECT_EQ(meta.preset, "SECAM");
  EXPECT_EQ(meta.signal_state_preset, "STANDARD_STABLE_UNLOCKED");
  EXPECT_EQ(meta.sequence_continuous, std::optional<bool>(false));
}

}  // namespace orc_functional_test
