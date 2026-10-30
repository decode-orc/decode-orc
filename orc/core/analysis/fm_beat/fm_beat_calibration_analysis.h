/*
 * File:        fm_beat_calibration_analysis.h
 * Module:      analysis
 * Purpose:     FM Beat Calibration: sets FM Beat Cancel's carrier from the
 *              beat on the stage's input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_ANALYSIS_FM_BEAT_CALIBRATION_ANALYSIS_H
#define ORC_CORE_ANALYSIS_FM_BEAT_CALIBRATION_ANALYSIS_H

#include "../analysis_tool.h"

namespace orc {

// Measures the master's FM carrier at black on an fm_beat_cancel node's
// input (fm_beat_calibration.h) and applies it as the node's
// carrier_black_mhz.
class FmBeatCalibrationAnalysisTool : public AnalysisTool {
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

#endif  // ORC_CORE_ANALYSIS_FM_BEAT_CALIBRATION_ANALYSIS_H
