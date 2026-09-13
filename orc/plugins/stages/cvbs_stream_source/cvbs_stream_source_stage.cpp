/*
 * File:        cvbs_stream_source_stage.cpp
 * Module:      orc-core
 * Purpose:     Sequential (pipe-compatible) CVBS source loading stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "cvbs_stream_source_stage.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/support/logging.h>
#include <orc/support/pipe_io.h>

#include <condition_variable>
#include <fstream>
#include <thread>

namespace orc {

namespace {

// ---------------------------------------------------------------------------
// Sample encoding normalisation
// ---------------------------------------------------------------------------
// Deliberately duplicated from cvbs_source_stage.cpp rather than shared: both
// implement the same fixed CVBS file format spec §3.1 conversion, but this
// stage's whole point is staying self-contained so it never has to touch the
// existing file-based source's code. See that file if this ever needs to
// change — the two must stay in agreement.
//
// SampleEncoding itself is declared in the header (CVBSStreamReader's
// constructor takes it) — everything else about it stays local to this file.

constexpr const char* kSupportedEncodings[] = {
    "CVBS_U10_4FSC", "CVBS_U16_4FSC", "CVBS_TPG21_4FSC", "CVBS_S16_4FSC"};

bool is_supported_encoding(const std::string& encoding) {
  for (const char* e : kSupportedEncodings) {
    if (encoding == e) return true;
  }
  return false;
}

SampleEncoding sample_encoding_from_name(const std::string& encoding) {
  if (encoding == "CVBS_U10_4FSC") return SampleEncoding::kU10;
  if (encoding == "CVBS_TPG21_4FSC") return SampleEncoding::kTPG21;
  if (encoding == "CVBS_S16_4FSC") return SampleEncoding::kS16;
  return SampleEncoding::kU16;
}

int16_t normalize_to_cvbs_u10(uint16_t raw, SampleEncoding encoding,
                              int32_t blanking_10bit) {
  switch (encoding) {
    case SampleEncoding::kU10:
      return static_cast<int16_t>(raw);
    case SampleEncoding::kTPG21:
      return static_cast<int16_t>(
          static_cast<int32_t>(static_cast<int16_t>(raw)) / 64 + 508);
    case SampleEncoding::kS16:
      return static_cast<int16_t>(
          static_cast<int32_t>(static_cast<int16_t>(raw)) / 32 +
          blanking_10bit);
    case SampleEncoding::kU16:
      break;
  }
  return static_cast<int16_t>(static_cast<int32_t>(raw) / 64);
}

void normalize_samples(const uint16_t* src, size_t count,
                       SampleEncoding encoding, int32_t blanking_10bit,
                       int16_t* dst) {
  for (size_t i = 0; i < count; ++i) {
    dst[i] = normalize_to_cvbs_u10(src[i], encoding, blanking_10bit);
  }
}

// ---------------------------------------------------------------------------
// SourceParameters population from spec constants (no .meta to read)
// ---------------------------------------------------------------------------

SourceParameters build_source_parameters(VideoSystem system,
                                         int32_t frame_count) {
  SourceParameters sp;
  sp.system = system;
  sp.number_of_sequential_frames = frame_count;

  switch (system) {
    case VideoSystem::PAL:
      sp.frame_width_nominal = kPalSamplesPerLineNominal;
      sp.frame_height = kPalFrameLines;
      sp.sync_tip_level = kPalSyncTip;
      sp.blanking_level = kPalBlanking;
      sp.black_level = kPalBlack;
      sp.white_level = kPalWhite;
      sp.peak_level = kPalPeak;
      sp.active_video_start = kPalActiveVideoStart;
      sp.active_video_end = kPalActiveVideoEnd;
      sp.first_active_frame_line = kPalFirstActiveFrameLine;
      sp.last_active_frame_line = kPalLastActiveFrameLine;
      break;
    case VideoSystem::NTSC:
      sp.frame_width_nominal = kNtscSamplesPerLine;
      sp.frame_height = kNtscFrameLines;
      sp.sync_tip_level = kNtscSyncTip;
      sp.blanking_level = kNtscBlanking;
      sp.black_level = kNtscBlack;
      sp.white_level = kNtscWhite;
      sp.peak_level = kNtscPeak;
      sp.active_video_start = kNtscActiveVideoStart;
      sp.active_video_end = kNtscActiveVideoEnd;
      sp.first_active_frame_line = kNtscFirstActiveFrameLine;
      sp.last_active_frame_line = kNtscLastActiveFrameLine;
      break;
    case VideoSystem::PAL_M:
      sp.frame_width_nominal = kPalMSamplesPerLine;
      sp.frame_height = kPalMFrameLines;
      sp.sync_tip_level = kNtscSyncTip;
      sp.blanking_level = kNtscBlanking;
      sp.black_level = kNtscBlack;
      sp.white_level = kNtscWhite;
      sp.peak_level = kNtscPeak;
      sp.active_video_start = kNtscActiveVideoStart;
      sp.active_video_end = kNtscActiveVideoEnd;
      sp.first_active_frame_line = kNtscFirstActiveFrameLine;
      sp.last_active_frame_line = kNtscLastActiveFrameLine;
      break;
    default:
      break;
  }
  return sp;
}

}  // namespace

// ---------------------------------------------------------------------------
// CVBSStreamReader
// ---------------------------------------------------------------------------
// Reads CVBS frames strictly forward from `input`, one full frame
// (frame_samples words) at a time, into a fixed-size ring buffer. A
// background thread owns the actual reads so no caller ever blocks another
// caller on I/O; get_frame() callers only ever wait on a condition variable.
//
// "raw" wire format: a flat, unframed sequence of 16-bit words — frame N at
// word offset N * frame_samples, exactly the on-disk .cvbs layout (see
// frame_samples_from_system() in cvbs_signal_constants.h). No headers, no
// per-frame markers: this is intentionally the simplest possible format,
// so a real .cvbs file piped in (`cat file.cvbs | orc-cli ...`) reads back
// identically to opening the file directly.
//
// Thread safety: get_frame() may be called concurrently from multiple
// threads (VideoFrameRepresentation's own contract, exercised in practice by
// VideoSinkStage's parallel export workers) as long as the SET of ids in
// flight at any moment spans no more than `buffer_frames` — the whole point
// of the bounded ring buffer is to tolerate that degree of reordering
// without requiring strict one-at-a-time access, while still detecting and
// failing loudly the moment something asks for a frame that has already
// scrolled out of the window, rather than silently returning stale data.
//
// Known limitation: destroying a CVBSStreamReader whose background thread is
// blocked inside a stdio read() with no more data coming (a stalled or dead
// producer) blocks the destructor too — there is no portable way to cancel a
// blocking read on an arbitrary std::istream from another thread. This is
// the same limitation any blocking-stdio pipe consumer has (ffmpeg included);
// killing the process is the escape hatch, same as it would be for one.
CVBSStreamReader::CVBSStreamReader(std::istream& input, size_t frame_samples,
                                   size_t frame_count, size_t buffer_frames,
                                   SampleEncoding encoding,
                                   int32_t blanking_10bit)
    : input_(input),
      frame_samples_(frame_samples),
      frame_count_(frame_count),
      buffer_frames_(buffer_frames == 0 ? 1 : buffer_frames),
      encoding_(encoding),
      blanking_10bit_(blanking_10bit),
      ring_(buffer_frames_) {
  thread_ = std::thread(&CVBSStreamReader::reader_loop, this);
}

CVBSStreamReader::~CVBSStreamReader() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

const int16_t* CVBSStreamReader::get_frame(FrameID id) const {
  if (id >= static_cast<FrameID>(frame_count_)) return nullptr;
  const size_t requested = static_cast<size_t>(id);

  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [&] { return failed_ || produced_ > requested; });
  if (failed_) return nullptr;
  if (produced_ - requested > buffer_frames_) {
    fail_locked("frame " + std::to_string(requested) +
                " requested after it scrolled out of the " +
                std::to_string(buffer_frames_) +
                "-frame buffer (reader is now at frame " +
                std::to_string(produced_) +
                "); increase buffer_frames if this access pattern is "
                "legitimate");
    return nullptr;
  }
  return ring_[requested % buffer_frames_].data();
}

bool CVBSStreamReader::failed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}

std::string CVBSStreamReader::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

void CVBSStreamReader::fail_locked(const std::string& message) const {
  // Called with mutex_ already held, from either get_frame() (const) or
  // reader_loop() (non-const) — both hand it the same lock, so this stays
  // logically const from the caller's point of view even though it mutates
  // the (mutable) failure state.
  if (!failed_) {
    failed_ = true;
    error_ = message;
    ORC_LOG_ERROR("CVBSStreamReader: {}", message);
  }
  cv_.notify_all();
}

void CVBSStreamReader::reader_loop() {
  std::vector<uint16_t> raw(frame_samples_);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_ || failed_ || produced_ >= frame_count_) return;
    }

    // Read outside the lock: a slow/blocked read must not stall other
    // threads' get_frame() lookups against already-produced frames. No
    // "wait for room" gate here — the reader always runs flat-out, bounded
    // in practice by the OS pipe's own backpressure on the producer's
    // write() calls (which is smaller than one frame) rather than by
    // anything measured here, and in memory only by the fixed ring size.
    // Racing ahead of a slow consumer just means overwriting ring slots it
    // never asked for — the eviction check below is what actually matters.
    input_.read(reinterpret_cast<char*>(raw.data()),
                static_cast<std::streamsize>(frame_samples_ * 2));
    const std::streamsize got = input_.gcount();

    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) return;  // destructor requested shutdown while this blocked
    if (got != static_cast<std::streamsize>(frame_samples_ * 2)) {
      fail_locked("unexpected end of input at frame " +
                  std::to_string(produced_) + " of " +
                  std::to_string(frame_count_) +
                  " declared (frame_count parameter does not match the "
                  "actual input length)");
      return;
    }

    auto& slot = ring_[produced_ % buffer_frames_];
    slot.resize(frame_samples_);
    normalize_samples(raw.data(), frame_samples_, encoding_, blanking_10bit_,
                      slot.data());
    ++produced_;
    cv_.notify_all();
  }
}

namespace {

// ---------------------------------------------------------------------------
// CVBSStreamFrameRepresentation
// ---------------------------------------------------------------------------
// VideoFrameRepresentation backed by a CVBSStreamReader. Owns the input file
// stream when reading from a real path (never when reading from stdin,
// which is process-global and outlives this object regardless).
class CVBSStreamFrameRepresentation final : public VideoFrameRepresentation,
                                            public Artifact {
 public:
  CVBSStreamFrameRepresentation(VideoSystem system, uint32_t frame_count,
                                int32_t frame_samples, int32_t frame_height,
                                int32_t spl_nominal,
                                std::unique_ptr<std::ifstream> owned_file,
                                std::istream& input, SampleEncoding encoding,
                                int32_t blanking_10bit, uint32_t buffer_frames,
                                SourceParameters video_params,
                                ArtifactID artifact_id, Provenance provenance)
      : Artifact(std::move(artifact_id), std::move(provenance)),
        system_(system),
        frame_count_(frame_count),
        frame_samples_(static_cast<size_t>(frame_samples)),
        frame_height_(static_cast<size_t>(frame_height)),
        spl_nominal_(static_cast<size_t>(spl_nominal)),
        video_params_(std::move(video_params)),
        owned_file_(std::move(owned_file)),
        reader_(input, static_cast<size_t>(frame_samples), frame_count,
                buffer_frames, encoding, blanking_10bit) {}

  std::string type_name() const override {
    return "CVBSStreamFrameRepresentation";
  }

  FrameIDRange frame_range() const override {
    if (frame_count_ == 0) return FrameIDRange{1, 0};
    return FrameIDRange{0, static_cast<FrameID>(frame_count_ - 1)};
  }

  size_t frame_count() const override { return frame_count_; }

  bool has_frame(FrameID id) const override {
    return id < static_cast<FrameID>(frame_count_);
  }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = system_;
    desc.height = frame_height_;
    desc.samples_total = frame_samples_;
    desc.samples_per_line_nominal = spl_nominal_;
    return desc;
  }

  const sample_type* get_frame(FrameID id) const override {
    if (!has_frame(id)) return nullptr;
    return reader_.get_frame(id);
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    const sample_type* ptr = get_frame(id);
    if (!ptr) return {};
    return std::vector<sample_type>(ptr, ptr + frame_samples_);
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    return video_params_;
  }

  bool failed() const { return reader_.failed(); }
  std::string last_error() const { return reader_.last_error(); }

 private:
  VideoSystem system_;
  uint32_t frame_count_;
  size_t frame_samples_;
  size_t frame_height_;
  size_t spl_nominal_;
  SourceParameters video_params_;

  // Declaration order matters: owned_file_ must outlive reader_ (which
  // holds a reference to *owned_file_ when set) and must be constructed
  // before it.
  std::unique_ptr<std::ifstream> owned_file_;
  CVBSStreamReader reader_;
};

}  // namespace

// ---------------------------------------------------------------------------
// FixedFormatCVBSStreamSourceStage
// ---------------------------------------------------------------------------

FixedFormatCVBSStreamSourceStage::FixedFormatCVBSStreamSourceStage(
    const char* stage_name, const char* fixed_display_name,
    const char* description, VideoFormatCompatibility compatible_formats,
    VideoSystem system)
    : system_(system),
      stage_name_(stage_name),
      display_name_(fixed_display_name),
      description_(description),
      compatible_formats_(compatible_formats) {}

std::vector<ParameterDescriptor>
FixedFormatCVBSStreamSourceStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType /*source_type*/) const {
  std::vector<ParameterDescriptor> desc;

  {
    ParameterDescriptor pd;
    pd.name = "input_path";
    pd.display_name = "Input Path";
    pd.description =
        "\"-\" reads from standard input (CLI only); a real named pipe "
        "path also works. There is no file-based fallback — use "
        "cvbs_source for a real .cvbs file on disk.";
    pd.type = ParameterType::FILE_PATH;
    pd.constraints.required = true;
    desc.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = "input_mode";
    pd.display_name = "Input Mode";
    pd.description =
        "Wire format read from input_path. \"raw\" is a flat, unframed "
        "sequence of samples identical to the on-disk .cvbs layout — no "
        "audio possible, since there is no container to carry a second "
        "stream. Currently the only implemented mode.";
    pd.type = ParameterType::STRING;
    pd.constraints.required = false;
    pd.constraints.default_value = std::string("raw");
    pd.constraints.allowed_strings = {"raw"};
    desc.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = "sample_encoding";
    pd.display_name = "Sample Encoding";
    pd.description =
        "Sample encoding of the incoming data. Required — there is no "
        ".meta sidecar to read it from.";
    pd.type = ParameterType::STRING;
    pd.constraints.required = true;
    pd.constraints.allowed_strings.assign(std::begin(kSupportedEncodings),
                                          std::end(kSupportedEncodings));
    desc.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = "frame_count";
    pd.display_name = "Frame Count";
    pd.description =
        "Total number of frames the input will provide. Required — with "
        "no sidecar and no seekable input, this cannot be measured from "
        "the file size the way cvbs_source does.";
    pd.type = ParameterType::UINT32;
    pd.constraints.required = true;
    pd.constraints.min_value = static_cast<uint32_t>(1);
    desc.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = "buffer_frames";
    pd.display_name = "Buffer Frames";
    pd.description =
        "How many frames of read-ahead to buffer. Every stage between "
        "this source and the piped endpoint has to be answerable from "
        "within this window at once — raise it if a downstream stage "
        "needs more temporal lookahead/lookbehind, or if export "
        "parallelism spreads requests wider than the default.";
    pd.type = ParameterType::UINT32;
    pd.constraints.required = false;
    pd.constraints.default_value = static_cast<uint32_t>(32);
    pd.constraints.min_value = static_cast<uint32_t>(1);
    desc.push_back(pd);
  }

  return desc;
}

std::map<std::string, ParameterValue>
FixedFormatCVBSStreamSourceStage::get_parameters() const {
  return {
      {"input_path", input_path_},
      {"input_mode", input_mode_.empty() ? std::string("raw") : input_mode_},
      {"sample_encoding", sample_encoding_},
      {"frame_count", frame_count_},
      {"buffer_frames", buffer_frames_}};
}

bool FixedFormatCVBSStreamSourceStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "input_path") {
      input_path_ = std::get<std::string>(value);
    } else if (key == "input_mode") {
      const auto& mode = std::get<std::string>(value);
      if (mode != "raw") {
        ORC_LOG_ERROR(
            "{}: unsupported input_mode '{}' — only 'raw' is implemented",
            stage_name_, mode);
        return false;
      }
      input_mode_ = mode;
    } else if (key == "sample_encoding") {
      const auto& encoding = std::get<std::string>(value);
      if (!is_supported_encoding(encoding)) {
        ORC_LOG_ERROR("{}: unsupported sample_encoding '{}'", stage_name_,
                      encoding);
        return false;
      }
      sample_encoding_ = encoding;
    } else if (key == "frame_count") {
      frame_count_ = std::get<uint32_t>(value);
    } else if (key == "buffer_frames") {
      buffer_frames_ = std::get<uint32_t>(value);
    }
  }
  return true;
}

std::vector<ArtifactPtr> FixedFormatCVBSStreamSourceStage::execute(
    const std::vector<ArtifactPtr>& /*inputs*/,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& /*observation_context*/) {
  if (!parameters.empty()) set_parameters(parameters);

  std::lock_guard<std::mutex> lock(execute_mutex_);

  // The stream can only be read once. A batch that triggers more than one
  // sink downstream of this source calls execute() once per sink against
  // the SAME instance (see triggerAllSinks()'s shared DAGExecutor) — as
  // long as nothing else about the configuration changed, return the
  // representation already built rather than trying to open input_path_ a
  // second time.
  const std::string config_key =
      input_path_ + "|" + input_mode_ + "|" + sample_encoding_ + "|" +
      std::to_string(frame_count_) + "|" + std::to_string(buffer_frames_);
  if (cached_representation_ && config_key == cached_config_key_) {
    return {cached_representation_};
  }

  if (input_path_.empty()) {
    ORC_LOG_ERROR("{}: input_path is required", stage_name_);
    return {};
  }
  if (sample_encoding_.empty()) {
    ORC_LOG_ERROR("{}: sample_encoding is required", stage_name_);
    return {};
  }
  if (frame_count_ == 0) {
    ORC_LOG_ERROR("{}: frame_count must be at least 1", stage_name_);
    return {};
  }

  std::unique_ptr<std::ifstream> owned_file;
  std::istream* input = nullptr;
  if (input_path_ == pipe_io::kStdioPathToken) {
    input = &pipe_io::stdin_binary_stream();
  } else {
    owned_file = std::make_unique<std::ifstream>(input_path_, std::ios::binary);
    if (!owned_file->is_open()) {
      ORC_LOG_ERROR("{}: failed to open '{}'", stage_name_, input_path_);
      return {};
    }
    input = owned_file.get();
  }

  const int32_t frame_samples = frame_samples_from_system(system_);
  const int32_t frame_height = frame_lines_from_system(system_);
  const SourceParameters src_params =
      build_source_parameters(system_, static_cast<int32_t>(frame_count_));
  const SampleEncoding encoding = sample_encoding_from_name(sample_encoding_);

  ORC_LOG_INFO("{}: streaming {} frames from '{}' ({}, {}, buffer_frames={})",
               stage_name_, frame_count_, input_path_,
               video_system_to_string(system_), sample_encoding_,
               buffer_frames_);

  Provenance prov;
  prov.stage_name = stage_name_;
  prov.stage_version = version();
  prov.parameters = {
      {"input_path", input_path_},
      {"video_system", video_system_to_string(system_)},
      {"sample_encoding", sample_encoding_},
      {"frame_count", std::to_string(frame_count_)},
  };

  auto representation = std::make_shared<CVBSStreamFrameRepresentation>(
      system_, frame_count_, frame_samples, frame_height,
      src_params.frame_width_nominal, std::move(owned_file), *input, encoding,
      src_params.blanking_level, buffer_frames_, src_params,
      ArtifactID(std::string(stage_name_) + ":" + config_key), std::move(prov));

  cached_representation_ = representation;
  cached_config_key_ = config_key;
  return {representation};
}

PALCVBSStreamSourceStage::PALCVBSStreamSourceStage()
    : FixedFormatCVBSStreamSourceStage(
          "pal_cvbs_stream_source", "PAL CVBS Stream Source",
          "Sequential (pipe-compatible) PAL CVBS source",
          VideoFormatCompatibility::PAL_ONLY, VideoSystem::PAL) {}

NTSCCVBSStreamSourceStage::NTSCCVBSStreamSourceStage()
    : FixedFormatCVBSStreamSourceStage(
          "ntsc_cvbs_stream_source", "NTSC CVBS Stream Source",
          "Sequential (pipe-compatible) NTSC CVBS source",
          VideoFormatCompatibility::NTSC_ONLY, VideoSystem::NTSC) {}

PALMCVBSStreamSourceStage::PALMCVBSStreamSourceStage()
    : FixedFormatCVBSStreamSourceStage(
          "palm_cvbs_stream_source", "PAL-M CVBS Stream Source",
          "Sequential (pipe-compatible) PAL-M CVBS source",
          VideoFormatCompatibility::PAL_M_ONLY, VideoSystem::PAL_M) {}

}  // namespace orc
