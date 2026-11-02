/*
 * File:        naplps_palette_timeline.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     How a NAPLPS page's colour map entries change over its
 *              presentation (X3.110 §5.3.2.5, §5.3.2.7)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_palette_timeline.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace orc {

namespace {

/// How far back a blink process carried in from an earlier record is run
/// before this record starts. Its phase comes from its own times whatever
/// this is; what a longer run-in would add is only what it copied from
/// entries other processes were blinking, which a minute settles.
constexpr int64_t kCarriedRunInMs = int64_t{60} * 1000;

/// Whole cycles a group of processes is given to settle into its loop before
/// the last one tried is taken as it stands.
constexpr int kLoopAttempts = 4;

/// Steps a track may hold. A handful of fast processes over a long
/// presentation stays well under this; a track that reaches it has its loop
/// cut short, and the timeline says it was approximated.
constexpr size_t kMaxStepsPerTrack = 65536;

/// §5.3.2.7.3 counts intervals in tenths of a second, so every period is a
/// multiple of this, which is what the least common multiples are taken in.
constexpr int64_t kTenthMs = 100;

/// A blink process being run.
struct Running {
  const NabtsBlinkProcess* process = nullptr;
  /// Its place in the order defined: later is newer.
  uint32_t order = 0;
  /// When it next changes the map, and whether that starts an ON interval.
  int64_t next_ms = 0;
  bool next_is_on = true;
  /// Started within an ON interval, whose blink-to colour has yet to go in.
  bool entered_on = false;
};

/**
 * @brief Put |running| in step with its process's phase at |time|
 *
 * Returns true where |time| falls within an ON interval — the process has
 * started part way through one, and the blink-to colour has to go in now.
 */
bool enter_phase(Running& running, int64_t time) {
  const NabtsBlinkProcess& process = *running.process;
  const int64_t period = process.period_ms();
  if (time < process.first_on_ms || period <= 0) {
    running.next_ms = process.first_on_ms;
    running.next_is_on = true;
    return false;
  }
  const int64_t into = (time - process.first_on_ms) % period;
  const int64_t cycle_start = time - into;
  if (into < static_cast<int64_t>(process.on_ms)) {
    running.next_ms = cycle_start + process.on_ms;
    running.next_is_on = false;
    return true;
  }
  running.next_ms = cycle_start + period;
  running.next_is_on = true;
  return false;
}

/// The simulation itself: the live map, the colour each entry is restored to,
/// and the processes running.
class Simulation {
 public:
  explicit Simulation(const NabtsPresentationTimeline& timeline)
      : timeline_(timeline) {
    for (size_t i = 0; i < kNabtsColourMapEntries; ++i) {
      base_[i] = timeline.initial_map[i];
      live_[i] = timeline.initial_map[i];
    }
  }

  const NabtsColour& live(size_t entry) const { return live_[entry]; }

  /// The next time a running process changes the map, or kNabtsNeverMs.
  int64_t next_transition() const {
    int64_t next = kNabtsNeverMs;
    for (const Running& running : running_) {
      next = std::min(next, running.next_ms);
    }
    return next;
  }

  /// Apply one of the record's own events, as of |time|.
  void apply(const NabtsPaletteEvent& event, int64_t time) {
    switch (event.kind) {
      case NabtsPaletteEvent::Kind::kMapWrite: {
        const size_t entry = event.address % kNabtsColourMapEntries;
        base_[entry] = event.colour;
        live_[entry] = event.colour;
        return;
      }
      case NabtsPaletteEvent::Kind::kBlinkStart: {
        if (event.process >= timeline_.blinks.size()) {
          return;
        }
        Running running;
        running.process = &timeline_.blinks[event.process];
        running.order = event.process;
        running.entered_on = enter_phase(running, time);
        running_.push_back(running);
        return;
      }
      case NabtsPaletteEvent::Kind::kBlinkEnd: {
        const auto it = std::find_if(
            running_.begin(), running_.end(),
            [&event](const Running& r) { return r.order == event.process; });
        if (it == running_.end()) {
          return;
        }
        const size_t from = it->process->from % kNabtsColourMapEntries;
        running_.erase(it);
        // "The original blink-from color shall be restored (unless it has been
        // changed explicitly by a SET COLOR command) when all blink processes
        // using that blink-from color are terminated." A SET COLOR since has
        // already changed what the entry is restored to.
        const bool still_blinking = std::any_of(
            running_.begin(), running_.end(), [from](const Running& r) {
              return r.process->from % kNabtsColourMapEntries == from;
            });
        if (!still_blinking) {
          live_[from] = base_[from];
        }
        return;
      }
    }
  }

  /// Run every process due at |time|, newest first (§5.3.2.7.2) — those just
  /// started within an ON interval included, as the Norpak TTX 650 orders
  /// them.
  void run_transitions(int64_t time) {
    std::vector<Running*> due;
    for (Running& running : running_) {
      if (running.entered_on || running.next_ms == time) {
        due.push_back(&running);
      }
    }
    std::sort(due.begin(), due.end(), [](const Running* a, const Running* b) {
      return a->order > b->order;
    });
    for (Running* running : due) {
      const NabtsBlinkProcess& process = *running->process;
      if (running->entered_on) {
        // enter_phase() has already set when the interval ends.
        run_on(process);
        running->entered_on = false;
      } else if (running->next_is_on) {
        run_on(process);
        running->next_ms = time + process.on_ms;
        running->next_is_on = false;
      } else {
        live_[process.from % kNabtsColourMapEntries] =
            base_[process.from % kNabtsColourMapEntries];
        running->next_ms = time + process.off_ms;
        running->next_is_on = true;
      }
    }
  }

 private:
  /// The start of an ON interval: the blink-to colour replaces the blink-from
  /// entry's (§5.3.2.7.2's "substitutes the current contents of another
  /// entry").
  void run_on(const NabtsBlinkProcess& process) {
    live_[process.from % kNabtsColourMapEntries] =
        process.to_address >= 0
            ? live_[static_cast<size_t>(process.to_address) %
                    kNabtsColourMapEntries]
            : process.to_colour;
  }

  const NabtsPresentationTimeline& timeline_;
  NabtsColour base_[kNabtsColourMapEntries];
  NabtsColour live_[kNabtsColourMapEntries];
  std::vector<Running> running_;
};

/// The colour a list of steps gives at |time|.
const NabtsColour& colour_at(const std::vector<NaplpsColourStep>& steps,
                             int64_t time) {
  const NaplpsColourStep* at = &steps.front();
  for (const NaplpsColourStep& step : steps) {
    if (static_cast<int64_t>(step.time_ms) > time) {
      break;
    }
    at = &step;
  }
  return at->colour;
}

/// Entries a set of processes joins together: a process copies its blink-to
/// entry into its blink-from entry, so the two cycle together.
class EntryGroups {
 public:
  EntryGroups() { std::iota(parent_.begin(), parent_.end(), size_t{0}); }

  size_t find(size_t entry) {
    while (parent_[entry] != entry) {
      parent_[entry] = parent_[parent_[entry]];
      entry = parent_[entry];
    }
    return entry;
  }
  void join(size_t a, size_t b) { parent_[find(a)] = find(b); }

 private:
  std::array<size_t, kNabtsColourMapEntries> parent_{};
};

/**
 * @brief The loop length for processes of the given periods, in milliseconds
 *
 * Their least common multiple, or — where that is longer than a loop may be —
 * the least common multiple of as many of the fastest as fit, with
 * |approximated| set.
 */
int64_t loop_length(std::vector<int64_t> periods, bool& approximated) {
  std::sort(periods.begin(), periods.end());
  const int64_t cap = kNaplpsMaxLoopMs / kTenthMs;
  int64_t tenths = 1;
  for (const int64_t period : periods) {
    const int64_t step = std::max<int64_t>(1, period / kTenthMs);
    const int64_t next = std::lcm(tenths, step);
    if (next > cap) {
      approximated = true;
      continue;
    }
    tenths = next;
  }
  return tenths * kTenthMs;
}

}  // namespace

NaplpsPaletteTimeline naplps_palette_timeline(
    const NabtsPresentationTimeline& timeline) {
  NaplpsPaletteTimeline out;
  if (timeline.events.empty()) {
    return out;  // A map nothing touched stands still.
  }

  // Where the simulation starts: the record's own start, or a little before it
  // for a process carried in from an earlier record.
  int64_t start = 0;
  for (const NabtsPaletteEvent& event : timeline.events) {
    start = std::min(start, event.time_ms);
  }
  start = std::max(start, -kCarriedRunInMs);

  // Where the record's own doing ends and the processes left running have all
  // started: from here on the map changes only on their cycles.
  int64_t settled = 0;
  for (const NabtsPaletteEvent& event : timeline.events) {
    settled = std::max(settled, event.time_ms);
  }
  settled = std::max<int64_t>(settled, timeline.end_ms);
  for (const NabtsBlinkProcess& process : timeline.blinks) {
    if (process.ended_ms == kNabtsNeverMs) {
      settled =
          std::max(settled, std::max(process.defined_ms, process.first_on_ms));
    }
  }

  // The processes still running at the end, grouped by the entries they join,
  // and each group's loop. A process joins its two entries only where the
  // blink-to entry is itself being blinked: one that nothing changes any more
  // is a constant, and two processes reading it cycle independently.
  std::array<bool, kNabtsColourMapEntries> blinked{};
  bool any_running = false;
  for (const NabtsBlinkProcess& process : timeline.blinks) {
    if (process.ended_ms == kNabtsNeverMs) {
      blinked[process.from % kNabtsColourMapEntries] = true;
      any_running = true;
    }
  }
  EntryGroups groups;
  for (const NabtsBlinkProcess& process : timeline.blinks) {
    if (process.ended_ms != kNabtsNeverMs || process.to_address < 0) {
      continue;
    }
    const size_t to =
        static_cast<size_t>(process.to_address) % kNabtsColourMapEntries;
    if (blinked[to]) {
      groups.join(process.from % kNabtsColourMapEntries, to);
    }
  }
  std::array<int64_t, kNabtsColourMapEntries> group_loop{};
  if (any_running) {
    std::array<std::vector<int64_t>, kNabtsColourMapEntries> periods;
    for (const NabtsBlinkProcess& process : timeline.blinks) {
      if (process.ended_ms == kNabtsNeverMs && process.period_ms() > 0) {
        periods[groups.find(process.from % kNabtsColourMapEntries)].push_back(
            process.period_ms());
      }
    }
    for (size_t root = 0; root < kNabtsColourMapEntries; ++root) {
      if (!periods[root].empty()) {
        group_loop[root] = loop_length(periods[root], out.approximated);
      }
    }
  }
  int64_t longest_loop = 0;
  for (const int64_t loop : group_loop) {
    longest_loop = std::max(longest_loop, loop);
  }
  const int64_t horizon = settled + (kLoopAttempts + 1) * longest_loop;

  // Run it, noting every change from the start of the presentation on.
  Simulation simulation(timeline);
  std::array<std::vector<NaplpsColourStep>, kNabtsColourMapEntries> steps;
  size_t next_event = 0;
  bool recording = false;
  const auto record = [&](int64_t time) {
    for (size_t entry = 0; entry < kNabtsColourMapEntries; ++entry) {
      std::vector<NaplpsColourStep>& track = steps[entry];
      const NabtsColour& colour = simulation.live(entry);
      if (!track.empty() && track.back().colour == colour) {
        continue;
      }
      if (track.size() >= kMaxStepsPerTrack) {
        out.approximated = true;
        continue;
      }
      track.push_back(NaplpsColourStep{
          static_cast<uint32_t>(std::max<int64_t>(time, 0)), colour});
    }
  };

  int64_t time = start;
  while (true) {
    const int64_t event_time =
        next_event < timeline.events.size()
            ? std::max(timeline.events[next_event].time_ms, start)
            : kNabtsNeverMs;
    const int64_t transition_time = simulation.next_transition();
    const int64_t now = std::min(event_time, transition_time);
    // The first instant at or after zero, and every change after it: the state
    // the presentation opens on is whatever has happened by then.
    if (!recording && now > 0) {
      record(0);
      recording = true;
    }
    if (now == kNabtsNeverMs || now > horizon) {
      break;
    }
    time = now;
    while (next_event < timeline.events.size() &&
           std::max(timeline.events[next_event].time_ms, start) == time) {
      simulation.apply(timeline.events[next_event], time);
      ++next_event;
    }
    simulation.run_transitions(time);
    if (recording) {
      record(time);
    }
  }
  if (!recording) {
    record(0);
  }

  // Each running group's loop: the first whole cycle after the page settled
  // that leaves its entries as it found them.
  std::array<int64_t, kNabtsColourMapEntries> loop_start{};
  for (size_t root = 0; root < kNabtsColourMapEntries; ++root) {
    const int64_t loop = group_loop[root];
    if (loop == 0) {
      continue;
    }
    int64_t found = -1;
    for (int attempt = 0; attempt < kLoopAttempts && found < 0; ++attempt) {
      const int64_t from = settled + attempt * loop;
      bool repeats = true;
      for (size_t entry = 0; entry < kNabtsColourMapEntries && repeats;
           ++entry) {
        if (groups.find(entry) == root) {
          repeats = colour_at(steps[entry], from) ==
                    colour_at(steps[entry], from + loop);
        }
      }
      if (repeats) {
        found = from;
      }
    }
    if (found < 0) {
      out.approximated = true;
      found = settled + (kLoopAttempts - 1) * loop;
    }
    loop_start[root] = found;
  }

  for (size_t entry = 0; entry < kNabtsColourMapEntries; ++entry) {
    const std::vector<NaplpsColourStep>& all = steps[entry];
    NaplpsPaletteTrack& track = out.tracks[entry];
    const size_t root = groups.find(entry);
    const int64_t loop = group_loop[root];
    if (loop == 0) {
      track.steps = all;
    } else {
      // The steps up to the loop, a step exactly where it starts, and the one
      // cycle it repeats.
      const int64_t from = loop_start[root];
      for (const NaplpsColourStep& step : all) {
        if (static_cast<int64_t>(step.time_ms) >= from) {
          break;
        }
        track.steps.push_back(step);
      }
      track.steps.push_back(
          NaplpsColourStep{static_cast<uint32_t>(from), colour_at(all, from)});
      for (const NaplpsColourStep& step : all) {
        const auto at = static_cast<int64_t>(step.time_ms);
        if (at > from && at < from + loop) {
          track.steps.push_back(step);
        }
      }
      track.loop_start_ms = static_cast<uint32_t>(from);
      track.loop_period_ms = static_cast<uint32_t>(loop);
    }

    // An entry that shows one colour throughout does not change, however many
    // times it was written or blinked to the colour it already had.
    const bool one_colour =
        std::all_of(track.steps.begin(), track.steps.end(),
                    [&track](const NaplpsColourStep& step) {
                      return step.colour == track.steps.front().colour;
                    });
    if (one_colour) {
      track = NaplpsPaletteTrack{};
    }
  }
  return out;
}

}  // namespace orc
