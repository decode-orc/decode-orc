/*
 * File:        disc_mapper_analysis.h
 * Module:      analysis
 * Purpose:     Disc mapper analysis tool: detects skipped, repeated, and
 * missing frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_ANALYSIS_DISC_MAPPER_ANALYSIS_H
#define ORC_CORE_ANALYSIS_DISC_MAPPER_ANALYSIS_H

#include <orc/stage/common_types.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <orc_analysis.h>

#include <map>
#include <string>

#include "../analysis_tool.h"
#include "disc_mapper_analyzer.h"

namespace orc {

/**
 * @brief What the disc says of itself over the whole side
 *
 * The disc-level vote of the disc metadata document the video sink embeds
 * (disc-metadata/disc_metadata_document.h), read from the biphase observations
 * of |frames| in |observations|, with the lowest and highest picture number
 * |decision| mapped (CAV only). Values never recovered are left out. The keys
 * are listed in docs/technical/disc-mapping.md.
 */
std::map<std::string, StatisticValue> disc_identity_statistics(
    const IObservationContext& observations, FrameIDRange frames,
    VideoSystem system, const FieldMappingDecision& decision);

/**
 * @brief Disc mapper analysis tool
 *
 * Analyzes source captures to detect skipped, repeated, and missing frames
 * that indicate laserdisc player tracking problems.
 */
class DiscMapperAnalysisTool : public AnalysisTool {
 public:
  std::string id() const override;
  std::string name() const override;
  std::string description() const override;
  std::string category() const override;

  std::vector<ParameterDescriptor> parameters() const override;
  bool canAnalyze(AnalysisSourceType source_type) const override;
  bool isApplicableToStage(const std::string& stage_name) const override;
  int priority() const override { return 1; }  // Stage-specific tool

  AnalysisResult analyze(const AnalysisContext& ctx,
                         AnalysisProgress* progress) override;

  bool canApplyToGraph() const override;
  bool applyToGraph(AnalysisResult& result, const Project& project,
                    NodeID node_id) override;

  int estimateDurationSeconds(const AnalysisContext& ctx) const override;
};

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_DISC_MAPPER_ANALYSIS_H
