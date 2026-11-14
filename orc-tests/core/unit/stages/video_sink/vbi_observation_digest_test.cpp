/*
 * File:        vbi_observation_digest_test.cpp
 * Module:      orc-core-unit-tests
 * Purpose:     Unit tests for the video sink's per-frame VBI digest
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/vbi_observation_digest.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <cstdint>
#include <string>

namespace orc_unit_test {
namespace {

// What the closed-caption and biphase observers write for one field.
void observe_field(orc::ObservationContext& ctx, uint32_t field,
                   int32_t chapter, int32_t picture, bool caption) {
  const orc::FieldID fid(field);
  ctx.set(fid, "vbi", "chapter_number", chapter);
  ctx.set(fid, "vbi", "picture_number", picture);
  ctx.set(fid, "biphase", "vbi_line_16", int32_t{0x8BA123});
  ctx.set(fid, "closed_caption", "present", caption);
  ctx.set(fid, "closed_caption", "data0", int32_t{0x80 | 0x41});
  ctx.set(fid, "closed_caption", "data1", int32_t{0x42});
}

}  // namespace

// The digest keeps what the outputs read and leaves the context empty after
// every frame, so the context never holds more than the frame in hand however
// long the export is.
TEST(VbiObservationDigestTest, KeepsTheValuesAndReleasesEachFrame) {
  constexpr uint64_t kFirst = 100;
  constexpr uint64_t kFrames = 2000;
  orc::ObservationContext ctx;
  orc::VbiObservationDigest digest;
  digest.configure(kFirst, kFrames, /*captions=*/true, /*chapters=*/true,
                   /*disc_metadata=*/true);

  for (uint64_t frame = kFirst; frame < kFirst + kFrames; ++frame) {
    const auto chapter = static_cast<int32_t>(frame / 500);
    observe_field(ctx, static_cast<uint32_t>(frame * 2), chapter,
                  static_cast<int32_t>(frame), frame % 3 == 0);
    observe_field(ctx, static_cast<uint32_t>(frame * 2 + 1), chapter,
                  static_cast<int32_t>(frame), false);
    digest.take_captions(ctx, frame);
    // The caption pass clears the frame; the VBI pass observes it again.
    observe_field(ctx, static_cast<uint32_t>(frame * 2), chapter,
                  static_cast<int32_t>(frame), false);
    digest.take_vbi(ctx, frame);

    ASSERT_TRUE(ctx.get_namespaces(orc::FieldID(frame * 2)).empty());
    ASSERT_TRUE(ctx.get_namespaces(orc::FieldID(frame * 2 + 1)).empty());
  }

  for (uint64_t n = 0; n < kFrames; ++n) {
    const uint64_t frame = kFirst + n;
    const auto* cc = digest.caption(n * 2);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->line21.present, frame % 3 == 0) << n;
    if (cc->line21.present) {
      EXPECT_EQ(cc->line21.data0, 0x41);  // parity bit removed
      EXPECT_EQ(cc->line21.data1, 0x42);
    }
    EXPECT_FALSE(cc->other_line.present) << n;
    EXPECT_EQ(digest.chapter(n * 2), static_cast<int32_t>(frame / 500));
  }
  ASSERT_EQ(digest.disc_frames().size(), kFrames);
  EXPECT_EQ(digest.disc_frames()[7].picture_number,
            static_cast<int32_t>(kFirst + 7));
  EXPECT_EQ(digest.disc_frames()[7].raw_vbi[0], 0x8BA123);
  // Past the export, nothing.
  EXPECT_EQ(digest.caption(kFrames * 2), nullptr);
  EXPECT_FALSE(digest.chapter(kFrames * 2).has_value());
}

// Frames outside the configured range are not recorded, and an output that
// is off keeps nothing.
TEST(VbiObservationDigestTest, KeepsOnlyWhatIsEnabled) {
  orc::ObservationContext ctx;
  orc::VbiObservationDigest digest;
  digest.configure(0, 10, /*captions=*/false, /*chapters=*/true,
                   /*disc_metadata=*/false);
  observe_field(ctx, 0, 4, 1, true);
  digest.take_vbi(ctx, 0);
  digest.take_vbi(ctx, 50);  // outside the range: ignored

  EXPECT_EQ(digest.caption(0), nullptr);
  EXPECT_EQ(digest.chapter(0), 4);
  EXPECT_TRUE(digest.disc_frames().empty());
}

}  // namespace orc_unit_test
