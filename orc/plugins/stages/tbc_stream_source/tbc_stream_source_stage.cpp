/*
 * File:        tbc_stream_source_stage.cpp
 * Module:      orc-core
 * Purpose:     Sequential (pipe-compatible) TBC source loading stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tbc_stream_source_stage.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/support/logging.h>
#include <orc/support/pipe_io.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "ntsc_tbc_converter.h"
#include "pal_m_tbc_converter.h"
#include "pal_tbc_converter.h"

namespace orc {

namespace {

// ---------------------------------------------------------------------------
// TBC-domain level derivation
// ---------------------------------------------------------------------------
// Deliberately duplicated from
// orc/plugins/stages/tbc_source/tbc_level_derivation.h rather than shared —
// see tbc_level_scale.h's own note on why. The two must stay in agreement if
// this math ever changes.
//
// SMPTE 170M-2004 §4.4: standard NTSC picture black sits on a 7.5 IRE setup
// pedestal above the 0 IRE blanking level. NTSC-J (Japanese NTSC) has no
// setup — picture black IS the blanking level. This stage always assumes the
// standard pedestal for NTSC/PAL_M (no per-capture NTSC-J auto-detection —
// see the class comment in the header); a genuinely NTSC-J capture would
// need tbc_source instead.
constexpr double kNtscSetupIre = 7.5;

// frame_count == 0 (the parameter's unset/default value) means "unbounded":
// the source reads until a real end-of-stream rather than requiring an
// exact count up front. Internally this is represented as this sentinel
// instead of a genuinely infinite value so it stays a normal, finite
// FrameIDRange for frame_range()/has_frame() — see the class comment on
// TBCStreamFrameRepresentation. ~4.29 billion frames is years of video at
// any real frame rate, so it is never a practical limit.
constexpr uint32_t kUnboundedFrameCount = UINT32_MAX;

int32_t derive_blanking_16b(VideoSystem system, int32_t black_16b_ire,
                            int32_t white_16b_ire) {
  if (system == VideoSystem::NTSC || system == VideoSystem::PAL_M) {
    const double units_per_ire =
        static_cast<double>(white_16b_ire - black_16b_ire) /
        (100.0 - kNtscSetupIre);
    return static_cast<int32_t>(
        std::round(black_16b_ire - kNtscSetupIre * units_per_ire));
  }
  // PAL: no setup pedestal; black == blanking (0 IRE).
  return black_16b_ire;
}

// ---------------------------------------------------------------------------
// Per-system field geometry
// ---------------------------------------------------------------------------
// ld-decode stores every field (both parities) at field1_lines *
// samples_per_line words per slot on disk, regardless of which field it is
// — the same stride tbc_source's own read_field_samples() callers use for
// both fields' byte offsets. field2's real content is only field2_lines
// long; the remaining (field1_lines - field2_lines) lines in its slot are
// padding to discard, never real samples.
struct TbcFieldGeometry {
  int32_t field1_lines = 0;
  int32_t field2_lines = 0;
  int32_t samples_per_line = 0;
  int32_t frame_samples = 0;
  int32_t frame_height = 0;
};

TbcFieldGeometry tbc_field_geometry(VideoSystem system) {
  TbcFieldGeometry g;
  switch (system) {
    case VideoSystem::PAL:
      g.field1_lines = kPalField1Lines;
      g.field2_lines = kPalFrameLines - kPalField1Lines;
      g.samples_per_line = kPalSamplesPerLineNominal;
      g.frame_samples = kPalFrameSamples;
      g.frame_height = kPalFrameLines;
      break;
    case VideoSystem::PAL_M:
      g.field1_lines = kPalMField1Lines;
      g.field2_lines = kPalMFrameLines - kPalMField1Lines;
      g.samples_per_line = kPalMSamplesPerLine;
      g.frame_samples = kPalMFrameSamples;
      g.frame_height = kPalMFrameLines;
      break;
    case VideoSystem::NTSC:
    default:
      g.field1_lines = kNtscField1Lines;
      g.field2_lines = kNtscFrameLines - kNtscField1Lines;
      g.samples_per_line = kNtscSamplesPerLine;
      g.frame_samples = kNtscFrameSamples;
      g.frame_height = kNtscFrameLines;
      break;
  }
  return g;
}

std::vector<int16_t> assemble(VideoSystem system,
                              const std::vector<uint16_t>& field1,
                              const std::vector<uint16_t>& field2,
                              int32_t tbc_blanking, int32_t tbc_white) {
  switch (system) {
    case VideoSystem::PAL:
      return PalTBCConverter::assemble_frame(field1, field2, tbc_blanking,
                                             tbc_white);
    case VideoSystem::PAL_M:
      return PalMTBCConverter::assemble_frame(field1, field2, tbc_blanking,
                                              tbc_white);
    case VideoSystem::NTSC:
    default:
      return NtscTBCConverter::assemble_frame(field1, field2, tbc_blanking,
                                              tbc_white);
  }
}

// ---------------------------------------------------------------------------
// SourceParameters population from spec constants (no metadata to read)
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
// TBCStreamReader
// ---------------------------------------------------------------------------
// Reads TBC frames strictly forward from `input`, one frame (two fields) at
// a time. All the threading/ring-buffer/throttle machinery lives in
// orc::pipe_io::ThrottledRingReader (see that header); this constructor
// supplies only the "how do I produce frame N" piece specific to the TBC
// wire format and the given video system.
//
// Wire format: field 1 then field 2, each stored at field1_lines *
// samples_per_line words regardless of which one it is (ld-decode's own
// convention — see the TbcFieldGeometry comment above); field 2's trailing
// (field1_lines - field2_lines) lines are padding, discarded before handing
// the two fields to the system's converter.
TBCStreamReader::TBCStreamReader(std::istream& input, VideoSystem system,
                                 size_t frame_count, size_t buffer_frames,
                                 int32_t black_16b_ire, int32_t white_16b_ire)
    : reader_(
          [&input, system, frame_count, black_16b_ire, white_16b_ire](
              size_t frame_index, std::vector<int16_t>& out,
              std::string& error) -> pipe_io::ProduceStatus {
            const bool unbounded = (frame_count == kUnboundedFrameCount);
            const TbcFieldGeometry geometry = tbc_field_geometry(system);
            const size_t stored_field_words =
                static_cast<size_t>(geometry.field1_lines) *
                static_cast<size_t>(geometry.samples_per_line);
            const size_t field2_used_words =
                static_cast<size_t>(geometry.field2_lines) *
                static_cast<size_t>(geometry.samples_per_line);

            std::vector<uint16_t> field1(stored_field_words);
            input.read(reinterpret_cast<char*>(field1.data()),
                       static_cast<std::streamsize>(stored_field_words * 2));
            const std::streamsize got1 = input.gcount();
            if (got1 == 0) {
              if (unbounded) return pipe_io::ProduceStatus::kEof;
              error = "unexpected end of input reading field 1 of frame " +
                      std::to_string(frame_index) + " of " +
                      std::to_string(frame_count) +
                      " declared (frame_count parameter does not match the "
                      "actual input length)";
              return pipe_io::ProduceStatus::kError;
            }
            if (got1 != static_cast<std::streamsize>(stored_field_words * 2)) {
              error = "truncated frame " + std::to_string(frame_index) +
                      " reading field 1 (" + std::to_string(got1) + " of " +
                      std::to_string(stored_field_words * 2) +
                      " bytes) — input ended mid-field";
              return pipe_io::ProduceStatus::kError;
            }

            std::vector<uint16_t> field2_stored(stored_field_words);
            input.read(reinterpret_cast<char*>(field2_stored.data()),
                       static_cast<std::streamsize>(stored_field_words * 2));
            const std::streamsize got2 = input.gcount();
            if (got2 == 0) {
              if (unbounded) return pipe_io::ProduceStatus::kEof;
              error = "unexpected end of input reading field 2 of frame " +
                      std::to_string(frame_index) + " of " +
                      std::to_string(frame_count) +
                      " declared (frame_count parameter does not match the "
                      "actual input length)";
              return pipe_io::ProduceStatus::kError;
            }
            if (got2 != static_cast<std::streamsize>(stored_field_words * 2)) {
              error = "truncated frame " + std::to_string(frame_index) +
                      " reading field 2 (" + std::to_string(got2) + " of " +
                      std::to_string(stored_field_words * 2) +
                      " bytes) — input ended mid-field, after field 1 was "
                      "already read";
              return pipe_io::ProduceStatus::kError;
            }
            const std::vector<uint16_t> field2(
                field2_stored.begin(),
                field2_stored.begin() +
                    static_cast<std::ptrdiff_t>(field2_used_words));

            const int32_t tbc_blanking =
                derive_blanking_16b(system, black_16b_ire, white_16b_ire);
            try {
              out =
                  assemble(system, field1, field2, tbc_blanking, white_16b_ire);
            } catch (const std::exception& e) {
              error = std::string("frame assembly failed for frame ") +
                      std::to_string(frame_index) + ": " + e.what();
              return pipe_io::ProduceStatus::kError;
            }
            return pipe_io::ProduceStatus::kOk;
          },
          frame_count, buffer_frames, "TBCStreamReader") {}

namespace {

// ---------------------------------------------------------------------------
// TBCStreamFrameRepresentation
// ---------------------------------------------------------------------------
// VideoFrameRepresentation backed by a TBCStreamReader, reading from the
// input stream it owns: a real path's file, or this instance's reader of
// stdin or named pipe (see pipe_io::open_pipe_reader()).
class TBCStreamFrameRepresentation final : public VideoFrameRepresentation,
                                           public Artifact {
 public:
  TBCStreamFrameRepresentation(VideoSystem system, uint32_t frame_count,
                               int32_t frame_samples, int32_t frame_height,
                               int32_t spl_nominal,
                               std::unique_ptr<std::istream> owned_input,
                               int32_t black_16b_ire, int32_t white_16b_ire,
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
        reader_(*owned_input_, system, frame_count, buffer_frames,
                black_16b_ire, white_16b_ire) {}

  // A shared pipe reader is cut off here so the reader thread does not
  // wait on data this instance no longer wants; stopping the reader first
  // keeps that cut from being logged as a truncated input.
  ~TBCStreamFrameRepresentation() override {
    reader_.request_stop();
    if (auto* shared =
            dynamic_cast<pipe_io::SharedInputStream*>(owned_input_.get())) {
      shared->detach();
    }
  }

  std::string type_name() const override {
    return "TBCStreamFrameRepresentation";
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
  TBCStreamReader reader_;
};

}  // namespace

// ---------------------------------------------------------------------------
// FixedFormatTBCStreamSourceStage
// ---------------------------------------------------------------------------

FixedFormatTBCStreamSourceStage::FixedFormatTBCStreamSourceStage(
    const char* stage_name, const char* fixed_display_name,
    const char* description, VideoFormatCompatibility compatible_formats,
    VideoSystem system)
    : system_(system),
      stage_name_(stage_name),
      display_name_(fixed_display_name),
      description_(description),
      compatible_formats_(compatible_formats) {}

std::vector<ParameterDescriptor>
FixedFormatTBCStreamSourceStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType /*source_type*/) const {
  std::vector<ParameterDescriptor> desc;

  {
    ParameterDescriptor pd;
    pd.name = "input_path";
    pd.display_name = "Input Path";
    pd.description =
        "\"-\" reads from standard input (CLI only); a real named pipe "
        "path also works. There is no file-based fallback — use "
        "tbc_source for a real .tbc file on disk. Composite only.";
    pd.type = ParameterType::FILE_PATH;
    pd.constraints.required = true;
    desc.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = "black_16b_ire";
    pd.display_name = "Black Level (16-bit)";
    pd.description =
        "The capture's black16bIre value (from its .tbc.json/.tbc.db "
        "metadata — copy it verbatim for an accurate result). Defaults to "
        "the nominal SMPTE/ITU-R level for this system (standard 7.5 IRE "
        "setup for NTSC/PAL_M; use tbc_source for an NTSC-J capture) when "
        "left unset, since most captures are close to nominal and a piped "
        "source has no metadata of its own to read this from.";
    pd.type = ParameterType::INT32;
    pd.constraints.required = false;
    pd.constraints.default_value =
        (system_ == VideoSystem::PAL) ? kTbcPalBlanking : kTbcNtscBlack;
    desc.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = "white_16b_ire";
    pd.display_name = "White Level (16-bit)";
    pd.description =
        "The capture's white16bIre value (from its .tbc.json/.tbc.db "
        "metadata — copy it verbatim for an accurate result). Defaults to "
        "the nominal SMPTE/ITU-R level for this system when left unset, for "
        "the same reason as Black Level above.";
    pd.type = ParameterType::INT32;
    pd.constraints.required = false;
    pd.constraints.default_value =
        (system_ == VideoSystem::PAL) ? kTbcPalWhite : kTbcNtscWhite;
    desc.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = "frame_count";
    pd.display_name = "Frame Count";
    pd.description =
        "Total number of frames the input will provide. With no sidecar "
        "and no seekable input, this cannot be measured from the file "
        "size the way tbc_source does. Leave at 0 (the default) for an "
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
FixedFormatTBCStreamSourceStage::get_parameters() const {
  return {{"input_path", input_path_},
          {"black_16b_ire", black_16b_ire_},
          {"white_16b_ire", white_16b_ire_},
          {"frame_count", frame_count_},
          {"buffer_frames", buffer_frames_},
          {kStreamReaderCountParameter, stream_reader_count_}};
}

bool FixedFormatTBCStreamSourceStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "input_path") {
      input_path_ = std::get<std::string>(value);
    } else if (key == "black_16b_ire") {
      black_16b_ire_ = std::get<int32_t>(value);
    } else if (key == "white_16b_ire") {
      white_16b_ire_ = std::get<int32_t>(value);
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

std::vector<ArtifactPtr> FixedFormatTBCStreamSourceStage::execute(
    const std::vector<ArtifactPtr>& /*inputs*/,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& /*observation_context*/) {
  if (!parameters.empty()) set_parameters(parameters);

  std::lock_guard<std::mutex> lock(execute_mutex_);

  // The stream can only be read once, and the executor may call execute()
  // again on this instance on a cache miss: as long as the configuration is
  // unchanged, return the representation already built rather than trying
  // to open input_path_ a second time.
  const std::string config_key =
      input_path_ + "|" + std::to_string(black_16b_ire_) + "|" +
      std::to_string(white_16b_ire_) + "|" + std::to_string(frame_count_) +
      "|" + std::to_string(buffer_frames_) + "|" +
      std::to_string(stream_reader_count_);
  if (cached_representation_ && config_key == cached_config_key_) {
    return {cached_representation_};
  }

  if (input_path_.empty()) {
    ORC_LOG_ERROR("{}: input_path is required", stage_name_);
    return {};
  }
  if (white_16b_ire_ <= black_16b_ire_) {
    ORC_LOG_ERROR(
        "{}: white_16b_ire ({}) must be greater than black_16b_ire ({})",
        stage_name_, white_16b_ire_, black_16b_ire_);
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

  const TbcFieldGeometry geometry = tbc_field_geometry(system_);
  const SourceParameters src_params = build_source_parameters(
      system_, static_cast<int32_t>(effective_frame_count));

  if (frame_count_ == 0) {
    ORC_LOG_INFO(
        "{}: streaming until end-of-stream from '{}' ({}, black_16b_ire={}, "
        "white_16b_ire={}, buffer_frames={})",
        stage_name_, input_path_, video_system_to_string(system_),
        black_16b_ire_, white_16b_ire_, buffer_frames_);
  } else {
    ORC_LOG_INFO(
        "{}: streaming {} frames from '{}' ({}, black_16b_ire={}, "
        "white_16b_ire={}, buffer_frames={})",
        stage_name_, frame_count_, input_path_, video_system_to_string(system_),
        black_16b_ire_, white_16b_ire_, buffer_frames_);
  }

  Provenance prov;
  prov.stage_name = stage_name_;
  prov.stage_version = version();
  prov.parameters = {
      {"input_path", input_path_},
      {"video_system", video_system_to_string(system_)},
      {"black_16b_ire", std::to_string(black_16b_ire_)},
      {"white_16b_ire", std::to_string(white_16b_ire_)},
      {"frame_count", std::to_string(frame_count_)},
  };

  auto representation = std::make_shared<TBCStreamFrameRepresentation>(
      system_, effective_frame_count, geometry.frame_samples,
      geometry.frame_height, geometry.samples_per_line, std::move(input),
      black_16b_ire_, white_16b_ire_, buffer_frames_, src_params,
      ArtifactID(std::string(stage_name_) + ":" + config_key), std::move(prov));

  cached_representation_ = representation;
  cached_config_key_ = config_key;
  return {representation};
}

PALTBCStreamSourceStage::PALTBCStreamSourceStage()
    : FixedFormatTBCStreamSourceStage(
          "pal_tbc_stream_source", "PAL TBC Stream Source",
          "Sequential (pipe-compatible) PAL TBC source",
          VideoFormatCompatibility::PAL_ONLY, VideoSystem::PAL) {}

NTSCTBCStreamSourceStage::NTSCTBCStreamSourceStage()
    : FixedFormatTBCStreamSourceStage(
          "ntsc_tbc_stream_source", "NTSC TBC Stream Source",
          "Sequential (pipe-compatible) NTSC TBC source",
          VideoFormatCompatibility::NTSC_ONLY, VideoSystem::NTSC) {}

PALMTBCStreamSourceStage::PALMTBCStreamSourceStage()
    : FixedFormatTBCStreamSourceStage(
          "palm_tbc_stream_source", "PAL-M TBC Stream Source",
          "Sequential (pipe-compatible) PAL-M TBC source",
          VideoFormatCompatibility::PAL_M_ONLY, VideoSystem::PAL_M) {}

}  // namespace orc
