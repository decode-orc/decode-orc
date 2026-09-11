/*
 * File:        waveform_count_grid_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for the waveform monitor's retained count grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "waveform_count_grid.h"

#include <gtest/gtest.h>

namespace orc::gui {
namespace {

TEST(WaveformCountGrid, CountsSamplesIntoTheirOwnCells) {
  WaveformCountGrid grid;
  grid.reset(4, 3);

  grid.increment(0, 0);
  grid.increment(0, 0);
  grid.increment(3, 2);

  EXPECT_EQ(grid.at(0, 0), 2u);
  EXPECT_EQ(grid.at(3, 2), 1u);
  EXPECT_EQ(grid.at(1, 1), 0u);
  EXPECT_EQ(grid.xSamples(), 4);
  EXPECT_EQ(grid.yBins(), 3);
}

// A signal that leaves the displayed millivolt range is normal, so the caller
// hands over every sample and the grid discards what it cannot hold.
TEST(WaveformCountGrid, IgnoresSamplesOutsideTheGrid) {
  WaveformCountGrid grid;
  grid.reset(2, 2);

  grid.increment(-1, 0);
  grid.increment(0, -1);
  grid.increment(2, 0);
  grid.increment(0, 2);

  for (const std::uint32_t cell : grid.cells()) {
    EXPECT_EQ(cell, 0u);
  }
  EXPECT_EQ(grid.at(-1, 0), 0u);
  EXPECT_EQ(grid.at(0, 5), 0u);
}

TEST(WaveformCountGrid, ReducesACellBlockToItsLargestCount) {
  WaveformCountGrid grid;
  grid.reset(4, 4);
  grid.increment(1, 1);
  grid.increment(2, 2);
  grid.increment(2, 2);
  grid.increment(2, 2);

  EXPECT_EQ(grid.maxIn(0, 3, 0, 3), 3u);
  EXPECT_EQ(grid.maxIn(0, 1, 0, 1), 1u);
  EXPECT_EQ(grid.maxIn(3, 3, 3, 3), 0u);
  // A block reaching past the edge is clamped, not read out of bounds.
  EXPECT_EQ(grid.maxIn(-5, 99, -5, 99), 3u);
}

TEST(WaveformCountGrid, ClearsEveryCellOnReset) {
  WaveformCountGrid grid;
  grid.reset(3, 3);
  grid.increment(1, 1);
  ASSERT_EQ(grid.at(1, 1), 1u);

  grid.reset(3, 3);
  EXPECT_EQ(grid.at(1, 1), 0u);
}

// The point of the grid: a playing preview re-shapes it to the same picture 25
// times a second, and must not pay for the storage again each time. The
// vector-of-vectors it replaced allocated once per active sample column.
TEST(WaveformCountGrid, ReusesItsStorageAcrossFramesOfTheSameShape) {
  WaveformCountGrid grid;
  grid.reset(922, 1300);

  const std::uint32_t* first_cell = grid.cells().data();
  const std::size_t first_capacity = grid.cells().capacity();

  for (int frame = 0; frame < 100; ++frame) {
    grid.reset(922, 1300);
    grid.increment(frame, frame);
  }

  EXPECT_EQ(grid.cells().data(), first_cell)
      << "the grid re-allocated between frames";
  EXPECT_EQ(grid.cells().capacity(), first_capacity);
}

// A narrower picture must not throw the allocation away either: the next frame
// of the wider one would then have to buy it back.
TEST(WaveformCountGrid, KeepsItsCapacityWhenTheGridShrinks) {
  WaveformCountGrid grid;
  grid.reset(900, 1300);
  const std::size_t wide_capacity = grid.cells().capacity();

  grid.reset(700, 1300);

  EXPECT_EQ(grid.xSamples(), 700);
  EXPECT_EQ(grid.cells().size(), 700u * 1300u);
  EXPECT_GE(grid.cells().capacity(), wide_capacity);
}

TEST(WaveformCountGrid, IsEmptyUntilItIsGivenAShape) {
  WaveformCountGrid grid;
  EXPECT_TRUE(grid.empty());
  EXPECT_EQ(grid.maxIn(0, 10, 0, 10), 0u);

  grid.reset(2, 2);
  EXPECT_FALSE(grid.empty());

  // A degenerate or negative extent is no grid at all, not a crash.
  grid.reset(0, 5);
  EXPECT_TRUE(grid.empty());
  grid.reset(-3, -4);
  EXPECT_TRUE(grid.empty());
}

}  // namespace
}  // namespace orc::gui
