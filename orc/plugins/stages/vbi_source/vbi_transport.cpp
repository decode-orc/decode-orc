/*
 * File:        vbi_transport.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Raw-file, FLAC-wrapped and video-file byte transports for VBI
 *              captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_transport.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

namespace orc {

const char kFlacStreamMarker[5] = {'f', 'L', 'a', 'C', '\0'};

bool has_flac_stream_marker(const uint8_t* buffer, size_t length) {
  if (buffer == nullptr || length < 4) {
    return false;
  }
  return std::memcmp(buffer, kFlacStreamMarker, 4) == 0;
}

const uint8_t kMatroskaStreamMarker[4] = {0x1A, 0x45, 0xDF, 0xA3};

bool has_matroska_stream_marker(const uint8_t* buffer, size_t length) {
  if (buffer == nullptr || length < sizeof(kMatroskaStreamMarker)) {
    return false;
  }
  return std::memcmp(buffer, kMatroskaStreamMarker,
                     sizeof(kMatroskaStreamMarker)) == 0;
}

uint16_t vbi_sample_word_from_luma(uint32_t luma,
                                   const VBIVideoLumaCoding& coding) {
  // ITU-R BT.601-5 Section 3.5.3: limited-range luma is 219 E + 16 at 8 bits,
  // and each further bit of depth doubles both figures.
  constexpr double kLimitedBlack8 = 16.0;
  constexpr double kLimitedSpan8 = 219.0;
  constexpr double kLimitedWhiteWord = 65280.0;
  constexpr double kWordMax = std::numeric_limits<uint16_t>::max();

  const uint32_t depth = std::clamp<uint32_t>(coding.bit_depth, 8u, 16u);
  double word = 0.0;
  if (coding.full_range) {
    const double luma_max = static_cast<double>((1u << depth) - 1u);
    word = static_cast<double>(luma) * kWordMax / luma_max;
  } else {
    const double scale = static_cast<double>(1u << (depth - 8u));
    word = (static_cast<double>(luma) - kLimitedBlack8 * scale) *
           kLimitedWhiteWord / (kLimitedSpan8 * scale);
  }
  return static_cast<uint16_t>(std::clamp(std::round(word), 0.0, kWordMax));
}

namespace {

// How far ahead of the current decode position a request may sit before it is
// cheaper to seek than to decode forward.  The captures in circulation come in
// two block sizes: libFLAC's --blocksize=65535 (64 KiB of 8-bit samples, 128
// KiB of 16-bit ones) and FFmpeg's own encoder, which writes 4608-sample
// blocks.  4 MiB is a few dozen of the one and several hundred of the other:
// far enough that ordinary sequential frame reads never seek, close enough
// that a scrub across the capture does.  A reader that knows better sets its
// own (set_forward_decode_limit()).
constexpr uint64_t kForwardDecodeLimitBytes = 4ull * 1024 * 1024;

// How far back a failed seek is retried.  Captures written by FFmpeg's encoder
// (every "Lavf" .flac in circulation) carry no SEEKTABLE, so libavformat
// locates a target by bisecting the stream and parsing a frame header at each
// probe position to read its timestamp; those written by libFLAC carry one,
// and the bisection starts from its nearest points.  A probe that lands where
// the parser cannot complete a frame abandons the whole seek, even though
// positions on either side of it are perfectly seekable -- the reference
// capture has one such target in its last few frames, which is exactly where
// reading the final frames lands.  Seeking earlier than asked is always sound
// here, because the caller only needs to be positioned at or before the
// target and decodes forward from there; the only cost is decoding the bytes
// stepped over, which is why the ladder starts small.
constexpr uint64_t kSeekRetryBackoffBytes[] = {
    1ull * 1024 * 1024,
    8ull * 1024 * 1024,
    64ull * 1024 * 1024,
};

// Bits per sample this transport can unwrap: 8-bit for the card capture family
// and 16-bit for the TBC-derived one.  Both are re-biased to unsigned on the
// way out, because the FLAC header carries no signedness and every capture in
// circulation was written by the community encoder's `--sign=unsigned`
// invocation.
constexpr uint32_t kSupportedBitsPerSample[] = {8, 16};

bool is_supported_bits_per_sample(int bits) {
  for (const uint32_t supported : kSupportedBitsPerSample) {
    if (bits == static_cast<int>(supported)) {
      return true;
    }
  }
  return false;
}

// -------------------------------------------------------------------------
// Plain file transport
// -------------------------------------------------------------------------

class VBIRawFileByteSource final : public IVBIByteSource {
 public:
  static std::unique_ptr<VBIRawFileByteSource> open(
      const std::string& path, std::string& error_message) {
    auto source =
        std::unique_ptr<VBIRawFileByteSource>(new VBIRawFileByteSource(path));
    if (!source->stream_.is_open()) {
      error_message = "Could not open VBI capture '" + path + "' for reading.";
      return nullptr;
    }
    return source;
  }

  std::optional<uint64_t> size_bytes() const override { return size_bytes_; }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& error_message) override {
    if (count == 0) {
      return 0;
    }
    if (byte_offset >= size_bytes_) {
      return 0;
    }

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(byte_offset), std::ios::beg);
    if (!stream_) {
      error_message = "Seek to byte " + std::to_string(byte_offset) +
                      " failed in VBI capture '" + path_ + "'.";
      return 0;
    }

    const uint64_t available = size_bytes_ - byte_offset;
    const size_t wanted = static_cast<size_t>(
        std::min<uint64_t>(available, static_cast<uint64_t>(count)));
    stream_.read(reinterpret_cast<char*>(out_buffer),
                 static_cast<std::streamsize>(wanted));
    const size_t produced = static_cast<size_t>(stream_.gcount());
    bytes_read_ += produced;
    return produced;
  }

  uint64_t bytes_decoded() const override { return bytes_read_; }

 private:
  explicit VBIRawFileByteSource(const std::string& path)
      : path_(path), stream_(path, std::ios::binary) {
    if (stream_.is_open()) {
      stream_.seekg(0, std::ios::end);
      size_bytes_ = static_cast<uint64_t>(stream_.tellg());
      stream_.seekg(0, std::ios::beg);
    }
  }

  std::string path_;
  mutable std::ifstream stream_;
  uint64_t size_bytes_ = 0;
  uint64_t bytes_read_ = 0;
};

// -------------------------------------------------------------------------
// FLAC transport
//
// FLAC is a lossless byte compressor here, not audio.  The declared sample
// rate is a conventional placeholder and is never consulted; only the
// declared bits per sample is meaningful, and it seeds sample-format
// detection rather than any timing decision (design §3.3).
//
// The stream is decoded lazily, one block at a time, and never materialised:
// a four-hour bt8x8 capture is 24 GB raw.  Random access asks the demuxer to
// seek so that scrubbing does not re-decode from the head of the file; where
// the capture carries a SEEKTABLE (libFLAC's encoder writes one) the search
// starts from its points, and where it does not (FFmpeg's encoder, which wrote
// most captures in circulation) libavformat bisects the stream instead, which
// is fallible in the way seek_at_or_before covers.
//
// One instance decodes strictly forward from one position, so it serves one
// reader moving through the capture in order and nothing else well: a request
// behind the current block is a seek and a re-decode.  Readers working a
// capture from several threads go through the stage's read-ahead
// (vbi_frame_read_ahead.h), which runs instances of this on its own threads.
// -------------------------------------------------------------------------

class VBIFlacByteSource final : public IVBIByteSource {
 public:
  ~VBIFlacByteSource() override {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_ctx_);
    if (format_ctx_ != nullptr) {
      avformat_close_input(&format_ctx_);
    }
  }

  static std::unique_ptr<VBIFlacByteSource> open(const std::string& path,
                                                 std::string& error_message);

  std::optional<uint64_t> size_bytes() const override {
    if (total_bytes_ == 0) {
      return std::nullopt;
    }
    return total_bytes_;
  }

  std::optional<uint32_t> declared_bits_per_sample() const override {
    return bits_per_sample_;
  }

  uint64_t bytes_decoded() const override { return bytes_decoded_; }

  void set_forward_decode_limit(uint64_t bytes) override {
    forward_decode_limit_ = bytes;
  }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& error_message) override;

 private:
  VBIFlacByteSource() = default;

  // Reposition the decoder so that the next decoded block covers or precedes
  // target_byte, seeking only when decoding forward would be wasteful.
  bool ensure_position(uint64_t target_byte, std::string& error_message);

  // Seek to the last stream position at or before target_byte, retrying
  // earlier positions when the demuxer refuses one.  On success
  // out_landed_byte is the position actually asked for, which is at or before
  // target_byte and is where the decoder will resume.
  bool seek_at_or_before(uint64_t target_byte, uint64_t& out_landed_byte);

  // Decode the next block into block_bytes_, setting block_start_byte_ from
  // its presentation timestamp.  Returns false at end of stream (with an
  // empty error message) or on a decode failure.
  bool decode_next_block(std::string& error_message);

  // Convert one decoded AVFrame into the capture's raw byte sequence.
  bool convert_frame_to_bytes(std::string& error_message);

  AVFormatContext* format_ctx_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  int stream_index_ = -1;

  uint32_t bits_per_sample_ = 0;
  uint32_t bytes_per_sample_ = 0;
  uint64_t total_bytes_ = 0;

  // Used only to convert a block's timestamp into a sample index, never to
  // derive VBI timing: the declared rate is a placeholder (design §3.3).
  AVRational stream_time_base_{0, 1};
  int declared_sample_rate_ = 0;

  std::vector<uint8_t> block_bytes_;
  uint64_t block_start_byte_ = 0;
  bool have_block_ = false;
  bool draining_ = false;
  bool at_end_ = false;
  uint64_t bytes_decoded_ = 0;
  uint64_t forward_decode_limit_ = kForwardDecodeLimitBytes;

  // Where the decoder will resume when no block has been decoded since the
  // last reposition.  Without a seek it resumes at the head of the stream;
  // after one it resumes at or before the byte that was seeked to.
  bool seek_pending_ = false;
  uint64_t seek_target_byte_ = 0;
};

std::unique_ptr<VBIFlacByteSource> VBIFlacByteSource::open(
    const std::string& path, std::string& error_message) {
  auto source = std::unique_ptr<VBIFlacByteSource>(new VBIFlacByteSource());

  if (avformat_open_input(&source->format_ctx_, path.c_str(), nullptr,
                          nullptr) < 0) {
    error_message = "Could not open FLAC-wrapped VBI capture '" + path + "'.";
    return nullptr;
  }
  if (avformat_find_stream_info(source->format_ctx_, nullptr) < 0) {
    error_message = "Could not read the stream information of '" + path + "'.";
    return nullptr;
  }

  for (unsigned int index = 0; index < source->format_ctx_->nb_streams;
       ++index) {
    const AVCodecParameters* params =
        source->format_ctx_->streams[index]->codecpar;
    if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
      source->stream_index_ = static_cast<int>(index);
      break;
    }
  }
  if (source->stream_index_ < 0) {
    error_message = "'" + path + "' contains no FLAC stream to unwrap.";
    return nullptr;
  }

  AVStream* stream = source->format_ctx_->streams[source->stream_index_];
  AVCodecParameters* params = stream->codecpar;

  if (params->codec_id != AV_CODEC_ID_FLAC) {
    error_message = "'" + path +
                    "' is compressed with an unsupported codec; VBI captures "
                    "are transported either raw or FLAC-wrapped.";
    return nullptr;
  }

  // The community encoder invocation uses --channels=1; a multi-channel
  // stream would interleave the byte sequence and is not a VBI capture.
  const int channels = params->ch_layout.nb_channels;
  if (channels != 1) {
    error_message = "'" + path + "' has " + std::to_string(channels) +
                    " channels; a FLAC-wrapped VBI capture is mono.";
    return nullptr;
  }

  int declared_bits = params->bits_per_raw_sample;
  if (declared_bits <= 0) {
    declared_bits = params->bits_per_coded_sample;
  }
  if (!is_supported_bits_per_sample(declared_bits)) {
    error_message = "'" + path + "' declares " + std::to_string(declared_bits) +
                    " bits per sample; a FLAC-wrapped VBI capture holds 8-bit "
                    "or 16-bit samples.";
    return nullptr;
  }
  source->bits_per_sample_ = static_cast<uint32_t>(declared_bits);
  source->bytes_per_sample_ = (source->bits_per_sample_ + 7u) / 8u;

  const AVCodec* codec = avcodec_find_decoder(params->codec_id);
  if (codec == nullptr) {
    error_message = "No FLAC decoder is available in this build.";
    return nullptr;
  }
  source->codec_ctx_ = avcodec_alloc_context3(codec);
  if (source->codec_ctx_ == nullptr) {
    error_message = "Could not allocate a FLAC decoder context.";
    return nullptr;
  }
  if (avcodec_parameters_to_context(source->codec_ctx_, params) < 0) {
    error_message = "Could not configure the FLAC decoder for '" + path + "'.";
    return nullptr;
  }
  // Required for the decoder to propagate packet timestamps onto frames,
  // which is how a decoded block's byte position is established after a seek.
  source->codec_ctx_->pkt_timebase = stream->time_base;
  source->stream_time_base_ = stream->time_base;
  source->declared_sample_rate_ = params->sample_rate;
  if (avcodec_open2(source->codec_ctx_, codec, nullptr) < 0) {
    error_message = "Could not open the FLAC decoder for '" + path + "'.";
    return nullptr;
  }

  source->packet_ = av_packet_alloc();
  source->frame_ = av_frame_alloc();
  if (source->packet_ == nullptr || source->frame_ == nullptr) {
    error_message = "Could not allocate FLAC decoding buffers.";
    return nullptr;
  }

  // The FLAC STREAMINFO block carries the total sample count, which the
  // demuxer exposes as the stream duration in sample units.  Note that the
  // declared sample rate is deliberately not used for anything.
  if (stream->duration > 0) {
    source->total_bytes_ =
        static_cast<uint64_t>(stream->duration) * source->bytes_per_sample_;
  }

  ORC_LOG_DEBUG(
      "VBI transport: unwrapping FLAC capture '{}' ({} bits/sample, {} raw "
      "bytes); the declared sample rate of {} Hz is ignored",
      path, source->bits_per_sample_, source->total_bytes_,
      params->sample_rate);

  return source;
}

bool VBIFlacByteSource::convert_frame_to_bytes(std::string& error_message) {
  const AVSampleFormat sample_fmt = static_cast<AVSampleFormat>(frame_->format);
  const int container_bytes = av_get_bytes_per_sample(sample_fmt);
  if (container_bytes != 2 && container_bytes != 4) {
    error_message =
        "The FLAC decoder produced an unexpected sample format for a VBI "
        "capture.";
    return false;
  }

  // The decoder left-aligns the coded sample within its container word, so
  // the raw value is recovered by shifting back down.  The community encoder
  // uses --sign=unsigned, so the signed decoded value is re-biased to the
  // unsigned byte the capture card originally wrote.
  const int shift = container_bytes * 8 - static_cast<int>(bits_per_sample_);
  const int32_t unsigned_bias = 1 << (bits_per_sample_ - 1);

  const uint8_t* plane = frame_->extended_data[0];
  const int sample_count = frame_->nb_samples;

  // Captured before the resize below so the timestamp-free fallback can
  // advance the position by the block that is being replaced.
  const size_t previous_block_bytes = block_bytes_.size();

  block_bytes_.resize(static_cast<size_t>(sample_count) * bytes_per_sample_);
  uint8_t* out = block_bytes_.data();
  const auto* coded16 = reinterpret_cast<const int16_t*>(plane);
  if (container_bytes == 2 && bytes_per_sample_ == 2 && shift == 0) {
    // The 16-bit family, in 16-bit words: adding 2^15 to a 16-bit two's
    // complement value is flipping its top bit, so the whole conversion is an
    // XOR and a little-endian store, which the compiler can vectorise.  This
    // runs once per decoded sample of every capture of the family.
    for (int index = 0; index < sample_count; ++index) {
      const auto raw = static_cast<uint16_t>(
          static_cast<uint16_t>(coded16[index]) ^ 0x8000u);
      out[2 * static_cast<size_t>(index)] = static_cast<uint8_t>(raw & 0xFFu);
      out[2 * static_cast<size_t>(index) + 1] = static_cast<uint8_t>(raw >> 8);
    }
  } else if (container_bytes == 2 && bytes_per_sample_ == 1 && shift == 8) {
    // The 8-bit family, left-aligned in 16-bit words: the sample is the word's
    // top byte, and re-biasing it by 2^7 is flipping that byte's top bit.
    for (int index = 0; index < sample_count; ++index) {
      out[index] = static_cast<uint8_t>(
          (static_cast<uint16_t>(coded16[index]) >> 8) ^ 0x80u);
    }
  } else {
    for (int index = 0; index < sample_count; ++index) {
      int32_t coded = 0;
      if (container_bytes == 2) {
        coded = coded16[index];
      } else {
        coded = reinterpret_cast<const int32_t*>(plane)[index];
      }

      const uint32_t raw =
          static_cast<uint32_t>((coded >> shift) + unsigned_bias);
      for (uint32_t byte_index = 0; byte_index < bytes_per_sample_;
           ++byte_index) {
        out[static_cast<size_t>(index) * bytes_per_sample_ + byte_index] =
            static_cast<uint8_t>((raw >> (8u * byte_index)) & 0xFFu);
      }
    }
  }

  // The block's position comes from its timestamp rather than from an
  // accumulated count, so a seek lands at a known byte offset.
  if (frame_->pts != AV_NOPTS_VALUE && declared_sample_rate_ > 0) {
    const AVRational sample_time_base{1, declared_sample_rate_};
    const int64_t sample_index =
        av_rescale_q(frame_->pts, stream_time_base_, sample_time_base);
    block_start_byte_ = static_cast<uint64_t>(sample_index) * bytes_per_sample_;
  } else if (have_block_) {
    block_start_byte_ += previous_block_bytes;
  } else {
    error_message =
        "The FLAC stream has no timestamps, so a byte position cannot be "
        "established.";
    return false;
  }

  have_block_ = true;
  bytes_decoded_ += block_bytes_.size();
  return true;
}

bool VBIFlacByteSource::decode_next_block(std::string& error_message) {
  while (true) {
    const int receive_result = avcodec_receive_frame(codec_ctx_, frame_);
    if (receive_result == 0) {
      return convert_frame_to_bytes(error_message);
    }
    if (receive_result == AVERROR_EOF) {
      at_end_ = true;
      return false;
    }
    if (receive_result != AVERROR(EAGAIN)) {
      error_message = "FLAC decoding failed while unwrapping the capture.";
      return false;
    }

    if (draining_) {
      at_end_ = true;
      return false;
    }

    const int read_result = av_read_frame(format_ctx_, packet_);
    if (read_result == AVERROR_EOF) {
      avcodec_send_packet(codec_ctx_, nullptr);
      draining_ = true;
      continue;
    }
    if (read_result < 0) {
      error_message =
          "Reading the FLAC stream failed while unwrapping the "
          "capture.";
      return false;
    }
    if (packet_->stream_index != stream_index_) {
      av_packet_unref(packet_);
      continue;
    }
    const int send_result = avcodec_send_packet(codec_ctx_, packet_);
    av_packet_unref(packet_);
    if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
      error_message = "The FLAC decoder rejected a packet.";
      return false;
    }
  }
}

bool VBIFlacByteSource::seek_at_or_before(uint64_t target_byte,
                                          uint64_t& out_landed_byte) {
  const auto attempt = [&](uint64_t candidate_byte) {
    const int64_t candidate_sample =
        static_cast<int64_t>(candidate_byte / bytes_per_sample_);
    if (av_seek_frame(format_ctx_, stream_index_, candidate_sample,
                      AVSEEK_FLAG_BACKWARD) < 0) {
      return false;
    }
    out_landed_byte = candidate_byte;
    return true;
  };

  if (attempt(target_byte)) {
    return true;
  }

  for (const uint64_t backoff : kSeekRetryBackoffBytes) {
    if (backoff >= target_byte) {
      break;
    }
    if (attempt(target_byte - backoff)) {
      ORC_LOG_WARN(
          "VBI transport: the FLAC demuxer refused a seek to byte {}; "
          "resumed from byte {} and decoded forward",
          target_byte, target_byte - backoff);
      return true;
    }
  }

  // The head of the stream needs no bisection, so it is the one position that
  // is always reachable; taking it turns a hard failure into a slow read
  // rather than a wrong one.
  if (target_byte > 0 && attempt(0)) {
    ORC_LOG_WARN(
        "VBI transport: no seek point near byte {} was accepted; decoding "
        "forward from the head of the stream",
        target_byte);
    return true;
  }
  return false;
}

bool VBIFlacByteSource::ensure_position(uint64_t target_byte,
                                        std::string& error_message) {
  if (have_block_) {
    const uint64_t block_end = block_start_byte_ + block_bytes_.size();
    const bool behind = target_byte < block_start_byte_;
    const bool far_ahead = target_byte > block_end + forward_decode_limit_;
    if (!behind && !far_ahead) {
      return true;
    }
  } else if (seek_pending_) {
    // The decoder is positioned to resume at or before the last seek target,
    // so anything from there up to the forward window is reachable without
    // seeking again.
    if (target_byte >= seek_target_byte_ &&
        target_byte <= seek_target_byte_ + forward_decode_limit_) {
      return true;
    }
  } else if (target_byte <= forward_decode_limit_) {
    // Untouched stream: the decoder resumes at the head.
    return true;
  }

  uint64_t landed_byte = 0;
  if (!seek_at_or_before(target_byte, landed_byte)) {
    error_message = "Seeking the FLAC stream to byte " +
                    std::to_string(target_byte) + " failed.";
    return false;
  }

  avcodec_flush_buffers(codec_ctx_);
  block_bytes_.clear();
  have_block_ = false;
  draining_ = false;
  at_end_ = false;
  seek_pending_ = true;
  // Where the decoder resumes, which a retry may have put earlier than asked.
  seek_target_byte_ = landed_byte;
  return true;
}

size_t VBIFlacByteSource::read_at(uint64_t byte_offset, size_t count,
                                  uint8_t* out_buffer,
                                  std::string& error_message) {
  if (count == 0) {
    return 0;
  }
  if (!ensure_position(byte_offset, error_message)) {
    return 0;
  }

  size_t produced = 0;
  while (produced < count) {
    const uint64_t wanted_byte = byte_offset + produced;

    if (have_block_ && wanted_byte >= block_start_byte_ &&
        wanted_byte < block_start_byte_ + block_bytes_.size()) {
      const size_t within =
          static_cast<size_t>(wanted_byte - block_start_byte_);
      const size_t available = block_bytes_.size() - within;
      const size_t chunk = std::min(count - produced, available);
      std::memcpy(out_buffer + produced, block_bytes_.data() + within, chunk);
      produced += chunk;
      continue;
    }

    if (have_block_ && wanted_byte < block_start_byte_) {
      // A seek landed past the request; decoding forward can never reach it.
      error_message = "The FLAC stream seeked past byte " +
                      std::to_string(wanted_byte) +
                      "; the capture's seek points are inconsistent.";
      return 0;
    }

    if (!decode_next_block(error_message)) {
      if (!error_message.empty()) {
        return 0;
      }
      break;  // End of stream: a short read, which the caller interprets.
    }
  }

  return produced;
}

// -------------------------------------------------------------------------
// Video transport
//
// The crop of a time-base corrected capture is sometimes kept as the pictures
// of a lossless video rather than as raw words.  A vhs-decode archive can
// keep beside each .tbc a vbi.mkv made by its own script: field lines 9-24 of
// every field — the sixteen records of the .tbc crop formats — handed to FFmpeg
// two fields at a time as a 910 x 32 picture of 16-bit grey, which FFmpeg
// converts to limited-range 8-bit 4:2:0 and codes losslessly as FFV1 in
// Matroska.  A picture row is one line record, the pictures follow one another
// as the fields do, and nothing else in the file is capture data: the chroma
// planes hold FFmpeg's neutral 128.
//
// So the transport serves the luma rows in order, each sample turned back into
// the 16-bit word it was made from (vbi_sample_word_from_luma), and what it
// serves is the crop's own byte sequence, which the formats written for the
// crop read unchanged.  The words come back to within the 8-bit picture's
// step — 298 words, under an IRE — plus the conversion's ordered dither of a
// step either way: on the archive's tapes the copies also kept as 16-bit FLAC
// correlate with them at 0,9995 or better.
//
// How many fields a picture holds does not matter, because a picture row is a
// record and the records simply follow one another: one of the archive's
// files stores a field to a 910 x 16 picture and reads the same way.  What
// does matter is the picture's width, which is the record length, and the
// stage refuses a format whose records are any other length.
//
// A picture is decoded only when a read reaches it, and one instance decodes
// forward from one position, as the FLAC transport does.  Matroska indexes the
// keyframes FFV1 writes — every twelfth picture at FFmpeg's default — so a
// request behind the current picture, or further ahead of it than the forward
// limit, seeks to the keyframe at or before it and decodes on from there.
// -------------------------------------------------------------------------

// The capture words a video-carried capture is unwrapped into: those of the
// .tbc it was cropped from, whatever the picture's own depth.
constexpr uint32_t kVideoCaptureWordBits = 16;
constexpr uint32_t kVideoCaptureWordBytes = kVideoCaptureWordBits / 8;

// How far before a picture a seek is retried when the keyframe the demuxer
// lands on turns out to lie after it, in pictures.  A Matroska index lists
// every keyframe, so the first attempt lands at or before the picture on any
// file FFmpeg wrote; these are for a file whose index is incomplete, and they
// end at the head of the stream, which needs no index at all.
constexpr uint64_t kVideoSeekRetryBackoffPictures[] = {64, 4096};

// The luma coding of a decoded picture format, or false when its luma is not a
// plane of whole little-endian samples this transport can read.
bool video_luma_coding(AVPixelFormat pixel_format, AVColorRange color_range,
                       VBIVideoLumaCoding& out_coding,
                       uint32_t& out_bytes_per_sample) {
  const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixel_format);
  if (descriptor == nullptr || descriptor->nb_components == 0) {
    return false;
  }
  constexpr uint64_t kUnreadableFlags =
      AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
      AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_BE;
  if ((descriptor->flags & kUnreadableFlags) != 0) {
    return false;
  }

  // The luma has to be plane 0, one sample after another with nothing
  // between them, and right-aligned in its word.
  const AVComponentDescriptor& luma = descriptor->comp[0];
  const int depth = luma.depth;
  if (luma.plane != 0 || luma.offset != 0 || luma.shift != 0 || depth < 8 ||
      depth > 16) {
    return false;
  }
  const int bytes_per_sample = depth > 8 ? 2 : 1;
  if (luma.step != bytes_per_sample) {
    return false;
  }

  // FFmpeg's convention for a stream that does not say: grey and the JPEG
  // formats are full range, every other YUV format limited.
  bool full_range = false;
  if (color_range == AVCOL_RANGE_JPEG) {
    full_range = true;
  } else if (color_range == AVCOL_RANGE_UNSPECIFIED) {
    switch (pixel_format) {
      case AV_PIX_FMT_YUVJ411P:
      case AV_PIX_FMT_YUVJ420P:
      case AV_PIX_FMT_YUVJ422P:
      case AV_PIX_FMT_YUVJ440P:
      case AV_PIX_FMT_YUVJ444P:
        full_range = true;
        break;
      default:
        full_range = descriptor->nb_components == 1;
        break;
    }
  }

  out_coding.bit_depth = static_cast<uint32_t>(depth);
  out_coding.full_range = full_range;
  out_bytes_per_sample = static_cast<uint32_t>(bytes_per_sample);
  return true;
}

class VBIVideoByteSource final : public IVBIByteSource {
 public:
  ~VBIVideoByteSource() override {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_ctx_);
    if (format_ctx_ != nullptr) {
      avformat_close_input(&format_ctx_);
    }
  }

  static std::unique_ptr<VBIVideoByteSource> open(const std::string& path,
                                                  std::string& error_message);

  std::optional<uint64_t> size_bytes() const override {
    if (picture_count_ == 0) {
      return std::nullopt;
    }
    return picture_count_ * picture_bytes_;
  }

  std::optional<uint32_t> declared_bits_per_sample() const override {
    return kVideoCaptureWordBits;
  }

  std::optional<uint32_t> declared_record_samples() const override {
    return width_;
  }

  uint64_t bytes_decoded() const override { return bytes_decoded_; }

  void set_forward_decode_limit(uint64_t bytes) override {
    forward_decode_limit_ = bytes;
  }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& error_message) override;

 private:
  VBIVideoByteSource() = default;

  // The picture a timestamp of the stream stands for, and the timestamp of a
  // picture.  Pictures are numbered from the stream's first.
  uint64_t picture_at(int64_t timestamp) const;
  int64_t timestamp_of(uint64_t picture) const;

  // How many pictures the stream holds: one more than the last packet's
  // picture, read from the last keyframe the index lists onward without
  // decoding anything, or else what the stream's duration says.  Leaves the
  // demuxer wherever the count took it.
  uint64_t count_pictures();

  // Make |picture| the decoded one, decoding forward to it or repositioning
  // first as its distance from the decoder warrants.  Returns false with an
  // empty error message when the stream ends before it.
  bool ensure_picture(uint64_t picture, std::string& error_message);

  // Position the decoder so that the next picture it decodes is |picture| or
  // one before it, decoding that one.
  bool reposition(uint64_t picture, std::string& error_message);

  // Ask the demuxer for the keyframe at or before |picture|, and drop
  // everything decoded so far.
  bool seek_to_keyframe_before(uint64_t picture);

  // Decode the next picture into picture_words_.  Returns false at the end of
  // the stream (with an empty error message) or on a failure.
  bool decode_next_picture(std::string& error_message);

  // Turn the decoded frame's luma rows into capture words.
  bool convert_picture(std::string& error_message);

  AVFormatContext* format_ctx_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  int stream_index_ = -1;
  AVRational time_base_{0, 1};
  AVRational picture_duration_{0, 1};
  int64_t start_timestamp_ = 0;

  AVPixelFormat pixel_format_ = AV_PIX_FMT_NONE;
  uint32_t luma_bytes_per_sample_ = 1;
  std::vector<uint16_t> word_from_luma_;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint64_t picture_bytes_ = 0;
  uint64_t picture_count_ = 0;

  std::vector<uint8_t> picture_words_;
  uint64_t picture_ = 0;
  bool have_picture_ = false;
  bool draining_ = false;
  bool awaiting_keyframe_ = false;
  uint64_t bytes_decoded_ = 0;
  uint64_t forward_decode_limit_ = kForwardDecodeLimitBytes;

  // The picture the decoder resumes at, or before, when nothing has been
  // decoded since it was last positioned: the head of the stream, or the
  // picture a seek asked for.
  uint64_t resume_picture_ = 0;
};

std::unique_ptr<VBIVideoByteSource> VBIVideoByteSource::open(
    const std::string& path, std::string& error_message) {
  auto source = std::unique_ptr<VBIVideoByteSource>(new VBIVideoByteSource());

  if (avformat_open_input(&source->format_ctx_, path.c_str(), nullptr,
                          nullptr) < 0) {
    error_message = "Could not open the video-file VBI capture '" + path + "'.";
    return nullptr;
  }
  if (avformat_find_stream_info(source->format_ctx_, nullptr) < 0) {
    error_message = "Could not read the stream information of '" + path + "'.";
    return nullptr;
  }

  const int stream_index = av_find_best_stream(
      source->format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (stream_index < 0) {
    error_message = "'" + path + "' contains no video stream to unwrap.";
    return nullptr;
  }
  source->stream_index_ = stream_index;
  for (unsigned int index = 0; index < source->format_ctx_->nb_streams;
       ++index) {
    if (static_cast<int>(index) != stream_index) {
      source->format_ctx_->streams[index]->discard = AVDISCARD_ALL;
    }
  }

  AVStream* stream = source->format_ctx_->streams[stream_index];
  const AVCodecParameters* params = stream->codecpar;

  // A lossy codec would have rebuilt the waveform rather than kept it, and
  // the data is in the waveform.
  const AVCodecDescriptor* codec_descriptor =
      avcodec_descriptor_get(params->codec_id);
  if (codec_descriptor == nullptr ||
      (codec_descriptor->props & AV_CODEC_PROP_LOSSLESS) == 0) {
    error_message =
        "'" + path + "' is coded with " +
        std::string(codec_descriptor != nullptr ? codec_descriptor->name
                                                : "an unknown codec") +
        ", which cannot code a picture losslessly; a VBI capture carried in "
        "a video file has to be coded losslessly (FFV1, for one).";
    return nullptr;
  }

  source->pixel_format_ = static_cast<AVPixelFormat>(params->format);
  VBIVideoLumaCoding coding;
  if (!video_luma_coding(source->pixel_format_, params->color_range, coding,
                         source->luma_bytes_per_sample_)) {
    const char* name = av_get_pix_fmt_name(source->pixel_format_);
    error_message =
        "'" + path + "' stores its pictures as " +
        std::string(name != nullptr ? name : "an unknown pixel format") +
        "; a VBI capture carried in a video file is read from a luma plane "
        "of 8 to 16 bits.";
    return nullptr;
  }

  if (params->width <= 0 || params->height <= 0) {
    error_message = "'" + path + "' declares no picture size.";
    return nullptr;
  }
  source->width_ = static_cast<uint32_t>(params->width);
  source->height_ = static_cast<uint32_t>(params->height);
  source->picture_bytes_ = static_cast<uint64_t>(source->width_) *
                           source->height_ * kVideoCaptureWordBytes;
  source->picture_words_.assign(source->picture_bytes_, 0);

  // Pictures are placed by their timestamps, which need the picture rate to
  // turn into a count.  The rate is a container convention here, never a
  // timing: what the samples are is the format's business.
  AVRational rate = stream->avg_frame_rate;
  if (rate.num <= 0 || rate.den <= 0) {
    rate = stream->r_frame_rate;
  }
  if (rate.num <= 0 || rate.den <= 0) {
    error_message = "'" + path +
                    "' declares no picture rate, so its pictures cannot be "
                    "placed in order.";
    return nullptr;
  }
  source->picture_duration_ = av_inv_q(rate);
  source->time_base_ = stream->time_base;
  source->start_timestamp_ =
      stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;

  const uint32_t luma_codes = 1u << (source->luma_bytes_per_sample_ * 8u);
  source->word_from_luma_.resize(luma_codes);
  for (uint32_t luma = 0; luma < luma_codes; ++luma) {
    source->word_from_luma_[luma] = vbi_sample_word_from_luma(
        std::min(luma, (1u << coding.bit_depth) - 1u), coding);
  }

  const AVCodec* codec = avcodec_find_decoder(params->codec_id);
  if (codec == nullptr) {
    error_message = "No " + std::string(codec_descriptor->name) +
                    " decoder is available in this build.";
    return nullptr;
  }
  source->codec_ctx_ = avcodec_alloc_context3(codec);
  if (source->codec_ctx_ == nullptr) {
    error_message = "Could not allocate a video decoder context.";
    return nullptr;
  }
  if (avcodec_parameters_to_context(source->codec_ctx_, params) < 0) {
    error_message = "Could not configure the video decoder for '" + path + "'.";
    return nullptr;
  }
  source->codec_ctx_->pkt_timebase = stream->time_base;
  // One thread a decoder: the stage's read-ahead runs a decoder a thread and
  // sizes their number to the readers it serves.
  source->codec_ctx_->thread_count = 1;
  if (avcodec_open2(source->codec_ctx_, codec, nullptr) < 0) {
    error_message = "Could not open the video decoder for '" + path + "'.";
    return nullptr;
  }

  source->packet_ = av_packet_alloc();
  source->frame_ = av_frame_alloc();
  if (source->packet_ == nullptr || source->frame_ == nullptr) {
    error_message = "Could not allocate video decoding buffers.";
    return nullptr;
  }

  source->picture_count_ = source->count_pictures();
  if (source->picture_count_ == 0) {
    error_message = "'" + path + "' holds no pictures that can be counted.";
    return nullptr;
  }
  if (!source->seek_to_keyframe_before(0)) {
    error_message = "'" + path +
                    "' could not be repositioned at its first picture after "
                    "its pictures were counted.";
    return nullptr;
  }

  ORC_LOG_DEBUG(
      "VBI transport: unwrapping video capture '{}' ({}, {}, {}x{} pictures, "
      "{} of them; {}-bit {}-range luma read back as 16-bit words)",
      path, codec_descriptor->name, av_get_pix_fmt_name(source->pixel_format_),
      source->width_, source->height_, source->picture_count_, coding.bit_depth,
      coding.full_range ? "full" : "limited");

  return source;
}

uint64_t VBIVideoByteSource::picture_at(int64_t timestamp) const {
  const int64_t picture =
      av_rescale_q_rnd(timestamp - start_timestamp_, time_base_,
                       picture_duration_, AV_ROUND_NEAR_INF);
  return picture > 0 ? static_cast<uint64_t>(picture) : 0u;
}

int64_t VBIVideoByteSource::timestamp_of(uint64_t picture) const {
  return start_timestamp_ + av_rescale_q_rnd(static_cast<int64_t>(picture),
                                             picture_duration_, time_base_,
                                             AV_ROUND_NEAR_INF);
}

uint64_t VBIVideoByteSource::count_pictures() {
  int64_t last_timestamp = AV_NOPTS_VALUE;
  if (av_seek_frame(format_ctx_, stream_index_,
                    std::numeric_limits<int64_t>::max(),
                    AVSEEK_FLAG_BACKWARD) >= 0) {
    while (av_read_frame(format_ctx_, packet_) >= 0) {
      if (packet_->stream_index == stream_index_) {
        const int64_t timestamp =
            packet_->pts != AV_NOPTS_VALUE ? packet_->pts : packet_->dts;
        if (timestamp != AV_NOPTS_VALUE &&
            (last_timestamp == AV_NOPTS_VALUE || timestamp > last_timestamp)) {
          last_timestamp = timestamp;
        }
      }
      av_packet_unref(packet_);
    }
  }

  uint64_t count = 0;
  const AVStream* stream = format_ctx_->streams[stream_index_];
  if (last_timestamp != AV_NOPTS_VALUE) {
    count = picture_at(last_timestamp) + 1u;
  } else if (stream->duration > 0) {
    count = picture_at(start_timestamp_ + stream->duration);
  } else if (format_ctx_->duration > 0) {
    count = static_cast<uint64_t>(
        av_rescale_q_rnd(format_ctx_->duration, AV_TIME_BASE_Q,
                         picture_duration_, AV_ROUND_NEAR_INF));
  }
  return count;
}

bool VBIVideoByteSource::seek_to_keyframe_before(uint64_t picture) {
  if (av_seek_frame(format_ctx_, stream_index_, timestamp_of(picture),
                    AVSEEK_FLAG_BACKWARD) < 0) {
    return false;
  }
  avcodec_flush_buffers(codec_ctx_);
  have_picture_ = false;
  draining_ = false;
  // A decoder resuming anywhere but at a keyframe has no picture to predict
  // the next from; the demuxer lands on one, and anything else is skipped.
  awaiting_keyframe_ = true;
  resume_picture_ = picture;
  return true;
}

bool VBIVideoByteSource::reposition(uint64_t picture,
                                    std::string& error_message) {
  std::vector<uint64_t> candidates = {picture};
  for (const uint64_t backoff : kVideoSeekRetryBackoffPictures) {
    if (backoff < picture) {
      candidates.push_back(picture - backoff);
    }
  }
  candidates.push_back(0);

  for (const uint64_t candidate : candidates) {
    if (!seek_to_keyframe_before(candidate)) {
      continue;
    }
    if (!decode_next_picture(error_message)) {
      if (!error_message.empty()) {
        return false;
      }
      continue;
    }
    if (picture_ <= picture) {
      if (candidate != picture) {
        ORC_LOG_WARN(
            "VBI transport: the video's keyframe index did not reach picture "
            "{}; resumed from picture {} and decoded forward",
            picture, picture_);
      }
      return true;
    }
  }
  error_message = "No keyframe at or before picture " +
                  std::to_string(picture) +
                  " could be reached in the video stream.";
  return false;
}

bool VBIVideoByteSource::ensure_picture(uint64_t picture,
                                        std::string& error_message) {
  if (have_picture_ && picture_ == picture) {
    return true;
  }

  // Decoding forward is the cheaper way to a picture a little ahead of the
  // decoder; one behind it, or further ahead than the limit, is reached from
  // the keyframe before it.
  const uint64_t resume = have_picture_ ? picture_ + 1u : resume_picture_;
  const bool forward =
      picture >= resume &&
      (picture - resume) * picture_bytes_ <= forward_decode_limit_;
  if (!forward && !reposition(picture, error_message)) {
    return false;
  }

  while (!have_picture_ || picture_ < picture) {
    if (!decode_next_picture(error_message)) {
      return false;
    }
  }
  if (picture_ != picture) {
    error_message = "Picture " + std::to_string(picture) +
                    " is missing from the video stream; the next picture it "
                    "holds is " +
                    std::to_string(picture_) + ".";
    return false;
  }
  return true;
}

bool VBIVideoByteSource::decode_next_picture(std::string& error_message) {
  while (true) {
    const int receive_result = avcodec_receive_frame(codec_ctx_, frame_);
    if (receive_result == 0) {
      const bool converted = convert_picture(error_message);
      av_frame_unref(frame_);
      return converted;
    }
    if (receive_result == AVERROR_EOF) {
      return false;
    }
    if (receive_result != AVERROR(EAGAIN)) {
      error_message = "Video decoding failed while unwrapping the capture.";
      return false;
    }

    if (draining_) {
      return false;
    }

    const int read_result = av_read_frame(format_ctx_, packet_);
    if (read_result == AVERROR_EOF) {
      avcodec_send_packet(codec_ctx_, nullptr);
      draining_ = true;
      continue;
    }
    if (read_result < 0) {
      error_message =
          "Reading the video stream failed while unwrapping the capture.";
      return false;
    }
    if (packet_->stream_index != stream_index_ ||
        (awaiting_keyframe_ && (packet_->flags & AV_PKT_FLAG_KEY) == 0)) {
      av_packet_unref(packet_);
      continue;
    }
    awaiting_keyframe_ = false;
    const int send_result = avcodec_send_packet(codec_ctx_, packet_);
    av_packet_unref(packet_);
    if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
      error_message = "The video decoder rejected a picture of the capture.";
      return false;
    }
  }
}

bool VBIVideoByteSource::convert_picture(std::string& error_message) {
  if (frame_->width != static_cast<int>(width_) ||
      frame_->height != static_cast<int>(height_) ||
      frame_->format != pixel_format_) {
    error_message =
        "The video stream changes its picture size or pixel format part-way "
        "through, so its rows are not one capture's line records.";
    return false;
  }

  const int64_t timestamp = frame_->best_effort_timestamp;
  if (timestamp != AV_NOPTS_VALUE) {
    picture_ = picture_at(timestamp);
  } else if (have_picture_) {
    ++picture_;
  } else {
    error_message =
        "The video stream's pictures carry no timestamps, so their place in "
        "the capture cannot be established.";
    return false;
  }
  have_picture_ = true;

  uint8_t* out = picture_words_.data();
  const uint16_t* words = word_from_luma_.data();
  for (uint32_t row = 0; row < height_; ++row) {
    const uint8_t* line =
        frame_->data[0] + static_cast<ptrdiff_t>(row) * frame_->linesize[0];
    for (uint32_t column = 0; column < width_; ++column) {
      uint32_t luma = line[column];
      if (luma_bytes_per_sample_ == 2) {
        luma = static_cast<uint32_t>(line[size_t{2} * column]) |
               (static_cast<uint32_t>(line[size_t{2} * column + 1u]) << 8u);
      }
      const uint16_t word = words[luma];
      *out++ = static_cast<uint8_t>(word & 0xFFu);
      *out++ = static_cast<uint8_t>(word >> 8u);
    }
  }
  bytes_decoded_ += picture_words_.size();
  return true;
}

size_t VBIVideoByteSource::read_at(uint64_t byte_offset, size_t count,
                                   uint8_t* out_buffer,
                                   std::string& error_message) {
  const uint64_t total_bytes = picture_count_ * picture_bytes_;
  if (count == 0 || byte_offset >= total_bytes) {
    return 0;
  }
  const size_t wanted =
      static_cast<size_t>(std::min<uint64_t>(count, total_bytes - byte_offset));

  size_t produced = 0;
  while (produced < wanted) {
    const uint64_t position = byte_offset + produced;
    const uint64_t picture = position / picture_bytes_;
    if (!ensure_picture(picture, error_message)) {
      if (!error_message.empty()) {
        return 0;
      }
      break;  // End of stream: a short read, which the caller interprets.
    }
    const size_t within = static_cast<size_t>(position % picture_bytes_);
    const size_t chunk = std::min(wanted - produced,
                                  static_cast<size_t>(picture_bytes_ - within));
    std::memcpy(out_buffer + produced, picture_words_.data() + within, chunk);
    produced += chunk;
  }
  return produced;
}

}  // namespace

std::unique_ptr<IVBIByteSource> open_vbi_byte_source(
    const std::string& path, std::string& error_message) {
  uint8_t magic[4] = {0, 0, 0, 0};
  size_t magic_length = 0;
  {
    std::ifstream probe(path, std::ios::binary);
    if (!probe.is_open()) {
      error_message = "Could not open VBI capture '" + path + "' for reading.";
      return nullptr;
    }
    probe.read(reinterpret_cast<char*>(magic), sizeof(magic));
    magic_length = static_cast<size_t>(probe.gcount());
  }

  if (has_flac_stream_marker(magic, magic_length)) {
    return VBIFlacByteSource::open(path, error_message);
  }
  if (has_matroska_stream_marker(magic, magic_length)) {
    return VBIVideoByteSource::open(path, error_message);
  }
  return VBIRawFileByteSource::open(path, error_message);
}

}  // namespace orc
