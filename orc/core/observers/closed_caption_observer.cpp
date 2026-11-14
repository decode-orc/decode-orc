/*
 * File:        closed_caption_observer.cpp
 * Module:      orc-core
 * Purpose:     Closed caption observer implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <closed_caption_observer.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>
#include <orc/support/vbi_utilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace orc {

namespace {

// Line 21 of each field, 0-based within the field [CTA-608-E §4.1]; the
// second field's lines follow the first's in the frame.
constexpr size_t kLine21Index = 20;

// The lines either side of it, read where line 21 carried no pair, line 22
// first: a recording whose time base slipped a line carries its captions
// there: on line 22 only, or on lines 20, 21 and 22 by turns.
constexpr std::array<std::pair<size_t, int32_t>, 2> kNeighbourLines{
    {{21, 22}, {19, 20}}};

// CTA-608-E §4, Table 2: the bit period is 1 / (32 fH), so a line holds 32
// bit periods whatever the sampling rate.
constexpr double kBitsPerLine = 32.0;
// SMPTE 170M-2004: the 525-line line period, in microseconds.
constexpr double kLineMicroseconds = 63.5556;
// A line too short to hold the run-in, the start bits and both bytes.
constexpr size_t kMinLineSamples = 64;

constexpr double kTwoPi = 6.283185307179586;

// The clock run-in is seven cycles of a sine at the bit rate, starting 10.5 us
// after 0H [CTA-608-E Table 2]; the decode-orc test corpus puts it 9.6 to 11.1
// us after 0H, and a few stretches at 7.75 to 8 us (see
// kEarliestRunInMicroseconds). It is measured over five whole cycles from 12
// us, inside it on all of them but those few, whose first start bit takes the
// window's last 8 %: a whole number of cycles so that the window's mean is the
// run-in's midpoint, the slicing level for the data [CTA-608-E Table 2 notes 2
// and 7: the run-in is symmetrical about the data's midpoint, its extremes
// within 1 IRE of the data levels].
constexpr double kRunInWindowStartMicroseconds = 12.0;
constexpr double kRunInWindowCycles = 5.0;

// How much of the window's variance the bit-rate tone accounts for: its
// purity. A clean run-in is a sine, purity 1; recorded ones keep 0.9 and more
// on nearly every line. A line carrying noise, picture, a test signal or
// another service's data keeps a few percent, however its noise slices. Of the
// 3.1 million lines of the decode-orc test corpus whose pair two independent
// decoders agree on, two fall under a quarter and 140 between a quarter and a
// half (a dropout in the run-in) — where, also, the few lines that are not
// line 21 data but slice into a start bit sit. So a line under a half is read
// only when every one of its data bits is clear of the slicing level
// (kMinWeakBitClearance): real data is, a slice of noise with a trace of tone
// hardly ever.
constexpr double kMinRunInPurity = 0.25;
constexpr double kClearRunInPurity = 0.5;
constexpr double kMinWeakBitClearance = 0.3;

// The 1 start bit begins 8.75 bit periods after the run-in starts: the run-in's
// negative-going midpoints are coherent with the bit transitions [CTA-608-E
// Table 2 note 3], the last of them 6.75 periods in, and two 0 start bits
// follow it. The run-in is looked for from 7 to 12 us after 0H. Eight tapes
// measured carry stretches of captions 2.6 to 3.1 us ahead of the rest, at
// 7.75 to 8 us, most of them in one advertisement; looked for from 8.5 us, 390
// of their pairs were lost, 177 of which cc_decoder (the Unlicense; it looks
// for the run-in along the whole line) or etv's decoder reads the same.
constexpr double kStartBitOffsetBits = 8.75;
constexpr double kEarliestRunInMicroseconds = 7.0;
constexpr double kLatestRunInMicroseconds = 12.0;

// A bit is read as the mean of the central half of its cell, clear of the
// ringing of its neighbours' edges.
constexpr double kCellMarginBits = 0.25;

// The start bits must read 0, 0, 1 clearly: each a quarter of the run-in's
// amplitude or more from its midpoint.
constexpr double kMinStartBitLevel = 0.25;

// Data bits after the 1 start bit: two bytes of seven data bits and an odd
// parity bit, least significant bit first [CTA-608-E §4].
constexpr int kDataBits = 16;

// The last data bit ends 25.75 bit periods after the run-in starts, 61.6 us
// after 0H for a run-in at 10.5 us [CTA-608-E Table 2] — where captures stop
// the line: vbi_source writes a crop's records up to sample 873 (61.0 us), a
// 720-sample BT.601 picture ends at 62.4 us. So the last bit is read from the
// early part of its cell, and not past the line's trailing fill: the run of
// samples equal to its last one that a capture which stops short pads it
// with. On the decode-orc test corpus this keeps 587 pairs whose last bit the
// central half of its cell would have read from the fill.
constexpr double kLastCellFromBits = 0.15;
constexpr double kLastCellToBits = 0.5;
constexpr size_t kMinFillSamples = 4;
constexpr size_t kMinCellSamples = 3;

// Mean of the samples in the central half of the bit cell starting at |start|.
double cell_level(const int16_t* samples, size_t count, double start,
                  double bit) {
  const auto first =
      static_cast<size_t>(std::ceil(start + kCellMarginBits * bit));
  const auto last = std::min(
      count,
      static_cast<size_t>(std::floor(start + (1.0 - kCellMarginBits) * bit)) +
          1);
  if (last <= first) {
    return 0.0;
  }
  double sum = 0.0;
  for (size_t i = first; i < last; ++i) {
    sum += samples[i];
  }
  return sum / static_cast<double>(last - first);
}

// The last data bit's level: the mean over [start + kLastCellFromBits bit,
// start + kLastCellToBits bit), clipped at |signal_end| but never to fewer than
// kMinCellSamples samples.
double last_cell_level(const int16_t* samples, size_t count, double start,
                       double bit, size_t signal_end) {
  const auto first = std::min(
      count, static_cast<size_t>(std::ceil(start + kLastCellFromBits * bit)));
  size_t last = std::min(
      signal_end,
      static_cast<size_t>(std::floor(start + kLastCellToBits * bit)) + 1);
  last = std::min(count, std::max(last, first + kMinCellSamples));
  if (last <= first) {
    return 0.0;
  }
  double sum = 0.0;
  for (size_t i = first; i < last; ++i) {
    sum += samples[i];
  }
  return sum / static_cast<double>(last - first);
}

}  // namespace

std::optional<ClosedCaptionObserver::Line21Pair>
ClosedCaptionObserver::read_line21(const int16_t* samples, size_t count) {
  if (samples == nullptr || count < kMinLineSamples) {
    return std::nullopt;
  }
  const double bit = static_cast<double>(count) / kBitsPerLine;
  const double per_microsecond = static_cast<double>(count) / kLineMicroseconds;

  // The run-in's midpoint, and its tone's amplitude and phase.
  const auto window_start = static_cast<size_t>(
      std::lround(kRunInWindowStartMicroseconds * per_microsecond));
  const auto window_length =
      static_cast<size_t>(std::lround(kRunInWindowCycles * bit));
  if (window_length == 0 || window_start + window_length > count) {
    return std::nullopt;
  }
  double sum = 0.0;
  for (size_t i = window_start; i < window_start + window_length; ++i) {
    sum += samples[i];
  }
  const double midpoint = sum / static_cast<double>(window_length);

  // The tone at the bit rate: the oscillator is stepped by rotation, the
  // window being far too short for its rounding to drift.
  const double step = kTwoPi / bit;
  const double step_cos = std::cos(step);
  const double step_sin = std::sin(step);
  double osc_cos = std::cos(step * static_cast<double>(window_start));
  double osc_sin = std::sin(step * static_cast<double>(window_start));
  double in_phase = 0.0;
  double quadrature = 0.0;
  double energy = 0.0;
  for (size_t i = window_start; i < window_start + window_length; ++i) {
    const double value = samples[i] - midpoint;
    in_phase += value * osc_cos;
    quadrature += value * osc_sin;
    energy += value * value;
    const double next_cos = osc_cos * step_cos - osc_sin * step_sin;
    osc_sin = osc_sin * step_cos + osc_cos * step_sin;
    osc_cos = next_cos;
  }
  const double length = static_cast<double>(window_length);
  const double amplitude = 2.0 * std::hypot(in_phase, quadrature) / length;
  // The tone's share of the window's variance: (A^2 / 2) / (energy / n).
  if (!(energy > 0.0) ||
      amplitude * amplitude * length < 2.0 * kMinRunInPurity * energy) {
    return std::nullopt;
  }

  // The window holds midpoint + A cos(step t + phase); its negative-going
  // midpoints, where step t + phase = pi / 2, are the bit boundaries.
  const double phase = std::atan2(-quadrature, in_phase);
  double boundary = std::fmod((kTwoPi / 4.0 - phase) / step, bit);
  if (boundary < 0.0) {
    boundary += bit;
  }

  // The 1 start bit: of the boundaries a run-in starting within the search
  // range allows, the one after which the line reads 0, 0, 1 most strongly.
  const double earliest =
      kEarliestRunInMicroseconds * per_microsecond + kStartBitOffsetBits * bit;
  const double latest =
      kLatestRunInMicroseconds * per_microsecond + kStartBitOffsetBits * bit;
  double start = boundary + std::ceil((earliest - boundary) / bit) * bit;
  double best_score = -std::numeric_limits<double>::infinity();
  double start_bit = 0.0;
  for (; start <= latest; start += bit) {
    const double score =
        (midpoint - cell_level(samples, count, start - 2.0 * bit, bit)) +
        (midpoint - cell_level(samples, count, start - bit, bit)) +
        (cell_level(samples, count, start, bit) - midpoint);
    if (score > best_score) {
      best_score = score;
      start_bit = start;
    }
  }
  if (!(best_score > -std::numeric_limits<double>::infinity()) ||
      start_bit + (kDataBits + 1) * bit > static_cast<double>(count)) {
    return std::nullopt;
  }
  const double low =
      std::max(cell_level(samples, count, start_bit - 2.0 * bit, bit),
               cell_level(samples, count, start_bit - bit, bit));
  const double high = cell_level(samples, count, start_bit, bit);
  const double clearance = kMinStartBitLevel * amplitude;
  if (midpoint - low < clearance || high - midpoint < clearance) {
    return std::nullopt;
  }

  // Where the line's trailing fill begins, if it has one.
  size_t signal_end = count;
  while (signal_end > 0 && samples[signal_end - 1] == samples[count - 1]) {
    --signal_end;
  }
  if (count - signal_end < kMinFillSamples) {
    signal_end = count;
  }

  // Sixteen bits, sliced at the run-in's midpoint; on a line whose run-in is
  // weak, each must be clear of it.
  const bool weak =
      amplitude * amplitude * length < 2.0 * kClearRunInPurity * energy;
  const double min_clearance = kMinWeakBitClearance * amplitude;
  uint32_t bits = 0;
  for (int b = 0; b < kDataBits; ++b) {
    const double cell = start_bit + static_cast<double>(b + 1) * bit;
    const double level =
        b + 1 < kDataBits
            ? cell_level(samples, count, cell, bit)
            : last_cell_level(samples, count, cell, bit, signal_end);
    if (weak && std::abs(level - midpoint) < min_clearance) {
      return std::nullopt;
    }
    if (level > midpoint) {
      bits |= 1U << static_cast<unsigned>(b);
    }
  }
  const uint32_t byte0 = bits & 0xFFU;
  const uint32_t byte1 = (bits >> 8U) & 0xFFU;
  Line21Pair pair;
  pair.data0 = static_cast<uint8_t>(byte0 & 0x7FU);
  pair.data1 = static_cast<uint8_t>(byte1 & 0x7FU);
  // Odd parity over all eight bits [CTA-608-E §4, Table 2 note 5].
  pair.parity0_valid = !vbi_utils::is_even_parity(byte0);
  pair.parity1_valid = !vbi_utils::is_even_parity(byte1);
  return pair;
}

void ClosedCaptionObserver::process_frame(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    IObservationContext& context) {
  auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    ORC_LOG_DEBUG("Frame {}: No video parameters available", frame_id);
    return;
  }
  const auto& vp = vp_opt.value();

  // Captions are 525-line only (see applies_to()); callers normally skip the
  // observer entirely, this early-return is defence in depth.
  if (!applies_to(vp)) {
    return;
  }

  const size_t f1_lines = field1_lines(vp.system);
  const auto line_width = static_cast<size_t>(vp.frame_width_nominal);

  for (size_t field_idx = 0; field_idx < 2; ++field_idx) {
    const FieldID field_id(frame_id * 2 + field_idx);
    const size_t line_offset = (field_idx == 0) ? 0 : f1_lines;
    const size_t field_height =
        (field_idx == 0) ? f1_lines
                         : static_cast<size_t>(vp.frame_height) - f1_lines;

    std::optional<Line21Pair> pair;
    if (kLine21Index < field_height) {
      const int16_t* line =
          representation.get_line(frame_id, line_offset + kLine21Index);
      if (line != nullptr) {
        pair = read_line21(line, line_width);
      }
    }

    context.set(field_id, "closed_caption", "present", pair.has_value());
    if (pair.has_value()) {
      context.set(field_id, "closed_caption", "data0",
                  static_cast<int32_t>(pair->data0));
      context.set(field_id, "closed_caption", "data1",
                  static_cast<int32_t>(pair->data1));
      context.set(field_id, "closed_caption", "parity0_valid",
                  pair->parity0_valid);
      context.set(field_id, "closed_caption", "parity1_valid",
                  pair->parity1_valid);
      continue;
    }

    // Line 21 carried nothing: the line next to it may, where the time base
    // slipped a line (kNeighbourLines).
    for (const auto& [index, number] : kNeighbourLines) {
      if (index >= field_height) {
        continue;
      }
      const int16_t* line =
          representation.get_line(frame_id, line_offset + index);
      if (line == nullptr) {
        continue;
      }
      const std::optional<Line21Pair> other = read_line21(line, line_width);
      if (!other.has_value()) {
        continue;
      }
      context.set(field_id, "closed_caption", "other_line", number);
      context.set(field_id, "closed_caption", "other_data0",
                  static_cast<int32_t>(other->data0));
      context.set(field_id, "closed_caption", "other_data1",
                  static_cast<int32_t>(other->data1));
      context.set(field_id, "closed_caption", "other_parity0_valid",
                  other->parity0_valid);
      context.set(field_id, "closed_caption", "other_parity1_valid",
                  other->parity1_valid);
      break;
    }
  }
}

std::vector<ObservationKey> ClosedCaptionObserver::get_provided_observations()
    const {
  return {
      {"closed_caption", "present", ObservationType::BOOL,
       "Line 21 waveform found and its byte pair read", true},
      {"closed_caption", "data0", ObservationType::INT32,
       "First EIA-608 byte (7 data bits)", true},
      {"closed_caption", "data1", ObservationType::INT32,
       "Second EIA-608 byte (7 data bits)", true},
      {"closed_caption", "parity0_valid", ObservationType::BOOL,
       "Odd parity over all eight bits of the first byte", true},
      {"closed_caption", "parity1_valid", ObservationType::BOOL,
       "Odd parity over all eight bits of the second byte", true},
      {"closed_caption", "other_line", ObservationType::INT32,
       "Where line 21 carried no pair, the line (22 or 20) that did", true},
      {"closed_caption", "other_data0", ObservationType::INT32,
       "First EIA-608 byte of that line (7 data bits)", true},
      {"closed_caption", "other_data1", ObservationType::INT32,
       "Second EIA-608 byte of that line (7 data bits)", true},
      {"closed_caption", "other_parity0_valid", ObservationType::BOOL,
       "Odd parity of that line's first byte", true},
      {"closed_caption", "other_parity1_valid", ObservationType::BOOL,
       "Odd parity of that line's second byte", true},
  };
}

}  // namespace orc
