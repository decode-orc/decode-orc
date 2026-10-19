/*
 * File:        vbi_video_transport_test.cpp
 * Module:      orc-tests
 * Purpose:     Functional tests for unwrapping a .tbc VBI crop kept as the
 *              pictures of an FFV1 Matroska video
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_line_reader.h"
#include "vbi_source_format.h"
#include "vbi_source_validation.h"
#include "vbi_transport.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

namespace orc {
namespace {

// The geometry of the crop: 910 samples a record, 16 records a field.
constexpr uint32_t kRecordSamples = 910;
constexpr uint32_t kFieldRecords = 16;

// The luma of a picture's sample, a pattern that differs from picture to
// picture, row to row and column to column and stays inside the limited range
// (16-235), so a sample read from the wrong place shows.
uint8_t pattern_luma(uint32_t picture, uint32_t row, uint32_t column) {
  return static_cast<uint8_t>(16u + (picture * 7u + row * 13u + column) % 220u);
}

// The capture word the transport should serve for that sample.
uint16_t pattern_word(uint32_t picture, uint32_t row, uint32_t column) {
  return vbi_sample_word_from_luma(pattern_luma(picture, row, column),
                                   VBIVideoLumaCoding{8, false});
}

uint16_t word_at(const std::vector<uint8_t>& bytes, size_t sample) {
  return static_cast<uint16_t>(bytes[2 * sample] |
                               (bytes[2 * sample + 1] << 8));
}

// Writes the pattern as limited-range 8-bit 4:2:0 FFV1 in Matroska, a keyframe
// every twelfth picture at 30000/1001 pictures a second: what FFmpeg makes of
// a VBI crop handed to it as 16-bit grey.
bool write_ffv1_matroska(const std::string& path, uint32_t width,
                         uint32_t height, uint32_t pictures,
                         std::string& error) {
  AVFormatContext* format = nullptr;
  if (avformat_alloc_output_context2(&format, nullptr, "matroska",
                                     path.c_str()) < 0) {
    error = "no Matroska muxer";
    return false;
  }
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
  AVCodecContext* encoder = codec ? avcodec_alloc_context3(codec) : nullptr;
  AVStream* stream = avformat_new_stream(format, nullptr);
  AVFrame* frame = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  bool ok = encoder != nullptr && stream != nullptr && frame != nullptr &&
            packet != nullptr;

  if (ok) {
    encoder->width = static_cast<int>(width);
    encoder->height = static_cast<int>(height);
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->color_range = AVCOL_RANGE_MPEG;
    encoder->time_base = AVRational{1001, 30000};
    encoder->framerate = AVRational{30000, 1001};
    encoder->gop_size = 12;
    if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
      encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    ok = avcodec_open2(encoder, codec, nullptr) >= 0 &&
         avcodec_parameters_from_context(stream->codecpar, encoder) >= 0;
  }
  if (ok) {
    stream->time_base = encoder->time_base;
    stream->avg_frame_rate = encoder->framerate;
    ok = avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0 &&
         avformat_write_header(format, nullptr) >= 0;
  }
  if (ok) {
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = static_cast<int>(width);
    frame->height = static_cast<int>(height);
    frame->color_range = AVCOL_RANGE_MPEG;
    ok = av_frame_get_buffer(frame, 0) >= 0;
  }

  const auto drain = [&]() {
    while (avcodec_receive_packet(encoder, packet) == 0) {
      av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
      packet->stream_index = stream->index;
      if (av_interleaved_write_frame(format, packet) < 0) {
        return false;
      }
    }
    return true;
  };

  for (uint32_t picture = 0; ok && picture < pictures; ++picture) {
    ok = av_frame_make_writable(frame) >= 0;
    for (uint32_t row = 0; ok && row < height; ++row) {
      uint8_t* luma = frame->data[0] + row * frame->linesize[0];
      for (uint32_t column = 0; column < width; ++column) {
        luma[column] = pattern_luma(picture, row, column);
      }
    }
    for (int plane = 1; ok && plane < 3; ++plane) {
      for (uint32_t row = 0; row < (height + 1) / 2; ++row) {
        std::fill_n(frame->data[plane] + row * frame->linesize[plane],
                    (width + 1) / 2, uint8_t{128});
      }
    }
    frame->pts = picture;
    ok = ok && avcodec_send_frame(encoder, frame) >= 0 && drain();
  }
  if (ok) {
    ok = avcodec_send_frame(encoder, nullptr) >= 0 && drain() &&
         av_write_trailer(format) >= 0;
  }
  if (!ok && error.empty()) {
    error = "FFV1 encoding failed";
  }

  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&encoder);
  if (format->pb != nullptr) {
    avio_closep(&format->pb);
  }
  avformat_free_context(format);
  return ok;
}

VBISourceFormat tbc_crop_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (NABTS)", format, error))
      << error;
  return format;
}

class VBIVideoTransport : public ::testing::Test {
 protected:
  void SetUp() override {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    directory_ = std::filesystem::temp_directory_path() /
                 ("orc-vbi-video-transport-" + std::string(info->name()));
    std::filesystem::remove_all(directory_);
    std::filesystem::create_directories(directory_);
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  // Writes the pattern into a video of its own and opens it.
  std::unique_ptr<IVBIByteSource> open_video(uint32_t width, uint32_t height,
                                             uint32_t pictures) {
    const std::string path = (directory_ / "vbi.mkv").string();
    std::string error;
    EXPECT_TRUE(write_ffv1_matroska(path, width, height, pictures, error))
        << error;
    std::unique_ptr<IVBIByteSource> source = open_vbi_byte_source(path, error);
    EXPECT_NE(source, nullptr) << error;
    return source;
  }

  std::filesystem::path directory_;
};

// Every picture row comes back as one record of 16-bit words, in order, each
// word the one the luma was made from.
TEST_F(VBIVideoTransport, PicturesUnwrapIntoTheCropsSixteenBitWords) {
  constexpr uint32_t kPictures = 40;
  const std::unique_ptr<IVBIByteSource> source =
      open_video(kRecordSamples, 2 * kFieldRecords, kPictures);
  ASSERT_NE(source, nullptr);

  const uint64_t picture_bytes = kRecordSamples * 2 * kFieldRecords * 2;
  ASSERT_TRUE(source->size_bytes().has_value());
  EXPECT_EQ(*source->size_bytes(), kPictures * picture_bytes);
  EXPECT_EQ(source->declared_bits_per_sample(), std::optional<uint32_t>(16));
  EXPECT_EQ(source->declared_record_samples(),
            std::optional<uint32_t>(kRecordSamples));

  std::vector<uint8_t> bytes(picture_bytes);
  std::string error;
  for (uint32_t picture = 0; picture < kPictures; ++picture) {
    ASSERT_EQ(source->read_at(picture * picture_bytes, bytes.size(),
                              bytes.data(), error),
              bytes.size())
        << error;
    for (uint32_t row = 0; row < 2 * kFieldRecords; ++row) {
      for (uint32_t column = 0; column < kRecordSamples; ++column) {
        ASSERT_EQ(word_at(bytes, row * kRecordSamples + column),
                  pattern_word(picture, row, column))
            << "picture " << picture << " row " << row << " column " << column;
      }
    }
  }

  // Past the last picture there is nothing more, which is not an error.
  EXPECT_EQ(source->read_at(kPictures * picture_bytes, 2, bytes.data(), error),
            0u);
  EXPECT_TRUE(error.empty()) << error;
}

// A reader that jumps about the capture — backwards, and further ahead than it
// decodes forward — is served from the keyframe before each picture it asks
// for, and a read may span two pictures.
TEST_F(VBIVideoTransport, PicturesAreReachedInAnyOrder) {
  constexpr uint32_t kPictures = 60;
  const std::unique_ptr<IVBIByteSource> source =
      open_video(kRecordSamples, 2 * kFieldRecords, kPictures);
  ASSERT_NE(source, nullptr);
  source->set_forward_decode_limit(0);

  const uint64_t picture_bytes = kRecordSamples * 2 * kFieldRecords * 2;
  std::vector<uint8_t> bytes(picture_bytes);
  std::string error;
  for (const uint32_t picture : {50u, 7u, 8u, 31u, 59u, 0u}) {
    ASSERT_EQ(source->read_at(picture * picture_bytes, bytes.size(),
                              bytes.data(), error),
              bytes.size())
        << error;
    EXPECT_EQ(word_at(bytes, 0), pattern_word(picture, 0, 0));
    EXPECT_EQ(word_at(bytes, 17 * kRecordSamples + 400),
              pattern_word(picture, 17, 400));
  }

  // The last record of picture 23 and the first of picture 24, in one read.
  const uint64_t straddle = 24 * picture_bytes - kRecordSamples * 2;
  std::vector<uint8_t> two_records(4 * kRecordSamples);
  ASSERT_EQ(
      source->read_at(straddle, two_records.size(), two_records.data(), error),
      two_records.size())
      << error;
  EXPECT_EQ(word_at(two_records, 5), pattern_word(23, 31, 5));
  EXPECT_EQ(word_at(two_records, kRecordSamples + 5), pattern_word(24, 0, 5));
}

// Through the .tbc crop format, a 910 x 32 picture is one stored frame: rows
// 0-15 the first stored field's records and rows 16-31 the second's.
TEST_F(VBIVideoTransport, TheCropFormatReadsTwoFieldsToAPicture) {
  constexpr uint32_t kPictures = 30;
  const std::unique_ptr<IVBIByteSource> source =
      open_video(kRecordSamples, 2 * kFieldRecords, kPictures);
  ASSERT_NE(source, nullptr);

  const VBISourceFormat format = tbc_crop_format();
  VBITransportHints hints;
  hints.bits_per_sample = source->declared_bits_per_sample();
  hints.record_samples = source->declared_record_samples();
  const std::vector<std::string> violations =
      validate_vbi_source_config(format, source->size_bytes(), hints);
  EXPECT_TRUE(violations.empty());

  VBILineReader reader(format, *source);
  ASSERT_EQ(reader.frame_count(), std::optional<uint64_t>(kPictures));
  EXPECT_FALSE(reader.has_partial_trailing_frame());

  std::string error;
  for (const uint64_t frame : {uint64_t{0}, uint64_t{17}}) {
    VBIFrameRecords records;
    ASSERT_TRUE(reader.read_frame(frame, records, error)) << error;
    ASSERT_FALSE(records.lines.empty());
    for (const VBILineRecord& line : records.lines) {
      const uint32_t row = line.field_index * kFieldRecords + line.record_index;
      ASSERT_EQ(line.samples.size(), kRecordSamples);
      for (const uint32_t column : {0u, 455u, 909u}) {
        EXPECT_EQ(line.samples[column],
                  pattern_word(static_cast<uint32_t>(frame), row, column))
            << "frame " << frame << " field " << line.field_index << " record "
            << line.record_index;
      }
    }
  }
}

// One field to a 910 x 16 picture reads the same way, two pictures to a stored
// frame, and an odd picture at the end is the trailing field a crop ends on.
TEST_F(VBIVideoTransport, TheCropFormatReadsOneFieldToAPicture) {
  constexpr uint32_t kPictures = 21;
  const std::unique_ptr<IVBIByteSource> source =
      open_video(kRecordSamples, kFieldRecords, kPictures);
  ASSERT_NE(source, nullptr);

  const VBISourceFormat format = tbc_crop_format();
  VBILineReader reader(format, *source);
  ASSERT_EQ(reader.frame_count(), std::optional<uint64_t>(kPictures / 2));
  EXPECT_EQ(reader.trailing_bytes(),
            std::optional<uint64_t>(format.bytes_per_field()));

  std::string error;
  VBIFrameRecords records;
  ASSERT_TRUE(reader.read_frame(3, records, error)) << error;
  for (const VBILineRecord& line : records.lines) {
    const uint32_t picture = 2 * 3 + line.field_index;
    EXPECT_EQ(line.samples[100], pattern_word(picture, line.record_index, 100))
        << "field " << line.field_index << " record " << line.record_index;
  }
}

// A video whose rows are not the format's records is refused by name rather
// than read with every record straddling two rows.
TEST_F(VBIVideoTransport, AVideoOfAnotherWidthDoesNotMatchTheCropFormat) {
  const std::unique_ptr<IVBIByteSource> source =
      open_video(720, kFieldRecords, 4);
  ASSERT_NE(source, nullptr);

  VBITransportHints hints;
  hints.bits_per_sample = source->declared_bits_per_sample();
  hints.record_samples = source->declared_record_samples();
  const std::vector<std::string> violations = validate_vbi_source_config(
      tbc_crop_format(), source->size_bytes(), hints);
  EXPECT_TRUE(std::any_of(
      violations.begin(), violations.end(), [](const std::string& violation) {
        return violation.find("720 samples wide") != std::string::npos;
      }));
}

}  // namespace
}  // namespace orc
