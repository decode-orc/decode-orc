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
 *
 * A stage returning true must still tolerate execute() being called more
 * than once on the SAME instance with the same effective input (the host's
 * DAGExecutor may re-execute a node on a cache miss even within a single
 * run) — it must materialise its result from the pipe exactly once and
 * serve every later call from that, the same way any well-behaved source
 * already caches what it loaded (see project_to_dag.h). This is distinct
 * from — and does not require tolerating — a second, independent instance
 * of the same node reading concurrently.
 *
 * The GUI's FILE_PATH editor accepts and saves a "-" or network-URL value
 * (stageparameterdialog.cpp) rather than blocking it, since the officially
 * supported workflow is to build the project in the GUI and run it via
 * `orc-cli ... --process`. Every GUI-only code path capable of executing a
 * real stage instance outside an explicitly validated CLI trigger checks
 * orc::dag_subgraph_targets_pipe_or_network()
 * (project_to_dag.h) — a backward walk from the node in question over
 * everything it (transitively) depends on — before doing so, and refuses
 * rather than let a stage attempt real stdin/stdout I/O against the GUI
 * process itself: ProjectPresenter::getNodeConfigurationStatus() (marks the
 * node unconfigured), RenderPresenter::triggerStage(),
 * RenderPresenter::sweepNodeForObservation() and
 * ::scheduleObservationsForPreview() (the background observation pool's two
 * entry points), and PreviewRenderer::ensure_node_executed() (every preview
 * rendering path, including the one the GUI runs automatically on project
 * load to populate the newly opened project's initial preview). This DOES
 * confirm the background pool and automatic preview would otherwise reach a
 * pipe-configured node — via cloned stage instances for the former, the
 * original ones for the latter — which is exactly the concurrent-instance
 * scenario this paragraph used to (incorrectly) call unreachable.
 * ProjectPresenter::triggerNode()/triggerAllSinks() are the one exception:
 * shared with the CLI's own legitimate, pre-validated pipe use
 * (validatePipeExecution(), called by command_process.cpp/command_filter.cpp
 * before either), they cannot refuse "-"/network URLs unconditionally
 * themselves — see the SAFETY note on their declarations
 * (project_presenter.h) before wiring either to a GUI action.
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
   * "No need to know the input's total length before starting" is not
   * aspirational: it is enforced per-artifact. A stage that reads a
   * VideoFrameRepresentation and returns true here MUST also check
   * VideoFrameRepresentation::has_unbounded_frame_range() before doing
   * anything that assumes frame_range() is the input's real length —
   * reserving a container sized from it, running a pre-count pass over it,
   * computing a total from it for a header written up front, and so on.
   * When it answers true, frame_range() is a placeholder (a piped/live
   * source with no declared frame_count), and the stage must instead detect
   * the actual end of input via VideoFrameRepresentation::is_exhausted() —
   * and, once it is true, fail if stream_error() is non-empty (the stream
   * stopped on a read error, not a clean end) rather than report a
   * truncated output as a success — the same way every current implementer
   * of this interface that consumes
   * a VideoFrameRepresentation does (ac3rf_sink, audio_sink, cvbs_sink,
   * daphne_vbi_sink, nabts_sink, raw_efm_sink, tbc_sink, teletext_sink,
   * video_sink — see video_sink_stage.cpp's run_streaming_export() for the
   * fullest example, including bounded-concurrency streaming decode). A
   * stage unable to do this for some input path should refuse cleanly on
   * that path specifically (return false from analyse()/trigger() with a
   * clear message) rather than return false unconditionally from this
   * method and lose streaming support for every OTHER input it could have
   * handled.
   *
   * @return true only if the current configuration satisfies the constraint
   *         above. false — including via not implementing this interface at
   *         all — is always a safe, conservative answer.
   */
  virtual bool supports_streaming_execution() const = 0;
};

}  // namespace orc
