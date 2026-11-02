/*
 * File:        vbi_line_reader_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for raw VBI line-record parsing and frame indexing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_line_reader.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_source_format.h"

namespace orc {
namespace {

// In-memory stand-in for a capture file. Unit tests never touch the
// filesystem, and every transport presents the same byte sequence through
// this interface anyway.
class FakeByteSource : public IVBIByteSource {
 public:
  explicit FakeByteSource(std::vector<uint8_t> bytes)
      : bytes_(std::move(bytes)) {}

  std::optional<uint64_t> size_bytes() const override {
    return static_cast<uint64_t>(bytes_.size());
  }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& error_message) override {
    (void)error_message;
    ++read_count_;
    if (byte_offset >= bytes_.size()) {
      return 0;
    }
    const size_t available = bytes_.size() - static_cast<size_t>(byte_offset);
    const size_t produced = std::min(count, available);
    std::memcpy(out_buffer, bytes_.data() + byte_offset, produced);
    last_read_count_ = count;
    return produced;
  }

  int read_count() const { return read_count_; }

  // Bytes the most recent read asked for, so a test can tell a whole-frame
  // fetch from a four-byte trailer read.
  size_t last_read_count() const { return last_read_count_; }

 private:
  std::vector<uint8_t> bytes_;
  int read_count_ = 0;
  size_t last_read_count_ = 0;
};

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
  return format;
}

// Deterministic per-sample value so a misplaced record shows up as a value
// mismatch rather than a length mismatch.
uint8_t synthetic_sample(uint64_t frame, uint32_t field, uint32_t record,
                         uint32_t sample) {
  return static_cast<uint8_t>(
      (frame * 7u + field * 53u + record * 11u + sample * 3u) & 0xFFu);
}

// Build a capture of whole frames matching the descriptor, filled with
// synthetic_sample() over the valid samples and 0xEE over the padding.
std::vector<uint8_t> make_capture(const VBISourceFormat& format,
                                  uint64_t frame_count) {
  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0xEE);

  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    for (uint32_t field = 0; field < 2; ++field) {
      for (uint32_t record = 0; record < format.field_lines; ++record) {
        const uint64_t record_offset = frame * format.bytes_per_frame() +
                                       field * format.bytes_per_field() +
                                       record * format.bytes_per_record();
        for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
          bytes[static_cast<size_t>(record_offset + sample)] =
              synthetic_sample(frame, field, record, sample);
        }
      }
    }
  }
  return bytes;
}

// Write the bt8x8 per-frame sequence number into the last four bytes of a
// frame, little-endian, as the kernel's read() does.
void set_frame_counter(std::vector<uint8_t>& bytes,
                       const VBISourceFormat& format, uint64_t frame,
                       uint32_t counter) {
  const size_t end =
      static_cast<size_t>((frame + 1) * format.bytes_per_frame());
  bytes[end - 4] = static_cast<uint8_t>(counter & 0xFFu);
  bytes[end - 3] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  bytes[end - 2] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  bytes[end - 1] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
}

TEST(VBILineReader, Bt8x8PALFrameYieldsThirtyTwoRecordsInStoredOrder) {
  const VBISourceFormat format = bt8x8_pal_format();
  FakeByteSource source(make_capture(format, 2));
  VBILineReader reader(format, source);

  VBIFrameRecords records;
  std::string error;
  ASSERT_TRUE(reader.read_frame(0, records, error)) << error;

  ASSERT_EQ(records.lines.size(), 32u);
  EXPECT_EQ(records.frame_index, 0u);

  for (size_t index = 0; index < records.lines.size(); ++index) {
    const VBILineRecord& line = records.lines[index];
    const uint32_t expected_field = static_cast<uint32_t>(index / 16);
    const uint32_t expected_record = static_cast<uint32_t>(index % 16);

    EXPECT_EQ(line.field_index, expected_field) << "at index " << index;
    EXPECT_EQ(line.record_index, expected_record) << "at index " << index;
    EXPECT_EQ(line.frame_index, 0u);
  }
}

// Padding must be excluded: a bt8x8 PAL record stores 2048 samples of which
// only 2044 are real.
TEST(VBILineReader, RecordsCarryOnlyTheValidSamples) {
  const VBISourceFormat format = bt8x8_pal_format();
  FakeByteSource source(make_capture(format, 1));
  VBILineReader reader(format, source);

  VBIFrameRecords records;
  std::string error;
  ASSERT_TRUE(reader.read_frame(0, records, error)) << error;

  for (const VBILineRecord& line : records.lines) {
    ASSERT_EQ(line.samples.size(), 2044u);
    EXPECT_DOUBLE_EQ(
        line.samples.front(),
        synthetic_sample(0, line.field_index, line.record_index, 0));
    EXPECT_DOUBLE_EQ(
        line.samples.back(),
        synthetic_sample(0, line.field_index, line.record_index, 2043));
    // 0xEE is the padding fill; no valid sample may equal it by accident of
    // over-reading past valid_samples.
    EXPECT_NE(line.samples.back(), 0xEE);
  }
}

TEST(VBILineReader, RecordsCarryTheirSourceByteRange) {
  const VBISourceFormat format = bt8x8_pal_format();
  FakeByteSource source(make_capture(format, 3));
  VBILineReader reader(format, source);

  VBIFrameRecords records;
  std::string error;
  ASSERT_TRUE(reader.read_frame(2, records, error)) << error;

  const uint64_t frame_base = 2 * 65536;
  EXPECT_EQ(records.lines.front().source_byte_offset, frame_base);
  EXPECT_EQ(records.lines.front().source_byte_length, 2044u);

  // First record of the second stored field.
  EXPECT_EQ(records.lines[16].source_byte_offset, frame_base + 32768);
  // Last record of the frame.
  EXPECT_EQ(records.lines.back().source_byte_offset,
            frame_base + 32768 + 15 * 2048);
}

// The frame counter occupies the padding of the last record, so reading it
// must not disturb that record's sample data.
TEST(VBILineReader, FrameCounterIsReadLittleEndianFromTheRecordPadding) {
  const VBISourceFormat format = bt8x8_pal_format();
  std::vector<uint8_t> bytes = make_capture(format, 2);
  set_frame_counter(bytes, format, 0, 0x01020304u);
  set_frame_counter(bytes, format, 1, 0x01020305u);
  FakeByteSource source(std::move(bytes));
  VBILineReader reader(format, source);

  VBIFrameRecords records;
  std::string error;
  ASSERT_TRUE(reader.read_frame(0, records, error)) << error;

  ASSERT_TRUE(records.frame_counter.has_value());
  EXPECT_EQ(*records.frame_counter, 0x01020304u);

  // The final record still carries its full 2044 samples, unaffected by the
  // four counter bytes sharing its 2048-byte stride.
  ASSERT_EQ(records.lines.back().samples.size(), 2044u);
  EXPECT_DOUBLE_EQ(records.lines.back().samples.back(),
                   synthetic_sample(0, 1, 15, 2043));

  ASSERT_TRUE(reader.read_frame(1, records, error)) << error;
  ASSERT_TRUE(records.frame_counter.has_value());
  EXPECT_EQ(*records.frame_counter, 0x01020305u);
}

TEST(VBILineReader, FrameCounterIsAbsentWhenTheFormatDeclaresNoTrailer) {
  VBISourceFormat format = bt8x8_pal_format();
  format.frame_trailer_bytes = 0;
  format.frame_trailer_is_counter = false;

  FakeByteSource source(make_capture(format, 1));
  VBILineReader reader(format, source);

  VBIFrameRecords records;
  std::string error;
  ASSERT_TRUE(reader.read_frame(0, records, error)) << error;
  EXPECT_FALSE(records.frame_counter.has_value());
}

// Frame-drop detection needs a frame's counter and nothing else, and asking
// for its records as well would decode 64 KiB to read four bytes.
TEST(VBILineReader, FrameCounterIsReadableWithoutDecodingTheFramesRecords) {
  const VBISourceFormat format = bt8x8_pal_format();
  std::vector<uint8_t> bytes = make_capture(format, 3);
  set_frame_counter(bytes, format, 0, 399598u);
  set_frame_counter(bytes, format, 1, 399599u);
  set_frame_counter(bytes, format, 2, 399604u);
  FakeByteSource source(std::move(bytes));
  VBILineReader reader(format, source);

  std::optional<uint32_t> counter;
  std::string error;
  ASSERT_TRUE(reader.read_frame_counter(2, counter, error)) << error;
  ASSERT_TRUE(counter.has_value());
  EXPECT_EQ(*counter, 399604u);

  // Four bytes, one read, and nothing else touched.
  EXPECT_EQ(source.read_count(), 1);
  EXPECT_EQ(source.last_read_count(), 4u);
}

// A format with no counter trailer reports that it has none, which is an
// answer rather than a failure: such a source cannot report drops at all.
TEST(VBILineReader, FrameCounterReadIsEmptyWhenTheFormatDeclaresNoTrailer) {
  VBISourceFormat format = bt8x8_pal_format();
  format.frame_trailer_bytes = 0;
  format.frame_trailer_is_counter = false;

  FakeByteSource source(make_capture(format, 1));
  VBILineReader reader(format, source);

  std::optional<uint32_t> counter;
  std::string error;
  EXPECT_TRUE(reader.read_frame_counter(0, counter, error)) << error;
  EXPECT_FALSE(counter.has_value());
  EXPECT_EQ(source.read_count(), 0);
}

TEST(VBILineReader, FrameCounterReadBeyondTheCaptureIsReported) {
  const VBISourceFormat format = bt8x8_pal_format();
  FakeByteSource source(make_capture(format, 1));
  VBILineReader reader(format, source);

  std::optional<uint32_t> counter;
  std::string error;
  EXPECT_FALSE(reader.read_frame_counter(4, counter, error));
  EXPECT_FALSE(counter.has_value());
  EXPECT_NE(error.find("part-way"), std::string::npos) << error;
}

// cx88 stores 18 records per field with the data service in 1..16; records 0
// and 17 must be skipped, and the surviving record indices keep their stored
// numbering so provenance stays exact.
TEST(VBILineReader, FieldRangeSelectsOnlyTheDataServiceRecords) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_lines = 18;
  format.field_range = VBIFieldRange{1, 16};

  FakeByteSource source(make_capture(format, 1));
  VBILineReader reader(format, source);

  VBIFrameRecords records;
  std::string error;
  ASSERT_TRUE(reader.read_frame(0, records, error)) << error;

  ASSERT_EQ(records.lines.size(), 32u);
  EXPECT_EQ(records.lines.front().record_index, 1u);
  EXPECT_EQ(records.lines[15].record_index, 16u);
  EXPECT_EQ(records.lines[16].field_index, 1u);
  EXPECT_EQ(records.lines[16].record_index, 1u);

  // Record 1 of field 0 starts one record stride into the frame.
  EXPECT_EQ(records.lines.front().source_byte_offset, 2048u);
  EXPECT_DOUBLE_EQ(records.lines.front().samples.front(),
                   synthetic_sample(0, 0, 1, 0));
}

TEST(VBILineReader, FrameCountIsWholeFramesOnly) {
  const VBISourceFormat format = bt8x8_pal_format();
  FakeByteSource source(make_capture(format, 5));
  VBILineReader reader(format, source);

  ASSERT_TRUE(reader.frame_count().has_value());
  EXPECT_EQ(*reader.frame_count(), 5u);
  EXPECT_FALSE(reader.has_partial_trailing_frame());
  EXPECT_EQ(reader.trailing_bytes().value_or(1u), 0u);
  EXPECT_EQ(reader.bytes_per_frame(), 65536u);
}

TEST(VBILineReader, PartialTrailingFrameIsReported) {
  const VBISourceFormat format = bt8x8_pal_format();
  std::vector<uint8_t> bytes = make_capture(format, 3);
  bytes.resize(bytes.size() - 100);  // truncate the final frame
  FakeByteSource source(std::move(bytes));
  VBILineReader reader(format, source);

  ASSERT_TRUE(reader.frame_count().has_value());
  EXPECT_EQ(*reader.frame_count(), 2u);
  EXPECT_TRUE(reader.has_partial_trailing_frame());

  // How much was left over is what tells an ordinary odd-field ending apart
  // from a capture cut mid-record, so the reader reports the count and not
  // just the fact.
  ASSERT_TRUE(reader.trailing_bytes().has_value());
  EXPECT_EQ(*reader.trailing_bytes(), format.bytes_per_frame() - 100u);
}

// A capture that ends exactly on a field boundary is the ordinary case, and
// the reported remainder is what lets the stage say so in those words.
TEST(VBILineReader, TrailingWholeFieldIsReportedAsAFieldsWorthOfBytes) {
  const VBISourceFormat format = bt8x8_pal_format();
  std::vector<uint8_t> bytes = make_capture(format, 3);
  bytes.resize(bytes.size() - static_cast<size_t>(format.bytes_per_field()));
  FakeByteSource source(std::move(bytes));
  VBILineReader reader(format, source);

  EXPECT_EQ(*reader.frame_count(), 2u);
  EXPECT_TRUE(reader.has_partial_trailing_frame());
  ASSERT_TRUE(reader.trailing_bytes().has_value());
  EXPECT_EQ(*reader.trailing_bytes(), format.bytes_per_field());
}

// A short final frame is an error, never a silent truncation: records must
// never be dropped without the user being told.
TEST(VBILineReader, ShortFinalFrameFailsRatherThanTruncating) {
  const VBISourceFormat format = bt8x8_pal_format();
  std::vector<uint8_t> bytes = make_capture(format, 2);
  bytes.resize(bytes.size() - 100);
  FakeByteSource source(std::move(bytes));
  VBILineReader reader(format, source);

  VBIFrameRecords records;
  std::string error;
  EXPECT_FALSE(reader.read_frame(1, records, error));
  EXPECT_NE(error.find("part-way through frame 1"), std::string::npos) << error;
  EXPECT_NE(error.find("65536"), std::string::npos) << error;
}

TEST(VBILineReader, ReadingBeyondTheCaptureFails) {
  const VBISourceFormat format = bt8x8_pal_format();
  FakeByteSource source(make_capture(format, 1));
  VBILineReader reader(format, source);

  VBIFrameRecords records;
  std::string error;
  EXPECT_FALSE(reader.read_frame(4, records, error));
  EXPECT_FALSE(error.empty());
}

// One read per frame: the reader must not fragment its access pattern, which
// would defeat a streaming transport.
TEST(VBILineReader, FrameIsFetchedInASingleRead) {
  const VBISourceFormat format = bt8x8_pal_format();
  FakeByteSource source(make_capture(format, 2));
  VBILineReader reader(format, source);

  VBIFrameRecords records;
  std::string error;
  ASSERT_TRUE(reader.read_frame(1, records, error)) << error;
  EXPECT_EQ(source.read_count(), 1);
}

TEST(VBILineReader, UnsetGeometryIsRejected) {
  // No preset expands to this — every one of them is complete — but the reader
  // is generic over the descriptor and must still refuse an empty one.
  const VBISourceFormat format;
  std::string error;

  FakeByteSource source(std::vector<uint8_t>(1024, 0));
  VBILineReader reader(format, source);

  VBIFrameRecords records;
  EXPECT_FALSE(reader.read_frame(0, records, error));
  EXPECT_NE(error.find("line_length"), std::string::npos) << error;
  EXPECT_FALSE(reader.frame_count().has_value());
}

TEST(VBILineReader, UnsignedEightBitSamplesDecodeToTheirRawValues) {
  std::vector<uint8_t> record = {0, 1, 127, 128, 254, 255};
  std::vector<double> samples;
  std::string error;

  ASSERT_TRUE(decode_vbi_samples(VBISampleFormat::kU8, record.data(),
                                 static_cast<uint32_t>(record.size()), samples,
                                 error))
      << error;
  ASSERT_EQ(samples.size(), record.size());
  for (size_t index = 0; index < record.size(); ++index) {
    EXPECT_DOUBLE_EQ(samples[index], static_cast<double>(record[index]));
  }
}

// The TBC-derived family's words: 16-bit, little-endian, unsigned, and passed
// through with no scaling — the level mapper owns the amplitude domain.
TEST(VBILineReader, U16LERecordsDecodeToTheirUnsignedLittleEndianValues) {
  // 0, 1, 255, 256, 32768, 65535.
  const std::vector<uint8_t> record = {0x00, 0x00, 0x01, 0x00, 0xFF, 0x00,
                                       0x00, 0x01, 0x00, 0x80, 0xFF, 0xFF};
  const std::vector<double> expected = {0.0,   1.0,     255.0,
                                        256.0, 32768.0, 65535.0};
  std::vector<double> samples;
  std::string error;

  ASSERT_TRUE(decode_vbi_samples(VBISampleFormat::kU16LE, record.data(),
                                 static_cast<uint32_t>(expected.size()),
                                 samples, error))
      << error;
  ASSERT_EQ(samples.size(), expected.size());
  for (size_t index = 0; index < expected.size(); ++index) {
    EXPECT_DOUBLE_EQ(samples[index], expected[index]) << "sample " << index;
  }
}

// No capture in circulation stores signed words, and the FLAC transport that
// carries these files has no signedness in its header to distinguish them, so
// the format is refused rather than guessed at.
TEST(VBILineReader, S16LEReportsThatItIsNotImplemented) {
  std::vector<uint8_t> record(8, 0);
  std::vector<double> samples;
  std::string error;

  EXPECT_FALSE(decode_vbi_samples(VBISampleFormat::kS16LE, record.data(), 4,
                                  samples, error));
  EXPECT_NE(error.find("s16le"), std::string::npos) << error;
}

// ---------------------------------------------------------------------------
// The byte read and the decode, apart
// ---------------------------------------------------------------------------

// Reading a frame is two steps — the bytes, then their decode — so a caller
// can hold its lock across the first and not the second. Together they must
// be read_frame() exactly.
TEST(VBILineReader, ReadingTheBytesThenDecodingThemIsReadingTheFrame) {
  const VBISourceFormat format = bt8x8_pal_format();
  std::vector<uint8_t> capture = make_capture(format, 3);
  set_frame_counter(capture, format, 2, 0xA1B2C3D4u);
  FakeByteSource source(capture);
  VBILineReader reader(format, source);

  std::string error;
  VBIFrameRecords whole;
  ASSERT_TRUE(reader.read_frame(2, whole, error)) << error;

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(reader.read_frame_bytes(2, bytes, error)) << error;
  EXPECT_EQ(bytes.size(), format.bytes_per_frame());
  VBIFrameRecords split;
  ASSERT_TRUE(reader.decode_frame(2, bytes, split, error)) << error;

  EXPECT_EQ(split.frame_index, whole.frame_index);
  EXPECT_EQ(split.frame_counter, whole.frame_counter);
  ASSERT_EQ(split.lines.size(), whole.lines.size());
  for (size_t index = 0; index < whole.lines.size(); ++index) {
    EXPECT_EQ(split.lines[index].field_index, whole.lines[index].field_index);
    EXPECT_EQ(split.lines[index].record_index, whole.lines[index].record_index);
    EXPECT_EQ(split.lines[index].source_byte_offset,
              whole.lines[index].source_byte_offset);
    EXPECT_EQ(split.lines[index].samples, whole.lines[index].samples)
        << "record " << index;
  }
}

// The decode is the part a caller runs outside its lock, so it must not
// touch the byte source at all.
TEST(VBILineReader, DecodingAFramesBytesReadsNothing) {
  const VBISourceFormat format = bt8x8_pal_format();
  const std::vector<uint8_t> capture = make_capture(format, 2);
  FakeByteSource source(capture);
  VBILineReader reader(format, source);

  std::string error;
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(reader.read_frame_bytes(1, bytes, error)) << error;
  EXPECT_EQ(source.read_count(), 1);

  VBIFrameRecords records;
  ASSERT_TRUE(reader.decode_frame(1, bytes, records, error)) << error;
  EXPECT_EQ(source.read_count(), 1);
  EXPECT_FALSE(records.lines.empty());
}

TEST(VBILineReader, DecodingABufferThatIsNotOneWholeFrameFails) {
  const VBISourceFormat format = bt8x8_pal_format();
  FakeByteSource source(make_capture(format, 1));
  VBILineReader reader(format, source);

  const std::vector<uint8_t> short_buffer(
      static_cast<size_t>(format.bytes_per_frame() - 1), 0);
  VBIFrameRecords records;
  std::string error;
  EXPECT_FALSE(reader.decode_frame(0, short_buffer, records, error));
  EXPECT_FALSE(error.empty());
}

TEST(VBILineReader, ReadingTheBytesOfAFramePastTheEndFails) {
  const VBISourceFormat format = bt8x8_pal_format();
  FakeByteSource source(make_capture(format, 2));
  VBILineReader reader(format, source);

  std::vector<uint8_t> bytes;
  std::string error;
  EXPECT_FALSE(reader.read_frame_bytes(2, bytes, error));
  EXPECT_NE(error.find("part-way through frame 2"), std::string::npos) << error;
}

}  // namespace
}  // namespace orc
