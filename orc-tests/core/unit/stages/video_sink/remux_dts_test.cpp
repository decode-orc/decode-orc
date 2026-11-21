/*
 * File:        remux_dts_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for rebuilding the leading decode timestamps of a
 *              remuxed Matroska stream
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/sinks/common/remux_dts.h"

#include <gtest/gtest.h>

namespace orc_unit_test {

// An x264 stream with a two-frame reorder delay at 25 fps in a 1/1000 time
// base: the first two packets had decode times -80 and -40, which Matroska
// cannot store, and the third arrives with 0.
TEST(RemuxDtsTest, BackfillsLeadingPacketsFromFirstKnownDts) {
  const auto dts = orc::backfill_leading_dts({40, 40}, 0, 40);

  ASSERT_EQ(dts.size(), 2U);
  EXPECT_EQ(dts[0], -80);
  EXPECT_EQ(dts[1], -40);
}

TEST(RemuxDtsTest, UsesFallbackDurationWhenPacketHasNone) {
  const auto dts = orc::backfill_leading_dts({0, 40}, 0, 40);

  ASSERT_EQ(dts.size(), 2U);
  EXPECT_EQ(dts[0], -80);
  EXPECT_EQ(dts[1], -40);
}

// Each packet steps back by its own duration, so uneven durations stay
// strictly increasing up to the first known decode time.
TEST(RemuxDtsTest, StepsBackByEachPacketsOwnDuration) {
  const auto dts = orc::backfill_leading_dts({33, 34, 33}, 1000, 33);

  ASSERT_EQ(dts.size(), 3U);
  EXPECT_EQ(dts[2], 967);
  EXPECT_EQ(dts[1], 933);
  EXPECT_EQ(dts[0], 900);
}

TEST(RemuxDtsTest, NothingHeldGivesNothing) {
  EXPECT_TRUE(orc::backfill_leading_dts({}, 0, 40).empty());
}

}  // namespace orc_unit_test
