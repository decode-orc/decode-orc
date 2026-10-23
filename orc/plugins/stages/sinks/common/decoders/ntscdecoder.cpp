/*
 * File:        ntscdecoder.cpp
 * Module:      orc-core
 * Purpose:     NTSC decoder wrapper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 * SPDX-FileCopyrightText: 2019-2020 Adam Sampson
 */

#include "ntscdecoder.h"

#include <orc/support/logging.h>

#include "../video_parameter_safety.h"

NtscDecoder::NtscDecoder(const Comb::Configuration& combConfig) {
  config.combConfig = combConfig;
}

bool NtscDecoder::configure(const ::orc::SourceParameters& videoParameters) {
  // A failed reconfiguration must not leave the previous configuration usable.
  configurationValid_ = false;

  // Ensure the source video is NTSC
  if (videoParameters.system != orc::VideoSystem::NTSC) {
    ORC_LOG_ERROR("This decoder is for NTSC video sources only");
    return false;
  }

  const auto safety = ::orc::chroma_sink::sanitize_video_parameters(
      videoParameters, ::orc::chroma_sink::DecoderVideoProfile::NtscColour);

  if (!safety.warnings.empty()) {
    ORC_LOG_WARN(
        "NtscDecoder::configure(): Adjusted unsafe video parameters: {}",
        ::orc::chroma_sink::join_issues(safety.warnings));
  }

  if (!safety.ok) {
    ORC_LOG_ERROR("NtscDecoder::configure(): Invalid video parameters: {}",
                  ::orc::chroma_sink::join_issues(safety.errors));
    return false;
  }

  config.videoParameters = safety.params;

  comb = std::make_unique<Comb>();
  comb->updateConfiguration(config.videoParameters, config.combConfig);

  // The Y/C luma channel is already clean, so filterChroma stays off.
  MonoDecoder::MonoConfiguration lumaConfig;
  lumaConfig.filterChroma = false;
  lumaConfig.videoParameters = config.videoParameters;
  ycLumaDecoder = std::make_unique<MonoDecoder>(lumaConfig);

  configurationValid_ = true;

  return true;
}

int32_t NtscDecoder::getLookBehind() const {
  return config.combConfig.getLookBehind();
}

int32_t NtscDecoder::getLookAhead() const {
  return config.combConfig.getLookAhead();
}

void NtscDecoder::decodeFrames(const std::vector<SourceField>& inputFields,
                               int32_t startIndex, int32_t endIndex,
                               std::vector<ComponentFrame>& componentFrames) {
  if (!configurationValid_) {
    ORC_LOG_ERROR(
        "NtscDecoder::decodeFrames(): Decoder configuration is invalid");
    return;
  }

  if (!inputFields.empty() && inputFields[0].is_yc) {
    decodeFramesYc(inputFields, startIndex, endIndex, componentFrames);
    return;
  }

  comb->decodeFrames(inputFields, startIndex, endIndex, componentFrames);
}

void NtscDecoder::decodeFramesYc(const std::vector<SourceField>& inputFields,
                                 int32_t startIndex, int32_t endIndex,
                                 std::vector<ComponentFrame>& componentFrames) {
  // Split each Y/C field into a luma-only and a chroma-only composite field.
  std::vector<SourceField> lumaFields;
  std::vector<SourceField> chromaFields;
  split_yc_fields(inputFields, lumaFields, chromaFields);

  std::vector<ComponentFrame> lumaFrames(componentFrames.size());
  ycLumaDecoder->decodeFrames(lumaFields, startIndex, endIndex, lumaFrames);
  comb->decodeFrames(chromaFields, startIndex, endIndex, componentFrames);

  for (size_t i = 0; i < componentFrames.size(); i++) {
    componentFrames[i].merge_luma_from(lumaFrames[i]);
  }
}
