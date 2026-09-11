/*
 * File:        response_pair_gate.h
 * Module:      orc-gui
 * Purpose:     Ordering guard for dialogues answered by a pair of responses
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_RESPONSE_PAIR_GATE_H
#define ORC_GUI_RESPONSE_PAIR_GATE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace orc::gui {

/**
 * @brief Assembles a frame's two answers, newest complete frame wins.
 *
 * The VBI and observation dialogues show one reading per displayed frame, but
 * a frame is two fields and each is asked for separately, so a reading is only
 * complete once both answers are in. Both are wanted, in either order, and a
 * frame the user has already moved past must not overwrite a newer one.
 *
 * The guard this replaces asked a different question - "is this response the
 * one I am currently waiting for?" - and starved the dialogue outright. During
 * playback the worker emits across a queued connection while the GUI thread is
 * still inside the preview callback, and that callback asks about the next
 * frame before returning to the event loop; every answer therefore arrives
 * with a newer question already outstanding and none was ever accepted. With
 * the vectorscope open, whose redraw lengthens the callback, the dialogue
 * stopped updating entirely.
 *
 * This gate instead admits any set newer than the newest already completed,
 * which is the condition actually wanted: the coordinator's ids are monotonic
 * and its worker is FIFO, so a set newer than the last one applied is never
 * stale. A dialogue may therefore run a frame or so behind the preview while
 * the GUI thread is busy, which is what it means for the data to arrive late.
 *
 * Only a bounded number of unfinished sets is kept; asking about a new frame
 * while several older ones have still not been answered means those answers
 * are never coming. Sets older than the newest completed one are discarded on
 * sight.
 *
 * Thread-safety: none. Belongs to the thread that handles the responses.
 */
template <typename Payload>
class ResponsePairGate {
 public:
  /// One frame's completed reading.
  struct Reading {
    Payload first;
    Payload second;
    /// False for a single-field mode, where @c second was never asked for and
    /// holds a default-constructed payload.
    bool has_second = false;
  };

  /// Note the two ids just requested for one frame. @p first_id and
  /// @p second_id must differ.
  void expectPair(std::uint64_t first_id, std::uint64_t second_id) {
    push(
        Set{nextGeneration(), first_id, second_id, true, {}, {}, false, false});
  }

  /// Note the single id just requested, for a mode that shows one field.
  void expectSingle(std::uint64_t request_id) {
    push(Set{nextGeneration(), request_id, 0, false, {}, {}, false, true});
  }

  /**
   * @brief Deliver one response.
   *
   * @return The completed reading when @p request_id finishes a set newer than
   *         anything already returned; nothing otherwise - the response was
   *         for a frame already overtaken, for a set no longer tracked, or for
   *         a set still waiting on its other half.
   */
  std::optional<Reading> deliver(std::uint64_t request_id, Payload payload) {
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      Set& set = *it;
      if (request_id == set.first_id && !set.have_first) {
        set.first = std::move(payload);
        set.have_first = true;
      } else if (set.has_second && request_id == set.second_id &&
                 !set.have_second) {
        set.second = std::move(payload);
        set.have_second = true;
      } else {
        continue;
      }

      if (!set.have_first || !set.have_second) {
        return std::nullopt;  // still waiting on the other half
      }

      const std::uint64_t generation = set.generation;
      if (generation <= newest_completed_) {
        // A newer frame has already been shown; this one would put the
        // dialogue backwards.
        pending_.erase(it);
        return std::nullopt;
      }

      Reading reading{std::move(set.first), std::move(set.second),
                      set.has_second};
      newest_completed_ = generation;
      discardThroughGeneration(generation);
      return reading;
    }
    return std::nullopt;
  }

  /// Forget every outstanding set and everything completed, so the next
  /// response is admitted. For a consumer whose id sequence restarts.
  void reset() {
    pending_.clear();
    newest_completed_ = 0;
    next_generation_ = 0;
  }

  /// Sets asked about but not yet answered. Introspection and testing.
  std::size_t outstanding() const { return pending_.size(); }

 private:
  // Asking about more frames than this while the answers do not arrive means
  // the oldest are never coming; keeping them would grow without bound.
  static constexpr std::size_t kMaxOutstanding = 8;

  struct Set {
    std::uint64_t generation = 0;
    std::uint64_t first_id = 0;
    std::uint64_t second_id = 0;
    bool has_second = false;
    Payload first{};
    Payload second{};
    bool have_first = false;
    bool have_second = false;
  };

  std::uint64_t nextGeneration() { return ++next_generation_; }

  void push(Set set) {
    pending_.push_back(std::move(set));
    while (pending_.size() > kMaxOutstanding) {
      pending_.pop_front();
    }
  }

  void discardThroughGeneration(std::uint64_t generation) {
    while (!pending_.empty() && pending_.front().generation <= generation) {
      pending_.pop_front();
    }
  }

  std::deque<Set> pending_;
  std::uint64_t newest_completed_ = 0;
  std::uint64_t next_generation_ = 0;
};

}  // namespace orc::gui

#endif  // ORC_GUI_RESPONSE_PAIR_GATE_H
