/*
 * File:        vbi_line_reader.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Turns a raw VBI byte stream into indexed line records
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_line_reader.h"

#include <utility>

namespace orc {

namespace {

// Fields stored per frame.  Every format in scope stores two sequential
// fields per frame; the mapping onto TV field parity is a separate concern
// (VBISourceFormat::first_field).
constexpr uint32_t kStoredFieldsPerFrame = 2;

// Width of the bt8x8 frame sequence number at the tail of each frame.
constexpr uint32_t kFrameCounterBytes = 4;

// The driver writes the counter in machine endianness, and every platform
// these captures come from is little-endian (design §3.3).
uint32_t decode_frame_counter(const uint8_t* counter_bytes) {
  return static_cast<uint32_t>(counter_bytes[0]) |
         (static_cast<uint32_t>(counter_bytes[1]) << 8) |
         (static_cast<uint32_t>(counter_bytes[2]) << 16) |
         (static_cast<uint32_t>(counter_bytes[3]) << 24);
}

}  // namespace

bool decode_vbi_samples(VBISampleFormat sample_format,
                        const uint8_t* record_bytes, uint32_t sample_count,
                        std::vector<double>& out_samples,
                        std::string& error_message) {
  out_samples.clear();

  // Every sample is written by a plain loop the compiler can vectorise, which
  // pushing them one at a time does not allow.
  switch (sample_format) {
    case VBISampleFormat::kU8:
      out_samples.assign(record_bytes, record_bytes + sample_count);
      return true;

    case VBISampleFormat::kU16LE: {
      // The TBC-derived family: 16-bit words, little-endian, unsigned — the
      // amplitude domain ld-decode and vhs-decode write, in which 0 IRE and
      // 100 IRE sit at fixed values.  Nothing is scaled here; the level mapper
      // owns the amplitude domain.
      out_samples.resize(sample_count);
      double* samples = out_samples.data();
      for (uint32_t index = 0; index < sample_count; ++index) {
        const size_t word = static_cast<size_t>(index) * 2u;
        const uint32_t low = record_bytes[word];
        const uint32_t high = record_bytes[word + 1u];
        samples[index] = static_cast<double>(low | (high << 8u));
      }
      return true;
    }

    case VBISampleFormat::kS16LE:
      // No capture in circulation stores signed words, and the FLAC transport
      // — which is how these files are actually shipped — carries no
      // signedness in its header, so it always unwraps to the unsigned
      // convention the community encoder writes.  Reading a signed container
      // would need that convention to be configuration on both sides of the
      // transport, which is more machinery than a format nobody has justifies.
      error_message = "Sample format '" + to_string(sample_format) +
                      "' is not implemented; captures are stored as 'u8' or "
                      "'u16le'.";
      return false;
  }

  error_message = "Unrecognised sample format.";
  return false;
}

VBILineReader::VBILineReader(VBISourceFormat format,
                             IVBIByteSource& byte_source)
    : format_(std::move(format)), byte_source_(&byte_source) {}

std::optional<uint64_t> VBILineReader::frame_count() const {
  const uint64_t frame_bytes = format_.bytes_per_frame();
  if (frame_bytes == 0) {
    return std::nullopt;
  }

  const std::optional<uint64_t> stream_bytes = byte_source_->size_bytes();
  if (!stream_bytes.has_value()) {
    return std::nullopt;
  }
  return *stream_bytes / frame_bytes;
}

bool VBILineReader::has_partial_trailing_frame() const {
  const std::optional<uint64_t> trailing = trailing_bytes();
  return trailing.has_value() && *trailing != 0;
}

std::optional<uint64_t> VBILineReader::trailing_bytes() const {
  const uint64_t frame_bytes = format_.bytes_per_frame();
  if (frame_bytes == 0) {
    return std::nullopt;
  }

  const std::optional<uint64_t> stream_bytes = byte_source_->size_bytes();
  if (!stream_bytes.has_value()) {
    return std::nullopt;
  }
  return *stream_bytes % frame_bytes;
}

bool VBILineReader::read_frame_counter(uint64_t frame_index,
                                       std::optional<uint32_t>& out_counter,
                                       std::string& error_message) const {
  out_counter.reset();

  const uint64_t frame_bytes = format_.bytes_per_frame();
  if (frame_bytes == 0) {
    error_message =
        "VBI container geometry is unset (line_length and field_lines must "
        "both be non-zero).";
    return false;
  }

  if (!format_.frame_trailer_is_counter ||
      format_.frame_trailer_bytes < kFrameCounterBytes ||
      frame_bytes < kFrameCounterBytes) {
    // The format carries no counter, which is an answer rather than a
    // failure: this source cannot report dropped frames at all.
    return true;
  }

  const uint64_t counter_offset =
      (frame_index + 1u) * frame_bytes - kFrameCounterBytes;

  uint8_t counter_bytes[kFrameCounterBytes] = {0, 0, 0, 0};
  const size_t bytes_read = byte_source_->read_at(
      counter_offset, kFrameCounterBytes, counter_bytes, error_message);
  if (!error_message.empty()) {
    return false;
  }
  if (bytes_read != kFrameCounterBytes) {
    error_message = "Capture ends part-way through frame " +
                    std::to_string(frame_index) +
                    ": its frame counter could not be read. The stream length "
                    "is not an exact multiple of the configured frame size.";
    return false;
  }

  out_counter = decode_frame_counter(counter_bytes);
  return true;
}

bool VBILineReader::read_frame(uint64_t frame_index,
                               VBIFrameRecords& out_records,
                               std::string& error_message) const {
  std::vector<uint8_t> frame_buffer;
  if (!read_frame_bytes(frame_index, frame_buffer, error_message)) {
    out_records = VBIFrameRecords{};
    out_records.frame_index = frame_index;
    return false;
  }
  return decode_frame(frame_index, frame_buffer, out_records, error_message);
}

bool VBILineReader::read_frame_bytes(uint64_t frame_index,
                                     std::vector<uint8_t>& out_bytes,
                                     std::string& error_message) const {
  const uint64_t frame_bytes = format_.bytes_per_frame();
  if (frame_bytes == 0) {
    out_bytes.clear();
    error_message =
        "VBI container geometry is unset (line_length and field_lines must "
        "both be non-zero).";
    return false;
  }

  const uint64_t frame_offset = frame_index * frame_bytes;

  out_bytes.resize(static_cast<size_t>(frame_bytes));
  const size_t bytes_read = byte_source_->read_at(
      frame_offset, out_bytes.size(), out_bytes.data(), error_message);
  if (!error_message.empty()) {
    return false;
  }
  if (bytes_read != out_bytes.size()) {
    // A short final frame is a reported error, never a silent truncation: it
    // means the configured frame size does not divide the capture, so the
    // whole configuration is suspect (design §1.1, §8).
    error_message = "Capture ends part-way through frame " +
                    std::to_string(frame_index) + ": read " +
                    std::to_string(bytes_read) + " of " +
                    std::to_string(frame_bytes) +
                    " bytes. The stream length is not an exact multiple of the "
                    "configured frame size.";
    return false;
  }
  return true;
}

bool VBILineReader::decode_frame(uint64_t frame_index,
                                 const std::vector<uint8_t>& frame_buffer,
                                 VBIFrameRecords& out_records,
                                 std::string& error_message) const {
  out_records = VBIFrameRecords{};
  out_records.frame_index = frame_index;

  const uint64_t frame_bytes = format_.bytes_per_frame();
  if (frame_bytes == 0) {
    error_message =
        "VBI container geometry is unset (line_length and field_lines must "
        "both be non-zero).";
    return false;
  }
  if (frame_buffer.size() != frame_bytes) {
    error_message = "Frame " + std::to_string(frame_index) + " holds " +
                    std::to_string(frame_buffer.size()) + " bytes where " +
                    std::to_string(frame_bytes) +
                    " were expected, so it is not one whole stored frame.";
    return false;
  }

  const uint64_t frame_offset = frame_index * frame_bytes;

  // The frame trailer overlaps the padding of the final record rather than
  // extending the frame, so it is read from the frame's last bytes.
  if (format_.frame_trailer_is_counter &&
      format_.frame_trailer_bytes >= kFrameCounterBytes &&
      frame_bytes >= kFrameCounterBytes) {
    out_records.frame_counter = decode_frame_counter(
        frame_buffer.data() + frame_buffer.size() - kFrameCounterBytes);
  }

  const uint64_t record_bytes = format_.bytes_per_record();
  const uint64_t field_bytes = format_.bytes_per_field();
  const uint32_t records_per_field =
      format_.field_range.count() + format_.caption_records;

  out_records.lines.reserve(static_cast<size_t>(records_per_field) *
                            kStoredFieldsPerFrame);

  for (uint32_t field_index = 0; field_index < kStoredFieldsPerFrame;
       ++field_index) {
    for (uint32_t record_index = format_.field_range.start;
         record_index <= format_.field_range.end + format_.caption_records &&
         record_index < format_.field_lines;
         ++record_index) {
      const uint64_t record_offset =
          field_index * field_bytes + record_index * record_bytes;

      VBILineRecord line;
      line.frame_index = frame_index;
      line.field_index = field_index;
      line.record_index = record_index;
      line.source_byte_offset = frame_offset + record_offset;
      line.source_byte_length = static_cast<uint64_t>(format_.valid_samples) *
                                format_.bytes_per_sample();

      if (!decode_vbi_samples(
              format_.sample_format, frame_buffer.data() + record_offset,
              format_.valid_samples, line.samples, error_message)) {
        return false;
      }
      line.byte_samples = format_.sample_format == VBISampleFormat::kU8;

      out_records.lines.push_back(std::move(line));
    }
  }

  return true;
}

}  // namespace orc
