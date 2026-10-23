/*
 * File:        paldecoder.cpp
 * Module:      orc-core
 * Purpose:     PAL decoder wrapper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 * SPDX-FileCopyrightText: 2019-2020 Adam Sampson
 */

#include "paldecoder.h"

#include <orc/support/logging.h>

#include "../video_parameter_safety.h"

PalDecoder::PalDecoder(const PalColour::Configuration& palConfig) {
  config.pal = palConfig;
}

bool PalDecoder::configure(const ::orc::SourceParameters& videoParameters) {
  // A failed reconfiguration must not leave the previous configuration usable.
  configurationValid_ = false;

  // Ensure the source video is PAL
  if (videoParameters.system != orc::VideoSystem::PAL &&
      videoParameters.system != orc::VideoSystem::PAL_M) {
    ORC_LOG_ERROR("This decoder is for PAL video sources only");
    return false;
  }

  const auto safety = ::orc::chroma_sink::sanitize_video_parameters(
      videoParameters, ::orc::chroma_sink::DecoderVideoProfile::PalColour);

  if (!safety.warnings.empty()) {
    ORC_LOG_WARN(
        "PalDecoder::configure(): Adjusted unsafe video parameters: {}",
        ::orc::chroma_sink::join_issues(safety.warnings));
  }

  if (!safety.ok) {
    ORC_LOG_ERROR("PalDecoder::configure(): Invalid video parameters: {}",
                  ::orc::chroma_sink::join_issues(safety.errors));
    return false;
  }

  config.videoParameters = safety.params;

  // Transform PAL builds FFTW plans here with FFTW_MEASURE; per-thread
  // construction must be serialised by the caller.
  palColour = std::make_unique<PalColour>();
  palColour->updateConfiguration(config.videoParameters, config.pal);

  // The Y/C luma channel is already clean, so filterChroma stays off.
  MonoDecoder::MonoConfiguration lumaConfig;
  lumaConfig.filterChroma = false;
  lumaConfig.videoParameters = config.videoParameters;
  ycLumaDecoder = std::make_unique<MonoDecoder>(lumaConfig);

  configurationValid_ = true;

  return true;
}

int32_t PalDecoder::getLookBehind() const { return config.pal.getLookBehind(); }

int32_t PalDecoder::getLookAhead() const { return config.pal.getLookAhead(); }

void PalDecoder::decodeFrames(const std::vector<SourceField>& inputFields,
                              int32_t startIndex, int32_t endIndex,
                              std::vector<ComponentFrame>& componentFrames) {
  if (!configurationValid_) {
    ORC_LOG_ERROR(
        "PalDecoder::decodeFrames(): Decoder configuration is invalid");
    return;
  }

  if (!inputFields.empty() && inputFields[0].is_yc) {
    decodeFramesYc(inputFields, startIndex, endIndex, componentFrames);
    return;
  }

  palColour->decodeFrames(inputFields, startIndex, endIndex, componentFrames);
}

void PalDecoder::decodeFramesYc(const std::vector<SourceField>& inputFields,
                                int32_t startIndex, int32_t endIndex,
                                std::vector<ComponentFrame>& componentFrames) {
  // Split each Y/C field into a luma-only and a chroma-only composite field.
  std::vector<SourceField> lumaFields;
  std::vector<SourceField> chromaFields;
  split_yc_fields(inputFields, lumaFields, chromaFields);

  std::vector<ComponentFrame> lumaFrames(componentFrames.size());
  ycLumaDecoder->decodeFrames(lumaFields, startIndex, endIndex, lumaFrames);
  palColour->decodeFrames(chromaFields, startIndex, endIndex, componentFrames);

  for (size_t i = 0; i < componentFrames.size(); i++) {
    componentFrames[i].merge_luma_from(lumaFrames[i]);
  }
}
