/*
 * File:        response_sequence_gate.h
 * Module:      orc-gui
 * Purpose:     Ordering guard for asynchronous render-coordinator responses
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_RESPONSE_SEQUENCE_GATE_H
#define ORC_GUI_RESPONSE_SEQUENCE_GATE_H

#include <cstdint>

namespace orc::gui {

/**
 * @brief Admits coordinator responses in request order.
 *
 * A consumer that re-requests on every displayed frame needs to drop responses
 * that a later one has already overtaken, so slow data cannot redraw a
 * dialogue with a frame the user has moved past. The obvious guard - compare
 * the response against the request currently in flight - starves the consumer
 * instead of protecting it: the worker emits while the GUI thread is still
 * inside the preview callback, and that callback issues the next frame's
 * request before returning to the event loop, so every response arrives one
 * request out of date and none is ever applied.
 *
 * This gate compares against the newest response already @em admitted, which
 * is the condition actually wanted. The coordinator's request ids are
 * monotonic and its worker is FIFO, so a response that is newer than the last
 * one applied is never stale.
 *
 * Thread-safety: none. Belongs to the thread that handles the responses.
 */
class ResponseSequenceGate {
 public:
  /// True when @p request_id is newer than everything admitted so far, in
  /// which case it becomes the newest. A response the caller admits but then
  /// declines to use still counts as admitted.
  bool admit(std::uint64_t request_id) {
    if (request_id <= newest_admitted_) {
      return false;
    }
    newest_admitted_ = request_id;
    return true;
  }

  /// Newest request id admitted so far; 0 before the first.
  std::uint64_t newestAdmitted() const { return newest_admitted_; }

  /// Forget what has been admitted, so any response is admitted next. For a
  /// consumer whose id sequence restarts, such as one bound to a new
  /// coordinator.
  void reset() { newest_admitted_ = 0; }

 private:
  std::uint64_t newest_admitted_ = 0;
};

}  // namespace orc::gui

#endif  // ORC_GUI_RESPONSE_SEQUENCE_GATE_H
