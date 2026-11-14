/*
 * File:        orc_stage_tooling.h
 * Module:      decode-orc Plugin SDK
 * Purpose:     Canonical stage helper/tooling contracts for plugin stages
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

#include <string>
#include <vector>

// The runtime instructions.md loader (platform file I/O) and the
// ORC_STAGE_INSTRUCTIONS_MD macro now live in the support tier. Included here
// so existing users of this header keep getting both without source changes.
#include <orc/support/stage_instructions.h>

namespace orc {

// Canonical helper/tool categories that allow host GUI/CLI to route tooling
// requests without hardcoded stage-id branches.
enum class StageToolKind {
  // A dialog that sets some of the stage's parameters for the user (a preset
  // picker, say). orc-gui opens the dialog a view binary registered for the
  // tool's contract_id, as for PluginView; it reads the node's values and
  // writes its choice through the session. orc-cli lists it and refuses to
  // run it.
  ConfigDialog,
  // A window the user keeps open beside the main one while editing what the
  // stage does (the Dropout Editor). orc-gui opens the window a view binary
  // registered for the tool's contract_id, as for PluginView; orc-cli lists
  // it and refuses to run it.
  NonModalEditor,
  // A sink whose trigger is the analysis. orc-cli `tools run` triggers it and
  // prints what it reports through orc::IStageResultsReportProvider
  // (<orc/stage/tooling/stage_results_report.h>); orc-gui opens the window a
  // view binary registered for the tool's contract_id, as for PluginView.
  BatchAnalysis,
  PreviewUtility,
  Custom,
  // The plugin's own view binary presents the tool. The host opens the widget
  // that view registered for this tool's contract_id and connects it to the
  // stage through an orc::view::IStageToolSession; the stage answers in
  // orc::IStageToolRequestHandler (<orc/stage/tooling/stage_tool_requests.h>).
  // GUI-only: orc-cli lists such a tool and refuses to run it. New values are
  // appended rather than inserted: the values above are what released plugins
  // encode.
  PluginView,
  // A measurement that proposes values for the stage's own parameters (a
  // carrier frequency, a frame map). The stage implements
  // orc::IStageAnalysisTool (<orc/stage/tooling/stage_analysis_tool.h>).
  // orc-cli `tools run` runs it (and --apply writes the result) and `tools
  // refresh` can run it for the node; orc-gui shows the host's analysis
  // window, or the window a view binary registered for the tool's
  // contract_id. contract_id is required: the GUI routes the tool by it.
  Analysis
};

struct StageToolDescriptor {
  std::string tool_id;
  std::string display_name;
  std::string description;
  StageToolKind kind{StageToolKind::Custom};

  // Not read by the host: the kind decides how the tool opens, and a view's
  // window decides its own modality.
  bool non_modal{false};

  // Optional stable contract string for tool payload semantics.
  std::string contract_id;
};

// What AnalysisToolProvider (below) returns. Not read by the host.
struct AnalysisToolDescriptor {
  // Tool identifier.
  std::string tool_id;

  // Human-friendly metadata for menus and dialogs.
  std::string display_name;
  std::string description;

  // Stable payload/execution contract string (for example,
  // "decode-orc.stage-tools.dropout-analysis.v1").
  std::string contract_id;

  // Whether host should automatically request analysis data after a trigger.
  bool auto_request_after_trigger{true};
};

// ---------------------------------------------------------------------------
// Stage documentation
// ---------------------------------------------------------------------------

// Preferred form: ORC_STAGE_INSTRUCTIONS_MD reads instructions.md from
// alongside the plugin .so at runtime. It is defined in the support-tier
// header <orc/support/stage_instructions.h> (included above) because it needs
// platform file I/O; this contract header only re-exports it.
//
// Usage (in the class body):
//   class MyStage : public DAGStage {
//    public:
//     ORC_STAGE_INSTRUCTIONS_MD
//   };

// Legacy form: embed instructions inline as a string literal.  Kept for
// backward compatibility with external plugins that do not ship an
// instructions.md file alongside their shared library.
//
//   class MyStage : public DAGStage {
//    public:
//     ORC_STAGE_INSTRUCTIONS(kInstructions_)
//    private:
//     static const char kInstructions_[];
//   };
//   const char MyStage::kInstructions_[] = R"(# My Stage ...)";
#define ORC_STAGE_INSTRUCTIONS(content) \
  std::string get_instructions() const override { return (content); }

// Optional mixin: stages can implement this to declare supported helper/tool
// integrations in a host-agnostic way.
class StageToolProvider {
 public:
  virtual ~StageToolProvider() = default;
  virtual std::vector<StageToolDescriptor> get_stage_tools() const = 0;
};

// Not read by the host, and implemented by no in-tree stage: offer an
// analysis as a StageToolKind::Analysis or BatchAnalysis tool instead. Kept
// until the next stage ABI change removes it, so a plugin implementing it
// still builds.
class AnalysisToolProvider {
 public:
  virtual ~AnalysisToolProvider() = default;
  virtual std::vector<AnalysisToolDescriptor> get_analysis_tools() const = 0;
};

}  // namespace orc
