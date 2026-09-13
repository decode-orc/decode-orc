/*
 * File:        streaming_capability.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Capability contract for a stage that can run inside a
 *              strict forward, single-pass execution (piping stdin/stdout;
 *              see the "-" convention in orc/support/pipe_io.h)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: stage/foundation — stage contract type crossing the plugin
// boundary. A layout change here bumps the host ABI version.

namespace orc {

/**
 * @brief Optional capability for a stage that can participate in a strict
 * single-pass pipeline — piping stdin/stdout via the "-" convention (see
 * orc/support/pipe_io.h), or a future non-seekable network endpoint.
 *
 * A source, transform, merger or sink stage opts in by additionally
 * inheriting from IStreamingCompatibility alongside its existing
 * DAGStage-derived interfaces. Not implementing this interface means "not
 * streaming-safe" — the default is deliberately conservative: no existing
 * stage's behaviour changes just because this interface exists, and a stage
 * only claims true once its author has actually checked the constraint
 * documented on supports_streaming_execution() below.
 *
 * The host is responsible for using this, not for computing it: before
 * running a project with a stdin source or a stdout sink, the host must
 * check every node reachable from that pipe endpoint (both directions —
 * a shared upstream node feeding a second, unrelated sink still has to be
 * safe) and refuse the run naming the first node that is not. This
 * interface only answers "can I"; it does not know about the surrounding
 * graph.
 */
class IStreamingCompatibility {
 public:
  virtual ~IStreamingCompatibility() = default;

  /**
   * @brief Whether this stage, configured with its CURRENT parameters, can
   * run under strict single-pass execution.
   *
   * Evaluated after the stage's parameters have been applied (via
   * set_parameters(), for a ParameterizedStage) — the answer is allowed to
   * depend on them. For example, a video sink that pre-scans every field for
   * chapter markers before writing its first frame cannot do that on an
   * unseekable pipe, so it returns true only while that option is off; a
   * stage with no such parameter-dependent behaviour can simply return a
   * constant.
   *
   * "Strict single-pass execution" means every input this stage reads
   * (upstream artifacts, and its own file/stream input for a source) is
   * consumed in one forward-only pass: no request for a frame already
   * handed to it earlier, no request that depends on a frame not yet
   * produced, and no need to know the input's total length before starting.
   * Return false when the stage needs temporal lookahead/lookbehind (e.g. a
   * 3D chroma decoder), needs a full pre-pass over its input before
   * producing its first output, or aligns/correlates more than one input
   * stream (source alignment, stacking).
   *
   * @return true only if the current configuration satisfies the constraint
   *         above. false — including via not implementing this interface at
   *         all — is always a safe, conservative answer.
   */
  virtual bool supports_streaming_execution() const = 0;
};

}  // namespace orc
