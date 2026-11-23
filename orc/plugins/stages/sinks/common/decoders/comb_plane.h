/*
 * File:        comb_plane.h
 * Module:      orc-core
 * Purpose:     Whole-frame sample planes and the filters the NTSC 3D comb
 *              builds its measurements from
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef COMB_PLANE_H
#define COMB_PLANE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace ntsc_comb {

// The frame's geometry in field-separated ("flat") rows: the first field's
// lines, top to bottom, then the second field's. A row's neighbours in the
// same field are the rows next to it, so a filter running down a field moves
// one row at a time.
struct FieldLayout {
  int32_t rows = 0;        // all lines of the frame
  int32_t cols = 0;        // samples per line
  int32_t field1Rows = 0;  // lines of the first field

  // The flat row holding interlaced frame line |frameLine|.
  int32_t rowOf(int32_t frameLine) const {
    return (frameLine / 2) + ((frameLine % 2 == 0) ? 0 : field1Rows);
  }
  bool sameField(int32_t a, int32_t b) const {
    return (a < field1Rows) == (b < field1Rows);
  }
};

// The active picture, in flat rows of each field and samples of each line.
struct ActivePicture {
  int32_t field1First = 0, field1End = 0;
  int32_t field2First = 0, field2End = 0;
  int32_t sampleStart = 0, sampleEnd = 0;

  bool containsRow(int32_t row) const {
    return (row >= field1First && row < field1End) ||
           (row >= field2First && row < field2End);
  }
  bool contains(int32_t row, int32_t col) const {
    return containsRow(row) && col >= sampleStart && col < sampleEnd;
  }

  // The picture between interlaced frame lines [first, last) and samples
  // [start, end) of a frame laid out as |layout|.
  static ActivePicture fromFrameLines(const FieldLayout& layout, int32_t first,
                                      int32_t last, int32_t start,
                                      int32_t end) {
    ActivePicture p;
    p.field1First = first / 2;
    p.field1End = std::min(layout.field1Rows, (last + 1) / 2);
    p.field2First = layout.field1Rows + (first / 2);
    p.field2End =
        layout.field1Rows + std::min(layout.rows - layout.field1Rows, last / 2);
    p.sampleStart = start;
    p.sampleEnd = end;
    return p;
  }
};

// The comb's planes store single precision: they are streamed through dozens
// of passes per frame, and memory bandwidth, not arithmetic, bounds the
// comb. Sums and filters accumulate in double.
using Sample = float;

namespace plane_storage {
// The comb builds and drops dozens of whole-frame planes for every frame;
// fresh allocations that size are mapped, page-faulted and zero-filled each
// time, which costs more than the arithmetic. Planes therefore hand their
// storage back to a store for the thread on destruction and take it from
// there again. At most kKept buffers are kept per thread (the 3D comb has
// about 16 planes live at its peak).
constexpr size_t kKept = 24;

inline std::vector<std::vector<Sample>>& store() {
  thread_local std::vector<std::vector<Sample>> buffers;
  return buffers;
}

// A buffer of |size| samples, contents unspecified: the most recent kept
// buffer of that size, or else the smallest that holds it (planes of a
// cropped frame and of the whole frame share the store).
inline std::vector<Sample> take(size_t size) {
  auto& buffers = store();
  auto best = buffers.rend();
  for (auto it = buffers.rbegin(); it != buffers.rend(); ++it) {
    if (it->size() == size) {
      best = it;
      break;
    }
    if (it->capacity() >= size &&
        (best == buffers.rend() || it->capacity() < best->capacity())) {
      best = it;
    }
  }
  if (best == buffers.rend()) return std::vector<Sample>(size);
  std::vector<Sample> buffer = std::move(*best);
  buffers.erase(std::next(best).base());
  buffer.resize(size);
  return buffer;
}

inline void give(std::vector<Sample>&& buffer) {
  if (buffer.empty()) return;
  auto& buffers = store();
  if (buffers.size() < kKept) buffers.push_back(std::move(buffer));
}
}  // namespace plane_storage

// A whole frame of samples in flat rows; a plane holds rows * cols of them.
// Storage is recycled within the thread (see plane_storage), so a plane made
// with Plane(rows, cols) has unspecified contents: every sample must be
// written.
struct Plane {
  int32_t rows = 0;
  int32_t cols = 0;
  std::vector<Sample> v;

  Plane() = default;
  Plane(int32_t r, int32_t c)
      : rows(r), cols(c), v(plane_storage::take(static_cast<size_t>(r) * c)) {}
  Plane(int32_t r, int32_t c, double fill) : Plane(r, c) {
    std::fill(v.begin(), v.end(), static_cast<Sample>(fill));
  }
  Plane(const Plane& other) : Plane(other.rows, other.cols) {
    std::copy(other.v.begin(), other.v.end(), v.begin());
  }
  Plane(Plane&& other) noexcept
      : rows(other.rows), cols(other.cols), v(std::move(other.v)) {
    other.rows = other.cols = 0;
  }
  Plane& operator=(const Plane& other) {
    if (this != &other) {
      if (v.size() != other.v.size()) {
        plane_storage::give(std::move(v));
        v = plane_storage::take(other.v.size());
      }
      rows = other.rows;
      cols = other.cols;
      std::copy(other.v.begin(), other.v.end(), v.begin());
    }
    return *this;
  }
  Plane& operator=(Plane&& other) noexcept {
    if (this != &other) {
      plane_storage::give(std::move(v));
      rows = other.rows;
      cols = other.cols;
      v = std::move(other.v);
      other.rows = other.cols = 0;
    }
    return *this;
  }
  ~Plane() { plane_storage::give(std::move(v)); }

  double at(int32_t r, int32_t c) const {
    return v[static_cast<size_t>(r) * cols + c];
  }
  void set(int32_t r, int32_t c, double value) {
    v[static_cast<size_t>(r) * cols + c] = static_cast<Sample>(value);
  }
  Sample* row(int32_t r) { return v.data() + static_cast<size_t>(r) * cols; }
  const Sample* row(int32_t r) const {
    return v.data() + static_cast<size_t>(r) * cols;
  }
  bool empty() const { return v.empty(); }
};

// The 1D chroma bandpass [-1/4, 0, 1/2, 0, -1/4] along every line: unity gain
// at the subcarrier (a quarter of the 4fsc rate), none at DC. The two samples
// at each end of a line are left at zero.
inline void bandpassLine(const Sample* in, Sample* o, int32_t cols) {
  for (int32_t c = 0; c < std::min(2, cols); c++) o[c] = 0.0F;
  for (int32_t c = std::max(2, cols - 2); c < cols; c++) o[c] = 0.0F;
  for (int32_t c = 2; c < cols - 2; c++) {
    const double value =
        (static_cast<double>(in[c]) -
         ((static_cast<double>(in[c - 2]) + static_cast<double>(in[c + 2])) /
          2.0)) /
        2.0;
    o[c] = static_cast<Sample>(value);
  }
}

inline Plane bandpass(const Plane& x) {
  Plane out(x.rows, x.cols);
  for (int32_t r = 0; r < x.rows; r++)
    bandpassLine(x.row(r), out.row(r), x.cols);
  return out;
}

// The mean of |width| samples of a line around each sample, the line's ends
// extended: sample c's window is [c - width / 2, c - width / 2 + width).
// Each window is summed afresh (no running sum), so the samples are
// independent and the compiler can vectorise across them.
template <int32_t Width>
void boxAlongLine(const Sample* in, Sample* out, int32_t cols) {
  constexpr int32_t kBefore = Width / 2;
  const int32_t last = cols - 1;
  const auto count = static_cast<double>(Width);
  auto clamped = [&](int32_t c) {
    double total = 0.0;
    for (int32_t k = c - kBefore; k < c - kBefore + Width; k++) {
      total += static_cast<double>(in[std::clamp(k, 0, last)]);
    }
    out[c] = static_cast<Sample>(total / count);
  };
  // Samples whose window lies inside the line
  const int32_t first = std::min(kBefore, cols);
  const int32_t end = std::max(first, cols - Width + kBefore + 1);
  for (int32_t c = 0; c < first; c++) clamped(c);
  for (int32_t c = first; c < end; c++) {
    const Sample* window = in + (c - kBefore);
    double total = 0.0;
    for (int32_t k = 0; k < Width; k++) {
      total += static_cast<double>(window[k]);
    }
    out[c] = static_cast<Sample>(total / count);
  }
  for (int32_t c = end; c < cols; c++) clamped(c);
}

inline void boxAlongLine(const Sample* in, Sample* out, int32_t cols,
                         int32_t width) {
  switch (width) {
    case 4:
      boxAlongLine<4>(in, out, cols);
      return;
    case 5:
      boxAlongLine<5>(in, out, cols);
      return;
    case 9:
      boxAlongLine<9>(in, out, cols);
      return;
    default:
      break;
  }
  // Other widths: a running sum
  if (width <= 1) {
    std::copy(in, in + cols, out);
    return;
  }
  const int32_t before = width / 2;
  const int32_t last = cols - 1;
  const auto count = static_cast<double>(width);
  double total = 0.0;
  for (int32_t k = -before; k < width - before; k++) {
    total += static_cast<double>(in[std::clamp(k, 0, last)]);
  }
  out[0] = static_cast<Sample>(total / count);
  for (int32_t c = 1; c < cols; c++) {
    total +=
        static_cast<double>(in[std::clamp(c - before + width - 1, 0, last)]) -
        static_cast<double>(in[std::clamp(c - 1 - before, 0, last)]);
    out[c] = static_cast<Sample>(total / count);
  }
}

// The mean of each sample's neighbourhood in a plane of |rows| x |cols|:
// |width| samples along the line by |lines| lines of the same field (fields
// are never mixed), the edges extended. Even widths take one more sample
// before than after. A width of 4 has a null at the subcarrier (and twice
// it), so a 4 x 1 box of a frame is the composite with its chroma removed.
//
// The plane averaged is never stored: |input|(r, scratch) gives its row r,
// returning either a row it already holds or |scratch| (|cols| samples)
// after writing the row there. Each row is asked for once, as the lines'
// windows reach it, and only the |lines| rows a window needs are kept.
template <typename RowSource>
Plane boxMeanOf(int32_t rows, int32_t cols, int32_t width, int32_t lines,
                int32_t field1Rows, RowSource input) {
  Plane out(rows, cols);
  std::vector<Sample> scratch(static_cast<size_t>(cols));
  if (lines <= 1) {
    for (int32_t r = 0; r < rows; r++) {
      boxAlongLine(input(r, scratch.data()), out.row(r), cols, width);
    }
    return out;
  }

  // The window's rows, averaged along the line, each in slot (row % slots).
  const int32_t slots = lines + 1;
  std::vector<Sample> across(static_cast<size_t>(slots) * cols);
  std::vector<int32_t> held(static_cast<size_t>(slots), -1);
  auto acrossRow = [&](int32_t k) -> const Sample* {
    const auto slot = static_cast<size_t>(k % slots);
    Sample* row = across.data() + (slot * cols);
    if (held[slot] != k) {
      boxAlongLine(input(k, scratch.data()), row, cols, width);
      held[slot] = k;
    }
    return row;
  };

  const int32_t before = lines / 2;
  const auto count = static_cast<double>(lines);
  std::vector<const Sample*> window(static_cast<size_t>(lines));
  auto field = [&](int32_t first, int32_t end) {
    for (int32_t r = first; r < end; r++) {
      for (int32_t k = 0; k < lines; k++) {
        window[k] = acrossRow(std::clamp(r - before + k, first, end - 1));
      }
      Sample* o = out.row(r);
      for (int32_t c = 0; c < cols; c++) {
        double total = 0.0;
        for (int32_t k = 0; k < lines; k++) {
          total += static_cast<double>(window[k][c]);
        }
        o[c] = static_cast<Sample>(total / count);
      }
    }
  };
  field(0, field1Rows);
  field(field1Rows, rows);
  return out;
}

// boxMeanOf a whole plane.
inline Plane boxMean(const Plane& x, int32_t width, int32_t lines,
                     int32_t field1Rows) {
  return boxMeanOf(x.rows, x.cols, width, lines, field1Rows,
                   [&](int32_t r, Sample*) { return x.row(r); });
}

// The local mean of |f(row)| for a chroma-band signal, over whole subcarrier
// cycles (4 samples) by 3 lines: |x| alternates between the I and Q samples,
// so a box of odd width would ripple from one sample to the next, and so
// would every weight taken from it. |row|(r, scratch) gives the signal's row
// r as boxMeanOf's input does.
template <typename RowSource>
Plane cycleMeanOf(int32_t rows, int32_t cols, int32_t field1Rows,
                  RowSource row) {
  return boxMeanOf(rows, cols, 4, 3, field1Rows,
                   [&](int32_t r, Sample* scratch) {
                     const Sample* in = row(r, scratch);
                     for (int32_t c = 0; c < cols; c++) {
                       scratch[c] = std::fabs(in[c]);
                     }
                     return static_cast<const Sample*>(scratch);
                   });
}

inline Plane cycleMean(const Plane& x, int32_t field1Rows) {
  return cycleMeanOf(x.rows, x.cols, field1Rows,
                     [&](int32_t r, Sample*) { return x.row(r); });
}

// The |q|th percentile of |values| (0..100), interpolating linearly between
// the two nearest ranks; values is reordered. 0 for no values.
inline double percentile(std::vector<double>& values, double q) {
  if (values.empty()) return 0.0;
  const double position = (q / 100.0) * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<size_t>(std::floor(position));
  const double fraction = position - static_cast<double>(lower);
  std::nth_element(values.begin(), values.begin() + lower, values.end());
  const double a = values[lower];
  if (fraction == 0.0 || lower + 1 >= values.size()) return a;
  const double b = *std::min_element(values.begin() + lower + 1, values.end());
  return a + (fraction * (b - a));
}

}  // namespace ntsc_comb

#endif  // COMB_PLANE_H
