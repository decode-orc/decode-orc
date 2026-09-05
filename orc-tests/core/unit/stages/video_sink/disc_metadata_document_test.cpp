/*
 * File:        disc_metadata_document_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the LaserDisc metadata document
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/disc_metadata_document.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace orc;

namespace {

// A frame the disc numbered, with intact VBI.
DiscMetadataFrame Numbered(int32_t picture) {
  DiscMetadataFrame f;
  f.picture_number = picture;
  f.raw_vbi = {0x000000, 0xf00001, 0xf00001, 0x000000, 0x000000, 0x000000};
  return f;
}

// A pulldown frame: VBI intact, but the disc never wrote a picture number.
DiscMetadataFrame Pulldown() {
  DiscMetadataFrame f;
  f.raw_vbi = {0x000000, 0x800ddd, 0x800ddd, 0x000000, 0x000000, 0x000000};
  return f;
}

// A pulldown frame carrying a chapter code, which IEC 60856/60857 s10.1.5
// places in the fields that have no picture number: positive identification.
DiscMetadataFrame PulldownWithChapter(int32_t chapter) {
  DiscMetadataFrame f = Pulldown();
  f.chapter_number = chapter;
  return f;
}

// A frame whose VBI did not decode at all: the number exists, we lost it.
DiscMetadataFrame Failed() { return DiscMetadataFrame{}; }

DiscMetadataFrame ClvFrame(int h, int m, int s, int p) {
  DiscMetadataFrame f;
  CLVTimecode tc{};
  tc.hours = h;
  tc.minutes = m;
  tc.seconds = s;
  tc.picture_number = p;
  f.clv_timecode = tc;
  f.raw_vbi = {0x80e000, 0xf0dd00, 0x000000, 0x000000, 0x000000, 0x000000};
  return f;
}

ProgrammeStatus Status(bool side1, VbiSoundMode mode = VbiSoundMode::STEREO) {
  ProgrammeStatus ps;
  ps.cx_enabled = true;
  ps.is_12_inch = true;
  ps.is_side_1 = side1;
  ps.has_teletext = false;
  ps.is_digital = false;
  ps.sound_mode = mode;
  ps.is_fm_multiplex = false;
  ps.is_programme_dump = false;
  ps.parity_valid = true;
  return ps;
}

DiscMetadataDocument Build(const std::vector<DiscMetadataFrame>& frames,
                           VideoSystem system = VideoSystem::PAL) {
  return build_disc_metadata_document(frames, system, /*is_tff=*/true,
                                      /*source_first_frame=*/0, "1.0.0");
}

// Decode a hex bitmap back into the set of file frames it names.
std::set<uint64_t> DecodeBitmap(const std::string& hex) {
  std::set<uint64_t> out;
  for (size_t byte = 0; byte * 2 + 1 < hex.size() + 1 && byte * 2 < hex.size();
       ++byte) {
    const auto nibble = [&](size_t i) -> int {
      const char c = hex[i];
      return (c >= '0' && c <= '9') ? c - '0' : (c - 'a' + 10);
    };
    const int value = nibble(byte * 2) * 16 + nibble(byte * 2 + 1);
    for (int bit = 0; bit < 8; ++bit) {
      if (value & (1 << bit)) out.insert(byte * 8 + bit);
    }
  }
  return out;
}

// The address rule, as a consumer would implement it from the format guide.
int64_t AddressOf(const DiscAddressMap& map, uint64_t file_frame) {
  for (const auto& run : map.runs) {
    if (file_frame < run.file_frame ||
        file_frame >= run.file_frame + run.count) {
      continue;
    }
    const uint64_t held = map.unnumbered.count_before(file_frame) -
                          map.unnumbered.count_before(run.file_frame);
    return run.address + static_cast<int64_t>(file_frame - run.file_frame) -
           static_cast<int64_t>(held);
  }
  return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Run collapsing
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, ContiguousPictureNumbersCollapseToOneRun) {
  std::vector<DiscMetadataFrame> frames;
  for (int32_t i = 1; i <= 500; ++i) frames.push_back(Numbered(i));

  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.kind, DiscAddressKind::CavPicture);
  EXPECT_EQ(doc.address_map.runs[0].file_frame, 0u);
  EXPECT_EQ(doc.address_map.runs[0].count, 500u);
  EXPECT_EQ(doc.address_map.runs[0].address, 1);
  EXPECT_TRUE(doc.address_map.unnumbered.empty());
  EXPECT_TRUE(doc.address_map.undecoded.empty());
  EXPECT_TRUE(doc.address_map.unmapped.empty());
}

TEST(DiscMetadataDocument, PictureNumberRestartBreaksTheRun) {
  std::vector<DiscMetadataFrame> frames = {
      Numbered(1), Numbered(2), Numbered(3), Numbered(100), Numbered(101)};
  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 2u);
  EXPECT_EQ(doc.address_map.runs[0].count, 3u);
  EXPECT_EQ(doc.address_map.runs[1].file_frame, 3u);
  EXPECT_EQ(doc.address_map.runs[1].address, 100);
}

TEST(DiscMetadataDocument, SingleNumberedFrameIsAOneFrameRun) {
  std::vector<DiscMetadataFrame> frames = {Numbered(42)};
  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.runs[0].count, 1u);
}

TEST(DiscMetadataDocument, NoAddressesAtAllLeavesEverythingUnmapped) {
  std::vector<DiscMetadataFrame> frames(10);
  const auto doc = Build(frames);
  EXPECT_EQ(doc.address_map.kind, DiscAddressKind::None);
  EXPECT_TRUE(doc.address_map.runs.empty());
  EXPECT_EQ(doc.address_map.unmapped.count(), 10u);
}

TEST(DiscMetadataDocument, LeadingUnnumberedFramesCannotAnchorARun) {
  // Nothing precedes them, so their kind cannot be measured.
  std::vector<DiscMetadataFrame> frames = {Pulldown(), Pulldown(), Numbered(1),
                                           Numbered(2)};
  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.runs[0].file_frame, 2u);
  EXPECT_EQ(doc.address_map.unmapped.count(), 2u);
}

// ---------------------------------------------------------------------------
// Pulldown: frames the disc never numbered
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, PulldownCadenceCollapsesToOneRun) {
  // 2:3 cadence: one frame in five carries no number, and the picture count
  // does not advance across it.
  std::vector<DiscMetadataFrame> frames;
  int32_t picture = 1;
  for (int i = 0; i < 500; ++i) {
    if (i % 5 == 2) {
      frames.push_back(Pulldown());
    } else {
      frames.push_back(Numbered(picture++));
    }
  }

  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u)
      << "a missing number must not break a run";
  EXPECT_EQ(doc.address_map.runs[0].count, 500u);
  EXPECT_EQ(doc.address_map.unnumbered.count(), 100u);
  EXPECT_TRUE(doc.address_map.undecoded.empty());
  EXPECT_TRUE(doc.address_map.unmapped.empty());

  // The address rule must reproduce every number the disc actually carried.
  int32_t expected = 1;
  for (int i = 0; i < 500; ++i) {
    if (i % 5 == 2) continue;
    EXPECT_EQ(AddressOf(doc.address_map, static_cast<uint64_t>(i)), expected)
        << "at file frame " << i;
    ++expected;
  }
}

TEST(DiscMetadataDocument, IrregularPulldownStillCollapsesToOneRun) {
  // An edit changes the cadence part-way through; the encoding must not care.
  const std::vector<int> holes = {2, 7, 11, 12, 20, 21, 22, 40};
  std::vector<DiscMetadataFrame> frames;
  int32_t picture = 1;
  for (int i = 0; i < 60; ++i) {
    if (std::find(holes.begin(), holes.end(), i) != holes.end()) {
      frames.push_back(Pulldown());
    } else {
      frames.push_back(Numbered(picture++));
    }
  }

  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.unnumbered.count(), holes.size());
}

TEST(DiscMetadataDocument, ConsecutiveUnnumberedFramesHoldTheCount) {
  std::vector<DiscMetadataFrame> frames = {Numbered(10), Pulldown(), Pulldown(),
                                           Numbered(11)};
  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.unnumbered.count(), 2u);
  EXPECT_EQ(AddressOf(doc.address_map, 3), 11);
}

// ---------------------------------------------------------------------------
// Hole classification
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, GapWhoseNumbersAdvanceIsADecodeFailure) {
  std::vector<DiscMetadataFrame> frames = {Numbered(1), Failed(), Numbered(3)};
  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.undecoded.count(), 1u);
  EXPECT_TRUE(doc.address_map.unnumbered.empty());
  // An undecoded frame's address exists; the same rule yields it.
  EXPECT_EQ(AddressOf(doc.address_map, 1), 2);
  EXPECT_EQ(AddressOf(doc.address_map, 2), 3);
}

TEST(DiscMetadataDocument, MultipleDecodeFailuresAdvanceTheCount) {
  std::vector<DiscMetadataFrame> frames = {Numbered(1), Failed(), Failed(),
                                           Numbered(4)};
  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.undecoded.count(), 2u);
  EXPECT_EQ(AddressOf(doc.address_map, 3), 4);
}

TEST(DiscMetadataDocument, PulldownAndDecodeFailureAreNotConflated) {
  // Adjacent but distinguishable: the pulldown frame holds the count, the
  // failed one advances it, so the pair advances the number by exactly one.
  std::vector<DiscMetadataFrame> frames = {Numbered(1), PulldownWithChapter(1),
                                           Failed(), Numbered(3)};
  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.unnumbered.count(), 1u);
  EXPECT_EQ(doc.address_map.undecoded.count(), 1u);
  EXPECT_TRUE(doc.address_map.unnumbered.contains(1));
  EXPECT_TRUE(doc.address_map.undecoded.contains(2));
  EXPECT_EQ(AddressOf(doc.address_map, 3), 3);
}

TEST(DiscMetadataDocument, DecodeFailureInsideAPulldownGapIsResolvedByHints) {
  // Frame 1 has clean VBI and no picture code; frame 2 decoded nothing. The
  // "nothing decoded" hint accounts for the single advance exactly.
  std::vector<DiscMetadataFrame> frames = {Numbered(1), Pulldown(), Failed(),
                                           Numbered(3)};
  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_TRUE(doc.address_map.unnumbered.contains(1));
  EXPECT_TRUE(doc.address_map.undecoded.contains(2));
}

TEST(DiscMetadataDocument, UnattributableMixedGapBreaksTheRun) {
  // Two indistinguishable holes - both clean VBI, neither carrying a chapter
  // code - but the numbers say exactly one of them advanced. Which one is
  // genuinely unknowable, so the honest answer is a run boundary.
  std::vector<DiscMetadataFrame> frames = {Numbered(1), Pulldown(), Pulldown(),
                                           Numbered(3)};
  const auto doc = Build(frames);
  EXPECT_EQ(doc.address_map.runs.size(), 2u)
      << "an ambiguous gap must not be silently resolved";
  EXPECT_EQ(doc.address_map.unmapped.count(), 2u);
}

TEST(DiscMetadataDocument, BackwardsJumpBreaksTheRun) {
  std::vector<DiscMetadataFrame> frames = {Numbered(10), Numbered(11),
                                           Numbered(5)};
  const auto doc = Build(frames);
  EXPECT_EQ(doc.address_map.runs.size(), 2u);
}

TEST(DiscMetadataDocument, TrailingHolesAreUnmapped) {
  std::vector<DiscMetadataFrame> frames = {Numbered(1), Numbered(2),
                                           Pulldown()};
  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.runs[0].count, 2u);
  EXPECT_EQ(doc.address_map.unmapped.count(), 1u);
}

// ---------------------------------------------------------------------------
// CLV timecode
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, ClvRollsOverAtTwentyFiveForPal) {
  std::vector<DiscMetadataFrame> frames;
  for (int i = 0; i < 60; ++i) {
    frames.push_back(ClvFrame(0, 0, i / 25, i % 25));
  }
  const auto doc = Build(frames, VideoSystem::PAL);
  EXPECT_EQ(doc.address_map.kind, DiscAddressKind::ClvTimecode);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.runs[0].count, 60u);
  EXPECT_EQ(doc.frame_rate_nominal, 25);
  EXPECT_EQ(doc.frame_rate_exact.first, 25);
  EXPECT_EQ(doc.frame_rate_exact.second, 1);
}

TEST(DiscMetadataDocument, ClvRollsOverAtThirtyForNtsc) {
  std::vector<DiscMetadataFrame> frames;
  for (int i = 0; i < 90; ++i) {
    frames.push_back(ClvFrame(0, 0, i / 30, i % 30));
  }
  const auto doc = Build(frames, VideoSystem::NTSC);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.runs[0].count, 90u);
  EXPECT_EQ(doc.frame_rate_nominal, 30);
  // Nominal 30 against an actual 30000/1001: CLV timecode is a disc address,
  // not elapsed time.
  EXPECT_EQ(doc.frame_rate_exact.first, 30000);
  EXPECT_EQ(doc.frame_rate_exact.second, 1001);
}

TEST(DiscMetadataDocument, ClvRollsOverMinutesAndHours) {
  const int rate = 25;
  const int64_t start = ((1 * 60 + 59) * 60 + 59) * rate + 24;  // 01:59:59.24
  std::vector<DiscMetadataFrame> frames;
  for (int i = 0; i < 4; ++i) {
    const CLVTimecode tc = clv_from_address(start + i, rate);
    frames.push_back(
        ClvFrame(tc.hours, tc.minutes, tc.seconds, tc.picture_number));
  }
  const auto doc = Build(frames, VideoSystem::PAL);
  ASSERT_EQ(doc.address_map.runs.size(), 1u);
  EXPECT_EQ(doc.address_map.runs[0].count, 4u);

  const CLVTimecode rolled = clv_from_address(start + 1, rate);
  EXPECT_EQ(rolled.hours, 2);
  EXPECT_EQ(rolled.minutes, 0);
  EXPECT_EQ(rolled.seconds, 0);
  EXPECT_EQ(rolled.picture_number, 0);
}

TEST(DiscMetadataDocument, ClvAddressRoundTrips) {
  for (int rate : {25, 30}) {
    for (int64_t a : {0LL, 1LL, 24LL, 25LL, 1000LL, 123456LL}) {
      EXPECT_EQ(clv_to_address(clv_from_address(a, rate), rate), a)
          << "rate " << rate << " address " << a;
    }
  }
}

// ---------------------------------------------------------------------------
// Frame set encoding
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, SparseHolesEmitAsRanges) {
  // A long file with a handful of decode failures: the bitmap would cost one
  // bit per frame across the whole side to say almost nothing.
  std::vector<DiscMetadataFrame> frames;
  for (int i = 0; i < 2000; ++i) frames.push_back(Numbered(i + 1));
  for (int i : {500, 1200, 1201}) frames[i] = Failed();

  const auto doc = Build(frames);
  ASSERT_EQ(doc.address_map.undecoded.count(), 3u);
  const std::string yaml =
      emit_disc_metadata_yaml(doc, DiscMetadataDetail::Map);
  EXPECT_NE(yaml.find("encoding: ranges"), std::string::npos);
  EXPECT_EQ(yaml.find("encoding: bitmap"), std::string::npos);
}

TEST(DiscMetadataDocument, ManyHolesEmitAsBitmap) {
  std::vector<DiscMetadataFrame> frames;
  int32_t picture = 1;
  for (int i = 0; i < 2000; ++i) {
    if (i % 5 == 2) {
      frames.push_back(Pulldown());
    } else {
      frames.push_back(Numbered(picture++));
    }
  }
  const auto doc = Build(frames);
  const std::string yaml =
      emit_disc_metadata_yaml(doc, DiscMetadataDetail::Map);
  EXPECT_NE(yaml.find("encoding: bitmap"), std::string::npos)
      << "400 single-frame ranges must lose to one bit per frame";
}

TEST(DiscMetadataDocument, BitmapRoundTripsToTheSameSet) {
  DiscFrameSet set;
  for (uint64_t f : {0ull, 1ull, 7ull, 8ull, 9ull, 63ull, 64ull, 199ull}) {
    set.add(f);
  }
  const std::string hex = disc_frame_set_bitmap(set, 200);
  const std::set<uint64_t> decoded = DecodeBitmap(hex);
  const std::set<uint64_t> expected = {0, 1, 7, 8, 9, 63, 64, 199};
  EXPECT_EQ(decoded, expected);
  EXPECT_EQ(hex.size(), 50u);  // (200 + 7) / 8 bytes, two hex digits each
}

TEST(DiscMetadataDocument, FrameSetCountBeforeIsHalfOpen) {
  DiscFrameSet set;
  set.add(2);
  set.add(3);
  set.add(10);
  EXPECT_EQ(set.count(), 3u);
  EXPECT_EQ(set.count_before(0), 0u);
  EXPECT_EQ(set.count_before(3), 1u);
  EXPECT_EQ(set.count_before(4), 2u);
  EXPECT_EQ(set.count_before(100), 3u);
}

// ---------------------------------------------------------------------------
// Disc status voting
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, StatusMajorityOutvotesDissentingFields) {
  std::vector<DiscMetadataFrame> frames;
  for (int i = 0; i < 100; ++i) {
    DiscMetadataFrame f = Numbered(i + 1);
    // One frame in fifty carries a mis-decoded side bit.
    const bool side1 = (i % 50 != 0);
    f.programme_status[0] = Status(side1);
    f.programme_status[1] = Status(side1);
    frames.push_back(f);
  }
  const auto doc = Build(frames);
  ASSERT_TRUE(doc.status.is_side_1.has_value());
  EXPECT_TRUE(*doc.status.is_side_1) << "two bad fields must not flip a side";
  EXPECT_EQ(doc.status.fields_total, 200u);
  EXPECT_EQ(doc.status.fields_with_status, 200u);
  EXPECT_EQ(doc.status.fields_parity_valid, 200u);
  EXPECT_LT(doc.status.agreement, 1.0);
  EXPECT_GT(doc.status.agreement, 0.9);
}

TEST(DiscMetadataDocument, NoStatusLeavesEveryFieldUnset) {
  std::vector<DiscMetadataFrame> frames = {Numbered(1), Numbered(2)};
  const auto doc = Build(frames);
  EXPECT_FALSE(doc.status.is_side_1.has_value());
  EXPECT_FALSE(doc.status.cx_enabled.has_value());
  EXPECT_FALSE(doc.status.sound_mode.has_value());
  EXPECT_EQ(doc.status.fields_with_status, 0u);
  EXPECT_DOUBLE_EQ(doc.status.agreement, 0.0);
}

TEST(DiscMetadataDocument, SoundModeVoteIsDeterministicOnATie) {
  std::vector<DiscMetadataFrame> frames;
  for (int i = 0; i < 4; ++i) {
    DiscMetadataFrame f = Numbered(i + 1);
    f.programme_status[0] =
        Status(true, i < 2 ? VbiSoundMode::STEREO : VbiSoundMode::MONO);
    frames.push_back(f);
  }
  const auto a = Build(frames);
  const auto b = Build(frames);
  ASSERT_TRUE(a.status.sound_mode.has_value());
  EXPECT_EQ(*a.status.sound_mode, *b.status.sound_mode);
  EXPECT_EQ(*a.status.sound_mode, VbiSoundMode::STEREO)
      << "ties resolve to the lowest enum value";
}

// ---------------------------------------------------------------------------
// Disc format detection
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, PictureNumbersImplyCav) {
  const auto doc = Build({Numbered(1), Numbered(2)});
  EXPECT_TRUE(doc.disc_format_known);
  EXPECT_TRUE(doc.disc_is_cav);
}

TEST(DiscMetadataDocument, TimecodesImplyClv) {
  const auto doc = Build({ClvFrame(0, 0, 0, 0), ClvFrame(0, 0, 0, 1)});
  EXPECT_TRUE(doc.disc_format_known);
  EXPECT_FALSE(doc.disc_is_cav);
}

TEST(DiscMetadataDocument, ClvIndicatorCodeIdentifiesAClvDiscWithNoAddresses) {
  // IEC 60856/60857 s10.1.7: 0x87FFFF on line 17 of a CLV disc. The observer
  // publishes no observation for it, so it is recovered from the raw words.
  DiscMetadataFrame f;
  f.raw_vbi = {0x000000, 0x87ffff, 0x000000, 0x000000, 0x000000, 0x000000};
  const auto doc = Build({f, f});
  EXPECT_EQ(doc.address_map.kind, DiscAddressKind::None);
  EXPECT_TRUE(doc.disc_format_known);
  EXPECT_FALSE(doc.disc_is_cav);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, EventsAreGatheredAndCollapsed) {
  std::vector<DiscMetadataFrame> frames;
  for (int i = 0; i < 10; ++i) frames.push_back(Numbered(i + 1));
  frames[0].lead_in = true;
  frames[1].lead_in = true;
  frames[8].lead_out = true;
  frames[9].lead_out = true;
  frames[4].stop_code = true;
  frames[2].chapter_number = 1;
  frames[3].chapter_number = 1;  // no transition
  frames[6].chapter_number = 2;

  const auto doc = Build(frames);
  ASSERT_EQ(doc.events.lead_in.size(), 1u);
  EXPECT_EQ(doc.events.lead_in[0].first, 0u);
  EXPECT_EQ(doc.events.lead_in[0].second, 2u);
  ASSERT_EQ(doc.events.lead_out.size(), 1u);
  EXPECT_EQ(doc.events.lead_out[0].first, 8u);
  ASSERT_EQ(doc.events.stop_codes.size(), 1u);
  EXPECT_EQ(doc.events.stop_codes[0], 4u);
  ASSERT_EQ(doc.events.chapters.size(), 2u)
      << "only chapter transitions are recorded";
  EXPECT_EQ(doc.events.chapters[0].first, 2u);
  EXPECT_EQ(doc.events.chapters[1].first, 6u);
}

// ---------------------------------------------------------------------------
// The header and emitted document
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, HeaderIsEmittedInOrder) {
  const auto doc = Build({Numbered(1)});
  const std::string yaml =
      emit_disc_metadata_yaml(doc, DiscMetadataDetail::Map);
  EXPECT_EQ(yaml.rfind("# decode-orc disc metadata\n%YAML 1.2\n---\n"
                       "format: orc-disc-metadata\n"
                       "format_version: \"1.0\"\n",
                       0),
            0u);
}

TEST(DiscMetadataDocument, VersionIsQuotedSoYamlCannotReadItAsAFloat) {
  const auto doc = Build({Numbered(1)});
  const std::string yaml =
      emit_disc_metadata_yaml(doc, DiscMetadataDetail::Map);
  // Unquoted, "1.10" would collapse to 1.1 once the minor number passes 9.
  EXPECT_NE(yaml.find("format_version: \""), std::string::npos);
  EXPECT_EQ(kDiscMetadataFormatMajor, 1);
  EXPECT_EQ(kDiscMetadataFormatMinor, 0);
}

TEST(DiscMetadataDocument, EmitterIsDeterministic) {
  std::vector<DiscMetadataFrame> frames;
  for (int i = 0; i < 50; ++i) frames.push_back(Numbered(i + 1));
  frames[3].programme_status[0] = Status(true);
  const auto doc = Build(frames);
  EXPECT_EQ(emit_disc_metadata_yaml(doc, DiscMetadataDetail::Map),
            emit_disc_metadata_yaml(doc, DiscMetadataDetail::Map));
}

TEST(DiscMetadataDocument, MapDetailOmitsRawVbi) {
  const auto doc = Build({Numbered(1), Numbered(2)});
  const std::string yaml =
      emit_disc_metadata_yaml(doc, DiscMetadataDetail::Map);
  EXPECT_EQ(yaml.find("raw_vbi:"), std::string::npos);
}

TEST(DiscMetadataDocument, FullDetailEmitsRawVbiWithAbsentLinesMarked) {
  std::vector<DiscMetadataFrame> frames = {Numbered(1)};
  frames[0].raw_vbi = {-1, 0xf00001, 0x800ddd, -1, -1, 0x000000};
  const auto doc = Build(frames);
  const std::string yaml =
      emit_disc_metadata_yaml(doc, DiscMetadataDetail::Full);
  EXPECT_NE(yaml.find("raw_vbi:"), std::string::npos);
  EXPECT_NE(yaml.find("encoding: hex24-per-field"), std::string::npos);
  EXPECT_NE(yaml.find("    ------ f00001 800ddd ------ ------ 000000\n"),
            std::string::npos);
}

TEST(DiscMetadataDocument, ClvRunsEmitTimecodeStrings) {
  std::vector<DiscMetadataFrame> frames;
  for (int i = 0; i < 30; ++i) frames.push_back(ClvFrame(1, 2, 3, i % 25));
  // Make the timecodes actually consecutive from 01:02:03.00.
  frames.clear();
  const int64_t start = clv_to_address(CLVTimecode{1, 2, 3, 0}, 25);
  for (int i = 0; i < 30; ++i) {
    const CLVTimecode tc = clv_from_address(start + i, 25);
    frames.push_back(
        ClvFrame(tc.hours, tc.minutes, tc.seconds, tc.picture_number));
  }
  const auto doc = Build(frames, VideoSystem::PAL);
  const std::string yaml =
      emit_disc_metadata_yaml(doc, DiscMetadataDetail::Map);
  EXPECT_NE(yaml.find("kind: clv_timecode"), std::string::npos);
  EXPECT_NE(yaml.find("time: \"01:02:03.00\""), std::string::npos);
}

TEST(DiscMetadataDocument, AgreementIsFormattedWithoutALocale) {
  std::vector<DiscMetadataFrame> frames;
  for (int i = 0; i < 4; ++i) {
    DiscMetadataFrame f = Numbered(i + 1);
    f.programme_status[0] = Status(true);
    frames.push_back(f);
  }
  const auto doc = Build(frames);
  const std::string yaml =
      emit_disc_metadata_yaml(doc, DiscMetadataDetail::Map);
  EXPECT_NE(yaml.find("agreement: 1.0000"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Container tags
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, TagsCarryTheVersionAndTheAddressRange) {
  std::vector<DiscMetadataFrame> frames;
  int32_t picture = 1;
  for (int i = 0; i < 100; ++i) {
    if (i % 5 == 2) {
      frames.push_back(Pulldown());
    } else {
      DiscMetadataFrame f = Numbered(picture++);
      f.programme_status[0] = Status(true);
      frames.push_back(f);
    }
  }
  const auto doc = Build(frames);

  std::map<std::string, std::string> tags;
  for (const auto& [k, v] : disc_metadata_tags(doc)) tags[k] = v;

  EXPECT_EQ(tags["ORC_DISC_METADATA"], "orc-disc-metadata.yaml");
  EXPECT_EQ(tags["ORC_DISC_METADATA_VERSION"], "1.0");
  EXPECT_EQ(tags["ORC_DISC_FORMAT"], "CAV");
  EXPECT_EQ(tags["ORC_VIDEO_SYSTEM"], "PAL");
  EXPECT_EQ(tags["ORC_DISC_SIDE"], "1");
  EXPECT_EQ(tags["ORC_FIRST_PICTURE"], "1");
  // 100 frames, 20 of them unnumbered, so the last picture is 80.
  EXPECT_EQ(tags["ORC_LAST_PICTURE"], "80");
}

TEST(DiscMetadataDocument, TagsOmitValuesNeverRecovered) {
  const auto doc = Build({Numbered(1), Numbered(2)});
  for (const auto& [key, value] : disc_metadata_tags(doc)) {
    EXPECT_NE(value, "unknown") << key;
    EXPECT_NE(key, "ORC_DISC_SIDE");
    EXPECT_NE(key, "ORC_SOUND_MODE");
  }
}

// ---------------------------------------------------------------------------
// System-dependent values
// ---------------------------------------------------------------------------

TEST(DiscMetadataDocument, VideoSystemIsCarriedThrough) {
  struct Case {
    VideoSystem system;
    const char* name;
    int rate;
  };
  const Case cases[] = {{VideoSystem::PAL, "PAL", 25},
                        {VideoSystem::NTSC, "NTSC", 30},
                        {VideoSystem::PAL_M, "PAL_M", 30}};
  for (const auto& c : cases) {
    const auto doc = Build({Numbered(1), Numbered(2)}, c.system);
    EXPECT_EQ(doc.frame_rate_nominal, c.rate) << c.name;
    const std::string yaml =
        emit_disc_metadata_yaml(doc, DiscMetadataDetail::Map);
    EXPECT_NE(yaml.find(std::string("system: ") + c.name), std::string::npos);
  }
}

TEST(DiscMetadataDocument, FieldOrderFollowsTheExport) {
  std::vector<DiscMetadataFrame> frames = {Numbered(1)};
  const auto tff =
      build_disc_metadata_document(frames, VideoSystem::PAL, true, 0, "1.0.0");
  const auto bff =
      build_disc_metadata_document(frames, VideoSystem::PAL, false, 0, "1.0.0");
  EXPECT_NE(emit_disc_metadata_yaml(tff, DiscMetadataDetail::Map)
                .find("field_order: tff"),
            std::string::npos);
  EXPECT_NE(emit_disc_metadata_yaml(bff, DiscMetadataDetail::Map)
                .find("field_order: bff"),
            std::string::npos);
}

TEST(DiscMetadataDocument, SourceProvenanceRecordsTheFirstSourceFrame) {
  std::vector<DiscMetadataFrame> frames = {Numbered(1), Numbered(2),
                                           Numbered(3)};
  const auto doc =
      build_disc_metadata_document(frames, VideoSystem::PAL, true, 500, "x");
  EXPECT_EQ(doc.source_first_frame, 500u);
  EXPECT_EQ(doc.source_last_frame, 502u);
  // The document's own frame axis stays 0-based regardless.
  EXPECT_EQ(doc.address_map.runs[0].file_frame, 0u);
}
