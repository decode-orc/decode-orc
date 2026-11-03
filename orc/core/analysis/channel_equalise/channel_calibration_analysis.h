/*
 * File:        channel_calibration_analysis.h
 * Module:      analysis
 * Purpose:     Channel Calibration: sets Channel Equalise's edge smear from
 *              the VITS on the stage's input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_ANALYSIS_CHANNEL_CALIBRATION_ANALYSIS_H
#define ORC_CORE_ANALYSIS_CHANNEL_CALIBRATION_ANALYSIS_H

#include "../analysis_tool.h"

namespace orc {

// Measures the edge smear on a channel_equalise node's input
// (channel_calibration.h) and applies it as the node's smear_fraction and
// smear_tau_samples.
class ChannelCalibrationAnalysisTool : public AnalysisTool {
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

#endif  // ORC_CORE_ANALYSIS_CHANNEL_CALIBRATION_ANALYSIS_H
