/*
 * File:        no_analysis_session.h
 * Module:      orc-tests/gui/unit
 * Purpose:     A base for fake tool sessions of tools that are not analysis
 *              tools: the session's analysis calls answer as a host's do
 *              for such a tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/view/stage_tool_session.h>

#include <memory>

namespace gui_unit_test {

class NoAnalysisSession : public orc::view::IStageToolSession {
 public:
  orc::view::ToolAnalysisInfo analysis_info() const override { return {}; }
  std::shared_ptr<orc::view::IToolJob> run_analysis(
      orc::view::ToolParameterMap /*parameters*/,
      orc::view::ToolProgressHandler /*on_progress*/,
      orc::view::ToolAnalysisHandler /*on_report*/) override {
    return nullptr;
  }
  orc::view::ToolRefreshState refresh_state() const override { return {}; }
  void set_refresh(bool /*use_this_tool*/,
                   orc::view::ToolParameterMap /*parameters*/,
                   orc::view::ToolReplyHandler /*on_done*/) override {}
};

}  // namespace gui_unit_test
