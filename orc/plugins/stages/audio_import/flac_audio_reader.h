/*
 * File:        flac_audio_reader.h
 * Module:      orc-stage-plugin-audio_import
 * Purpose:     Random-access reads of stereo pairs from a FLAC file, decoded
 *              with libavformat/libavcodec
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;

namespace orc {

// Serves the samples of a stereo FLAC file by pair index, in the
// 24-bit-in-int32 carrier, without decoding the whole file.
//
// FLAC decodes forward from a frame boundary, so the reader keeps one decoder
// moving through the file and a short window of the blocks it has decoded.
// A read inside the window is a copy, a read a little ahead decodes forward,
// and anything else seeks: the demuxer uses the file's SEEKTABLE where it has
// one (libFLAC's encoder writes one) and bisects the stream where it does
// not (FFmpeg's encoder). A seek the demuxer refuses, or one that lands past
// the target, is retried further back, and finally by decoding from the head
// of the file, so every pair is reachable even when seeking is not.
//
// Sequential reads, which is how the ingest conversion and playback work,
// never seek.
//
// Thread safety: read_pairs() may be called concurrently; the calls are
// serialised on one decoder.
class FlacAudioReader {
 public:
  struct StreamInfo {
    uint32_t sample_rate_hz = 0;
    uint16_t bits_per_sample = 0;  // as coded, 1 to 24
    // Stereo pairs in the stream, from STREAMINFO; 0 when the encoder could
    // not record it (a stream written to a pipe), in which case reads run to
    // the end of the data.
    uint64_t pair_count = 0;
  };

  // Opens |path| as a FLAC stream and checks it is importable (see
  // flac_stream_problem()). Returns nullptr and sets |error| otherwise.
  static std::unique_ptr<FlacAudioReader> open(const std::string& path,
                                               std::string& error);

  ~FlacAudioReader();
  FlacAudioReader(const FlacAudioReader&) = delete;
  FlacAudioReader& operator=(const FlacAudioReader&) = delete;

  const StreamInfo& info() const { return info_; }

  // Up to |pair_count| pairs starting at |first_pair|, interleaved. Fewer
  // (whole pairs only) at the end of the stream, or when the file cannot be
  // decoded past some point (logged).
  std::vector<int32_t> read_pairs(uint64_t first_pair, size_t pair_count);

 private:
  struct Block {
    uint64_t first_pair = 0;
    std::vector<int32_t> samples;  // interleaved carrier values
    uint64_t pair_count() const { return samples.size() / 2; }
  };

  explicit FlacAudioReader(std::string path) : path_(std::move(path)) {}

  // (Re)open the demuxer and decoder at the head of the stream.
  bool open_stream(std::string& error);
  void close_stream();

  // Decode the next block onto the window. False at the end of the stream
  // or on a failure that stops decoding.
  bool decode_next_block();

  // The window block holding |pair|, or nullptr.
  const Block* find_block(uint64_t pair) const;

  // Position the decoder so the window holds |pair|. False when it cannot
  // be reached (past the end, or the file stops decoding first).
  bool reach(uint64_t pair);
  bool seek_at_or_before(uint64_t pair);
  bool rewind_to_head();

  const std::string path_;
  StreamInfo info_;

  std::mutex mutex_;
  AVFormatContext* format_ctx_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  int stream_index_ = -1;

  std::deque<Block> window_;
  uint64_t window_pairs_ = 0;  // pairs held in window_
  uint64_t next_pair_ = 0;     // first pair of the next block to decode
  bool draining_ = false;      // the decoder has been sent end-of-stream
  bool at_end_ = false;        // nothing more to decode from here
  // The newest block was placed by its timestamp, which is the only way to
  // place the first block after a seek.
  bool last_block_timestamped_ = false;
};

}  // namespace orc
