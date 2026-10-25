/*
 * File:        vbi_source_stage.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Raw VBI capture source stage: places VBI records into CVBS
 * frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_stage.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/support/logging.h>
#include <orc/support/lru_cache.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>

#include "vbi_frame_builder.h"
#include "vbi_frame_index.h"
#include "vbi_level_mapper.h"
#include "vbi_line_reader.h"
#include "vbi_offset_calibration.h"
#include "vbi_source_validation.h"
#include "vbi_teletext_service.h"
#include "vbi_transport.h"

namespace orc {

namespace {

// ---------------------------------------------------------------------------
// Stage identity
// ---------------------------------------------------------------------------

constexpr const char* kStageName = "vbi_source";
constexpr const char* kDisplayName = "VBI Capture Source";
constexpr const char* kDescription =
    "Raw VBI capture source - places the teletext line records of card and TBC "
    "VBI captures onto CVBS frames";

// The namespace every observation this stage writes belongs to.
constexpr const char* kObservationNamespace = "vbi_source";

// ---------------------------------------------------------------------------
// Parameter names
// ---------------------------------------------------------------------------

constexpr const char* kParamInputPath = "input_path";
constexpr const char* kParamFormat = "format";
constexpr const char* kParamDrops = "drops";

// Values of the drop policy.
constexpr const char* kDropsPreserve = "preserve";
constexpr const char* kDropsPad = "pad";

// Spellings the stage's provenance and observations use for what a preset
// turned out to describe.  Nothing configures these any more; they are read
// back off the expanded format.
constexpr const char* kTeletextWST = "WST";
constexpr const char* kTeletextNABTS = "NABTS";
constexpr const char* kTvSystemPAL = "PAL";
constexpr const char* kTvSystemNTSC = "NTSC";

// Built frames held against a repeat request.  Each PAL frame is 1,4 MB, so the
// window is a memory-against-rework trade: it covers a preview scrubbing back
// and forth and the observers that re-read a frame they have just been handed,
// without holding a decode's worth of frames resident.
//
// It has to be comfortably larger than the number of readers, or a window the
// size of the reader pool is evicted before its frames are used: every reader
// then misses, rebuilds what another reader just built, and evicts a third
// reader's frame doing it. The background observation pool runs one worker per
// two cores and the preview render is another reader on top, so thirty-two
// frames (45 MB) covers the pool on any machine this runs on with room for the
// prefetch window either side of the preview.
constexpr size_t kFrameCacheSize = 32;

// Stored frames calibration samples across the capture.
constexpr uint32_t kCalibrationSampleFrames = 16;

// ---------------------------------------------------------------------------
// Television systems
// ---------------------------------------------------------------------------

const char* tv_system_name(VBITVSystem tv_system) {
  return (tv_system == VBITVSystem::kNTSC) ? kTvSystemNTSC : kTvSystemPAL;
}

const char* teletext_system_name(VBITeletextSystem tt_system) {
  return (tt_system == VBITeletextSystem::kNABTS) ? kTeletextNABTS
                                                  : kTeletextWST;
}

// The project's television system, as the source format model spells it.  An
// unknown project format has no answer, in which case the parameter surface
// offers everything the stage can place rather than guessing.
std::optional<VBITVSystem> project_tv_system(VideoSystem project_format) {
  switch (project_format) {
    case VideoSystem::PAL:
      return VBITVSystem::kPAL;
    case VideoSystem::NTSC:
      return VBITVSystem::kNTSC;
    default:
      return std::nullopt;
  }
}

// The preset a new node of a project starts on.  On 525 lines that is the WST
// one rather than the NABTS one, because WST is the service the host can
// actually decode; the user has to make the choice either way.
const char* default_preset_for(std::optional<VBITVSystem> tv_system) {
  if (tv_system == VBITVSystem::kNTSC) {
    return ".tbc VBI crop, 16-bit (WST)";
  }
  return "bt8x8 card dump, 8-bit (WST)";
}

// ---------------------------------------------------------------------------
// Parameter map access
// ---------------------------------------------------------------------------

std::string string_param(const std::map<std::string, ParameterValue>& params,
                         const char* key, const std::string& fallback) {
  const auto it = params.find(key);
  if (it == params.end()) return fallback;
  const auto* value = std::get_if<std::string>(&it->second);
  return value ? *value : fallback;
}

// ---------------------------------------------------------------------------
// Production dependencies
// ---------------------------------------------------------------------------

class VBISourceStageDeps final : public IVBISourceStageDeps {
 public:
  bool validate_input_file(const std::string& input_path,
                           std::string& error_message) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(input_path, ec)) {
      error_message = "VBI capture '" + input_path + "' does not exist.";
      return false;
    }
    if (!fs::is_regular_file(input_path, ec)) {
      error_message = "VBI capture '" + input_path + "' is not a regular file.";
      return false;
    }
    return true;
  }

  std::unique_ptr<IVBIByteSource> open_byte_source(
      const std::string& input_path,
      std::string& error_message) const override {
    return open_vbi_byte_source(input_path, error_message);
  }
};

// ---------------------------------------------------------------------------
// VBIFrameRepresentation
// ---------------------------------------------------------------------------

// The stage's output: CVBS frames built on the frame a consumer asks for, from
// the line records of the capture that frame came from.
//
// Inherits VideoFrameRepresentation (the read contract every downstream stage
// uses) and Artifact (so it can be returned from execute()), and implements the
// frame index's counter source over its own reader.
//
// Thread safety: two locks, deliberately narrow.  io_mutex_ covers reads
// through the byte source (which holds one stream position, so they are
// strictly serial) and cache_mutex_ covers the frame cache.  Building the frame
// from the records that read produced holds neither: the builder is const and
// carries only configuration, so building is reentrant.  Holding one lock
// across all of it made every reader in the process queue behind every other,
// and because std::mutex is not fair, a pool of background observation workers
// could starve the interactive preview render.  Nothing that is not stream
// state is locked here.
class VBIFrameRepresentation final : public VideoFrameRepresentation,
                                     public Artifact,
                                     private IVBIFrameCounterSource {
 public:
  // Everything a run is configured by, gathered so the factory signature does
  // not run to a dozen positional arguments.
  struct Setup {
    VBISourceFormat format;
    VBILevelMapperConfig levels;
    VBIFrameSequenceConfig sequence;
    VBIResolvedTiming timing;
    std::string input_path;
  };

  // Build the representation over an opened capture.  Returns nullptr with an
  // error message for any part of the configuration the stage cannot place, or
  // when the capture's frame counter could not be read.
  static std::shared_ptr<VBIFrameRepresentation> create(
      std::unique_ptr<IVBIByteSource> byte_source, Setup setup,
      ArtifactID artifact_id, Provenance provenance,
      std::string& error_message) {
    if (byte_source == nullptr) {
      error_message = "The VBI capture was not opened.";
      return nullptr;
    }

    auto representation =
        std::shared_ptr<VBIFrameRepresentation>(new VBIFrameRepresentation(
            std::move(byte_source), std::move(setup), std::move(artifact_id),
            std::move(provenance)));

    if (!representation->initialise(error_message)) {
      return nullptr;
    }
    return representation;
  }

  // --------------------------------------------------------------------------
  // Artifact
  // --------------------------------------------------------------------------
  std::string type_name() const override { return "VBIFrameRepresentation"; }

  // --------------------------------------------------------------------------
  // What the run made of the capture
  // --------------------------------------------------------------------------
  const VBIFrameIndex& frame_index() const { return index_; }

  // --------------------------------------------------------------------------
  // Navigation
  // --------------------------------------------------------------------------
  FrameIDRange frame_range() const override {
    if (frame_count_ == 0) return FrameIDRange{1, 0};  // empty: last < first
    return FrameIDRange{0, static_cast<FrameID>(frame_count_ - 1)};
  }

  size_t frame_count() const override { return frame_count_; }

  bool has_frame(FrameID id) const override {
    return id < static_cast<FrameID>(frame_count_);
  }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    const VBIOutputFrame& output = builder_.output_frame();
    FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = output.system;
    desc.height = static_cast<size_t>(output.lines_per_frame);
    desc.samples_total = static_cast<size_t>(output.samples_per_frame);
    desc.samples_per_line_nominal =
        static_cast<size_t>(output.samples_per_line_nominal);
    return desc;
  }

  // --------------------------------------------------------------------------
  // Flat sample access
  // --------------------------------------------------------------------------
  //
  // Line access is left to the base class, which reads the flat frame through
  // frame_line_util.h — the same lattice the frames are written on, so a caller
  // asking for a line gets the line the builder wrote.
  const sample_type* get_frame(FrameID id) const override {
    if (!has_frame(id)) return nullptr;
    const std::vector<sample_type>* frame = ensure_frame_cached(id);
    return frame ? frame->data() : nullptr;
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    if (!has_frame(id)) return {};
    ensure_frame_cached(id);
    // Copied with the cache lock held so an eviction on another thread cannot
    // free the entry mid-copy.
    std::lock_guard<std::mutex> lock(cache_mutex_);
    const std::vector<sample_type>* frame = frame_cache_.get_ptr(id);
    return frame != nullptr ? *frame : std::vector<sample_type>{};
  }

  // --------------------------------------------------------------------------
  // Hints
  // --------------------------------------------------------------------------
  std::optional<SourceParameters> get_video_parameters() const override {
    return video_params_;
  }

 private:
  VBIFrameRepresentation(std::unique_ptr<IVBIByteSource> byte_source,
                         Setup setup, ArtifactID artifact_id,
                         Provenance provenance)
      : Artifact(std::move(artifact_id), std::move(provenance)),
        byte_source_(std::move(byte_source)),
        setup_(std::move(setup)) {}

  bool initialise(std::string& error_message) {
    reader_ = std::make_unique<VBILineReader>(setup_.format, *byte_source_);

    if (!make_vbi_frame_builder(setup_.format, setup_.levels, setup_.timing,
                                builder_, error_message)) {
      return false;
    }

    const std::optional<uint64_t> stored_frames = reader_->frame_count();
    if (!stored_frames.has_value()) {
      error_message =
          "The capture's length could not be established, so the number of "
          "frames it holds is unknown.";
      return false;
    }

    if (!VBIFrameIndex::build(setup_.format, setup_.sequence, *stored_frames,
                              *this, index_, error_message)) {
      return false;
    }
    frame_count_ = static_cast<size_t>(index_.output_frame_count());

    build_video_parameters();
    return true;
  }

  // EBU Tech. 3280-E (PAL) and SMPTE 244M/170M (NTSC) level and geometry
  // constants for the emitted frames.  The active picture bounds are the
  // standard's, so that a consumer reading them sees the frame the stage claims
  // to produce rather than the empty raster it happens to hold.
  void build_video_parameters() {
    video_params_ = SourceParameters{};
    video_params_.number_of_sequential_frames =
        static_cast<int32_t>(frame_count_);

    if (builder_.output_frame().system == VideoSystem::NTSC) {
      video_params_.system = VideoSystem::NTSC;
      video_params_.frame_width_nominal = kNtscSamplesPerLine;
      video_params_.frame_height = kNtscFrameLines;
      video_params_.sync_tip_level = kNtscSyncTip;
      video_params_.blanking_level = kNtscBlanking;
      video_params_.black_level = kNtscBlack;
      video_params_.white_level = kNtscWhite;
      video_params_.peak_level = kNtscPeak;
      video_params_.active_video_start = kNtscActiveVideoStart;
      video_params_.active_video_end = kNtscActiveVideoEnd;
      video_params_.first_active_frame_line = kNtscFirstActiveFrameLine;
      video_params_.last_active_frame_line = kNtscLastActiveFrameLine;
      return;
    }

    video_params_.system = VideoSystem::PAL;
    video_params_.frame_width_nominal = kPalSamplesPerLineNominal;
    video_params_.frame_height = kPalFrameLines;
    video_params_.sync_tip_level = kPalSyncTip;
    video_params_.blanking_level = kPalBlanking;
    video_params_.black_level = kPalBlack;
    video_params_.white_level = kPalWhite;
    video_params_.peak_level = kPalPeak;
    video_params_.active_video_start = kPalActiveVideoStart;
    video_params_.active_video_end = kPalActiveVideoEnd;
    video_params_.first_active_frame_line = kPalFirstActiveFrameLine;
    video_params_.last_active_frame_line = kPalLastActiveFrameLine;
  }

  // IVBIFrameCounterSource.  Called by the frame index, which is only ever
  // reached with io_mutex_ already held.
  bool frame_counter(uint64_t stored_frame_index,
                     std::optional<uint32_t>& out_counter,
                     std::string& error_message) const override {
    return reader_->read_frame_counter(stored_frame_index, out_counter,
                                       error_message);
  }

  // Caller holds no lock.
  const std::vector<sample_type>* ensure_frame_cached(FrameID id) const {
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      if (frame_cache_.contains(id)) {
        return frame_cache_.get_ptr(id);
      }
    }

    // Built with no lock held. Two threads racing the same frame build it twice
    // and the loser's copy is dropped, which costs one frame of duplicated work
    // — far less than the alternative of holding every other reader off for the
    // duration (see the class comment).
    std::vector<sample_type> built = build_frame(id);

    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (!frame_cache_.contains(id)) {
      frame_cache_.put(id, std::move(built));
    }
    return frame_cache_.get_ptr(id);
  }

  // Caller holds no lock: this takes io_mutex_ for the parts that read the
  // capture and holds nothing for the rest.  Throws when the frame cannot be
  // built: a frame that cannot be built is a broken configuration or a broken
  // capture, and returning blanking for it would hide both.
  std::vector<sample_type> build_frame(FrameID id) const {
    std::string error;

    VBIOutputFramePlan plan;
    VBIFrameRecords records;
    {
      // Everything that reads through the byte source, and nothing else. The
      // frame index resolves a plan by reading frame counters through the same
      // reader, so it belongs inside the same critical section.
      std::lock_guard<std::mutex> lock(io_mutex_);

      if (!index_.frame_plan(static_cast<uint64_t>(id), plan, error)) {
        throw std::runtime_error("VBI source '" + setup_.input_path +
                                 "': " + error);
      }
      if (!plan.padding &&
          !reader_->read_frame(plan.source_frame_index, records, error)) {
        throw std::runtime_error("VBI source '" + setup_.input_path +
                                 "': " + error);
      }
    }

    std::vector<sample_type> samples;
    if (plan.padding) {
      builder_.build_blank_frame(samples);
      return samples;
    }

    uint32_t data_lines = 0;
    if (!builder_.build_frame(records.lines, samples, data_lines, error)) {
      throw std::runtime_error("VBI source '" + setup_.input_path +
                               "': " + error);
    }
    return samples;
  }

  std::unique_ptr<IVBIByteSource> byte_source_;
  Setup setup_;

  // Held by pointer because it cannot be built until the capture is open and
  // it is not default-constructible.
  std::unique_ptr<VBILineReader> reader_;

  VBIFrameBuilder builder_;
  VBIFrameIndex index_;

  size_t frame_count_ = 0;
  SourceParameters video_params_;

  // The capture: one stream position, so every read through it is serial.
  mutable std::mutex io_mutex_;

  // The cache, held only across a lookup or an insertion — never across a
  // build, and never across a read of the capture.
  mutable std::mutex cache_mutex_;
  mutable LRUCache<FrameID, std::vector<sample_type>> frame_cache_{
      kFrameCacheSize};
};

// ---------------------------------------------------------------------------
// Configuration helpers
// ---------------------------------------------------------------------------

VBIDropPolicy parse_drop_policy(const std::string& name) {
  return (name == kDropsPad) ? VBIDropPolicy::kPad : VBIDropPolicy::kPreserve;
}

// Join a list of messages into one, so a wrong configuration is reported in
// full rather than one field at a time.
std::string join_messages(const std::vector<std::string>& messages) {
  std::string joined;
  for (const std::string& message : messages) {
    if (!joined.empty()) joined += " ";
    joined += message;
  }
  return joined;
}

// Fit the capture offset from the clock run-in.
//
// The stage does not proceed with a bad fit: the offset is applied globally to
// a capture that may run for hours, so a wrong one silently mis-places every
// line of it (design §5.3.4).
double calibrate_capture_offset(const VBILineReader& reader,
                                const VBITeletextService& service,
                                const std::string& input_path,
                                ObservationContext& observation_context) {
  VBICalibrationConfig calibration_config;
  calibration_config.sample_frames = kCalibrationSampleFrames;

  VBIOffsetCalibration calibration;
  std::string error;
  if (!calibrate_vbi_capture_offset(reader, service, calibration_config,
                                    calibration, error)) {
    throw UserDataError("The capture offset of '" + input_path +
                        "' could not be measured: " + error);
  }

  if (!calibration.converged) {
    throw UserDataError(
        "The capture offset of '" + input_path + "' could not be trusted: " +
        join_messages(calibration.diagnostics) + " " + calibration.summary);
  }

  observation_context.set(FieldID(0), kObservationNamespace, "calibration",
                          calibration.summary);
  observation_context.set(FieldID(0), kObservationNamespace, "capture_offset",
                          calibration.capture_offset_samples);
  observation_context.set(FieldID(0), kObservationNamespace,
                          "capture_offset_spread", calibration.spread_samples);
  observation_context.set(FieldID(0), kObservationNamespace,
                          "capture_offset_acceptance",
                          calibration.acceptance_fraction);

  ORC_LOG_INFO("{}: {}", kStageName, calibration.summary);
  for (const std::string& warning : calibration.warnings) {
    ORC_LOG_WARN("{}: {}", kStageName, warning);
  }

  return calibration.capture_offset_samples;
}

// Measure where a time-base corrected capture's run-in actually is, and return
// the service anchor that describes it.  Returns the tabulated anchor unchanged
// when the run-in cannot be found well enough to trust.
//
// Unlike a card capture's offset, a failed fit here is not fatal.  The two
// failures are not comparable: a wrong global offset mis-places every line of
// the capture, where a missing anchor measurement only leaves the tabulated
// figure in place — which is what the stage used for these sources before it
// measured anything, and is right for the captures the figure was taken from.
// Refusing the file would turn an improvement into a regression.
double measure_service_anchor_ns(const VBILineReader& reader,
                                 const VBITeletextService& service,
                                 const std::string& input_path,
                                 ObservationContext& observation_context) {
  VBICalibrationConfig calibration_config;
  calibration_config.sample_frames = kCalibrationSampleFrames;

  VBIOffsetCalibration calibration;
  std::string error;
  if (!measure_vbi_service_anchor(reader, service, calibration_config,
                                  calibration, error)) {
    ORC_LOG_WARN(
        "{}: the clock run-in of '{}' could not be looked for ({}); the "
        "service's tabulated anchor of {:.0f} ns is used as transmitted.",
        kStageName, input_path, error, service.t_offset_ns);
    return service.t_offset_ns;
  }

  if (!calibration.converged) {
    ORC_LOG_WARN(
        "{}: the clock run-in of '{}' was not found well enough to measure "
        "where the transmission put it: {} {} The service's tabulated anchor "
        "of {:.0f} ns is used instead, which is right for a transmission that "
        "matches it and clips the head of the data line for one that does not.",
        kStageName, input_path, join_messages(calibration.diagnostics),
        calibration.summary, service.t_offset_ns);
    return service.t_offset_ns;
  }

  const double measured_ns =
      vbi_measured_anchor_ns(calibration, reader.format().sample_rate_hz);

  observation_context.set(FieldID(0), kObservationNamespace, "calibration",
                          calibration.summary);
  observation_context.set(FieldID(0), kObservationNamespace, "service_anchor",
                          measured_ns);
  observation_context.set(FieldID(0), kObservationNamespace,
                          "capture_offset_spread", calibration.spread_samples);
  observation_context.set(FieldID(0), kObservationNamespace,
                          "capture_offset_acceptance",
                          calibration.acceptance_fraction);

  ORC_LOG_INFO(
      "{}: capture offset held at 0.00 samples, which is what a time-base "
      "corrected record starts at. Run-in measured at {:.0f} ns from 0H "
      "against the service's tabulated {:.0f} ns, a difference of {:.2f} "
      "samples; the data window follows the measurement. Spread {:.2f} "
      "samples ({}), locked on {} of {} records ({:.1f}%).",
      kStageName, measured_ns, service.t_offset_ns,
      (measured_ns - service.t_offset_ns) * 1e-9 *
          reader.format().sample_rate_hz,
      calibration.spread_samples,
      calibration.spread_class == VBIOffsetSpreadClass::kTight
          ? "time-base corrected"
          : "mild jitter",
      calibration.records_accepted, calibration.records_examined,
      calibration.acceptance_fraction * 100.0);

  for (const std::string& warning : calibration.warnings) {
    ORC_LOG_WARN("{}: {}", kStageName, warning);
  }

  return measured_ns;
}

}  // namespace

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

bool VBISourceStage::make_source_format(const Configuration& configuration,
                                        VBISourceFormat& out_format,
                                        std::string& error_message) {
  // A preset expands to a complete descriptor, so this is the whole of the
  // configuration step: there is nothing left for the caller to override.
  return expand_vbi_source_preset(configuration.format_preset, out_format,
                                  error_message);
}

std::string VBISourceStage::Configuration::cache_key() const {
  return input_path + "|" + format_preset + "|" + drops;
}

VBISourceStage::Configuration VBISourceStage::configuration_from(
    const std::map<std::string, ParameterValue>& parameters) const {
  Configuration configuration = configuration_;

  configuration.input_path =
      string_param(parameters, kParamInputPath, configuration.input_path);
  configuration.format_preset =
      string_param(parameters, kParamFormat, configuration.format_preset);
  configuration.drops =
      string_param(parameters, kParamDrops, configuration.drops);

  return configuration;
}

// ---------------------------------------------------------------------------
// VBISourceStage
// ---------------------------------------------------------------------------

VBISourceStage::VBISourceStage(std::shared_ptr<IVBISourceStageDeps> deps)
    : deps_(deps ? std::move(deps) : std::make_shared<VBISourceStageDeps>()) {
  set_configuration_status(ConfigurationStatus::Red);
}

NodeTypeInfo VBISourceStage::get_node_type_info() const {
  return NodeTypeInfo{NodeType::SOURCE, kStageName, kDisplayName, kDescription,
                      0, 0, 1, UINT32_MAX,
                      // Both 625-line and 525-line frames can be placed; which
                      // formats the parameter surface offers follows the
                      // project's own system.
                      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> VBISourceStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  std::lock_guard<std::mutex> lock(execute_mutex_);

  if (!inputs.empty()) {
    throw std::runtime_error(std::string(kStageName) +
                             ": source stage expects no inputs");
  }

  const Configuration configuration = configuration_from(parameters);

  if (configuration.input_path.empty()) {
    ORC_LOG_DEBUG("{}: no input configured", kStageName);
    return {};
  }

  const std::string cache_key = configuration.cache_key();
  if (cached_representation_ && cached_key_ == cache_key) {
    return {cached_representation_};
  }

  // --- Container descriptor ---
  VBISourceFormat format;
  std::string error;
  if (!make_source_format(configuration, format, error)) {
    throw UserDataError(error);
  }

  // --- Input file ---
  if (!deps_->validate_input_file(configuration.input_path, error)) {
    throw UserDataError(error);
  }

  std::unique_ptr<IVBIByteSource> byte_source =
      deps_->open_byte_source(configuration.input_path, error);
  if (byte_source == nullptr) {
    throw UserDataError(error);
  }

  // --- Configuration validation against the stream ---
  VBITransportHints hints;
  hints.bits_per_sample = byte_source->declared_bits_per_sample();
  const std::vector<std::string> violations =
      validate_vbi_source_config(format, byte_source->size_bytes(), hints);
  if (!violations.empty()) {
    throw UserDataError(
        "VBI capture '" + configuration.input_path +
        "' does not match its configuration: " + join_messages(violations));
  }

  VBITeletextService service;
  if (!vbi_teletext_service(format.tv_system, format.tt_system, service,
                            error)) {
    throw UserDataError(error);
  }

  VBILineReader reader(format, *byte_source);
  const std::optional<uint64_t> stored_frames = reader.frame_count();
  if (!stored_frames.has_value() || *stored_frames == 0) {
    throw UserDataError("VBI capture '" + configuration.input_path +
                        "' is too short to hold one complete frame of the "
                        "configured container.");
  }

  // A capture that ends short of a whole frame has passed validation, because
  // such a file is perfectly ordinary; what is not ordinary is silently
  // dropping data, so it is said in as many words — and what was dropped is
  // worth telling apart, an odd field being how a capture ordinarily ends and
  // a ragged tail meaning the writer stopped part-way through a record.
  if (reader.has_partial_trailing_frame()) {
    const uint64_t trailing = reader.trailing_bytes().value_or(0);
    if (trailing == format.bytes_per_field()) {
      ORC_LOG_INFO(
          "{}: '{}' ends on an odd field. The trailing field is one short of a "
          "frame and is not emitted; {} whole frames were found.",
          kStageName, configuration.input_path, *stored_frames);
    } else {
      ORC_LOG_INFO(
          "{}: '{}' ends part-way through a frame, {} bytes past the last "
          "whole one, so the capture was cut short of a record boundary. Those "
          "bytes are not emitted; {} whole frames were found. If the capture "
          "was not interrupted, check the format: a wrong container geometry "
          "leaves a ragged tail too.",
          kStageName, configuration.input_path, trailing, *stored_frames);
    }
  }

  // --- Capture offset, or the service anchor in its place ---
  //
  // The two families have one unknown each and they are different unknowns.  A
  // card capture does not know when its window opened, so the offset is fitted
  // and the service's tabulated anchor is taken as given.  A TBC-derived
  // capture knows exactly when its window opened — at 0H — but not when the
  // broadcaster transmitted, so the offset is taken as given and the anchor is
  // measured.  Fitting both would be fitting one unknown twice.
  VBIResolvedTiming timing;
  timing.capture_offset_samples = format.capture_offset_samples;
  if (format.capture_offset_is_auto) {
    timing.capture_offset_samples = calibrate_capture_offset(
        reader, service, configuration.input_path, observation_context);
  } else if (format.family == VBISourceFamily::kTBCDerived) {
    timing.service_anchor_ns = measure_service_anchor_ns(
        reader, service, configuration.input_path, observation_context);
  } else {
    ORC_LOG_INFO(
        "{}: capture offset held at the configured {:.2f} samples; no "
        "calibration was run",
        kStageName, timing.capture_offset_samples);
  }
  const double capture_offset_samples = timing.capture_offset_samples;

  // The fitted figure replaces the descriptor's starting hint from here on, so
  // that everything derived from the record's own geometry — in particular the
  // windows the level mapper reads its logic levels from — sits where the
  // run-in actually is rather than where the preset's folklore predicted it.
  format.capture_offset_samples = capture_offset_samples;
  format.capture_offset_is_auto = false;

  // --- Output representation ---
  //
  // The level policy is the default per-line estimate, which is what a card
  // capture needs; the frame builder replaces it wholesale for a TBC-derived
  // format, whose levels are already absolute.  Neither is configurable,
  // because neither is a choice: it follows from what the capture is.
  VBIFrameRepresentation::Setup setup;
  setup.format = format;
  setup.sequence.drops = parse_drop_policy(configuration.drops);
  setup.timing = timing;
  setup.input_path = configuration.input_path;

  if (format.family == VBISourceFamily::kTBCDerived) {
    ORC_LOG_INFO(
        "{}: '{}' is a capture cropped from a decoded .tbc, so its levels are "
        "already absolute: the standard 16-bit decoder domain is mapped "
        "straight onto the output's and nothing is estimated.",
        kStageName, configuration.input_path);
  }

  Provenance provenance;
  provenance.stage_name = kStageName;
  provenance.stage_version = version();
  provenance.parameters = {
      {kParamInputPath, configuration.input_path},
      {kParamFormat, configuration.format_preset},
      // Read off the expanded preset rather than configured, and carried so a
      // downstream decoder can see which data service the capture holds
      // rather than assuming one (design §4).
      {"teletext_system", teletext_system_name(format.tt_system)},
      {"video_system", tv_system_name(format.tv_system)},
      {"sample_encoding", "CVBS_U10_4FSC"},
      {"capture_offset_samples", std::to_string(capture_offset_samples)},
      {kParamDrops, configuration.drops},
  };

  // Recorded only when it was measured, so that its presence says the anchor
  // came from the capture and its absence says the service table's figure was
  // used as transmitted.
  if (timing.service_anchor_ns.has_value()) {
    provenance.parameters["service_anchor_ns"] =
        std::to_string(*timing.service_anchor_ns);
  }

  auto representation = VBIFrameRepresentation::create(
      std::move(byte_source), std::move(setup),
      ArtifactID(std::string(kStageName) + ":" + cache_key),
      std::move(provenance), error);
  if (representation == nullptr) {
    throw UserDataError(error);
  }

  const VBIFrameIndex& index = representation->frame_index();
  const std::string sequence_summary = index.summary();
  observation_context.set(FieldID(0), kObservationNamespace, "frame_sequence",
                          sequence_summary);
  observation_context.set(FieldID(0), kObservationNamespace, "teletext_system",
                          std::string(teletext_system_name(format.tt_system)));
  observation_context.set(FieldID(0), kObservationNamespace, "video_system",
                          std::string(tv_system_name(format.tv_system)));

  ORC_LOG_INFO(
      "{}: loaded '{}' — {} format, {} {} teletext, {} stored frames, {} "
      "output frames. {}",
      kStageName, configuration.input_path, configuration.format_preset,
      tv_system_name(format.tv_system), teletext_system_name(format.tt_system),
      index.stored_frame_count(), index.output_frame_count(), sequence_summary);

  cached_representation_ = representation;
  cached_key_ = cache_key;
  return {representation};
}

std::vector<ParameterDescriptor> VBISourceStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType /*source_type*/) const {
  std::vector<ParameterDescriptor> descriptors;

  // A capture's television system fixes the geometry of the frames it is placed
  // on, so only the formats belonging to the project's own system can produce
  // frames the project can hold.  The surface offers those and nothing else,
  // rather than offering everything and refusing most of it at execution.
  const std::optional<VBITVSystem> tv_system =
      project_tv_system(project_format);
  const std::vector<std::string> preset_names =
      tv_system.has_value() ? vbi_source_preset_names(*tv_system)
                            : vbi_source_preset_names();

  {
    ParameterDescriptor pd;
    pd.name = kParamInputPath;
    pd.display_name = "VBI Capture Path";
    pd.description =
        "Path to the raw VBI capture. FLAC-wrapped captures (.vbi.flac) are "
        "unwrapped transparently; the wrapper's declared sample rate is "
        "ignored. Nothing about a capture is read from its name or its "
        "extension — the container is entirely the Capture Format you pick — "
        "so a capture whose extension is not one of the usual ones can still "
        "be selected through the dialog's All Files filter.";
    pd.type = ParameterType::FILE_PATH;
    pd.constraints.required = false;
    pd.constraints.default_value = std::string("");

    // The extensions these captures actually arrive under. .vbi is a card
    // dump, .flac the same thing losslessly wrapped, .u8 and .u16 the raw
    // word-size spellings a decoder's VBI-only export uses, and .tbc the crop
    // taken off a decoded capture.  It is only a filter on the browse dialog:
    // the stage reads the container the format says it is, whatever the file
    // is called, and the dialog keeps an All Files entry for anything else.
    pd.file_extension_hint = ".vbi|.flac|.u8|.u16|.tbc";
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamFormat;
    pd.display_name = "Capture Format";
    pd.description =
        "What the capture is, which is the whole of the configuration: the "
        "geometry, the data service and the timing all follow from it. Only "
        "the formats belonging to the project's television system are offered."
        "\n\n"
        "bt8x8 card dump, 8-bit (WST): a 625-line capture-card dump. 2048 "
        "samples per record of which 2044 are real, unsigned 8-bit, at 8x fsc "
        "(35 468 950 Hz); 16 records per field carrying field lines 7-22. The "
        "card's own time from 0H is unreliable, so it is measured from the "
        "clock run-in when the capture is opened, and the logic levels are "
        "estimated per line because they move with its gain control. The last "
        "four bytes of each frame are the driver's frame counter, which is "
        "what makes dropped frames detectable."
        "\n\n"
        "bt8x8 card dump, 8-bit (WST, SECAM source): the same container, byte "
        "for byte, from a SECAM source. Pick it when the capture came from a "
        "SECAM broadcast or tape: the vertical colour identification signal "
        "occupies field lines 8-15, leaving only a few of the sixteen records "
        "free for teletext, so the run-in is expected on far fewer of them "
        "than a PAL capture and the PAL entry rejects the capture on that "
        "count alone."
        "\n\n"
        "bt8x8 card dump, 8-bit (WST, NTSC source) / (NABTS, NTSC source): "
        "the same card from a 525-line source, which is not the same "
        "container. The driver's NTSC television norm has its own sampling "
        "clock (28 636 363 Hz, 8x fsc NTSC) and its own vbipack, so 1600 of "
        "the 2048 samples per record are real rather than 2044, and its "
        "records start at field line 10 — the head of the 525-line teletext "
        "list — so records 0-11 carry field lines 10-21 and the last four are "
        "picture. The record stride, the sixteen records a field stores and "
        "the frame counter are the 625-line entry's. Pick the variant "
        "matching the service the broadcast carried."
        "\n\n"
        "cx2388x card dump, 8-bit (WST, NTSC source) / (NABTS, NTSC source): "
        "a 525-line capture-card dump from a Conexant CX23880/1/2/3 card (the "
        "Linux cx88 driver). The bt8x8's sampling clock (28 636 363 Hz, 8x fsc "
        "NTSC) and record — 2048 samples of which 1600 are real — but 12 "
        "records per field carrying field lines 10-21, the whole 525-line "
        "teletext list, and no frame counter, so a dropped frame cannot be "
        "seen. The time from 0H is measured from the clock run-in when the "
        "capture is opened and the logic levels are estimated per line. Pick "
        "the variant matching the service the broadcast carried."
        "\n\n"
        "cx23885 card dump, 8-bit (WST) / (NABTS): a 525-line capture-card "
        "dump from a Hauppauge HVR-1250 or one of its siblings. 1440 samples "
        "per record with no padding, unsigned 8-bit, at 27 MHz; 12 records per "
        "field carrying field lines 10-21, which is the whole 525-line "
        "teletext list. The window the card hands over is the digital active "
        "line, so it holds no sync and no burst, and there is no frame counter "
        "and so no way to see a dropped frame. The time from 0H is measured "
        "from the clock run-in when the capture is opened and the logic levels "
        "are estimated per line, as on any card capture. Pick the variant "
        "matching the service the broadcast carried."
        "\n\n"
        ".tbc VBI crop, 16-bit (WST) / (NABTS): the first 16 line records of "
        "each field of a decoded 525-line luma .tbc. 910 samples per record "
        "with no padding, unsigned 16-bit, at 4x fsc (14 318 182 Hz); records "
        "1-12 carry field lines 10-21 and the rest are the last equalising "
        "line and the start of the picture. Sample 0 of every record is "
        "already 0H and the levels are already absolute, so neither is "
        "measured. Pick the variant matching the service the broadcast "
        "carried: nothing in the file records it, and the two differ only in "
        "framing code and packet length.";
    pd.type = ParameterType::STRING;
    pd.constraints.required = true;
    pd.constraints.default_value = std::string(default_preset_for(tv_system));
    pd.constraints.allowed_strings = preset_names;
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamDrops;
    pd.display_name = "Dropped Frames";
    pd.description =
        "What to do about frames the capture dropped, which a driver's frame "
        "counter makes visible. 'preserve' emits only the frames present; "
        "'pad' synthesises blank frames so output frame n stays aligned with "
        "source frame n. A format carrying no counter cannot report drops at "
        "all, so neither policy has anything to act on.";
    pd.type = ParameterType::STRING;
    pd.constraints.default_value = std::string(kDropsPreserve);
    pd.constraints.allowed_strings = {kDropsPreserve, kDropsPad};
    descriptors.push_back(pd);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> VBISourceStage::get_parameters() const {
  return {
      {kParamInputPath, configuration_.input_path},
      {kParamFormat, configuration_.format_preset},
      {kParamDrops, configuration_.drops},
  };
}

bool VBISourceStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key != kParamInputPath && key != kParamFormat && key != kParamDrops) {
      ORC_LOG_WARN("{}: unknown parameter '{}'", kStageName, key);
      return false;
    }
    if (!std::holds_alternative<std::string>(value)) {
      ORC_LOG_WARN("{}: parameter '{}' expects a string", kStageName, key);
      return false;
    }
  }

  configuration_ = configuration_from(params);

  if (configuration_.input_path.empty()) {
    set_configuration_status(ConfigurationStatus::Red);
    return true;
  }

  std::string error;
  if (!deps_->validate_input_file(configuration_.input_path, error)) {
    ORC_LOG_WARN("{}: capture not accessible: {}", kStageName, error);
    set_configuration_status(ConfigurationStatus::Red);
    return true;
  }

  // Everything that can be judged without reading the capture, which for a
  // preset is only that it exists and is internally consistent.  The stream
  // itself is checked against it at execution.
  VBISourceFormat format;
  if (!make_source_format(configuration_, format, error)) {
    ORC_LOG_WARN("{}: {}", kStageName, error);
    set_configuration_status(ConfigurationStatus::Red);
    return true;
  }

  const std::vector<std::string> violations =
      validate_vbi_source_config(format, std::nullopt, VBITransportHints{});
  if (!violations.empty()) {
    ORC_LOG_WARN("{}: {}", kStageName, join_messages(violations));
    set_configuration_status(ConfigurationStatus::Red);
    return true;
  }

  set_configuration_status(ConfigurationStatus::Green);
  return true;
}

StagePreviewCapability VBISourceStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(
          cached_representation_));
}

}  // namespace orc
