/*
 * File:        stage_tool_requests.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     The stage side of a view tool: requests from the plugin's view
 *              binary, answered with plugin-defined bytes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_TOOLING_STAGE_TOOL_REQUESTS_H
#define ORC_STAGE_TOOLING_STAGE_TOOL_REQUESTS_H

// SDK TIER: stage/tooling — stage contract type crossing the plugin boundary.
// A layout change here bumps the host ABI version.

#include <cstdint>
#include <string>
#include <vector>

namespace orc {

/// A stage's answer to one tool request. Set @ref ok and fill @ref data, or
/// leave @ref ok false and say why in @ref error.
struct StageToolReply {
  bool ok = false;
  std::vector<uint8_t> data;
  std::string error;
};

/**
 * @brief Answer requests from the plugin's view binary
 *
 * A stage that advertises a StageToolKind::PluginView tool implements this.
 * The host calls it once per IStageToolSession::request(), with the tool's
 * contract id, and hands the reply back to the view as it is. What the verbs
 * and bytes mean is up to the plugin; the host never looks inside them.
 *
 * Threading: called on the host's render worker, one request at a time, and
 * never while the stage is being triggered. The stage's results are those of
 * its last trigger, or none: answer "not triggered yet" in @ref
 * StageToolReply::error rather than running anything.
 *
 * Exceptions thrown here are caught and become an ok == false reply.
 */
class IStageToolRequestHandler {
 public:
  virtual ~IStageToolRequestHandler() = default;

  virtual StageToolReply handle_tool_request(
      const std::string& contract_id, const std::string& verb,
      const std::vector<uint8_t>& payload) = 0;
};

}  // namespace orc

#endif  // ORC_STAGE_TOOLING_STAGE_TOOL_REQUESTS_H
