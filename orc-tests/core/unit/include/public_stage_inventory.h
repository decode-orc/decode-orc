/*
 * File:        public_stage_inventory.h
 * Module:      orc-core-tests
 * Purpose:     Shared inventory of public core stages for cross-stage contracts
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Stage instances are obtained through the runtime registry, not by including
// concrete stage implementation headers. This keeps contract tests aligned with
// the plugin-first architecture and prevents coupling to internal stage
// classes.
#include "../../../orc/core/include/stage_registry.h"

namespace orc_unit_test {
enum class PublicStageFamily {
  Source,
  Transform,
  Sink,
};

struct PublicStageSpec {
  std::string inventory_id;
  PublicStageFamily family;
  // registry_backed is always true for all inventory entries.
  // Non-registry stages (e.g. internal chroma_sink) are excluded from the
  // shared inventory; their behaviour is covered by dedicated stage-level
  // tests.
  bool registry_backed;
  // create() goes through StageRegistry so tests exercise the same factory
  // path as the host runtime. No concrete stage classes are instantiated here.
  std::function<orc::DAGStagePtr()> create;
};

inline const std::vector<PublicStageSpec>& public_stage_specs() {
  static const std::vector<PublicStageSpec> specs = {
      {"tbc_source", PublicStageFamily::Source, true,
       [] {
         return orc::StageRegistry::instance().create_stage("tbc_source");
       }},
      {"PAL_CVBS_Source", PublicStageFamily::Source, true,
       [] {
         return orc::StageRegistry::instance().create_stage("PAL_CVBS_Source");
       }},
      {"NTSC_CVBS_Source", PublicStageFamily::Source, true,
       [] {
         return orc::StageRegistry::instance().create_stage("NTSC_CVBS_Source");
       }},
      {"PAL_M_CVBS_Source", PublicStageFamily::Source, true,
       [] {
         return orc::StageRegistry::instance().create_stage(
             "PAL_M_CVBS_Source");
       }},
      {"vbi_source", PublicStageFamily::Source, true,
       [] {
         return orc::StageRegistry::instance().create_stage("vbi_source");
       }},
      {"stacker", PublicStageFamily::Transform, true,
       [] { return orc::StageRegistry::instance().create_stage("stacker"); }},
      {"frame_map", PublicStageFamily::Transform, true,
       [] { return orc::StageRegistry::instance().create_stage("frame_map"); }},
      {"source_join", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage("source_join");
       }},
      {"video_params", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage("video_params");
       }},
      {"dropout_correct", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage("dropout_correct");
       }},
      {"dropout_map", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage("dropout_map");
       }},
      {"source_align", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage("source_align");
       }},
      {"mask_line", PublicStageFamily::Transform, true,
       [] { return orc::StageRegistry::instance().create_stage("mask_line"); }},
      {"picture_band_limit", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage(
             "picture_band_limit");
       }},
      {"fm_beat_cancel", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage("fm_beat_cancel");
       }},
      {"efm_audio_decode", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage("efm_audio_decode");
       }},
      {"audio_import", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage("audio_import");
       }},
      {"audio_channel_map", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage(
             "audio_channel_map");
       }},
      {"audio_align", PublicStageFamily::Transform, true,
       [] {
         return orc::StageRegistry::instance().create_stage("audio_align");
       }},
      {"video_sink", PublicStageFamily::Sink, true,
       [] {
         return orc::StageRegistry::instance().create_stage("video_sink");
       }},
      {"daphne_vbi_sink", PublicStageFamily::Sink, true,
       [] {
         return orc::StageRegistry::instance().create_stage("daphne_vbi_sink");
       }},
      {"AudioSink", PublicStageFamily::Sink, true,
       [] { return orc::StageRegistry::instance().create_stage("AudioSink"); }},
      {"CCSink", PublicStageFamily::Sink, true,
       [] { return orc::StageRegistry::instance().create_stage("CCSink"); }},
      {"tbc_sink", PublicStageFamily::Sink, true,
       [] { return orc::StageRegistry::instance().create_stage("tbc_sink"); }},
      {"CVBSSink", PublicStageFamily::Sink, true,
       [] { return orc::StageRegistry::instance().create_stage("CVBSSink"); }},
      {"EFMSink", PublicStageFamily::Sink, true,
       [] { return orc::StageRegistry::instance().create_stage("EFMSink"); }},
      {"RawEFMSink", PublicStageFamily::Sink, true,
       [] {
         return orc::StageRegistry::instance().create_stage("RawEFMSink");
       }},
      {"AC3RFSink", PublicStageFamily::Sink, true,
       [] { return orc::StageRegistry::instance().create_stage("AC3RFSink"); }},
      {"dropout_analysis_sink", PublicStageFamily::Sink, true,
       [] {
         return orc::StageRegistry::instance().create_stage(
             "dropout_analysis_sink");
       }},
      {"snr_analysis_sink", PublicStageFamily::Sink, true,
       [] {
         return orc::StageRegistry::instance().create_stage(
             "snr_analysis_sink");
       }},
      {"burst_level_analysis_sink", PublicStageFamily::Sink, true,
       [] {
         return orc::StageRegistry::instance().create_stage(
             "burst_level_analysis_sink");
       }},
      {"teletext_sink", PublicStageFamily::Sink, true,
       [] {
         return orc::StageRegistry::instance().create_stage("teletext_sink");
       }},
      {"nabts_sink", PublicStageFamily::Sink, true,
       [] {
         return orc::StageRegistry::instance().create_stage("nabts_sink");
       }},
  };

  return specs;
}
}  // namespace orc_unit_test
