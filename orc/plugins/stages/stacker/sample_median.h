/*
 * File:        sample_median.h
 * Module:      orc-core
 * Purpose:     Medians of a few samples by fixed sorting networks, for the
 *              Stacker's per-sample combine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace orc {

// The stacker takes a median per output sample, of a value per input (up to
// 16) or per input and still-frame copy.  A sort or selection that branches
// on each comparison mispredicts about half of them on noisy samples; a
// sorting network makes the same comparisons whatever the values are, so it
// compiles to min/max without branches.  Everything here is defined in the
// header, and the entry points are static: plugins build with default
// visibility, so a function the compiler does not inline, with external
// linkage, would cost a PLT call per sample.

// The largest count sorted by a network; larger sets fall back to
// std::nth_element.
inline constexpr size_t kMaxNetworkValues = 32;

// A comparator network on |count| values: comparator k orders the values at
// first[k] and second[k] so the smaller is at first[k].
struct SortingNetwork {
  static constexpr size_t kMaxComparators = 191;  // Batcher, 32 inputs
  std::array<uint8_t, kMaxComparators> first{};
  std::array<uint8_t, kMaxComparators> second{};
  size_t size = 0;
};

// Batcher's odd-even merge sort for 32 values, keeping only the comparators
// between the first |count| positions.  Dropping the rest is the network of
// |count| values followed by padding that sorts after every value, which
// those comparators never move, so it sorts |count| values.  When
// |middle_only|, only the comparators that the middle position (the two
// middle positions for an even count) depend on are kept: the values there
// are the sorted values', the rest are not sorted.
constexpr SortingNetwork make_sorting_network(size_t count, bool middle_only) {
  SortingNetwork all;
  constexpr size_t kPadded = kMaxNetworkValues;
  for (size_t p = 1; p < kPadded; p <<= 1U) {
    for (size_t k = p; k >= 1; k >>= 1U) {
      for (size_t j = k % p; j + k < kPadded; j += 2 * k) {
        for (size_t i = 0; i < k && i + j + k < kPadded; ++i) {
          const size_t a = i + j;
          const size_t b = i + j + k;
          if (a / (2 * p) != b / (2 * p) || b >= count) continue;
          all.first[all.size] = static_cast<uint8_t>(a);
          all.second[all.size] = static_cast<uint8_t>(b);
          ++all.size;
        }
      }
    }
  }
  if (!middle_only || count == 0) return all;

  // Walk back from the outputs wanted, keeping each comparator that writes a
  // position still wanted; both of its inputs are then wanted.
  std::array<bool, kPadded> wanted{};
  wanted[count / 2] = true;
  if (count % 2 == 0) wanted[(count / 2) - 1] = true;
  std::array<bool, SortingNetwork::kMaxComparators> keep{};
  for (size_t c = all.size; c-- > 0;) {
    if (!wanted[all.first[c]] && !wanted[all.second[c]]) continue;
    keep[c] = true;
    wanted[all.first[c]] = true;
    wanted[all.second[c]] = true;
  }
  SortingNetwork pruned;
  for (size_t c = 0; c < all.size; ++c) {
    if (!keep[c]) continue;
    pruned.first[pruned.size] = all.first[c];
    pruned.second[pruned.size] = all.second[c];
    ++pruned.size;
  }
  return pruned;
}

namespace sample_median_detail {

template <size_t N, bool MiddleOnly>
inline constexpr SortingNetwork kNetwork = make_sorting_network(N, MiddleOnly);

template <typename T, size_t A, size_t B>
inline void compare_exchange(T* v) {
  const T a = v[A];
  const T b = v[B];
  v[A] = std::min(a, b);
  v[B] = std::max(a, b);
}

template <typename T, size_t N, bool MiddleOnly, size_t... C>
inline void apply_network(T* v, std::index_sequence<C...> /*unused*/) {
  constexpr const SortingNetwork& net = kNetwork<N, MiddleOnly>;
  (compare_exchange<T, net.first[C], net.second[C]>(v), ...);
}

template <typename T, size_t N, bool MiddleOnly>
inline void run_network(T* v) {
  apply_network<T, N, MiddleOnly>(
      v, std::make_index_sequence<kNetwork<N, MiddleOnly>.size>{});
}

// One entry point per count, so a count is dispatched by one indirect call.
template <typename T, bool MiddleOnly, size_t... N>
constexpr std::array<void (*)(T*), sizeof...(N)> network_table(
    std::index_sequence<N...> /*unused*/) {
  return {&run_network<T, N, MiddleOnly>...};
}

template <typename T, bool MiddleOnly>
inline constexpr auto kNetworks = network_table<T, MiddleOnly>(
    std::make_index_sequence<kMaxNetworkValues + 1>{});

}  // namespace sample_median_detail

// Sorts the |count| values at |v| (count <= kMaxNetworkValues) ascending.
template <typename T>
static inline void sort_by_network(T* v, size_t count) {
  sample_median_detail::kNetworks<T, false>[count](v);
}

// The middle two of |count| values in sorted order: the values at positions
// count/2 - 1 and count/2, or the value at count/2 twice for an odd count.
// |count| must not be 0.
struct MiddleValues {
  int16_t lower = 0;
  int16_t upper = 0;
};

static inline MiddleValues middle_values(const int16_t* values, size_t count) {
  if (count <= kMaxNetworkValues) {
    std::array<int16_t, kMaxNetworkValues> v;
    std::copy(values, values + count, v.begin());
    sample_median_detail::kNetworks<int16_t, true>[count](v.data());
    return {v[(count - 1) / 2], v[count / 2]};
  }
  thread_local std::vector<int16_t> scratch;
  scratch.assign(values, values + count);
  const auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(count / 2);
  std::nth_element(scratch.begin(), mid, scratch.end());
  const int16_t upper = *mid;
  const int16_t lower =
      count % 2 == 0 ? *std::max_element(scratch.begin(), mid) : upper;
  return {lower, upper};
}

// The median of |values| (of the middle two, truncated, for an even count);
// 0 for none.
static inline int16_t median_sample(const std::vector<int16_t>& values) {
  if (values.empty()) return 0;
  const MiddleValues m = middle_values(values.data(), values.size());
  if (values.size() % 2 == 0) {
    return static_cast<int16_t>((static_cast<int32_t>(m.lower) + m.upper) / 2);
  }
  return m.upper;
}

// What group_medians() gives a set with no values.
inline constexpr int32_t kNoMedian = std::numeric_limits<int32_t>::min();

// For each group g below |group_count|, the median of the |values| tagged g
// in |groups| and the median of the values tagged otherwise, as
// median_sample() takes them, into medians[2g] and medians[2g + 1]; kNoMedian
// for a set with no values.  |count| and |group_count| are at most
// kMaxNetworkValues, and every tag is below |group_count|.
//
// The values are sorted once, each tagged with its group, and both medians
// of every group are read from that one sorted set: a group's own values sit
// at the positions it holds, and the k-th of the rest is k positions in plus
// the group's values before it.
// Inlined into its caller's per-sample loop: as a call, the stack cut's
// pressing repair took a third more cycles.
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
static inline void group_medians(const int16_t* values, const uint8_t* groups,
                                 size_t count, size_t group_count,
                                 int32_t* medians) {
  // A value's group rides in the low bits of its sort key; the value, offset
  // to be unsigned, in the bits above.
  constexpr uint32_t kGroupBits = 5;
  constexpr uint32_t kGroupMask = (1U << kGroupBits) - 1;
  constexpr int32_t kOffset = 1 << 15;
  static_assert(kMaxNetworkValues <= (1U << kGroupBits));
  std::array<uint32_t, kMaxNetworkValues> keys;
  for (size_t i = 0; i < count; ++i) {
    keys[i] =
        (static_cast<uint32_t>(values[i] + kOffset) << kGroupBits) | groups[i];
  }
  sort_by_network(keys.data(), count);

  // Where each group's values sit in the sorted set, in order, followed by
  // positions past the end.
  constexpr uint8_t kPast = 0xFF;
  std::array<std::array<uint8_t, kMaxNetworkValues>, kMaxNetworkValues> at;
  for (size_t g = 0; g < group_count; ++g) at[g].fill(kPast);
  std::array<uint8_t, kMaxNetworkValues> members{};
  for (size_t i = 0; i < count; ++i) {
    const size_t g = keys[i] & kGroupMask;
    at[g][members[g]++] = static_cast<uint8_t>(i);
  }
  size_t largest = 0;
  for (size_t g = 0; g < group_count; ++g) {
    largest = std::max<size_t>(largest, members[g]);
  }

  const auto value = [&keys](size_t position) {
    return static_cast<int32_t>(keys[position] >> kGroupBits) - kOffset;
  };
  const auto middle = [&value](size_t lower_at, size_t upper_at, size_t n) {
    if (n == 0) return kNoMedian;
    const int32_t lower = value(lower_at);
    const int32_t upper = value(upper_at);
    return n % 2 == 0 ? (lower + upper) / 2 : upper;
  };
  for (size_t g = 0; g < group_count; ++g) {
    const size_t own = members[g];
    const size_t rest = count - own;
    const auto& own_at = at[g];
    // The k-th of the rest sits k positions in plus the group's values
    // before it.  The group's positions rise, so one pass over them counts
    // those before it; the positions past the end never count, so every
    // group takes the same number of steps, the largest group's.
    size_t lower = (rest - 1) / 2;
    size_t upper = rest / 2;
    for (size_t j = 0; j < largest; ++j) {
      lower += own_at[j] <= lower ? 1 : 0;
      upper += own_at[j] <= upper ? 1 : 0;
    }
    medians[2 * g] = own == 0
                         ? kNoMedian
                         : middle(own_at[(own - 1) / 2], own_at[own / 2], own);
    medians[(2 * g) + 1] = middle(lower, upper, rest);
  }
}

}  // namespace orc
