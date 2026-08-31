/*
 * File:        multi_track_audio_roundtrip_test.cpp
 * Module:      orc-core-functional-tests
 * Purpose:     Channel-pair audio round-trip through cvbs_sink and
 *              cvbs_source (real files: 24-bit WAV sidecars and the .meta
 *              audio_channel_pair table, CVBS file format spec v1.4.0)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>

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

namespace orc_functional_test {

namespace {

// Bytes per interleaved 24-bit stereo pair (2 channels × 3 bytes).
constexpr uint64_t kAudioBytesPerPair = 6;

// Deterministic 24-bit sample generators covering the full signed 24-bit
// domain — a 24-bit container round-trips them bit-exactly.
int32_t pair0_sample(orc::FrameID frame_id, size_t index) {
  const uint64_t mixed = static_cast<uint64_t>(frame_id) * 77777u + index * 13u;
  return static_cast<int32_t>(mixed % 16777216u) - 8388608;
}

int32_t pair1_sample(orc::FrameID frame_id, size_t index) {
  const uint64_t mixed = static_cast<uint64_t>(frame_id) * 33333u + index * 7u;
  return static_cast<int32_t>(mixed % 16777216u) - 8388608;
}

// Synthetic video with two audio channel pairs: pair 0 "Analogue"
// (ANALOGUE), pair 1 "EFM digital audio" (EFM). Cadence-aware for both PAL
// and NTSC.
class ChannelPairVFR : public orc::VideoFrameRepresentation,
                       public orc::Artifact {
 public:
  ChannelPairVFR(orc::VideoSystem system, size_t frame_count,
                 size_t frame_samples, size_t height, size_t spl_nominal)
      : orc::Artifact(orc::ArtifactID("channel_pair_vfr"), orc::Provenance{}),
        system_(system),
        frame_count_(frame_count),
        frame_samples_(frame_samples),
        height_(height),
        spl_nominal_(spl_nominal) {
    for (size_t f = 0; f < frame_count_; ++f) {
      std::vector<sample_type> frame(frame_samples_);
      for (size_t i = 0; i < frame.size(); ++i) {
        frame[i] = static_cast<sample_type>((f * 17 + i) % 1024);
      }
      frames_.push_back(std::move(frame));
    }
  }

  std::string type_name() const override { return "ChannelPairVFR"; }

  orc::FrameIDRange frame_range() const override {
    return {0, static_cast<orc::FrameID>(frame_count_ - 1)};
  }
  size_t frame_count() const override { return frame_count_; }
  bool has_frame(orc::FrameID id) const override { return id < frame_count_; }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (id >= frame_count_) return std::nullopt;
    orc::FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = system_;
    desc.height = height_;
    desc.samples_total = frame_samples_;
    desc.samples_per_line_nominal = spl_nominal_;
    return desc;
  }

  const sample_type* get_frame(orc::FrameID id) const override {
    return (id < frame_count_) ? frames_[id].data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return (id < frame_count_) ? frames_[id] : std::vector<sample_type>{};
  }

  // Audio channel pairs
  size_t audio_channel_pair_count() const override { return 2; }

  std::optional<orc::AudioChannelPairDescriptor>
  get_audio_channel_pair_descriptor(size_t pair) const override {
    if (pair == 0) {
      return orc::AudioChannelPairDescriptor{"Analogue",
                                             orc::AudioOrigin::ANALOGUE};
    }
    if (pair == 1) {
      return orc::AudioChannelPairDescriptor{"EFM digital audio",
                                             orc::AudioOrigin::EFM};
    }
    return std::nullopt;
  }

  std::vector<int32_t> get_audio_samples(size_t pair,
                                         orc::FrameID id) const override {
    if (pair > 1 || id >= frame_count_) return {};
    std::vector<int32_t> samples(
        static_cast<size_t>(orc::audio_pairs_in_frame(id, system_)) * 2);
    for (size_t i = 0; i < samples.size(); ++i) {
      samples[i] = (pair == 0) ? pair0_sample(id, i) : pair1_sample(id, i);
    }
    return samples;
  }

 private:
  orc::VideoSystem system_;
  size_t frame_count_;
  size_t frame_samples_;
  size_t height_;
  size_t spl_nominal_;
  std::vector<std::vector<sample_type>> frames_;
};

// Little-endian word helpers over a WAV file's header bytes.
std::vector<uint8_t> read_file_bytes(const std::string& path, size_t count) {
  std::ifstream ifs(path, std::ios::binary);
  std::vector<uint8_t> bytes(count, 0);
  if (ifs.is_open()) {
    ifs.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(count));
  }
  return bytes;
}

uint16_t le16_at(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint16_t>(bytes[offset]) |
         (static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

uint32_t le32_at(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) |
         (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

// user_version from a SQLite database file: the 4-byte big-endian word at
// byte offset 60 of the database header (SQLite file format spec §1.3).
uint32_t read_sqlite_user_version(const std::string& path) {
  const auto bytes = read_file_bytes(path, 64);
  return (static_cast<uint32_t>(bytes[60]) << 24) |
         (static_cast<uint32_t>(bytes[61]) << 16) |
         (static_cast<uint32_t>(bytes[62]) << 8) |
         static_cast<uint32_t>(bytes[63]);
}

// Assert the CVBS spec v1.4.0 WAV properties of a channel pair file.
void expect_conformant_wav_header(const std::string& path,
                                  uint64_t expected_total_pairs) {
  const auto header = read_file_bytes(path, 44);
  EXPECT_EQ(le16_at(header, 20), 1u) << path;                       // PCM
  EXPECT_EQ(le16_at(header, 22), 2u) << path;                       // stereo
  EXPECT_EQ(le32_at(header, 24), orc::kAudioSampleRateHz) << path;  // 48 kHz
  EXPECT_EQ(le16_at(header, 34), orc::kAudioBitDepth) << path;      // 24-bit
  EXPECT_EQ(le32_at(header, 40), expected_total_pairs * kAudioBytesPerPair)
      << path;
  EXPECT_EQ(std::filesystem::file_size(path),
            44u + expected_total_pairs * kAudioBytesPerPair)
      << path;
}

// Round-trip one system through the real cvbs_sink deps and cvbs_source
// stage, verifying container conformance, descriptors, and bit-exact
// samples for every frame of the audio frame sequence.
void run_roundtrip(orc::VideoSystem system, size_t frame_count,
                   size_t frame_samples, size_t height, size_t spl_nominal,
                   orc::FixedFormatCVBSSourceStage& source,
                   const std::string& tag) {
  const std::filesystem::path dir =
      std::filesystem::path(::testing::TempDir()) /
      ("orc_channel_pair_roundtrip_" + tag);
  std::filesystem::create_directories(dir);
  const std::string base = (dir / "roundtrip").string();

  // --- Write with the real cvbs_sink deps ---
  ChannelPairVFR vfr(system, frame_count, frame_samples, height, spl_nominal);
  std::atomic<bool> cancel{false};
  orc::CVBSSinkStageDeps sink_deps;
  sink_deps.init({}, &cancel);

  orc::CVBSSinkWriteConfig config;
  config.output_base_path = base;
  const auto write_result = sink_deps.write_cvbs(&vfr, config);
  ASSERT_TRUE(write_result.success) << write_result.status_message;
  EXPECT_EQ(write_result.frames_written, frame_count);

  EXPECT_TRUE(std::filesystem::exists(base + ".cvbs"));
  ASSERT_TRUE(std::filesystem::exists(base + ".meta"));
  // Single-digit channel pair naming (CVBS spec v1.4.0); the legacy
  // two-digit names must not appear.
  ASSERT_TRUE(std::filesystem::exists(base + "_audio_0.wav"));
  ASSERT_TRUE(std::filesystem::exists(base + "_audio_1.wav"));
  EXPECT_FALSE(std::filesystem::exists(base + "_audio_00.wav"));
  EXPECT_FALSE(std::filesystem::exists(base + "_audio_2.wav"));

  // CVBS spec v1.6.0 metadata: PRAGMA user_version = 11.
  EXPECT_EQ(read_sqlite_user_version(base + ".meta"), 11u);

  // The sink's signal_state_preset reports what it measured. These synthetic
  // frames carry no colour burst and no observation service is installed
  // here, so the phase is unmeasurable and the file is written unlocked with
  // sequence_continuous unknown (CVBS spec v1.6.0); the read-back below is
  // what proves the source accepts that state.
  EXPECT_NE(write_result.status_message.find("STANDARD_STABLE_UNLOCKED"),
            std::string::npos)
      << write_result.status_message;

  // Equal-length 24-bit 48 kHz pair files: exactly
  // audio_pair_offset(frame_count) stereo pairs each.
  const uint64_t total_pairs =
      orc::audio_pair_offset(static_cast<uint64_t>(frame_count), system);
  expect_conformant_wav_header(base + "_audio_0.wav", total_pairs);
  expect_conformant_wav_header(base + "_audio_1.wav", total_pairs);

  // --- Read back with the real cvbs_source stage ---
  const std::map<std::string, orc::ParameterValue> parameters{
      {"input_path", base + ".cvbs"}};
  ASSERT_TRUE(source.set_parameters(parameters));

  orc::ObservationContext observation_context;
  const auto outputs = source.execute({}, parameters, observation_context);
  ASSERT_EQ(outputs.size(), 1u);

  const auto read_vfr =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
  ASSERT_NE(read_vfr, nullptr);
  EXPECT_EQ(read_vfr->frame_count(), frame_count);

  // The container conforms to the spec, so the read must raise no
  // audio-related warnings.
  EXPECT_FALSE(observation_context.has(orc::FieldID(0), "cvbs_source",
                                       "audio_missing_metadata_row_0"));
  EXPECT_FALSE(observation_context.has(orc::FieldID(0), "cvbs_source",
                                       "audio_length_mismatch_0"));

  // --- Descriptors round-trip via the .meta audio_channel_pair table ---
  ASSERT_EQ(read_vfr->audio_channel_pair_count(), 2u);

  const auto pair0 = read_vfr->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(pair0.has_value());
  EXPECT_EQ(pair0->name, "Analogue");
  // The container carries no origin metadata.
  EXPECT_EQ(pair0->origin, orc::AudioOrigin::UNKNOWN);

  const auto pair1 = read_vfr->get_audio_channel_pair_descriptor(1);
  ASSERT_TRUE(pair1.has_value());
  EXPECT_EQ(pair1->name, "EFM digital audio");
  EXPECT_EQ(pair1->origin, orc::AudioOrigin::UNKNOWN);

  // --- Samples round-trip per frame, bit-exact, cadence-sized ---
  for (orc::FrameID fid = 0; fid < frame_count; ++fid) {
    const size_t frame_values =
        static_cast<size_t>(orc::audio_pairs_in_frame(fid, system)) * 2;
    for (size_t pair = 0; pair < 2; ++pair) {
      const auto samples = read_vfr->get_audio_samples(pair, fid);
      ASSERT_EQ(samples.size(), frame_values)
          << tag << " pair " << pair << " frame " << fid;
      bool all_equal = true;
      for (size_t i = 0; i < samples.size(); ++i) {
        const int32_t expected =
            (pair == 0) ? pair0_sample(fid, i) : pair1_sample(fid, i);
        if (samples[i] != expected) {
          all_equal = false;
          break;
        }
      }
      EXPECT_TRUE(all_equal)
          << tag << ": samples differ in pair " << pair << " frame " << fid;
    }
  }

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace

TEST(ChannelPairAudioRoundTrip, PAL_ConstantCadence_PreservesChannelPairs) {
  // PAL: 1920 stereo pairs per frame, constant (SMPTE 272M-1994).
  orc::PALCVBSSourceStage source;
  run_roundtrip(orc::VideoSystem::PAL, 2, orc::kPalFrameSamples, 625, 1135,
                source, "pal");
}

TEST(ChannelPairAudioRoundTrip, NTSC_FiveFrameCadence_PreservesChannelPairs) {
  // NTSC: 5-frame audio frame sequence of 1602/1601/1602/1601/1602 stereo
  // pairs (SMPTE 272M-1994 §14.3); 6 frames also cover the sequence wrap.
  orc::NTSCCVBSSourceStage source;
  run_roundtrip(orc::VideoSystem::NTSC, 6, orc::kNtscFrameSamples, 525, 910,
                source, "ntsc");
}

}  // namespace orc_functional_test
