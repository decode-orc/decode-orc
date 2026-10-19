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
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "vbi_frame_builder.h"
#include "vbi_frame_index.h"
#include "vbi_frame_read_ahead.h"
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
//
// Only readers of whole frames fill it.  Readers of lines are served from the
// placed-line cache below and never build a frame.
constexpr size_t kFrameCacheSize = 32;

// Placed frames — the data lines of a frame without the blanking around them,
// 44 kB on NTSC and 73 kB on PAL — held against the next line asked of the
// same frame.  A decoder reads a dozen or two lines of every frame, all of
// them from the one placement and all on one thread, and each thread keeps
// the frame it is reading in front of this cache (t_last_placement).  So this
// only has to cover frames two readers share — a preview and the observers it
// starts, as the whole-frame cache above does — and not every frame every
// reader has in hand at once, which at 256 entries held 11 MB on NTSC (19 MB
// on PAL) whatever the number of readers.
constexpr size_t kPlacedFrameCacheSize = 32;

// The read-ahead's trailing margin and the distance within which two requests
// count as one reader moving through the capture.  Both cover readers that
// work a block of frames at once and so ask a little out of order: the NABTS
// and teletext sinks slice up to 128 frames at a time (their block scanners'
// largest block), on one thread per core.  A margin narrower than a block lets
// the readers at its far end carry the window past frames the ones at its near
// end have not had yet, and each of those is then read again on its own — on a
// 192-thread machine a margin of 64 read a capture 2.5 to 2.8 times over and
// ran three to six times slower than one of 128, which reads it 1.2 times and
// sends the same packets.
constexpr uint32_t kReadAheadTrailingFrames = 128;
constexpr uint32_t kReadAheadSequentialDistance = 128;

// Most read-ahead decoders a capture runs at once.  They are started only as
// readers are seen waiting on them, so this is a ceiling rather than a
// setting: one FLAC decoder keeps several slicing threads fed, and the
// ceiling lets a pass on every core of a large machine find as many as it
// needs without a handful of readers ever starting more than one.  On all
// 192 threads of a large machine a pass ran as fast on sixteen as on 32, where
// waiting started 14 to 24 over sshfs and held 15 to 30 MB more on three
// captures measured; eight slowed one of them by a twelfth.
constexpr uint32_t kReadAheadMaxDecoders = 16;

// Reading threads one read-ahead decoder is started for.  Readers wait on the
// decoders whenever they work at the front of the window, whether or not
// another decoder would bring their frames any sooner, so waiting alone grew
// the decoders past what a pass needs: on a large machine six slicing threads
// started four to six of them and 24 up to sixteen, each holding a chunk of
// the window and a FLAC decoder of its own (one capture on 24 threads over
// sshfs: 219 MB at the peak with sixteen, 99 MB with eight).  A decoder
// delivers about 1,250 frames a second there, and one for every three slicing
// threads ran every pass measured as fast as waiting alone did, on 6 and 24
// threads, over sshfs and from memory; one for every six slowed a capture on
// 24 threads from 5.5-6.1 s to 6.9-7.5 s.
constexpr uint32_t kReadAheadReadersPerDecoder = 3;

uint32_t read_ahead_decoder_limit() {
  const unsigned hardware = std::thread::hardware_concurrency();
  const uint32_t half = std::max(1u, hardware / 2u);
  return std::min(half, kReadAheadMaxDecoders);
}

// The placement the calling thread's last get_line() pointer points into, kept
// alive until that thread asks for another line.  get_line() hands out a raw
// pointer, and the placement it points into is shared with a cache any other
// thread may evict it from; holding it here is what makes the pointer good for
// as long as the read contract promises, whatever the other threads do.
thread_local std::shared_ptr<const VBIPlacedFrame> t_line_owner;

// The placement the calling thread last read a line of, and whose: a reader
// asks for every line it wants of one frame before it moves on, so this
// answers all but the first of them without touching the shared placement
// cache.  The representation is named by a number never reused, rather than by
// its address, which a later representation can occupy.  Holds one placement
// per reading thread until that thread reads another frame.
struct ThreadPlacement {
  uint64_t representation = 0;
  FrameID frame = 0;
  std::shared_ptr<const VBIPlacedFrame> placed;
};
thread_local ThreadPlacement t_last_placement;

// Numbers for VBIFrameRepresentation instances (t_last_placement).
std::atomic<uint64_t> g_next_representation{1};

// Stored frames calibration samples across the capture.
constexpr uint32_t kCalibrationSampleFrames = 16;

// Stored frames the line-21 caption fit samples.  A captions-only capture has
// two records of each frame to fit on rather than twenty-four, and its
// captions can come and go: a six-hour dump can carry them on one field in
// its last quarter only, which sixteen frames
// sample four times and each field's fit needs eight.  A hundred and
// twenty-eight frames put some thirty records under that field's fit, and two
// hundred and fifty-six line-21 records cost a few milliseconds to correlate.
constexpr uint32_t kCaptionCalibrationSampleFrames = 128;

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

// The preset a new node of a project starts on: on 525 lines the .tbc crop's
// WST one.  The user has to make the choice either way.
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
// A frame is served in two forms.  Its placement — the records' lines, level
// mapped, resampled and clamped, without the blanking around them — is what
// every line read is answered from, so a decoder reading a few lines of every
// frame never has a whole frame built for it — nor, once its first lines are
// read, the lines it does not read resampled (wanted_slots_).  The whole frame
// is rendered from a placement of every line only when a consumer asks for the
// whole frame.
//
// A consumer moving through the capture in order is fed by the read-ahead
// (VBIFrameReadAhead), which decodes the stored frames ahead of it on threads
// and byte sources of its own; everything else — a preview jumping about, the
// first frame of a pass — reads through this object's own byte source.
//
// Thread safety: every const method may be called from any number of threads.
// io_mutex_ covers reads through the byte source (which holds one stream
// position, so they are strictly serial) and nothing else.  Decoding the bytes
// a read produced, placing the records and rendering frames hold no lock: the
// reader's decode, the builder and the index's plan are const and reentrant.
// Holding one lock across all of it made every reader in the process queue
// behind every other, and because std::mutex is not fair, a pool of background
// observation workers could starve the interactive preview render.  The two
// caches lock themselves, for a lookup or an insertion and never longer.
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
    // Opens the capture again, for the read-ahead's decoders.  Empty leaves
    // every read to this object's own byte source.
    VBIByteSourceOpener open_byte_source;
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

  // Line access is answered from the frame's placement rather than from a
  // built frame.  The frame lattice is frame_line_util.h's, and a placement
  // holds each placed line exactly as the built frame holds it, so a line read
  // here is the line get_frame() would show, sample for sample; a line no
  // record lands on is blanking in every frame.  The frame's placement is
  // resolved even then, so that a frame which cannot be read is reported on a
  // line read as it is on a frame read rather than shown as blanking.
  const sample_type* get_line(FrameID id, size_t line) const override {
    if (!has_frame(id) || line >= lines_per_frame()) return nullptr;
    std::shared_ptr<const VBIPlacedFrame> placed = placed_frame(id);
    const int32_t slot = builder_.slot_of_frame_line(line);
    if (slot == VBIFrameBuilder::kNoSlot) {
      return blank_line_.data();
    }
    if (placed->slot_resolved[static_cast<size_t>(slot)] == 0) {
      placed = placed_with_slot(id, slot);
    }
    const sample_type* samples = placed->line(static_cast<size_t>(slot));
    t_line_owner = std::move(placed);
    return samples;
  }

  // What the read contract's default returns — the line's first
  // frame_width_nominal samples — without building the frame it reads them
  // from.
  std::vector<sample_type> get_line_samples(FrameID id,
                                            size_t line) const override {
    if (!has_frame(id) || line >= lines_per_frame()) return {};
    const size_t width = builder_.output_frame().samples_per_line_nominal;
    std::shared_ptr<const VBIPlacedFrame> placed = placed_frame(id);
    const int32_t slot = builder_.slot_of_frame_line(line);
    if (slot == VBIFrameBuilder::kNoSlot) {
      return std::vector<sample_type>(
          blank_line_.begin(),
          blank_line_.begin() + static_cast<std::ptrdiff_t>(width));
    }
    if (placed->slot_resolved[static_cast<size_t>(slot)] == 0) {
      placed = placed_with_slot(id, slot);
    }
    const sample_type* samples = placed->line(static_cast<size_t>(slot));
    return std::vector<sample_type>(samples, samples + width);
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
    wanted_slots_.assign(builder_.slot_count(), 0);

    // What a padded frame is placed as, and what every line no record lands
    // on reads as: blanking, shared by every frame that needs it.  The line is
    // as long as the longest line of the frame, so a caller reading a line to
    // its full length (frame_line_util.h) stays inside it.
    auto blank = std::make_shared<VBIPlacedFrame>();
    builder_.place_blank_frame(*blank);
    blank_placed_ = std::move(blank);
    const VBIOutputFrame& output = builder_.output_frame();
    size_t longest_line = output.samples_per_line_nominal;
    for (uint32_t line = 0; line < output.lines_per_frame; ++line) {
      longest_line = std::max(longest_line, output.line_length(line));
    }
    blank_line_.assign(longest_line,
                       static_cast<sample_type>(output.levels.blanking));

    if (setup_.open_byte_source) {
      VBIReadAheadConfig read_ahead;
      read_ahead.frame_bytes = setup_.format.bytes_per_frame();
      read_ahead.frame_count = *stored_frames;
      read_ahead.chunk_frames =
          vbi_read_ahead_chunk_frames(read_ahead.frame_bytes);
      read_ahead.trailing_frames = kReadAheadTrailingFrames;
      read_ahead.max_decoders = read_ahead_decoder_limit();
      read_ahead.readers_per_decoder = kReadAheadReadersPerDecoder;
      read_ahead.sequential_distance = kReadAheadSequentialDistance;
      read_ahead_ = std::make_unique<VBIFrameReadAhead>(
          read_ahead, setup_.open_byte_source);
    }

    build_video_parameters();
    return true;
  }

  uint32_t lines_per_frame() const {
    return builder_.output_frame().lines_per_frame;
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
    std::vector<sample_type> built;
    builder_.render_frame(*placed_frame(id, /*whole=*/true), built);

    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (!frame_cache_.contains(id)) {
      frame_cache_.put(id, std::move(built));
    }
    return frame_cache_.get_ptr(id);
  }

  // The placement of output frame |id|: the one the calling thread read last,
  // from the cache, or placed now — with every slot resolved when |whole|, as
  // a whole frame needs them, and otherwise with at least the slots line reads
  // have asked for (wanted_slots_).  Caller holds no lock.  Two threads racing
  // the same frame place it twice and the loser's copy is dropped, as for
  // whole frames.
  std::shared_ptr<const VBIPlacedFrame> placed_frame(FrameID id,
                                                     bool whole = false) const {
    ThreadPlacement& last = t_last_placement;
    if (last.representation == instance_ && last.frame == id &&
        last.placed != nullptr && (!whole || last.placed->complete())) {
      return last.placed;
    }
    std::shared_ptr<const VBIPlacedFrame> placed;
    std::optional<std::shared_ptr<const VBIPlacedFrame>> cached =
        placed_cache_.get(id);
    if (cached.has_value() && (!whole || (*cached)->complete())) {
      placed = std::move(*cached);
    } else if (whole) {
      placed = place(id, nullptr, /*again=*/cached.has_value());
      placed_cache_.put(id, placed);
    } else {
      const std::vector<uint8_t> wanted = wanted_slots();
      placed = place(id, &wanted);
      placed_cache_.put_if_absent(id, placed);
    }
    last = ThreadPlacement{instance_, id, placed};
    return placed;
  }

  // The record slots line reads have asked for so far.
  std::vector<uint8_t> wanted_slots() const {
    std::lock_guard<std::mutex> lock(wanted_mutex_);
    return wanted_slots_;
  }

  // Frame |id| placed again, now with slot |slot| among those it resolves —
  // which every frame placed from now on resolves too.  Caller holds no lock.
  std::shared_ptr<const VBIPlacedFrame> placed_with_slot(FrameID id,
                                                         int32_t slot) const {
    std::vector<uint8_t> wanted;
    {
      std::lock_guard<std::mutex> lock(wanted_mutex_);
      wanted_slots_[static_cast<size_t>(slot)] = 1;
      wanted = wanted_slots_;
    }
    std::shared_ptr<const VBIPlacedFrame> placed =
        place(id, &wanted, /*again=*/true);
    placed_cache_.put(id, placed);
    t_last_placement = ThreadPlacement{instance_, id, placed};
    return placed;
  }

  // Frame |id| placed with the slots |wanted| marks, or with every slot when
  // it is null.  |again| is for a frame placed before, whose second request
  // the read-ahead must not see: it would take it for a reader moving through
  // the capture, and start a decoder for a frame it already gave out.
  //
  // Caller holds no lock: this takes io_mutex_ for what reads through the
  // capture's own byte source and holds nothing for the rest.  Throws when the
  // frame cannot be placed: a frame that cannot be placed is a broken
  // configuration or a broken capture, and returning blanking for it would
  // hide both.
  std::shared_ptr<const VBIPlacedFrame> place(
      FrameID id, const std::vector<uint8_t>* wanted,
      bool again = false) const {
    std::string error;
    const auto fail = [this, &error]() {
      return std::runtime_error("VBI source '" + setup_.input_path +
                                "': " + error);
    };

    VBIOutputFramePlan plan;
    if (index_.plans_without_reading()) {
      if (!index_.frame_plan(static_cast<uint64_t>(id), plan, error)) {
        throw fail();
      }
    } else {
      // The index resolves a plan by reading frame counters through the
      // capture's byte source, so it shares that source's lock.
      std::lock_guard<std::mutex> lock(io_mutex_);
      if (!index_.frame_plan(static_cast<uint64_t>(id), plan, error)) {
        throw fail();
      }
    }
    if (plan.padding) {
      return blank_placed_;
    }

    // A reader moving through the capture in order is fed by the read-ahead;
    // anything it declines is read here, through the capture's own byte
    // source, which is the only part of placing a frame that is serial.
    VBIFrameReadAhead::FrameBytes ahead;
    if (read_ahead_ != nullptr && !again) {
      ahead = read_ahead_->fetch(plan.source_frame_index);
    }
    std::vector<uint8_t> read_here;
    if (ahead == nullptr) {
      std::lock_guard<std::mutex> lock(io_mutex_);
      if (!reader_->read_frame_bytes(plan.source_frame_index, read_here,
                                     error)) {
        throw fail();
      }
    }
    const std::vector<uint8_t>& frame_bytes =
        ahead != nullptr ? *ahead : read_here;

    VBIFrameRecords records;
    if (!reader_->decode_frame(plan.source_frame_index, frame_bytes, records,
                               error)) {
      throw fail();
    }
    auto placed = std::make_shared<VBIPlacedFrame>();
    if (!(wanted != nullptr
              ? builder_.place_frame(records.lines, *wanted, *placed, error)
              : builder_.place_frame(records.lines, *placed, error))) {
      throw fail();
    }
    return placed;
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

  // A padded frame's placement, and a line of blanking as long as the
  // frame's longest line.
  std::shared_ptr<const VBIPlacedFrame> blank_placed_;
  std::vector<sample_type> blank_line_;

  // The capture's own byte source: one stream position, so every read
  // through it is serial.
  mutable std::mutex io_mutex_;

  // The whole-frame cache, held only across a lookup or an insertion — never
  // across a build, and never across a read of the capture.
  mutable std::mutex cache_mutex_;
  mutable LRUCache<FrameID, std::vector<sample_type>> frame_cache_{
      kFrameCacheSize};

  // The placement cache, which locks itself for a lookup or an insertion.
  mutable LRUCache<FrameID, std::shared_ptr<const VBIPlacedFrame>>
      placed_cache_{kPlacedFrameCacheSize};

  // The record slots line reads have asked for, which every frame placed is
  // placed with: a decoder of line 21 has two records of a frame resampled
  // rather than all of them, and one reading every line has every frame
  // placed whole once its first has been read.  The frame's levels are
  // measured on all its records either way, so a line reads the same whatever
  // else was placed with it (VBIFrameBuilder::place_frame()).  It only grows,
  // a slot joining it the first time a line read finds it unresolved
  // (placed_with_slot()).  Guarded by wanted_mutex_.
  mutable std::mutex wanted_mutex_;
  mutable std::vector<uint8_t> wanted_slots_;

  // This representation's number, for t_last_placement.
  const uint64_t instance_ = g_next_representation.fetch_add(1);

  // Declared last so that it is destroyed first: its decoders are joined
  // before anything they could be reading on behalf of goes away.
  std::unique_ptr<VBIFrameReadAhead> read_ahead_;
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

// What a card capture's calibration settled.
struct CardCalibration {
  // Time from 0H to sample 0 of every record, in source samples.
  double capture_offset_samples = 0.0;

  // The source levels of the service's logic 0 and logic 1, measured on the
  // capture's data lines; empty when none gave a measurement.
  std::optional<VBILineLevels> data_levels;

  // Set when the offset was fitted on the line-21 captions because the
  // capture carries no teletext: the caption service, whose region is then
  // the one written.
  std::optional<VBITeletextService> written_service;
};

// Report an accepted fit to the log and the observation context.
void report_calibration(const VBIOffsetCalibration& calibration,
                        ObservationContext& observation_context) {
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
}

// Fit the capture offset from the clock run-in, and measure the levels the
// capture's data lines carry their data at.
//
// The stage does not proceed with a bad fit: the offset is applied globally to
// a capture that may run for hours, so a wrong one silently mis-places every
// line of it (design §5.3.4).  The one exception is a 525-line capture that
// carries no teletext at all — a captions-only dump — which no teletext fit can
// tell from a wrong configuration: the line-21 caption run-in is fitted
// instead, and the capture opens only if that fit passes the same checks.
CardCalibration calibrate_card_capture(
    const VBILineReader& reader, const VBITeletextService& service,
    const std::string& input_path, const VBILevelMapperConfig& level_config,
    ObservationContext& observation_context) {
  VBICalibrationConfig calibration_config;
  calibration_config.sample_frames = kCalibrationSampleFrames;
  calibration_config.levels = level_config;

  VBIOffsetCalibration calibration;
  std::string error;
  if (!calibrate_vbi_capture_offset(reader, service, calibration_config,
                                    calibration, error)) {
    throw UserDataError("The capture offset of '" + input_path +
                        "' could not be measured: " + error);
  }

  CardCalibration result;
  if (calibration.converged) {
    report_calibration(calibration, observation_context);
    result.capture_offset_samples = calibration.capture_offset_samples;
    if (calibration.data_levels.usable) {
      result.data_levels = calibration.data_levels;
      observation_context.set(FieldID(0), kObservationNamespace, "data_logic0",
                              calibration.data_levels.logic0);
      observation_context.set(FieldID(0), kObservationNamespace, "data_logic1",
                              calibration.data_levels.logic1);
      ORC_LOG_INFO(
          "{}: the data lines carry logic 0 at {:.1f} and logic 1 at {:.1f} "
          "counts (median over the {} records the run-in was found on); each "
          "line's levels are read from its own payload, and a frame with no "
          "data line is mapped at these.",
          kStageName, calibration.data_levels.logic0,
          calibration.data_levels.logic1, calibration.records_accepted);
    }
    return result;
  }

  const std::string refusal =
      "The capture offset of '" + input_path +
      "' could not be trusted: " + join_messages(calibration.diagnostics) +
      " " + calibration.summary;

  // Only a 525-line capture can carry line-21 captions.
  const VBISourceFormat& format = reader.format();
  VBITeletextService captions;
  std::string caption_error;
  if (!vbi_caption_service(format.tv_system, captions, caption_error)) {
    throw UserDataError(refusal);
  }

  // A capture that carries the other 525-line teletext service is a wrong
  // choice of format, not a captions-only capture, and the wrong choice is
  // meant to be loud: it is refused, naming the service it does carry, rather
  // than opened on its captions with nothing for the teletext decoder.
  const VBITeletextSystem other_system =
      (format.tt_system == VBITeletextSystem::kNABTS)
          ? VBITeletextSystem::kWST
          : VBITeletextSystem::kNABTS;
  VBITeletextService other_service;
  VBIOffsetCalibration other_calibration;
  if (vbi_teletext_service(format.tv_system, other_system, other_service,
                           error) &&
      calibrate_vbi_capture_offset(reader, other_service, calibration_config,
                                   other_calibration, error) &&
      other_calibration.converged) {
    throw UserDataError(refusal + " Its clock run-in and framing code are " +
                        teletext_system_name(other_system) + "'s: choose the " +
                        teletext_system_name(other_system) +
                        " entry of this card's formats.");
  }

  VBICalibrationConfig caption_config = calibration_config;
  caption_config.sample_frames = kCaptionCalibrationSampleFrames;
  VBIOffsetCalibration caption_calibration;
  if (!calibrate_vbi_capture_offset_on_captions(
          reader, caption_config, caption_calibration, caption_error) ||
      !caption_calibration.converged) {
    const std::string caption_reason =
        caption_error.empty() ? join_messages(caption_calibration.diagnostics) +
                                    " " + caption_calibration.summary
                              : caption_error;
    throw UserDataError(refusal +
                        " Nor could it be fitted on the line-21 caption "
                        "run-in, which opens a capture that carries no "
                        "teletext: " +
                        caption_reason);
  }

  ORC_LOG_WARN(
      "{}: '{}' carries no clock run-in of the configured service that a fit "
      "can trust ({}). It does carry line-21 captions, so its capture offset "
      "is fitted on their run-in instead and every record is written over the "
      "caption line; expect little or no teletext from it.",
      kStageName, input_path, calibration.summary);
  report_calibration(caption_calibration, observation_context);
  result.capture_offset_samples = caption_calibration.capture_offset_samples;
  result.written_service = captions;

  VBIOutputFrame output_frame;
  if (caption_calibration.data_levels.usable &&
      make_vbi_output_frame(format.tv_system, format.tt_system, output_frame,
                            error)) {
    const VBILineLevels levels = vbi_levels_from_caption_levels(
        caption_calibration.data_levels, output_frame.levels);
    if (levels.usable) {
      result.data_levels = levels;
      ORC_LOG_INFO(
          "{}: the caption data swings from {:.1f} counts at blanking to "
          "{:.1f} at 50 IRE; a line with no level reference of its own is "
          "mapped on that scale.",
          kStageName, caption_calibration.data_levels.logic0,
          caption_calibration.data_levels.logic1);
    }
  }
  return result;
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
  hints.record_samples = byte_source->declared_record_samples();
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
  // A length is unknown only for a FLAC whose header leaves out its total
  // sample count; a known length under one frame has already been refused by
  // validation, but is refused here too rather than trusted to it.
  const std::optional<uint64_t> stored_frames = reader.frame_count();
  if (!stored_frames.has_value()) {
    throw UserDataError("VBI capture '" + configuration.input_path +
                        "' does not say how long it is (a FLAC stream with no "
                        "total sample count in its header), so the number of "
                        "frames it holds is unknown.");
  }
  if (*stored_frames == 0) {
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
  VBILevelMapperConfig level_config;
  if (format.capture_offset_is_auto) {
    const CardCalibration calibration =
        calibrate_card_capture(reader, service, configuration.input_path,
                               level_config, observation_context);
    timing.capture_offset_samples = calibration.capture_offset_samples;
    timing.written_service = calibration.written_service;
    level_config.capture_levels = calibration.data_levels;
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
  setup.levels = level_config;
  setup.sequence.drops = parse_drop_policy(configuration.drops);
  setup.timing = timing;
  setup.input_path = configuration.input_path;
  // The read-ahead's decoders each open the capture for themselves, through
  // the same dependency the stage opened it with.
  setup.open_byte_source =
      [deps = deps_, path = configuration.input_path](std::string& open_error) {
        return deps->open_byte_source(path, open_error);
      };

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
        "ignored. So are .tbc crops kept as the pictures of a lossless video "
        "(a vbi.mkv in FFV1), whose rows are read back into the crop's 16-bit "
        "records. Nothing about a capture is read from its name or its "
        "extension — the container is entirely the Capture Format you pick — "
        "so a capture whose extension is not one of the usual ones can still "
        "be selected through the dialog's All Files filter.";
    pd.type = ParameterType::FILE_PATH;
    pd.constraints.required = false;
    pd.constraints.default_value = std::string("");

    // The extensions these captures actually arrive under. .vbi is a card
    // dump, .flac the same thing losslessly wrapped, .u8 and .u16 the raw
    // word-size spellings a decoder's VBI-only export uses, .tbc the crop
    // taken off a decoded capture and .mkv that crop kept as a video.  It is
    // only a filter on the browse dialog: the stage reads the container the
    // format says it is, whatever the file is called, and the dialog keeps an
    // All Files entry for anything else.
    pd.file_extension_hint = ".vbi|.flac|.u8|.u16|.tbc|.mkv";
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
        "framing code and packet length. A crop kept as a lossless video "
        "(vbi.mkv: FFV1, a picture row to a record, two fields to a 910 x 32 "
        "picture) is the same capture and is read with the same two entries.";
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
