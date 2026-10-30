/*
 * File:        picture_band_limit_stage.h
 * Module:      orc-stage-plugin-picture-band-limit
 * Purpose:     Cuts the band above the picture: a low-pass of the luma
 *              (composite stays composite)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/artifact.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/lru_cache.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// The default cutoff for a system: where the Domesday captures' picture ends
// on PAL; NTSC and PAL-M are not yet measured on real discs.
double default_picture_cutoff_mhz(VideoSystem system);

inline constexpr double kDefaultRolloffMhz = 0.4;

// The low-pass, as taps either side of the centre: a Kaiser-windowed sinc
// whose amplitude falls to half at |cutoff_hz|, across a transition
// |rolloff_hz| wide, with 50 dB of stop band (0.3 % ripple in the pass
// band).
std::vector<double> picture_band_taps(double cutoff_hz, double rolloff_hz,
                                      double sample_rate_hz);

// Whether a system's whole roll-off lies above its chroma band, so the
// composite can be low-passed as it is; otherwise only the luma is.
bool picture_band_limits_composite(VideoSystem system, double cutoff_hz,
                                   double rolloff_hz);

class PictureBandLimitStage;

// ============================================================================
// BandLimitedVideoFrameRepresentation
// ============================================================================
class BandLimitedVideoFrameRepresentation
    : public VideoFrameRepresentationWrapper,
      public Artifact {
 public:
  BandLimitedVideoFrameRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source, double cutoff_mhz,
      double rolloff_mhz);

  std::string type_name() const override {
    return "band_limited_video_frame_representation";
  }

  const int16_t* get_frame(FrameID id) const override;
  const int16_t* get_line(FrameID id, size_t line) const override;
  std::vector<int16_t> get_frame_copy(FrameID id) const override;

  const int16_t* get_frame_luma(FrameID id) const override;
  const int16_t* get_line_luma(FrameID id, size_t line) const override;

 private:
  static constexpr size_t kMaxCachedFrames = 64;

  // The low-pass either side of its centre, and whether it applies to the
  // whole composite rather than to its luma.
  std::vector<double> taps_;
  bool whole_composite_ = false;

  // Filtered composite (or, for Y/C sources, filtered luma) frames.
  mutable LRUCache<FrameID, std::vector<int16_t>> frames_;

  const std::vector<int16_t>* filtered(FrameID id) const;
  const int16_t* line_of(FrameID id, size_t line) const;
};

// ============================================================================
// PictureBandLimitStage
// ============================================================================
class PictureBandLimitStage : public DAGStage,
                              public ParameterizedStage,
                              public IStagePreviewCapability {
 public:
  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::TRANSFORM,
                        "picture_band_limit",
                        "Picture Band Limit",
                        "Cut the band above the picture: low-pass the luma, "
                        "leaving the chroma as it is",
                        1,
                        1,
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

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

 private:
  // Unset until configured: the source's system's default applies.
  std::optional<double> cutoff_mhz_;
  double rolloff_mhz_ = kDefaultRolloffMhz;

  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
};

}  // namespace orc
