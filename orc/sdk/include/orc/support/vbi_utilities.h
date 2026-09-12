/*
 * File:        vbi_utilities.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Vbi Utilities
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_CORE_VBI_UTILITIES_H
#define ORC_CORE_VBI_UTILITIES_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orc {
/**
 * @brief Utilities for VBI (Vertical Blanking Interval) decoding
 *
 * Provides low-level functions for processing VBI data lines including
 * transition detection and parity checking.
 */
namespace vbi_utils {

/**
 * @brief Convert analog samples to binary transitions at zero-crossing point
 *
 * Processes a line of 16-bit analog samples and generates a transition map
 * where each element indicates whether the sample is above or below the
 * zero-crossing threshold.
 *
 * @param line_data Pointer to array of 16-bit samples
 * @param sample_count Number of samples in line_data
 * @param zero_crossing Threshold value for determining transitions
 * @return Vector where 1 = above zero-crossing, 0 = below
 */
std::vector<uint8_t> get_transition_map(const int16_t* line_data,
                                        size_t sample_count,
                                        int16_t zero_crossing);

/**
 * @brief Buffer-reusing form of get_transition_map()
 *
 * Writes the transition map into @p out instead of returning a fresh vector,
 * so callers that decode many lines reuse a single allocation. An observer
 * scanning a whole recording decodes several VBI lines per frame, where the
 * per-call heap traffic of the returning form is pure overhead.
 *
 * @p out is resized to @p sample_count and every element overwritten; its
 * prior contents are never read, so any buffer may be passed in. Capacity is
 * retained between calls, so after the first call on a given buffer the
 * common case allocates nothing.
 *
 * @param line_data Pointer to array of 16-bit samples
 * @param sample_count Number of samples in line_data
 * @param zero_crossing Threshold value for determining transitions
 * @param out [out] Receives the map: 1 = above zero-crossing, 0 = below
 */
void get_transition_map_into(const int16_t* line_data, size_t sample_count,
                             int16_t zero_crossing, std::vector<uint8_t>& out);

/**
 * @brief Find the next transition in a transition map
 *
 * Searches for the next transition (false->true or true->false) after
 * the given start position.
 *
 * @param transition_map Binary transition map from get_transition_map()
 * @param target_state State to search for (true or false)
 * @param position [in/out] Start position; updated to transition point if found
 * @param limit Maximum position to search to
 * @return true if transition found before limit, false otherwise
 */
bool find_transition(const std::vector<uint8_t>& transition_map,
                     bool target_state, double& position, double limit);

/**
 * @brief Check if a value has even parity
 *
 * @param value Value to check
 * @return true if value has even parity, false otherwise
 */
bool is_even_parity(uint32_t value);

}  // namespace vbi_utils
}  // namespace orc

#endif  // ORC_CORE_VBI_UTILITIES_H
