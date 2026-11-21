/*
 * File:        audio_import_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for AudioImportStage (WAV validation, ingest
 *              conversion to synchronous 48 kHz 24-bit channel pairs, cap
 *              enforcement)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/audio_import/audio_import_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "../../include/video_frame_representation_artifact_mock.h"
#include "audio-resample/audio_resampler.h"
#include "audio-resample/synchronous_audio_cache.h"
#include "audio-resample/wav_header.h"

namespace orc_unit_test {
namespace {

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

class MockAudioImportDeps : public orc::IAudioImportDeps {
 public:
  MOCK_METHOD(orc::WavProbeResult, open, (const std::string& wav_path),
              (override));
  MOCK_METHOD((std::vector<int32_t>), read_pairs,
              (uint64_t first_pair, size_t pair_count), (const, override));
  MOCK_METHOD(std::unique_ptr<orc::ISynchronousAudioStore>, make_audio_store,
              (), (const, override));

  // Serve |samples| (interleaved carrier values) as the WAV's data chunk.
  void serve(std::vector<int32_t> samples) {
    auto data = std::make_shared<std::vector<int32_t>>(std::move(samples));
    ON_CALL(*this, read_pairs(_, _))
        .WillByDefault([data](uint64_t first, size_t count) {
          const uint64_t total = data->size() / 2;
          const uint64_t begin = std::min<uint64_t>(first, total);
          const uint64_t end = std::min<uint64_t>(first + count, total);
          return std::vector<int32_t>(
              data->begin() + static_cast<std::ptrdiff_t>(begin * 2),
              data->begin() + static_cast<std::ptrdiff_t>(end * 2));
        });
    ON_CALL(*this, make_audio_store()).WillByDefault([] {
      return orc::make_memory_audio_store();
    });
  }
};

const orc::ParameterDescriptor* find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descs,
    const std::string& name) {
  auto it = std::find_if(
      descs.begin(), descs.end(),
      [&](const orc::ParameterDescriptor& d) { return d.name == name; });
  return it == descs.end() ? nullptr : &(*it);
}

// PAL source with 2 frames and one existing audio channel pair.
std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>>
make_pal_source() {
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  orc::SourceParameters params;
  params.system = orc::VideoSystem::PAL;
  ON_CALL(*vfr, get_video_parameters()).WillByDefault(Return(params));
  ON_CALL(*vfr, frame_count()).WillByDefault(Return(2u));
  ON_CALL(*vfr, frame_range())
      .WillByDefault(
          Return(orc::FrameIDRange{orc::FrameID(0), orc::FrameID(1)}));
  ON_CALL(*vfr, has_frame(_)).WillByDefault(Return(true));
  ON_CALL(*vfr, audio_channel_pair_count()).WillByDefault(Return(1u));
  return vfr;
}

std::shared_ptr<const orc::VideoFrameRepresentation> run(
    orc::AudioImportStage& stage, orc::ArtifactPtr input,
    const std::map<std::string, orc::ParameterValue>& params) {
  orc::ObservationContext ctx;
  auto outputs = stage.execute({std::move(input)}, params, ctx);
  EXPECT_EQ(outputs.size(), 1u);
  auto output = std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs[0]);
  EXPECT_NE(output, nullptr);
  return output;
}

TEST(AudioImportStageTest, NodeTypeInfo_ReportsTransformWithOneInput) {
  orc::AudioImportStage stage;
  const auto info = stage.get_node_type_info();

  EXPECT_EQ(info.type, orc::NodeType::TRANSFORM);
  EXPECT_EQ(info.stage_name, "audio_import");
  EXPECT_EQ(stage.required_input_count(), 1u);
  EXPECT_EQ(stage.output_count(), 1u);
}

TEST(AudioImportStageTest, Descriptors_DefaultsRoundTripThroughSetGet) {
  orc::AudioImportStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  const auto* wav_path = find_descriptor(descriptors, "wav_path");
  const auto* pair_name = find_descriptor(descriptors, "pair_name");
  ASSERT_NE(wav_path, nullptr);
  ASSERT_NE(pair_name, nullptr);
  EXPECT_EQ(wav_path->type, orc::ParameterType::FILE_PATH);
  EXPECT_TRUE(wav_path->constraints.required);
  EXPECT_EQ(wav_path->file_extension_hint, ".wav");

  // The lock_mode parameter is gone: audio is always synchronous 48 kHz.
  EXPECT_EQ(find_descriptor(descriptors, "lock_mode"), nullptr);
  EXPECT_FALSE(stage.set_parameters({{"lock_mode", std::string("auto")}}));

  EXPECT_TRUE(stage.set_parameters({{"wav_path", std::string("/tmp/a.wav")},
                                    {"pair_name", std::string("Commentary")}}));
  const auto params = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(params.at("wav_path")), "/tmp/a.wav");
  EXPECT_EQ(std::get<std::string>(params.at("pair_name")), "Commentary");
}

TEST(AudioImportStageTest, Execute_ThrowsWhenWavPathUnset) {
  orc::AudioImportStage stage;
  auto vfr = make_pal_source();
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({vfr}, {}, ctx), orc::DAGExecutionError);
}

TEST(AudioImportStageTest, Execute_ThrowsWhenPairCapReached) {
  orc::AudioImportStage stage;
  auto vfr = make_pal_source();
  ON_CALL(*vfr, audio_channel_pair_count())
      .WillByDefault(Return(orc::kMaxAudioChannelPairs));

  orc::ObservationContext ctx;
  EXPECT_THROW(
      stage.execute({vfr}, {{"wav_path", std::string("/tmp/a.wav")}}, ctx),
      orc::DAGExecutionError);
}

TEST(AudioImportStageTest, Execute_ThrowsWhenWavInvalid) {
  orc::AudioImportStage stage;
  auto deps = std::make_shared<NiceMock<MockAudioImportDeps>>();
  stage.set_deps_override(deps);
  ON_CALL(*deps, open(_))
      .WillByDefault(Return(orc::WavProbeResult{false, "not stereo", 0, 0, 0}));

  auto vfr = make_pal_source();
  orc::ObservationContext ctx;
  EXPECT_THROW(
      stage.execute({vfr}, {{"wav_path", std::string("/tmp/a.wav")}}, ctx),
      orc::DAGExecutionError);
}

TEST(AudioImportStageTest, Execute_ThrowsWhenVideoSystemUnknown) {
  orc::AudioImportStage stage;
  auto deps = std::make_shared<NiceMock<MockAudioImportDeps>>();
  stage.set_deps_override(deps);
  ON_CALL(*deps, open(_))
      .WillByDefault(Return(orc::WavProbeResult{true, "", 48000, 16, 1000}));

  auto vfr = make_pal_source();
  ON_CALL(*vfr, get_video_parameters())
      .WillByDefault(Return(std::optional<orc::SourceParameters>()));

  orc::ObservationContext ctx;
  EXPECT_THROW(
      stage.execute({vfr}, {{"wav_path", std::string("/tmp/a.wav")}}, ctx),
      orc::DAGExecutionError);
}

TEST(AudioImportStageTest, Import_Converts16BitWavToSynchronousPairs) {
  orc::AudioImportStage stage;
  auto deps = std::make_shared<NiceMock<MockAudioImportDeps>>();
  stage.set_deps_override(deps);

  // A 48000 Hz 16-bit WAV of exactly 2 frames × 1920 PAL pairs passes
  // through unchanged after the deps' 16→24-bit widening; no store is made.
  constexpr size_t kPairs = 2u * 1920u;
  std::vector<int16_t> raw(kPairs * 2);
  for (size_t i = 0; i < raw.size(); ++i) {
    raw[i] = static_cast<int16_t>(static_cast<int>(i % 1000) - 500);
  }
  ON_CALL(*deps, open(_))
      .WillByDefault(Return(orc::WavProbeResult{true, "", 48000, 16, kPairs}));
  deps->serve(orc::AudioResampler::widen_16_to_24(raw));
  EXPECT_CALL(*deps, make_audio_store()).Times(0);

  auto vfr = make_pal_source();
  auto output =
      run(stage, vfr, {{"wav_path", std::string("/tmp/My Pair.wav")}});

  ASSERT_EQ(output->audio_channel_pair_count(), 2u);
  const auto desc = output->get_audio_channel_pair_descriptor(1);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->name, "My Pair");  // file stem when pair_name is empty
  EXPECT_EQ(desc->origin, orc::AudioOrigin::IMPORTED);

  const auto frame0 = output->get_audio_samples(1, orc::FrameID(0));
  const auto frame1 = output->get_audio_samples(1, orc::FrameID(1));
  ASSERT_EQ(frame0.size(), 1920u * 2u);
  ASSERT_EQ(frame1.size(), 1920u * 2u);
  // Widening is a 256× scale (<< 8, sign-preserving).
  for (size_t i = 0; i < frame0.size(); ++i) {
    EXPECT_EQ(frame0[i], static_cast<int32_t>(raw[i]) * 256) << "index " << i;
  }
  EXPECT_EQ(frame1.front(), static_cast<int32_t>(raw[1920u * 2u]) * 256);
  EXPECT_EQ(frame1.back(), static_cast<int32_t>(raw.back()) * 256);
}

TEST(AudioImportStageTest, Import_Converts24BitInputPath) {
  orc::AudioImportStage stage;
  auto deps = std::make_shared<NiceMock<MockAudioImportDeps>>();
  stage.set_deps_override(deps);

  // 24-bit material is already in the int32 carrier: no widening, and the
  // 48000 Hz rate passes through the resampler unchanged. Short material is
  // silence-padded to the cadence size.
  const std::vector<int32_t> raw{100000, -100000, 8388607, -8388608};
  ON_CALL(*deps, open(_))
      .WillByDefault(Return(orc::WavProbeResult{true, "", 48000, 24, 2}));
  deps->serve(raw);

  auto vfr = make_pal_source();
  auto output = run(stage, vfr, {{"wav_path", std::string("/tmp/a.wav")}});

  const auto frame0 = output->get_audio_samples(1, orc::FrameID(0));
  ASSERT_EQ(frame0.size(), 1920u * 2u);
  EXPECT_EQ(frame0[0], 100000);
  EXPECT_EQ(frame0[1], -100000);
  EXPECT_EQ(frame0[2], 8388607);
  EXPECT_EQ(frame0[3], -8388608);
  EXPECT_EQ(frame0[4], 0);  // silence-padded past end of material

  const auto frame1 = output->get_audio_samples(1, orc::FrameID(1));
  ASSERT_EQ(frame1.size(), 1920u * 2u);
  EXPECT_TRUE(std::all_of(frame1.begin(), frame1.end(),
                          [](int32_t s) { return s == 0; }));
}

TEST(AudioImportStageTest, Import_ResamplesNonNativeRateToCadenceBlocks) {
  orc::AudioImportStage stage;
  auto deps = std::make_shared<NiceMock<MockAudioImportDeps>>();
  stage.set_deps_override(deps);

  // A 44100 Hz WAV is resampled to 48000 Hz on ingest; every frame block is
  // cadence-sized regardless of the source length.
  ON_CALL(*deps, open(_))
      .WillByDefault(Return(orc::WavProbeResult{true, "", 44100, 16, 1000}));
  deps->serve(std::vector<int32_t>(1000 * 2, 100 << 8));

  auto vfr = make_pal_source();
  auto output = run(stage, vfr, {{"wav_path", std::string("/tmp/a.wav")}});

  EXPECT_EQ(output->get_audio_samples(1, orc::FrameID(0)).size(), 1920u * 2u);
  EXPECT_EQ(output->get_audio_samples(1, orc::FrameID(1)).size(), 1920u * 2u);
}

TEST(AudioImportStageTest, Import_SegmentsNtscCadence) {
  orc::AudioImportStage stage;
  auto deps = std::make_shared<NiceMock<MockAudioImportDeps>>();
  stage.set_deps_override(deps);

  // NTSC audio frame sequence (SMPTE 272M-1994 §14.3): frames 0 and 1 carry
  // 1602 and 1601 stereo pairs.
  constexpr size_t kPairs = 1602u + 1601u;
  ON_CALL(*deps, open(_))
      .WillByDefault(Return(orc::WavProbeResult{true, "", 48000, 16, kPairs}));
  deps->serve(std::vector<int32_t>(kPairs * 2, 7 << 8));

  auto vfr = make_pal_source();
  orc::SourceParameters params;
  params.system = orc::VideoSystem::NTSC;
  ON_CALL(*vfr, get_video_parameters()).WillByDefault(Return(params));

  auto output = run(stage, vfr, {{"wav_path", std::string("/tmp/a.wav")}});

  EXPECT_EQ(output->get_audio_samples(1, orc::FrameID(0)).size(), 1602u * 2u);
  EXPECT_EQ(output->get_audio_samples(1, orc::FrameID(1)).size(), 1601u * 2u);
}

TEST(AudioImportStageTest, Import_44kHzEqualsWholeStreamConversion) {
  orc::AudioImportStage stage;
  auto deps = std::make_shared<NiceMock<MockAudioImportDeps>>();
  stage.set_deps_override(deps);

  // 44.1 kHz material over many frames: the streamed conversion must equal
  // the whole-stream one sample for sample.
  constexpr size_t kFrames = 60;
  std::vector<int32_t> raw(1764 * kFrames * 2);
  for (size_t i = 0; i < raw.size(); ++i) {
    raw[i] = static_cast<int32_t>(2000000.0 *
                                  std::sin(static_cast<double>(i) * 0.011));
  }
  ON_CALL(*deps, open(_))
      .WillByDefault(
          Return(orc::WavProbeResult{true, "", 44100, 24, raw.size() / 2}));
  deps->serve(raw);

  auto vfr = make_pal_source();
  ON_CALL(*vfr, frame_count()).WillByDefault(Return(kFrames));
  ON_CALL(*vfr, frame_range())
      .WillByDefault(Return(
          orc::FrameIDRange{orc::FrameID(0), orc::FrameID(kFrames - 1)}));
  auto output = run(stage, vfr, {{"wav_path", std::string("/tmp/a.wav")}});

  const auto expected = orc::AudioResampler::resample_to_synchronous(
      raw, 44100.0, orc::VideoSystem::PAL, kFrames);
  for (size_t i = 0; i < kFrames; ++i) {
    ASSERT_EQ(output->get_audio_samples(1, orc::FrameID(i)), expected[i])
        << "frame " << i;
  }
}

TEST(AudioImportStageTest, OutOfRangePair_ReturnsEmptyAndNullopt) {
  orc::AudioImportStage stage;
  auto deps = std::make_shared<NiceMock<MockAudioImportDeps>>();
  stage.set_deps_override(deps);
  ON_CALL(*deps, open(_))
      .WillByDefault(Return(orc::WavProbeResult{true, "", 48000, 16, 4}));

  auto vfr = make_pal_source();
  auto output = run(stage, vfr, {{"wav_path", std::string("/tmp/a.wav")}});

  ASSERT_EQ(output->audio_channel_pair_count(), 2u);
  EXPECT_FALSE(output->get_audio_channel_pair_descriptor(2).has_value());
  EXPECT_TRUE(output->get_audio_samples(2, orc::FrameID(0)).empty());
}

TEST(AudioImportStageTest, SourcePairs_ForwardUntouched) {
  orc::AudioImportStage stage;
  auto deps = std::make_shared<NiceMock<MockAudioImportDeps>>();
  stage.set_deps_override(deps);
  ON_CALL(*deps, open(_))
      .WillByDefault(Return(orc::WavProbeResult{true, "", 48000, 16, 4}));

  auto vfr = make_pal_source();
  const orc::AudioChannelPairDescriptor source_desc{"Analogue",
                                                    orc::AudioOrigin::ANALOGUE};
  ON_CALL(*vfr, get_audio_channel_pair_descriptor(0))
      .WillByDefault(Return(source_desc));
  ON_CALL(*vfr, get_audio_samples(0, orc::FrameID(0)))
      .WillByDefault(Return(std::vector<int32_t>{1, 2}));

  auto output = run(stage, vfr,
                    {{"wav_path", std::string("/tmp/a.wav")},
                     {"pair_name", std::string("Commentary")}});

  const auto fwd = output->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(fwd.has_value());
  EXPECT_EQ(fwd->name, "Analogue");
  EXPECT_EQ(fwd->origin, orc::AudioOrigin::ANALOGUE);
  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{1, 2}));

  const auto desc = output->get_audio_channel_pair_descriptor(1);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->name, "Commentary");  // pair_name overrides the file stem
}

// ===========================================================================
// WAV header parsing (shared by audio_import and cvbs_source)
// ===========================================================================

// A WAV image in memory: header bytes plus a declared total file size, the
// samples past the header being implied (the parser never reads them).
struct WavImage {
  std::vector<uint8_t> header;
  uint64_t file_size = 0;

  orc::wav::WavLayout parse() const {
    return orc::wav::parse_header(
        [this](uint64_t offset, uint8_t* out, size_t length) -> size_t {
          if (offset >= header.size()) return 0;
          const size_t n = std::min<size_t>(
              length, header.size() - static_cast<size_t>(offset));
          std::memcpy(out, header.data() + offset, n);
          return n;
        },
        file_size);
  }
};

void put_tag(std::vector<uint8_t>& b, const char* tag) {
  b.insert(b.end(), tag, tag + 4);
}
void put_le(std::vector<uint8_t>& b, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i)
    b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_fmt(std::vector<uint8_t>& b, uint32_t rate, uint16_t bits) {
  put_tag(b, "fmt ");
  put_le(b, 16, 4);
  put_le(b, 1, 2);  // PCM
  put_le(b, 2, 2);  // stereo
  put_le(b, rate, 4);
  put_le(b, rate * 2u * bits / 8u, 4);
  put_le(b, 2u * bits / 8u, 2);
  put_le(b, bits, 2);
}

WavImage make_riff(uint64_t data_bytes, uint64_t actual_data_bytes) {
  WavImage img;
  put_tag(img.header, "RIFF");
  put_le(img.header, static_cast<uint32_t>(36 + data_bytes), 4);
  put_tag(img.header, "WAVE");
  put_fmt(img.header, 48000, 24);
  put_tag(img.header, "data");
  put_le(img.header, static_cast<uint32_t>(data_bytes), 4);
  img.file_size = img.header.size() + actual_data_bytes;
  return img;
}

WavImage make_rf64(uint64_t data_bytes) {
  WavImage img;
  put_tag(img.header, "RF64");
  put_le(img.header, 0xFFFFFFFFu, 4);
  put_tag(img.header, "WAVE");
  put_tag(img.header, "ds64");
  put_le(img.header, 28, 4);
  put_le(img.header, 36 + 36 + data_bytes, 8);  // riff size
  put_le(img.header, data_bytes, 8);            // data size
  put_le(img.header, data_bytes / 6, 8);        // sample count
  put_le(img.header, 0, 4);                     // table length
  put_fmt(img.header, 48000, 24);
  put_tag(img.header, "data");
  put_le(img.header, 0xFFFFFFFFu, 4);
  img.file_size = img.header.size() + data_bytes;
  return img;
}

TEST(WavHeaderTest, Riff_ReadsFormatAndDataChunk) {
  const auto layout = make_riff(1920 * 6, 1920 * 6).parse();
  ASSERT_TRUE(layout.valid) << layout.error;
  EXPECT_FALSE(layout.rf64);
  EXPECT_EQ(layout.format_tag, 1);
  EXPECT_EQ(layout.channels, 2);
  EXPECT_EQ(layout.sample_rate_hz, 48000u);
  EXPECT_EQ(layout.bits_per_sample, 24);
  EXPECT_EQ(layout.fmt_offset, 12u);
  EXPECT_EQ(layout.data_offset, 44u);
  EXPECT_EQ(layout.data_bytes, 1920u * 6u);
}

TEST(WavHeaderTest, Rf64_TakesThe64BitDataSizeFromDs64) {
  // Six hours of 48 kHz 24-bit stereo: 6,220,800,000 bytes, past 4 GiB.
  const uint64_t data_bytes = 6220800000ull;
  const auto layout = make_rf64(data_bytes).parse();
  ASSERT_TRUE(layout.valid) << layout.error;
  EXPECT_TRUE(layout.rf64);
  EXPECT_EQ(layout.data_bytes, data_bytes);
  EXPECT_EQ(layout.data_offset, make_rf64(data_bytes).header.size());
}

TEST(WavHeaderTest, Riff_WrappedSizeIsAnErrorNotAShortRead) {
  // A 32-bit writer stored 6,220,800,000 mod 2^32 = 1,925,832,704 bytes.
  const uint64_t actual = 6220800000ull;
  const uint64_t wrapped = actual & 0xFFFFFFFFull;
  const auto layout = make_riff(wrapped, actual).parse();
  EXPECT_FALSE(layout.valid);
  EXPECT_TRUE(layout.size_wrapped);
  EXPECT_NE(layout.error.find("RF64"), std::string::npos);
}

TEST(WavHeaderTest, Riff_TruncatedFileIsNotReportedAsWrapped) {
  const auto layout = make_riff(1920 * 6, 1920 * 3).parse();
  ASSERT_TRUE(layout.valid);
  EXPECT_FALSE(layout.size_wrapped);
}

TEST(WavHeaderTest, NotAWav_IsRejected) {
  WavImage img;
  put_tag(img.header, "OggS");
  put_le(img.header, 0, 8);
  img.file_size = img.header.size();
  EXPECT_FALSE(img.parse().valid);
}

// What build_pcm_header() writes reads back through parse_header(), on both
// sides of the RIFF limit.
WavImage built(uint64_t data_bytes) {
  WavImage img;
  img.header = orc::wav::build_pcm_header(2, 48000, 24, data_bytes);
  img.file_size = img.header.size() + data_bytes;
  return img;
}

uint64_t le_at(const std::vector<uint8_t>& b, size_t at, int bytes) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; ++i) {
    v |= static_cast<uint64_t>(b[at + static_cast<size_t>(i)]) << (8 * i);
  }
  return v;
}

TEST(WavHeaderTest, Build_AtTheRiffLimitIsCanonicalRiff) {
  // The largest whole number of 6-byte pairs a RIFF header can describe.
  const uint64_t data_bytes = orc::wav::kRiffMaxDataBytes / 6 * 6;
  const auto img = built(data_bytes);
  ASSERT_EQ(img.header.size(), 44u);
  EXPECT_EQ(orc::wav::pcm_header_bytes(data_bytes), 44u);
  EXPECT_EQ(std::string(img.header.begin(), img.header.begin() + 4), "RIFF");
  EXPECT_EQ(le_at(img.header, 4, 4), 36 + data_bytes);
  EXPECT_EQ(le_at(img.header, 40, 4), data_bytes);

  const auto layout = img.parse();
  ASSERT_TRUE(layout.valid) << layout.error;
  EXPECT_FALSE(layout.rf64);
  EXPECT_EQ(layout.data_bytes, data_bytes);
  EXPECT_EQ(layout.data_offset, 44u);
  EXPECT_EQ(layout.bits_per_sample, 24);
}

TEST(WavHeaderTest, Build_PastTheRiffLimitIsRf64WithNoWrap) {
  for (const uint64_t data_bytes :
       {orc::wav::kRiffMaxDataBytes / 6 * 6 + 6, uint64_t{6220800000ull}}) {
    SCOPED_TRACE(data_bytes);
    const auto img = built(data_bytes);
    ASSERT_EQ(img.header.size(), 80u);
    EXPECT_EQ(orc::wav::pcm_header_bytes(data_bytes), 80u);
    EXPECT_EQ(std::string(img.header.begin(), img.header.begin() + 4), "RF64");
    EXPECT_EQ(le_at(img.header, 4, 4), 0xFFFFFFFFu);
    EXPECT_EQ(le_at(img.header, 20, 8), img.file_size - 8);  // ds64 riffSize
    EXPECT_EQ(le_at(img.header, 28, 8), data_bytes);         // ds64 dataSize
    EXPECT_EQ(le_at(img.header, 36, 8), data_bytes / 6);     // sample frames
    EXPECT_EQ(le_at(img.header, 76, 4), 0xFFFFFFFFu);        // data placeholder

    const auto layout = img.parse();
    ASSERT_TRUE(layout.valid) << layout.error;
    EXPECT_TRUE(layout.rf64);
    EXPECT_FALSE(layout.size_wrapped);
    EXPECT_EQ(layout.data_bytes, data_bytes);
    EXPECT_EQ(layout.data_offset, 80u);
    EXPECT_EQ(layout.sample_rate_hz, 48000u);
    EXPECT_EQ(layout.channels, 2);
  }
}

}  // namespace
}  // namespace orc_unit_test
