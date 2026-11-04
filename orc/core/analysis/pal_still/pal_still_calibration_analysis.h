/*
 * File:        pal_still_calibration_analysis.h
 * Module:      analysis
 * Purpose:     PAL Still Calibration: sets the video sink's line-alternating
 *              chroma matrix for the PAL Still decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_ANALYSIS_PALSTILL_CALIBRATION_ANALYSIS_H
#define ORC_CORE_ANALYSIS_PALSTILL_CALIBRATION_ANALYSIS_H

#include "../analysis_tool.h"

namespace orc {

// Measures the line-alternating chroma matrix on a video_sink node's input
// (pal_still_calibration.h) and applies it as the node's hanover_uu,
// hanover_uv, hanover_vu and hanover_vv.
class PalStillCalibrationAnalysisTool : public AnalysisTool {
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
};

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_PALSTILL_CALIBRATION_ANALYSIS_H
