/*
 * File:        flac_audio_import_test.cpp
 * Module:      orc-core-functional-tests
 * Purpose:     Functional tests for the Audio Import stage's FLAC reading:
 *              FLAC files written by FFmpeg's encoder are read back exactly,
 *              in order and at random
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "audio-resample/synchronous_audio_cache.h"
#include "audio_import_stage_deps.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace orc_functional_test {
namespace {

// A deterministic, noise-like stereo signal at |bits| bits per sample (an
// LCG, so FLAC's predictor cannot shrink it to nothing and its frames vary
// in size, as real audio does). Values are interleaved, on the |bits| scale.
std::vector<int32_t> make_signal(uint64_t pairs, int bits, int channels = 2) {
  std::vector<int32_t> samples(pairs * static_cast<uint64_t>(channels));
  uint32_t state = 0x2961u;
  const int32_t half = 1 << (bits - 1);
  for (auto& v : samples) {
    state = state * 1664525u + 1013904223u;
    v = static_cast<int32_t>(state >> (32 - bits)) - half;
  }
  return samples;
}

// The 24-bit carrier values the reader should produce for |signal|.
std::vector<int32_t> to_carrier(const std::vector<int32_t>& signal, int bits) {
  std::vector<int32_t> carrier(signal.size());
  for (size_t i = 0; i < signal.size(); ++i) {
    carrier[i] = signal[i] * (1 << (24 - bits));
  }
  return carrier;
}

std::vector<int32_t> slice(const std::vector<int32_t>& v, uint64_t first_pair,
                           uint64_t pairs) {
  const uint64_t end = std::min<uint64_t>(first_pair + pairs, v.size() / 2);
  if (first_pair >= end) return {};
  return std::vector<int32_t>(v.begin() + static_cast<long>(first_pair * 2),
                              v.begin() + static_cast<long>(end * 2));
}

// Writes |signal| (interleaved, |channels| channels, |bits| bits) as a FLAC
// file with FFmpeg's encoder, which writes no SEEKTABLE: seeks bisect the
// stream, the harder case.
void write_flac(const std::string& path, const std::vector<int32_t>& signal,
                int sample_rate, int bits, int channels) {
  AVFormatContext* oc = nullptr;
  ASSERT_GE(avformat_alloc_output_context2(&oc, nullptr, "flac", path.c_str()),
            0);
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
  ASSERT_NE(codec, nullptr);
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  ctx->sample_rate = sample_rate;
  av_channel_layout_default(&ctx->ch_layout, channels);
  const bool wide = bits > 16;
  ctx->sample_fmt = wide ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
  ctx->bits_per_raw_sample = bits;
  ctx->time_base = AVRational{1, sample_rate};
  ASSERT_GE(avcodec_open2(ctx, codec, nullptr), 0);

  AVStream* stream = avformat_new_stream(oc, nullptr);
  ASSERT_GE(avcodec_parameters_from_context(stream->codecpar, ctx), 0);
  stream->time_base = ctx->time_base;
  ASSERT_GE(avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE), 0);
  ASSERT_GE(avformat_write_header(oc, nullptr), 0);

  AVPacket* packet = av_packet_alloc();
  auto drain = [&] {
    while (avcodec_receive_packet(ctx, packet) == 0) {
      av_packet_rescale_ts(packet, ctx->time_base, stream->time_base);
      packet->stream_index = stream->index;
      av_interleaved_write_frame(oc, packet);
    }
  };

  const uint64_t total = signal.size() / static_cast<uint64_t>(channels);
  const int frame_size = ctx->frame_size > 0 ? ctx->frame_size : 4608;
  for (uint64_t pos = 0; pos < total; pos += frame_size) {
    const int n = static_cast<int>(
        std::min<uint64_t>(static_cast<uint64_t>(frame_size), total - pos));
    AVFrame* frame = av_frame_alloc();
    frame->nb_samples = n;
    frame->format = ctx->sample_fmt;
    frame->sample_rate = sample_rate;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    ASSERT_GE(av_frame_get_buffer(frame, 0), 0);
    const size_t count = static_cast<size_t>(n) * channels;
    const int32_t* src = signal.data() + pos * channels;
    // The encoder takes samples left-justified in the word, as the decoder
    // hands them back.
    if (wide) {
      auto* dst = reinterpret_cast<int32_t*>(frame->data[0]);
      for (size_t i = 0; i < count; ++i) {
        dst[i] =
            static_cast<int32_t>(static_cast<uint32_t>(src[i]) << (32 - bits));
      }
    } else {
      auto* dst = reinterpret_cast<int16_t*>(frame->data[0]);
      for (size_t i = 0; i < count; ++i) {
        dst[i] = static_cast<int16_t>(src[i] * (1 << (16 - bits)));
      }
    }
    frame->pts = static_cast<int64_t>(pos);
    ASSERT_GE(avcodec_send_frame(ctx, frame), 0);
    av_frame_free(&frame);
    drain();
  }
  avcodec_send_frame(ctx, nullptr);
  drain();
  ASSERT_GE(av_write_trailer(oc), 0);
  av_packet_free(&packet);
  avcodec_free_context(&ctx);
  avio_closep(&oc->pb);
  avformat_free_context(oc);
}

class FlacAudioImport : public ::testing::Test {
 protected:
  void SetUp() override {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    directory_ = std::filesystem::temp_directory_path() /
                 ("orc-flac-audio-import-" + std::string(info->name()));
    std::filesystem::remove_all(directory_);
    std::filesystem::create_directories(directory_);
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  std::string path(const std::string& name) const {
    return (directory_ / name).string();
  }

  std::filesystem::path directory_;
};

constexpr int kRate48k = 48000;
constexpr uint64_t kPalFramePairs = 1920;

TEST_F(FlacAudioImport, SixteenBitFileReadsBackExactlyInOrder) {
  const uint64_t pairs = 5 * kRate48k + 123;  // not a whole block
  const auto signal = make_signal(pairs, 16);
  const std::string file = path("a.flac");
  write_flac(file, signal, kRate48k, 16, 2);

  orc::AudioImportDeps deps;
  const auto probe = deps.open(file);
  ASSERT_TRUE(probe.valid) << probe.error;
  EXPECT_EQ(probe.sample_rate, 48000u);
  EXPECT_EQ(probe.bits_per_sample, 16u);
  EXPECT_EQ(probe.pair_count, pairs);

  const auto expected = to_carrier(signal, 16);
  std::vector<int32_t> read;
  for (uint64_t first = 0; first < pairs; first += kPalFramePairs) {
    const auto chunk = deps.read_pairs(first, kPalFramePairs);
    read.insert(read.end(), chunk.begin(), chunk.end());
  }
  EXPECT_EQ(read, expected);
  EXPECT_TRUE(deps.read_pairs(pairs, 10).empty());
}

TEST_F(FlacAudioImport, TwentyFourBitFileReadsBackExactly) {
  const uint64_t pairs = 3 * 44100;
  const auto signal = make_signal(pairs, 24);
  const std::string file = path("b.flac");
  write_flac(file, signal, 44100, 24, 2);

  orc::AudioImportDeps deps;
  const auto probe = deps.open(file);
  ASSERT_TRUE(probe.valid) << probe.error;
  EXPECT_EQ(probe.sample_rate, 44100u);
  EXPECT_EQ(probe.bits_per_sample, 24u);
  EXPECT_EQ(deps.read_pairs(0, pairs), to_carrier(signal, 24));
}

TEST_F(FlacAudioImport, RandomReadsMatchTheSignalAcrossSeeks) {
  // 40 s: long enough that reads far ahead and far behind the decoder seek
  // rather than decode forward or copy from the window.
  const uint64_t pairs = 40 * kRate48k;
  const auto signal = make_signal(pairs, 16);
  const std::string file = path("c.flac");
  write_flac(file, signal, kRate48k, 16, 2);
  const auto expected = to_carrier(signal, 16);

  orc::AudioImportDeps deps;
  ASSERT_TRUE(deps.open(file).valid);

  const uint64_t firsts[] = {
      35 * kRate48k + 17,   // far ahead of the head: seek
      2 * kRate48k + 5,     // far behind: seek back
      2 * kRate48k,         // just behind: from the window
      30 * kRate48k,        // far ahead again
      29 * kRate48k + 999,  // before where that seek landed: seek back
      pairs - 700,          // the tail: a short read
      0,                    // the head again: decoded from the start
      17 * kRate48k + 1,
  };
  for (const uint64_t first : firsts) {
    EXPECT_EQ(deps.read_pairs(first, kPalFramePairs),
              slice(expected, first, kPalFramePairs))
        << "first pair " << first;
  }
}

TEST_F(FlacAudioImport, ConvertedFramesMatchTheSameSignalFromMemory) {
  // The stage feeds the reader to the synchronous cache; a 44.1 kHz file
  // must convert to exactly what the same samples from memory convert to.
  const uint64_t pairs = 4 * 44100;
  const auto signal = make_signal(pairs, 16);
  const std::string file = path("d.flac");
  write_flac(file, signal, 44100, 16, 2);
  const auto expected = to_carrier(signal, 16);

  auto deps = std::make_shared<orc::AudioImportDeps>();
  ASSERT_TRUE(deps->open(file).valid);
  constexpr uint64_t kFrames = 100;  // 4 s of PAL
  orc::SynchronousAudioCache from_file(
      [deps](uint64_t first, size_t count) {
        return deps->read_pairs(first, count);
      },
      44100.0, orc::VideoSystem::PAL, kFrames,
      [] { return orc::make_memory_audio_store(); });
  orc::SynchronousAudioCache from_memory(
      [&expected](uint64_t first, size_t count) {
        return slice(expected, first, count);
      },
      44100.0, orc::VideoSystem::PAL, kFrames,
      [] { return orc::make_memory_audio_store(); });

  for (const uint64_t frame :
       {uint64_t{70}, uint64_t{0}, uint64_t{99}, uint64_t{12}, uint64_t{50}}) {
    EXPECT_EQ(from_file.frame_block(frame), from_memory.frame_block(frame))
        << "frame " << frame;
  }
}

TEST_F(FlacAudioImport, Id3TaggedFileOpens) {
  const auto signal = make_signal(kRate48k, 16);
  const std::string plain = path("plain.flac");
  write_flac(plain, signal, kRate48k, 16, 2);

  // An ID3v2.4 tag with 16 bytes of padding in front of the stream.
  std::vector<char> bytes = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 16};
  bytes.resize(bytes.size() + 16, 0);
  {
    std::ifstream in(plain, std::ios::binary);
    bytes.insert(bytes.end(), std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
  }
  const std::string tagged = path("tagged.flac");
  {
    std::ofstream out(tagged, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  orc::AudioImportDeps deps;
  const auto probe = deps.open(tagged);
  ASSERT_TRUE(probe.valid) << probe.error;
  EXPECT_EQ(deps.read_pairs(0, 4096), slice(to_carrier(signal, 16), 0, 4096));
}

TEST_F(FlacAudioImport, MonoFileIsRefusedWithAReason) {
  const std::string file = path("mono.flac");
  write_flac(file, make_signal(kRate48k, 16, 1), kRate48k, 16, 1);

  orc::AudioImportDeps deps;
  const auto probe = deps.open(file);
  EXPECT_FALSE(probe.valid);
  EXPECT_EQ(probe.error, "not stereo (1 channel(s))");
}

TEST_F(FlacAudioImport, FileThatIsNeitherWavNorFlacIsRefused) {
  const std::string file = path("notes.flac");
  {
    std::ofstream out(file, std::ios::binary);
    out << "this is not audio";
  }
  orc::AudioImportDeps deps;
  const auto probe = deps.open(file);
  EXPECT_FALSE(probe.valid);
  EXPECT_EQ(probe.error, "not a WAV (RIFF/WAVE or RF64) or FLAC file");
}

TEST_F(FlacAudioImport, WavFilesStillOpenByContent) {
  // A WAV given a .flac name is still read as a WAV: the content decides.
  const std::string file = path("misnamed.flac");
  const std::vector<int16_t> samples = {1, -1, 2, -2};
  {
    std::ofstream out(file, std::ios::binary);
    auto put32 = [&out](uint32_t v) {
      out.write(reinterpret_cast<const char*>(&v), 4);
    };
    auto put16 = [&out](uint16_t v) {
      out.write(reinterpret_cast<const char*>(&v), 2);
    };
    out.write("RIFF", 4);
    put32(36 + 8);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(2);
    put32(48000);
    put32(48000 * 4);
    put16(4);
    put16(16);
    out.write("data", 4);
    put32(8);
    out.write(reinterpret_cast<const char*>(samples.data()), 8);
  }
  orc::AudioImportDeps deps;
  const auto probe = deps.open(file);
  ASSERT_TRUE(probe.valid) << probe.error;
  EXPECT_EQ(deps.read_pairs(0, 2),
            (std::vector<int32_t>{256, -256, 512, -512}));
}

}  // namespace
}  // namespace orc_functional_test
