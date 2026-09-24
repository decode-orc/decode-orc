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

#include <algorithm>
#include <fstream>

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

// frame_count == 0 (the parameter's unset/default value) means "unbounded":
// the source reads until a real end-of-stream rather than requiring an
// exact count up front. Internally this is represented as this sentinel
// instead of a genuinely infinite value so it stays a normal, finite
// FrameIDRange for frame_range()/has_frame() — see the class comment on
// CVBSStreamFrameRepresentation. ~4.29 billion frames is years of video at
// any real frame rate, so it is never a practical limit.
constexpr uint32_t kUnboundedFrameCount = UINT32_MAX;

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
// (frame_samples words) at a time. All the threading/ring-buffer/throttle
// machinery lives in orc::pipe_io::ThrottledRingReader (see that header);
// this constructor supplies only the "how do I produce frame N" piece
// specific to the CVBS wire format.
//
// "raw" wire format: a flat, unframed sequence of 16-bit words — frame N at
// word offset N * frame_samples, exactly the on-disk .cvbs layout (see
// frame_samples_from_system() in cvbs_signal_constants.h). No headers, no
// per-frame markers: this is intentionally the simplest possible format,
// so a real .cvbs file piped in (`cat file.cvbs | orc-cli ...`) reads back
// identically to opening the file directly.
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
    : reader_(
          [&input, frame_samples, encoding, blanking_10bit, frame_count](
              size_t frame_index, std::vector<int16_t>& out,
              std::string& error) -> pipe_io::ProduceStatus {
            const bool unbounded = (frame_count == kUnboundedFrameCount);
            std::vector<uint16_t> raw(frame_samples);
            input.read(reinterpret_cast<char*>(raw.data()),
                       static_cast<std::streamsize>(frame_samples * 2));
            const std::streamsize got = input.gcount();
            if (got == 0) {
              if (unbounded) return pipe_io::ProduceStatus::kEof;
              error = "unexpected end of input at frame " +
                      std::to_string(frame_index) + " of " +
                      std::to_string(frame_count) +
                      " declared (frame_count parameter does not match the "
                      "actual input length)";
              return pipe_io::ProduceStatus::kError;
            }
            if (got != static_cast<std::streamsize>(frame_samples * 2)) {
              error = "truncated frame " + std::to_string(frame_index) + " (" +
                      std::to_string(got) + " of " +
                      std::to_string(frame_samples * 2) +
                      " bytes) — input ended mid-frame";
              return pipe_io::ProduceStatus::kError;
            }
            out.resize(frame_samples);
            normalize_samples(raw.data(), frame_samples, encoding,
                              blanking_10bit, out.data());
            return pipe_io::ProduceStatus::kOk;
          },
          frame_count, buffer_frames, "CVBSStreamReader") {}

namespace {

// ---------------------------------------------------------------------------
// CVBSStreamFrameRepresentation
// ---------------------------------------------------------------------------
// VideoFrameRepresentation backed by a CVBSStreamReader, reading from the
// input stream it owns: a real path's file, or this instance's reader of
// stdin or named pipe (see pipe_io::open_pipe_reader()).
class CVBSStreamFrameRepresentation final : public VideoFrameRepresentation,
                                            public Artifact {
 public:
  CVBSStreamFrameRepresentation(VideoSystem system, uint32_t frame_count,
                                int32_t frame_samples, int32_t frame_height,
                                int32_t spl_nominal,
                                std::unique_ptr<std::istream> owned_input,
                                SampleEncoding encoding, int32_t blanking_10bit,
                                uint32_t buffer_frames,
                                SourceParameters video_params,
                                ArtifactID artifact_id, Provenance provenance)
      : Artifact(std::move(artifact_id), std::move(provenance)),
        system_(system),
        frame_count_(frame_count),
        frame_samples_(static_cast<size_t>(frame_samples)),
        frame_height_(static_cast<size_t>(frame_height)),
        spl_nominal_(static_cast<size_t>(spl_nominal)),
        video_params_(std::move(video_params)),
        buffer_frames_(buffer_frames),
        owned_input_(std::move(owned_input)),
        reader_(*owned_input_, static_cast<size_t>(frame_samples), frame_count,
                buffer_frames, encoding, blanking_10bit) {}

  // A shared pipe reader is cut off here so the reader thread does not
  // wait on data this instance no longer wants; stopping the reader first
  // keeps that cut from being logged as a truncated input.
  ~CVBSStreamFrameRepresentation() override {
    reader_.request_stop();
    if (auto* shared =
            dynamic_cast<pipe_io::SharedInputStream*>(owned_input_.get())) {
      shared->detach();
    }
  }

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

  bool is_exhausted() const override {
    return reader_.failed() || reader_.is_eof();
  }
  std::string stream_error() const override { return reader_.last_error(); }
  bool has_unbounded_frame_range() const override {
    return frame_count_ == kUnboundedFrameCount;
  }
  size_t max_concurrent_frame_requests() const override {
    return buffer_frames_;
  }

 private:
  VideoSystem system_;
  uint32_t frame_count_;
  size_t frame_samples_;
  size_t frame_height_;
  size_t spl_nominal_;
  SourceParameters video_params_;
  size_t buffer_frames_;

  // Declaration order matters: owned_input_ must outlive reader_ (which
  // holds a reference to *owned_input_) and must be constructed before it.
  std::unique_ptr<std::istream> owned_input_;
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
        "Total number of frames the input will provide. With no sidecar "
        "and no seekable input, this cannot be measured from the file "
        "size the way cvbs_source does. Leave at 0 (the default) for an "
        "unbounded/live source: the stage then reads until the input "
        "reaches a clean end-of-stream instead of requiring an exact "
        "count up front.";
    pd.type = ParameterType::UINT32;
    pd.constraints.required = false;
    pd.constraints.default_value = static_cast<uint32_t>(0);
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

  {
    // Host-owned: set by the DAG builder to the number of sinks sharing this
    // stream, and hidden from the GUI and CLI parameter surfaces.
    ParameterDescriptor pd;
    pd.name = kStreamReaderCountParameter;
    pd.display_name = "Stream Readers";
    pd.description =
        "Host-supplied number of sinks reading this stream; not "
        "user-editable.";
    pd.type = ParameterType::UINT32;
    pd.constraints.required = false;
    pd.constraints.default_value = static_cast<uint32_t>(1);
    pd.constraints.min_value = static_cast<uint32_t>(1);
    desc.push_back(pd);
  }

  return desc;
}

std::map<std::string, ParameterValue>
FixedFormatCVBSStreamSourceStage::get_parameters() const {
  return {{"input_path", input_path_},
          {"sample_encoding", sample_encoding_},
          {"frame_count", frame_count_},
          {"buffer_frames", buffer_frames_},
          {kStreamReaderCountParameter, stream_reader_count_}};
}

bool FixedFormatCVBSStreamSourceStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "input_path") {
      input_path_ = std::get<std::string>(value);
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
    } else if (key == kStreamReaderCountParameter) {
      stream_reader_count_ = std::max<uint32_t>(std::get<uint32_t>(value), 1);
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

  // The stream can only be read once, and the executor may call execute()
  // again on this instance on a cache miss: as long as the configuration is
  // unchanged, return the representation already built rather than trying
  // to open input_path_ a second time.
  const std::string config_key = input_path_ + "|" + sample_encoding_ + "|" +
                                 std::to_string(frame_count_) + "|" +
                                 std::to_string(buffer_frames_) + "|" +
                                 std::to_string(stream_reader_count_);
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
  std::unique_ptr<std::istream> input;
  if (pipe_io::is_pipe_path(input_path_)) {
    // stdin or a named pipe: one reader per sink sharing the stream, each
    // getting the whole of it while it is read once (see
    // kStreamReaderCountParameter).
    input = pipe_io::open_pipe_reader(input_path_, stream_reader_count_);
  } else {
    auto file = std::make_unique<std::ifstream>(input_path_, std::ios::binary);
    if (file->is_open()) input = std::move(file);
  }
  if (!input) {
    ORC_LOG_ERROR("{}: failed to open '{}'", stage_name_, input_path_);
    return {};
  }

  // frame_count_ == 0 means "unbounded" — see the frame_count parameter's
  // description and kUnboundedFrameCount's comment above.
  const uint32_t effective_frame_count =
      (frame_count_ == 0) ? kUnboundedFrameCount : frame_count_;

  const int32_t frame_samples = frame_samples_from_system(system_);
  const int32_t frame_height = frame_lines_from_system(system_);
  const SourceParameters src_params = build_source_parameters(
      system_, static_cast<int32_t>(effective_frame_count));
  const SampleEncoding encoding = sample_encoding_from_name(sample_encoding_);

  if (frame_count_ == 0) {
    ORC_LOG_INFO(
        "{}: streaming until end-of-stream from '{}' ({}, {}, "
        "buffer_frames={})",
        stage_name_, input_path_, video_system_to_string(system_),
        sample_encoding_, buffer_frames_);
  } else {
    ORC_LOG_INFO("{}: streaming {} frames from '{}' ({}, {}, buffer_frames={})",
                 stage_name_, frame_count_, input_path_,
                 video_system_to_string(system_), sample_encoding_,
                 buffer_frames_);
  }

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
      system_, effective_frame_count, frame_samples, frame_height,
      src_params.frame_width_nominal, std::move(input), encoding,
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
