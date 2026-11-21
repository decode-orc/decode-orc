/*
 * File:        flac_audio_reader.cpp
 * Module:      orc-stage-plugin-audio_import
 * Purpose:     Random-access reads of stereo pairs from a FLAC file, decoded
 *              with libavformat/libavcodec
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "flac_audio_reader.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <cerrno>

#include "audio_import_formats.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

namespace orc {

namespace {

// Decoded blocks kept behind the decoder, in pairs: about 2.7 s at 48 kHz.
// Frame reads arrive slightly out of order from concurrent workers; a read
// that falls a frame or two behind the decoder is then a copy, not a seek.
// The largest FLAC block is 65535 pairs, so this always holds more than one.
constexpr uint64_t kWindowPairs = 1u << 17;

// How far ahead of the decoder a read may sit, in seconds, before seeking
// beats decoding forward. FLAC decodes at hundreds of times real time, so a
// few seconds of forward decoding costs less than a bisecting seek.
constexpr uint64_t kForwardDecodeSeconds = 10;

// How far back of its target a seek is retried, in seconds, when the demuxer
// refuses the position or lands past it. Landing early is always sound (the
// reader decodes forward from wherever it lands); the cost is the decoding
// stepped over, so the ladder starts at the target. A stream without a
// SEEKTABLE is bisected by parsing frame headers at probe positions, and a
// probe that cannot complete a frame abandons the whole seek even though the
// positions either side are seekable; a little further back usually is.
constexpr uint64_t kSeekBackoffSeconds[] = {0, 1, 10, 60};

std::string av_error_text(int code) {
  char text[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, text, sizeof(text));
  return text;
}

bool decoded_layout_of(int format, DecodedSampleLayout& layout) {
  switch (format) {
    case AV_SAMPLE_FMT_S16:
      layout = DecodedSampleLayout::kS16Interleaved;
      return true;
    case AV_SAMPLE_FMT_S16P:
      layout = DecodedSampleLayout::kS16Planar;
      return true;
    case AV_SAMPLE_FMT_S32:
      layout = DecodedSampleLayout::kS32Interleaved;
      return true;
    case AV_SAMPLE_FMT_S32P:
      layout = DecodedSampleLayout::kS32Planar;
      return true;
    default:
      return false;
  }
}

}  // namespace

std::unique_ptr<FlacAudioReader> FlacAudioReader::open(const std::string& path,
                                                       std::string& error) {
  auto reader = std::unique_ptr<FlacAudioReader>(new FlacAudioReader(path));
  if (!reader->open_stream(error)) return nullptr;

  const AVStream* stream = reader->format_ctx_->streams[reader->stream_index_];
  const AVCodecParameters* params = stream->codecpar;
  int bits = params->bits_per_raw_sample;
  if (bits <= 0) bits = params->bits_per_coded_sample;
  error = flac_stream_problem(params->ch_layout.nb_channels, bits,
                              params->sample_rate);
  if (!error.empty()) return nullptr;

  reader->info_.sample_rate_hz = static_cast<uint32_t>(params->sample_rate);
  reader->info_.bits_per_sample = static_cast<uint16_t>(bits);
  // STREAMINFO's total sample count, which the demuxer gives as the stream
  // duration; zero when the encoder left it unrecorded.
  if (stream->duration > 0 && stream->duration != AV_NOPTS_VALUE) {
    reader->info_.pair_count =
        static_cast<uint64_t>(av_rescale_q(stream->duration, stream->time_base,
                                           AVRational{1, params->sample_rate}));
  }
  return reader;
}

FlacAudioReader::~FlacAudioReader() { close_stream(); }

bool FlacAudioReader::open_stream(std::string& error) {
  close_stream();

  // The flac demuxer is named rather than probed: only FLAC is read here,
  // and a file that is not one fails with a clear reason instead of being
  // opened as whatever the probe guesses. libavformat skips a leading ID3v2
  // tag for every demuxer.
  const AVInputFormat* flac_format = av_find_input_format("flac");
  if (flac_format == nullptr) {
    error = "no FLAC demuxer is available in this build";
    return false;
  }
  int ret =
      avformat_open_input(&format_ctx_, path_.c_str(), flac_format, nullptr);
  if (ret < 0) {
    format_ctx_ = nullptr;
    error = "not a FLAC stream (" + av_error_text(ret) + ")";
    return false;
  }
  ret = avformat_find_stream_info(format_ctx_, nullptr);
  if (ret < 0) {
    error = "unreadable FLAC stream information (" + av_error_text(ret) + ")";
    close_stream();
    return false;
  }
  stream_index_ =
      av_find_best_stream(format_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (stream_index_ < 0 ||
      format_ctx_->streams[stream_index_]->codecpar->codec_id !=
          AV_CODEC_ID_FLAC) {
    error = "no FLAC audio stream";
    close_stream();
    return false;
  }

  const AVStream* stream = format_ctx_->streams[stream_index_];
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_FLAC);
  if (codec == nullptr) {
    error = "no FLAC decoder is available in this build";
    close_stream();
    return false;
  }
  codec_ctx_ = avcodec_alloc_context3(codec);
  packet_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  if (codec_ctx_ == nullptr || packet_ == nullptr || frame_ == nullptr) {
    error = "out of memory opening the FLAC decoder";
    close_stream();
    return false;
  }
  if (avcodec_parameters_to_context(codec_ctx_, stream->codecpar) < 0) {
    error = "cannot configure the FLAC decoder";
    close_stream();
    return false;
  }
  // Carries packet timestamps onto decoded frames, which is how a block's
  // position is known after a seek.
  codec_ctx_->pkt_timebase = stream->time_base;
  ret = avcodec_open2(codec_ctx_, codec, nullptr);
  if (ret < 0) {
    error = "cannot open the FLAC decoder (" + av_error_text(ret) + ")";
    close_stream();
    return false;
  }

  window_.clear();
  window_pairs_ = 0;
  next_pair_ = 0;
  draining_ = false;
  at_end_ = false;
  return true;
}

void FlacAudioReader::close_stream() {
  av_frame_free(&frame_);
  av_packet_free(&packet_);
  avcodec_free_context(&codec_ctx_);
  if (format_ctx_ != nullptr) avformat_close_input(&format_ctx_);
  stream_index_ = -1;
}

bool FlacAudioReader::decode_next_block() {
  if (at_end_ || codec_ctx_ == nullptr) return false;

  for (;;) {
    int ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret == 0) break;
    if (ret == AVERROR_EOF) {
      at_end_ = true;
      return false;
    }
    if (ret != AVERROR(EAGAIN)) {
      ORC_LOG_WARN("AudioImportStage: FLAC decode of '{}' stopped: {}", path_,
                   av_error_text(ret));
      at_end_ = true;
      return false;
    }
    if (draining_) {
      at_end_ = true;
      return false;
    }

    ret = av_read_frame(format_ctx_, packet_);
    if (ret < 0) {
      // End of the data (or a read failure, which ends it the same way):
      // flush the frames the decoder still holds.
      avcodec_send_packet(codec_ctx_, nullptr);
      draining_ = true;
      continue;
    }
    if (packet_->stream_index == stream_index_) {
      ret = avcodec_send_packet(codec_ctx_, packet_);
      if (ret < 0 && ret != AVERROR(EAGAIN)) {
        // A damaged frame: skip it. The next block's timestamp places it,
        // and the gap is filled with silence below.
        ORC_LOG_DEBUG(
            "AudioImportStage: skipping a damaged FLAC frame in "
            "'{}': {}",
            path_, av_error_text(ret));
      }
    }
    av_packet_unref(packet_);
  }

  DecodedSampleLayout layout;
  if (!decoded_layout_of(frame_->format, layout)) {
    ORC_LOG_WARN("AudioImportStage: unexpected FLAC sample format {} in '{}'",
                 frame_->format, path_);
    av_frame_unref(frame_);
    at_end_ = true;
    return false;
  }

  Block block;
  const int64_t pts = frame_->best_effort_timestamp;
  last_block_timestamped_ = pts != AV_NOPTS_VALUE && pts >= 0;
  if (last_block_timestamped_) {
    const AVStream* stream = format_ctx_->streams[stream_index_];
    block.first_pair = static_cast<uint64_t>(
        av_rescale_q(pts, stream->time_base,
                     AVRational{1, static_cast<int>(info_.sample_rate_hz)}));
  } else {
    block.first_pair = next_pair_;
  }

  // Keep the window contiguous: a gap left by a skipped frame is silence,
  // and an overlap (which a well-formed stream never has) is dropped.
  size_t skip_pairs = 0;
  if (!window_.empty()) {
    if (block.first_pair > next_pair_) {
      block.samples.assign((block.first_pair - next_pair_) * 2, 0);
      block.first_pair = next_pair_;
    } else if (block.first_pair < next_pair_) {
      skip_pairs = static_cast<size_t>(
          std::min<uint64_t>(next_pair_ - block.first_pair,
                             static_cast<uint64_t>(frame_->nb_samples)));
      block.first_pair = next_pair_;
    }
  }

  const size_t frame_pairs = static_cast<size_t>(frame_->nb_samples);
  if (skip_pairs < frame_pairs) {
    std::vector<int32_t> decoded;
    append_decoded_pairs(layout, frame_->extended_data, frame_pairs, decoded);
    block.samples.insert(
        block.samples.end(),
        decoded.begin() + static_cast<std::ptrdiff_t>(skip_pairs * 2),
        decoded.end());
  }
  av_frame_unref(frame_);

  next_pair_ = block.first_pair + block.pair_count();
  window_pairs_ += block.pair_count();
  window_.push_back(std::move(block));
  while (window_.size() > 1 &&
         window_pairs_ - window_.front().pair_count() >= kWindowPairs) {
    window_pairs_ -= window_.front().pair_count();
    window_.pop_front();
  }
  return true;
}

const FlacAudioReader::Block* FlacAudioReader::find_block(uint64_t pair) const {
  if (window_.empty() || pair < window_.front().first_pair ||
      pair >= next_pair_) {
    return nullptr;
  }
  // The window is contiguous and short; walk back from the newest block,
  // where sequential reads land.
  for (auto it = window_.rbegin(); it != window_.rend(); ++it) {
    if (pair >= it->first_pair) {
      return pair < it->first_pair + it->pair_count() ? &*it : nullptr;
    }
  }
  return nullptr;
}

bool FlacAudioReader::reach(uint64_t pair) {
  if (find_block(pair) != nullptr) return true;

  const uint64_t forward_limit =
      static_cast<uint64_t>(info_.sample_rate_hz) * kForwardDecodeSeconds;
  const bool decode_forward = codec_ctx_ != nullptr && pair >= next_pair_ &&
                              pair - next_pair_ <= forward_limit;
  if (!decode_forward) {
    if (codec_ctx_ != nullptr && at_end_ && pair >= next_pair_ &&
        !window_.empty()) {
      return false;  // past the end of the stream
    }
    if (!seek_at_or_before(pair)) return false;
  }
  while (find_block(pair) == nullptr) {
    if (pair < next_pair_ && !window_.empty() &&
        pair >= window_.front().first_pair) {
      return false;  // cannot happen with a contiguous window
    }
    if (!decode_next_block()) return false;
  }
  return true;
}

bool FlacAudioReader::seek_at_or_before(uint64_t pair) {
  if (format_ctx_ == nullptr) return rewind_to_head();
  const AVStream* stream = format_ctx_->streams[stream_index_];
  const AVRational pair_base{1, static_cast<int>(info_.sample_rate_hz)};

  for (const uint64_t backoff_seconds : kSeekBackoffSeconds) {
    const uint64_t backoff =
        backoff_seconds * static_cast<uint64_t>(info_.sample_rate_hz);
    if (pair <= backoff) break;  // as far back as the head: rewind instead
    const uint64_t target = pair - backoff;

    const int64_t timestamp = av_rescale_q(static_cast<int64_t>(target),
                                           pair_base, stream->time_base);
    if (av_seek_frame(format_ctx_, stream_index_, timestamp,
                      AVSEEK_FLAG_BACKWARD) < 0) {
      continue;
    }
    avcodec_flush_buffers(codec_ctx_);
    window_.clear();
    window_pairs_ = 0;
    draining_ = false;
    at_end_ = false;
    // The first block after a seek is placed by its timestamp alone.
    next_pair_ = 0;
    if (!decode_next_block() || !last_block_timestamped_) continue;
    if (window_.front().first_pair <= pair) return true;
  }
  return rewind_to_head();
}

bool FlacAudioReader::rewind_to_head() {
  std::string error;
  if (!open_stream(error)) {
    ORC_LOG_WARN("AudioImportStage: cannot reopen '{}': {}", path_, error);
    return false;
  }
  return true;
}

std::vector<int32_t> FlacAudioReader::read_pairs(uint64_t first_pair,
                                                 size_t pair_count) {
  std::lock_guard<std::mutex> lock(mutex_);

  uint64_t end = first_pair + pair_count;
  if (info_.pair_count > 0) end = std::min(end, info_.pair_count);

  std::vector<int32_t> out;
  if (first_pair >= end) return out;
  out.reserve(static_cast<size_t>(end - first_pair) * 2);

  uint64_t want = first_pair;
  while (want < end) {
    if (!reach(want)) break;
    const Block* block = find_block(want);
    const uint64_t offset = want - block->first_pair;
    const uint64_t take = std::min(block->pair_count() - offset, end - want);
    const auto begin =
        block->samples.begin() + static_cast<std::ptrdiff_t>(offset * 2);
    out.insert(out.end(), begin, begin + static_cast<std::ptrdiff_t>(take * 2));
    want += take;
  }
  return out;
}

}  // namespace orc
