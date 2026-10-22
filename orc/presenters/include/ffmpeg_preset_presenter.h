/*
 * File:        ffmpeg_preset_presenter.h
 * Module:      orc-presenters
 * Purpose:     Presenter for FFmpeg Preset analysis tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_PRESENTERS_FFMPEG_PRESET_PRESENTER_H
#define ORC_PRESENTERS_FFMPEG_PRESET_PRESENTER_H

#include <functional>

#include "analysis_tool_presenter.h"

namespace orc::presenters {

/**
 * @brief Presenter for FFmpeg Export Preset configuration tool
 *
 * Handles:
 * - Preparing configuration parameters for the tool
 * - Generating FFmpeg export parameters based on presets
 * - Formatting results for display
 * - Applying generated configuration to video_sink stage
 *
 * The FFmpeg Preset tool provides convenient presets for video export
 * without requiring users to understand codec details, based on profiles
 * from the legacy tbc-video-export tool:
 * - Lossless archival (FFV1, ProRes, lossless H.264/H.265/AV1)
 * - Professional editing (ProRes variants)
 * - Web delivery (H.264, H.265, AV1)
 * - Broadcast (D10/IMX)
 * - Hardware-accelerated encoding
 *
 * **Requirements:**
 * - Node must be a video_sink stage
 * - Tool operates as instant configuration (no DAG execution needed)
 * - User selects presets via FFmpegPresetDialog
 *
 * **Note:** The actual GUI dialog (FFmpegPresetDialog) handles the user
 * interaction and applies parameters directly. This presenter exists for
 * architectural consistency and potential future CLI support.
 */
class FFmpegPresetPresenter : public AnalysisToolPresenter {
 public:
  /**
   * @brief Construct an FFmpeg Preset presenter
   * @param project_handle Opaque handle to project
   */
  explicit FFmpegPresetPresenter(void* project_handle);

  /**
   * @brief Run FFmpeg preset configuration
   *
   * This method:
   * 1. Validates the node is a video_sink stage
   * 2. Calls the core FFmpeg preset tool
   * 3. Returns configuration success
   *
   * The tool generates configuration immediately without DAG execution.
   * In practice, the FFmpegPresetDialog handles parameter application
   * directly in the GUI, but this presenter provides a clean interface
   * for the analysis tool system.
   *
   * @param node_id The video_sink node to configure
   * @param parameters User-selected parameters (handled by dialog)
   * @param progress_callback Optional progress updates (not used for instant
   * tool)
   * @param cancel_check Optional poll; return true to cancel the analysis
   * @return Analysis result with configuration status
   */
  orc::AnalysisResult runAnalysis(
      NodeID node_id,
      const std::map<std::string, orc::ParameterValue>& parameters,
      std::function<void(int, const std::string&)> progress_callback,
      std::function<bool()> cancel_check) override;

 protected:
  std::string toolId() const override;
  std::string toolName() const override;

 private:
  /**
   * @brief Validate that the node is a video_sink stage
   * @param node_id Node to validate
   * @param error_message Output parameter for error description
   * @return true if valid, false otherwise
   */
  bool validateNode(NodeID node_id, std::string& error_message);
};

}  // namespace orc::presenters

#endif  // ORC_PRESENTERS_FFMPEG_PRESET_PRESENTER_H
