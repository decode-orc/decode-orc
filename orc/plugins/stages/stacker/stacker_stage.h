/*
 * File:        stacker_stage.h
 * Module:      orc-core
 * Purpose:     Multi-source CVBS_U10_4FSC frame stacking stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_content.h>
#include <orc/support/lru_cache.h>
#include <orc/support/stack_measurement.h>

#include <array>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace orc {

// Forward declaration
class StackerStage;

// The keys some thread is building now, so that another thread wanting one
// waits for that build instead of repeating it. Readers running ahead of a
// serial loop would otherwise stack the frame the loop has just reached, and
// build their neighbours' still-frame signatures, a second time.
class BuildsInProgress {
 public:
  // True when the caller is now building |key| and must release() it; false
  // when another thread already is.
  bool try_claim(FrameID key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return building_.insert(key).second;
  }

  void release(FrameID key) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      building_.erase(key);
    }
    done_.notify_all();
  }

  // Returns once no thread is building |key|.
  void wait(FrameID key) {
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&] { return building_.count(key) == 0; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable done_;
  std::set<FrameID> building_;
};

// ============================================================================
// StackedVideoFrameRepresentation
// ============================================================================
// Wraps multiple source VFrameRs and stacks corresponding frames on demand.
//
// Frame alignment: inputs arrive already frame-aligned — frame_map normalises
// each source onto the disc's frame numbering and source_align applies any
// residual offset, both driven by VBI frame numbers — so the same frame id is
// read from every source.  See collect_source_frame_ids() for why this stage
// deliberately does not re-align on colour_frame_index.
//
// Sample-grid alignment: independent decodes of the same disc need not place
// a line start on the same 4FSC sample, and the difference drifts across a
// side.  sample_offsets() measures each source's grid offset against the first
// source, per segment of kSampleAlignSegmentFrames frames, so the samples can
// be brought onto a common grid before they are combined.
//
// Field agreement: before combining, each input's fields are compared with
// the median of the inputs, and an input whose field carries a different
// picture is left out of that field (see StackerStage::check_field_agreement).
//
// Input noise: for the Weighted mode, noise_model() measures each source's
// noise per segment, from the variance of its differences with the other
// sources once their grids are aligned, and groups the sources by pressing.
//
// Still-frame copies: with still_frame_stacking on, the repeats of a held
// still within the same source are found per output frame (still_copies())
// and stacked as extra inputs on the active picture lines.
//
// Padding frames (FrameDescriptor::is_padding_frame == true) are silently
// skipped and contribute no sample data to the stack.
//
// Thread safety: all const methods are safe for concurrent access; internal
// caches are protected by cache_mutex_.
class StackedVideoFrameRepresentation : public VideoFrameRepresentationWrapper,
                                        public Artifact {
 public:
  StackedVideoFrameRepresentation(
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      StackerStage* stage);

  ~StackedVideoFrameRepresentation() override = default;

  std::string type_name() const override {
    return "stacked_video_frame_representation";
  }

  // Navigation — the union of the sources' ranges; descriptors come from the
  // first source with a real (non-padding) frame at that id. With
  // still_frame_stacking on, a real frame's descriptor also says how far its
  // picture is held, from the furthest copy either side that matched in both
  // fields (held_frames_before/after); that runs the frame's copy search, so
  // the first descriptor of a frame costs what stacking it would.
  FrameIDRange frame_range() const override;
  size_t frame_count() const override;
  bool has_frame(FrameID id) const override;
  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override;

  // Flat access — stacks on demand, caches result
  const sample_type* get_frame(FrameID id) const override;
  const sample_type* get_line(FrameID id, size_t line) const override;
  std::vector<sample_type> get_frame_copy(FrameID id) const override;

  // YC
  bool has_separate_channels() const override;
  const sample_type* get_frame_luma(FrameID id) const override;
  const sample_type* get_frame_chroma(FrameID id) const override;
  const sample_type* get_line_luma(FrameID id, size_t line) const override;
  const sample_type* get_line_chroma(FrameID id, size_t line) const override;

  // Hints — after stacking only residual dropouts remain
  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override;

  // Audio — channel-pair count and descriptors come from the reference
  // source (the first source carrying audio). Inputs are stacked at the same
  // frame ID across sources, so per-frame pair counts always agree. Every
  // channel pair present in ALL inputs is stacked per the audio_stacking
  // parameter; pairs not common to all inputs pass through from the
  // per-frame best source.
  size_t audio_channel_pair_count() const override;
  std::optional<AudioChannelPairDescriptor> get_audio_channel_pair_descriptor(
      size_t pair) const override;
  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override;

  // EFM
  bool has_efm() const override;
  uint32_t get_efm_sample_count(FrameID id) const override;
  std::vector<uint8_t> get_efm_samples(FrameID id) const override;

  // AC3 RF — not stacked: each frame's symbols pass through from the
  // per-frame best source that carries AC3 RF there, or the first that does.
  bool has_ac3_rf() const override;
  uint32_t get_ac3_symbol_count(FrameID id) const override;
  std::vector<uint8_t> get_ac3_symbols(FrameID id) const override;

  // Resolve which source frame ID corresponds to the reference (first) frame
  // id.  Returns UINT64_MAX when the source has no usable frame there.
  FrameID resolve_source_frame(size_t src_idx, FrameID ref_id) const;

  // Frames per sample-grid measurement segment.
  static constexpr size_t kSampleAlignSegmentFrames = 500;

  // Per-source sample-grid offset against sources_[0] at frame |id|, in 4FSC
  // samples: reference sample n corresponds to source sample n + offset.
  // Measured once per segment of kSampleAlignSegmentFrames frames, on first
  // use, and cached.  In a segment where sources_[0] has no frame to measure
  // against, the sources are measured against a stand-in, the first other
  // source that has one, and the stand-in's own offset from sources_[0] in
  // the nearest segment where both were measured is added, so the output
  // stays on sources_[0]'s grid.  A segment that cannot be measured holds the
  // offset of the nearest one that could; zero when alignment is disabled,
  // when there is a single source, or when nothing nearby could be measured.
  std::vector<int32_t> sample_offsets(FrameID id) const;

  // One source's noise against the picture common to every source: its
  // standard deviation in 10-bit codes, its stacking weight (relative to the
  // other sources'), and the pressing group it was placed in.  Sources of one
  // pressing share the noise recorded into its stamper, and share a group.
  using SourceNoise = PressingNoise;

  // Per-source noise at frame |id|.  Measured once per segment on first use
  // and cached; a segment that cannot be measured holds the nearest one that
  // could, a source never measured nearby takes the typical figures of the
  // others, and with nothing measured every source gets a nominal σ and equal
  // weight.
  std::vector<SourceNoise> noise_model(FrameID id) const;

  // Count of sources that have a usable frame for ref_id
  size_t get_source_count(FrameID ref_id) const;

  // A repeat of the picture of an output frame, found in one source: a frame
  // of that source whose picture matches the source's own frame at the output
  // frame's id, at the same subcarrier phase, and the fields it matches in.
  struct StillCopy {
    size_t source = 0;
    FrameID id = 0;
    std::array<bool, 2> field{false, false};
  };

  // The copies stacked into frame |id|: empty unless still_frame_stacking is
  // on.  Each source's frames at colour-cadence offsets within
  // still_frame_search of |id| are tried, against the signatures first and
  // then sample by sample, and a field takes copies from a source only when
  // it has at least kMinStillCopies of them.  Decided from
  // the two frames and the source's reference noise alone, so the result
  // does not depend on which frames were stacked before.
  std::vector<StillCopy> still_copies(FrameID id) const;

  // Copies a field needs from one source before any of them is used: with
  // one, the combine cannot tell a false match from the frame's own picture.
  static constexpr size_t kMinStillCopies = 2;

  // Reference noise of source |source| at frame |id|, in codes, as
  // same_picture takes it: BlockNoiseMeter over the matching pairs among a
  // spread of the segment's frames, found first against the within-frame
  // anchor.  Measured once per segment and cached; a segment with too few
  // matching pairs holds the nearest one that has them, and nullopt (no
  // copies) when none nearby does.
  std::optional<double> still_reference_noise(size_t source, FrameID id) const;

  friend class StackerStage;

 private:
  std::vector<std::shared_ptr<const VideoFrameRepresentation>> sources_;
  StackerStage* stage_;

  // LRU caches for stacked frames — composite and YC paths
  static constexpr size_t kMaxCachedFrames = 300;
  mutable LRUCache<FrameID, std::vector<sample_type>> stacked_frames_;
  mutable LRUCache<FrameID, std::vector<sample_type>> stacked_luma_;
  mutable LRUCache<FrameID, std::vector<sample_type>> stacked_chroma_;
  mutable LRUCache<FrameID, std::vector<DropoutRun>> stacked_dropouts_;
  mutable LRUCache<FrameID, std::vector<int32_t>> stacked_audio_;
  mutable LRUCache<FrameID, std::vector<uint8_t>> stacked_efm_;
  mutable LRUCache<FrameID, size_t> best_source_cache_;

  mutable std::mutex cache_mutex_;
  mutable BuildsInProgress frames_building_;
  mutable BuildsInProgress yc_frames_building_;

  // Copy of the first source's frame at |id|, for frames where every source
  // is padding.
  std::vector<sample_type> padding_frame_copy(FrameID id) const;

  // Stack frame |id| without touching the caches.
  void stack_composite(FrameID id, std::vector<sample_type>& samples,
                       std::vector<DropoutRun>& dropouts) const;
  void stack_yc(FrameID id, std::vector<sample_type>& luma,
                std::vector<sample_type>& chroma,
                std::vector<DropoutRun>& dropouts) const;

  // The cached stacked frame (or luma and chroma) at |id|, stacked and
  // cached first if missing. The frame is stacked outside cache_mutex_, so
  // concurrent readers of different frames stack in parallel.
  const std::vector<sample_type>* stacked_frame(FrameID id) const;
  std::pair<const std::vector<sample_type>*, const std::vector<sample_type>*>
  stacked_frame_yc(FrameID id) const;

  // Find the source index with the fewest dropout samples for this frame
  size_t get_best_source_index(FrameID id) const;

  // The source frame |id|'s AC3 RF passes through from; SIZE_MAX when no
  // source carries AC3 RF there.
  size_t ac3_source_index(FrameID id) const;

  // Build the per-source frame ID vector for stacking
  std::vector<FrameID> collect_source_frame_ids(FrameID ref_id) const;

  // Each source's measured offset in |segment|, nullopt where it could not
  // be measured; measures the segment on first use (must hold align_mutex_).
  const std::vector<std::optional<int32_t>>& segment_offsets(
      size_t segment) const;
  std::vector<std::optional<int32_t>> measure_segment(size_t segment) const;

  // The probe frames of |segment| that source |source| can be measured on.
  std::vector<FrameID> grid_probes(size_t segment, size_t source) const;

  // Each source's offset in |segment| from source |reference|, which must
  // have probe frames there, plus |shift|, the reference's own offset from
  // sources_[0]; nullopt where it could not be measured.
  std::vector<std::optional<int32_t>> measure_segment_against(
      size_t segment, size_t reference, int32_t shift) const;

  // Source |source|'s offset from sources_[0] in the segment nearest
  // |segment| where both were measured, earlier first on a tie; nullopt when
  // no segment links them.
  std::optional<int32_t> linked_offset(size_t segment, size_t source) const;

  mutable std::mutex align_mutex_;
  mutable std::map<size_t, std::vector<std::optional<int32_t>>>
      segment_offsets_;
  // The offset last reported per source, so a log line marks each change.
  mutable std::vector<int32_t> last_logged_offsets_;
  // The stand-in and its offset last reported, so a stretch of segments
  // measured against one stand-in is logged once: 0 while sources_[0] is
  // measured against itself, SIZE_MAX for a stretch with no stand-in.
  mutable size_t last_logged_stand_in_ = 0;
  mutable int32_t last_logged_stand_in_offset_ = 0;

  // noise_model(id) when the stage stacks in Weighted mode, which is the only
  // mode that uses it; empty otherwise.
  std::vector<SourceNoise> weighting_noise(FrameID id) const;

  // The segment holding |id|, and the number of segments; zero segments
  // when the stack is empty.
  std::pair<size_t, size_t> segment_of(FrameID id) const;

  // Each source's measured noise in |segment|, nullopt where it could not be
  // measured; measures the segment on first use (must hold noise_mutex_).
  const std::vector<std::optional<SourceNoise>>& segment_noise(
      size_t segment) const;
  std::vector<std::optional<SourceNoise>> measure_segment_noise(
      size_t segment) const;

  // Adds frame |id| of every source that has it, shifted onto the reference
  // grid, to |meter|; false when fewer than two sources could be added.
  bool add_noise_probe(PairNoiseMeter& meter, FrameID id) const;

  // Each source's pressing group and family (group_by_pressing(),
  // group_into_families()), decided once from frames spread across the whole
  // stack; measured on first use (must hold noise_mutex_).
  struct Pressings {
    std::vector<std::optional<size_t>> groups;
    std::vector<std::optional<size_t>> families;
  };
  const Pressings& pressings() const;
  Pressings measure_pressings() const;

  mutable std::mutex noise_mutex_;
  mutable std::optional<Pressings> pressings_;
  mutable std::map<size_t, std::vector<std::optional<SourceNoise>>>
      segment_noise_;
  // The noise last reported per source, so a log line marks a real change.
  mutable std::vector<double> last_logged_noise_;
  mutable bool last_fit_trusted_ = true;

  // The reference source for audio metadata: the first source carrying audio
  // (nullptr when none does).
  std::shared_ptr<const VideoFrameRepresentation> reference_audio_source()
      const;

  // True when channel pair |pair| exists in every source — the precondition
  // for combining it across inputs.
  bool pair_in_all_sources(size_t pair) const;

  // What the still-frame matcher keeps of a source frame.
  struct FrameSignature {
    std::array<FieldSignature, 2> fields;
    ChromaSignature chroma;
    uint64_t hash = 0;
  };

  // The samples a signature is built from, fetched from its source.
  struct SignatureInput {
    std::vector<sample_type> plane;
    std::vector<sample_type> chroma;  // YC sources only
    std::vector<DropoutRun> dropouts;
    bool yc = false;
    VideoSystem system = VideoSystem::PAL;
    size_t width = 0;
    size_t height = 0;
  };

  // The signatures of the (source, frame) pairs in |wanted|, by
  // signature_key, building those not cached yet and, when |keep|, keeping
  // them in an LRU; nullptr where the source has no usable frame.  Frames are
  // fetched one at a time and the signatures built on worker threads, so a
  // batch costs about one signature's time a thread.
  using SignatureMap = std::map<FrameID, std::shared_ptr<const FrameSignature>>;
  SignatureMap prefetch_signatures(
      const std::vector<std::pair<size_t, FrameID>>& wanted,
      bool keep = true) const;

  std::optional<SignatureInput> signature_input(size_t source,
                                                FrameID id) const;
  static std::shared_ptr<const FrameSignature> build_signature(
      const SignatureInput& input);
  static FrameID signature_key(size_t source, FrameID id) {
    return id * kMaxSources + source;
  }

  // The plane signatures and duplicate reads are taken from: composite, or
  // luma for a YC source.  Empty when the frame is unusable.
  std::vector<sample_type> picture_plane(size_t source, FrameID id) const;

  std::vector<StillCopy> find_still_copies(FrameID id) const;

  // Drops each field of |copies| (all of source |source|) whose samples do
  // not show the picture of frame |id| (compare_field_samples), the fields
  // compared on worker threads.
  void check_still_copy_samples(size_t source, FrameID id,
                                std::vector<StillCopy>& copies) const;

  // Threads the still-frame matcher builds and compares on: the stage's
  // thread count, or the hardware's when that is 0.
  size_t still_worker_threads() const;

  // Each source's measured reference noise in |segment|; measures the
  // segment on first use (must hold still_noise_mutex_).
  const std::vector<std::optional<double>>& segment_still_noise(
      size_t segment) const;
  std::vector<std::optional<double>> measure_segment_still_noise(
      size_t segment) const;

  // Signatures are keyed by frame id times kMaxSources plus source.
  static constexpr size_t kMaxSources = 16;
  mutable LRUCache<FrameID, std::shared_ptr<const FrameSignature>> signatures_;
  mutable BuildsInProgress signatures_building_;
  // Sample checks of pairs of frames of one source, per field: 1 the same,
  // 0 different, -1 not checked.  Keyed by the first frame's signature_key
  // times kMaxPairOffsets plus the distance to the second.
  static constexpr FrameID kMaxPairOffsets = 128;
  mutable LRUCache<FrameID, std::array<int8_t, 2>> sample_checks_;
  mutable LRUCache<FrameID, std::vector<StillCopy>> still_copies_;

  mutable std::mutex still_noise_mutex_;
  mutable std::map<size_t, std::vector<std::optional<double>>>
      segment_still_noise_;
  // The reference noise last reported per source, so a log line marks a
  // real change.
  mutable std::vector<double> last_logged_still_noise_;

  // stacked_audio_ is keyed per (frame, pair); kMaxAudioChannelPairs = 8
  // leaves the low 3 bits for the pair index.
  static FrameID audio_cache_key(FrameID id, size_t pair) {
    return (id << 3) | static_cast<FrameID>(pair);
  }
};

// ============================================================================
// StackerStage
// ============================================================================
class StackerStage : public DAGStage,
                     public ParameterizedStage,
                     public IStagePreviewCapability {
 public:
  StackerStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::MERGER,
                        "stacker",
                        "Stacker",
                        "Combine multiple VFrameR sources by stacking frames "
                        "for superior output quality (1 input = passthrough "
                        "unless still-frame stacking is on)",
                        1,
                        16,
                        1,
                        UINT32_MAX,
                        VideoFormatCompatibility::ALL};
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  std::shared_ptr<const VideoFrameRepresentation> process(
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources) const;

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  enum class StackingMode { MEAN, MEDIAN, WEIGHTED };
  enum class AudioStackingMode { DISABLED, MEAN, MEDIAN };
  enum class EFMStackingMode { DISABLED, MEAN, MEDIAN, CONFIDENCE };

  using sample_type = VideoFrameRepresentation::sample_type;

  friend class StackedVideoFrameRepresentation;

  // ── Chroma gain matching ──
  //
  // The decode's high-frequency (MTF) correction can differ between captures
  // of one disc, over stretches of a side, lifting one capture's subcarrier
  // band, burst and chroma alike, by up to about a fifth while its luma still
  // matches the others'.  Combined as it is, that chroma does not line up
  // with the other inputs'.  The burst sets the chroma level, as a
  // receiver's ACC takes it, so each input's subcarrier band is brought to
  // the inputs' median burst before the inputs are combined.

  // The colour burst's amplitude in |plane| (composite, or a YC source's
  // chroma), in codes: the trimmed mean over the active lines whose burst
  // window holds a clean subcarrier and no flagged sample; nullopt when too
  // few lines do.
  static std::optional<double> burst_amplitude(
      const std::vector<sample_type>& plane,
      const std::vector<DropoutRun>& dropouts, VideoSystem system,
      size_t nominal_width, size_t height);

  // The median of the measured |amplitudes|; nullopt when none was measured.
  static std::optional<double> burst_reference(
      const std::vector<std::optional<double>>& amplitudes);

  // The gain bringing an input whose burst measures |amplitude| to
  // |reference|: 1 when either is missing, when they are within the
  // measurement's own scatter of each other, or when the gain is too large to
  // be a correction.
  static double chroma_gain(std::optional<double> amplitude,
                            std::optional<double> reference);

  // Scale the subcarrier band of |plane| by |gain|: s + (gain - 1)·hp(s),
  // where hp's response is (1 - cos ωT)², 1 at fsc and 0 at DC.  That is the
  // shape of the mismatch measured between real captures, which carries on
  // rising above fsc, so luma below about 3 MHz is left as it was.  A gain of
  // 1 leaves the plane untouched.
  static void apply_chroma_gain(std::vector<sample_type>& plane, double gain);

 private:
  StackingMode m_mode = StackingMode::WEIGHTED;
  double m_reject_sigma = 5.0;
  bool m_group_pressings = true;
  bool m_no_diff_dod = false;
  bool m_passthrough = false;
  bool m_sample_align = true;
  bool m_chroma_gain_match = true;
  bool m_luma_level_match = true;
  int32_t m_thread_count = 0;
  bool m_still_frame_stacking = false;
  int32_t m_still_frame_search = 24;
  AudioStackingMode m_audio_stacking_mode = AudioStackingMode::MEAN;
  EFMStackingMode m_efm_stacking_mode = EFMStackingMode::DISABLED;

  std::map<std::string, ParameterValue> parameters_;

  mutable std::mutex execute_mutex_;
  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
  mutable std::vector<std::shared_ptr<const VideoFrameRepresentation>>
      cached_sources_;

  // Which inputs each field may be stacked from (field_valid[source][field])
  // and which fields no two inputs agree on.
  struct FieldAgreement {
    std::vector<std::array<bool, 2>> field_valid;
    std::array<bool, 2> disagreed{false, false};
  };

  // Compare each valid input's fields with the per-sample median of the
  // inputs, on low-passed luma over a decimated grid.  An input below
  // kMinFieldCorrelation is left out of that field when at least two inputs
  // agree; when fewer do, every input is kept and the field is reported as
  // disagreed.  Fields with too little picture detail are not judged.
  static FieldAgreement check_field_agreement(
      const std::vector<std::vector<sample_type>>& planes,
      const std::vector<bool>& frame_valid,
      const std::vector<std::vector<DropoutRun>>& dropouts, VideoSystem system,
      size_t nominal_width, size_t height, FrameID frame_id);

  // Replace the output dropouts inside every disagreed field with runs
  // covering the whole field.
  static void mark_disagreed_fields(const FieldAgreement& agreement,
                                    VideoSystem system, size_t nominal_width,
                                    size_t height,
                                    std::vector<DropoutRun>& output_dropouts);

  using SourceNoise = StackedVideoFrameRepresentation::SourceNoise;
  using StillCopy = StackedVideoFrameRepresentation::StillCopy;

  // What the Weighted mode needs per row: its weight in each field, and the
  // distance from the per-sample median beyond which a value is rejected
  // (reject_sigma times the stack's noise scale).  The weights are per field
  // because the rows a field combines are: the field agreement check can
  // leave a source's field out, and a still-frame copy can match in one
  // field only.  |model| is the sources' noise model the weights came from,
  // empty when the mode does not weight, and |scale| what its weights were
  // multiplied by so that the largest source's is 1, which keeps equal
  // weights exact.
  struct NoiseWeights {
    std::array<std::vector<double>, 2> weight;
    double reject_distance = 0.0;
    std::vector<SourceNoise> model;
    double scale = 1.0;
  };

  // Weights for |model| (see StackedVideoFrameRepresentation::noise_model)
  // over the sources |valid| and, per field, |field_valid| mark, as
  // pressing_weights() gives them for those sources combined; equal weights
  // and a nominal distance when |model| is empty.
  NoiseWeights noise_weights(
      const std::vector<SourceNoise>& model, const std::vector<bool>& valid,
      const std::vector<std::array<bool, 2>>& field_valid) const;

  // Stack composite frame from per-source FrameIDs.  |sample_offsets| carries
  // each source's sample-grid offset (see
  // StackedVideoFrameRepresentation::sample_offsets()); pass an empty vector
  // to combine on the sources' own grids.  |noise_model| carries each
  // source's noise for the Weighted mode; other modes ignore it.  |copies|
  // are still-frame copies (see StackedVideoFrameRepresentation::
  // still_copies()), added as extra inputs on active picture lines only,
  // each on its source's grid and with its source's weight.  A single source
  // without copies passes through unchanged, and with copies its lines that
  // take none do too.
  void stack_frame(
      const std::vector<FrameID>& source_ids,
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      const std::vector<int32_t>& sample_offsets,
      const std::vector<SourceNoise>& noise_model,
      const std::vector<StillCopy>& copies,
      std::vector<sample_type>& output_samples,
      std::vector<DropoutRun>& output_dropouts) const;

  // Stack YC frame from per-source FrameIDs
  void stack_frame_yc(
      const std::vector<FrameID>& source_ids,
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      const std::vector<int32_t>& sample_offsets,
      const std::vector<SourceNoise>& noise_model,
      const std::vector<StillCopy>& copies,
      std::vector<sample_type>& output_luma,
      std::vector<sample_type>& output_chroma,
      std::vector<DropoutRun>& output_dropouts) const;

  // Apply the chosen stacking mode to a set of unweighted sample values.
  // Weighted mode, given values without weights (recovered or passed-through
  // dropout samples), takes their rounded mean.
  int16_t stack_mode(const std::vector<int16_t>& values) const;

  // Weighted mode: the weighted mean of the values within
  // |reject_distance| of their median, rounded.  |low_confidence| is set
  // when no majority of the values lies within |reject_distance| of the
  // result, or when two values differ by more than it.
  static int16_t stack_weighted(const std::vector<int16_t>& values,
                                const std::vector<double>& weights,
                                double reject_distance, bool& low_confidence);

  int16_t compute_median(std::vector<int16_t> values) const;
  int32_t compute_mean(const std::vector<int16_t>& values) const;

  // Differential dropout detection: recover samples that are all marked as
  // dropout but lie within 10 % of their median, as ld-disc-stacker does.
  std::vector<int16_t> diff_dod(const std::vector<int16_t>& input) const;

  // Which lines of a frame take still-frame copies: the active picture
  // lines of each field some copy is valid in, never NTSC line 21 or 284
  // (closed captions).  Empty when there are no copies.
  static std::vector<uint8_t> copy_lines(
      const std::vector<std::array<bool, 2>>& field_valid, size_t num_sources,
      VideoSystem system, size_t height);

  // The samples of a line, counted from its start, that take still-frame
  // copies on the lines copy_lines() marks: the active picture, [first,
  // second).  Horizontal blanking is left to the sources: the frames of a
  // held picture differ there, alike on every disc, so its repeats there
  // disagree and every sample on the sync edge would read as low confidence.
  static std::pair<size_t, size_t> copy_samples(VideoSystem system);

  // Rows of the inputs the stack is built from: one per source, then one per
  // still-frame copy.
  struct StackRows {
    std::vector<std::vector<sample_type>> luma;    // composite, or YC luma
    std::vector<std::vector<sample_type>> chroma;  // YC only
    std::vector<bool> valid;
    std::vector<std::array<bool, 2>> field_valid;
    std::vector<std::vector<DropoutRun>> dropouts;
    // Per source, the luma level its rows were brought from (see
    // match_luma_levels()); nullopt where they were left as they were.
    std::vector<std::optional<LevelMatch>> levels;
  };

  // Bring each source's luma (its composite, or its YC luma plane) to the
  // typical source's gain and level (LumaLevelMeter, apply_luma_level()),
  // measured on this frame, and record what was done in |rows.levels|.  A
  // source within a hair of the typical one is left exactly as it was, and
  // one beyond the plausible range (a different picture, not a different
  // player) is left too.  Nothing is done when luma_level_match is off.
  void match_luma_levels(StackRows& rows, VideoSystem system,
                         size_t nominal_width, size_t height,
                         FrameID frame_id) const;

  // Each valid row's burst amplitude (see burst_amplitude()), measured on
  // the composite or, for |yc|, the chroma rows; all nullopt when
  // chroma_gain_match is off.
  std::vector<std::optional<double>> row_burst_amplitudes(const StackRows& rows,
                                                          bool yc,
                                                          VideoSystem system,
                                                          size_t nominal_width,
                                                          size_t height) const;

  // Bring |plane| (input |input| of frame |frame_id|, for the log) to the
  // burst |reference| from its own burst |amplitude|.
  static void match_chroma_gain(std::vector<sample_type>& plane,
                                std::optional<double> amplitude,
                                std::optional<double> reference,
                                FrameID frame_id, size_t input);

  // Append |copies| to |rows| (which holds the sources' rows), each shifted
  // by its source's sample offset and its subcarrier band brought to
  // |burst_reference| (none when nullopt); a copy whose source frame is not
  // used, or whose fields the field agreement check left out, adds nothing
  // there.  A copy takes its source's luma level correction.  The copies
  // of one frame share no stamper noise with the frame
  // they are stacked into, only with each other, so in each field they are
  // weighted as that frame's copies of the field combined
  // (pressing_weights()); without a model, as their sources are.
  void append_copy_rows(
      const std::vector<StillCopy>& copies,
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      const std::vector<int32_t>& sample_offsets, VideoSystem system,
      size_t nominal_width, size_t height, bool yc,
      std::optional<double> burst_reference, StackRows& rows,
      NoiseWeights& noise) const;

  // Put a single source's samples that took no copies (the lines
  // |takes_copies| leaves out, and the blanking of those it marks) back as
  // they were, with its own dropout runs.
  static void restore_uncopied_lines(
      const std::vector<uint8_t>& takes_copies,
      const std::vector<sample_type>& source_plane,
      const std::vector<DropoutRun>& source_dropouts, VideoSystem system,
      size_t nominal_width, size_t height, std::vector<sample_type>& output,
      std::vector<DropoutRun>& output_dropouts);

  // Line processing (parallel-friendly).  |dropout_masks| holds one byte per
  // sample of each row's frame, non-zero where it is flagged.  The first
  // |num_sources| rows are the sources; the rest are still-frame copies,
  // used only on lines |takes_copies| marks, within copy_samples().
  void process_lines_range(
      size_t start_line, size_t end_line, size_t width, VideoSystem system,
      const std::vector<std::vector<sample_type>>& all_frames,
      const std::vector<bool>& frame_valid,
      const std::vector<std::array<bool, 2>>& field_valid,
      const std::vector<std::vector<uint8_t>>& dropout_masks,
      const std::vector<uint8_t>& takes_copies, const NoiseWeights& noise,
      size_t num_sources, int32_t black_level,
      std::vector<sample_type>& output_samples,
      std::vector<DropoutRun>& output_dropouts, size_t& total_dropouts,
      size_t& total_stacked) const;

  void process_lines_range_yc(
      size_t start_line, size_t end_line, size_t width, VideoSystem system,
      const std::vector<std::vector<sample_type>>& all_luma,
      const std::vector<std::vector<sample_type>>& all_chroma,
      const std::vector<bool>& frame_valid,
      const std::vector<std::array<bool, 2>>& field_valid,
      const std::vector<std::vector<uint8_t>>& dropout_masks,
      const std::vector<uint8_t>& takes_copies, const NoiseWeights& noise,
      size_t num_sources, int32_t black_level,
      std::vector<sample_type>& output_luma,
      std::vector<sample_type>& output_chroma,
      std::vector<DropoutRun>& output_dropouts, size_t& total_dropouts,
      size_t& total_stacked) const;

  // Audio and EFM stacking helpers. Audio samples are 24-bit values carried
  // in int32_t (see audio_channel_pair.h); mean/median results are saturated
  // to the 24-bit range.
  std::vector<int32_t> stack_audio(
      size_t pair, const std::vector<FrameID>& source_ids,
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      size_t best_src) const;

  std::vector<uint8_t> stack_efm(
      const std::vector<FrameID>& source_ids,
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      size_t best_src) const;

  int32_t audio_mean(const std::vector<int32_t>& v) const;
  int32_t audio_median(std::vector<int32_t> v) const;
  uint8_t efm_mean(const std::vector<uint8_t>& v) const;
  uint8_t efm_median(std::vector<uint8_t> v) const;
};

}  // namespace orc
