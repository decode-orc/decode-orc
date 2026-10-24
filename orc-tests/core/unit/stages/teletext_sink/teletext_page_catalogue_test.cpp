/*
 * File:        teletext_page_catalogue_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the teletext sink's page catalogue
 *
 * Covers: snapshot merge and content replacement, appearance counting across
 * carousel repeats and rolling headers, subtitle-flag stickiness, page and
 * sub-page ordering, multi-page sets, and the sub-page caps. Pure in-memory
 * accumulation; no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_catalogue.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace orc_unit_test {

namespace {

// A snapshot of one appearance of a page, identified by the field its header
// arrived in — which is what tells one appearance from the next.
orc::TeletextPageSnapshot make_snapshot(int magazine, int page_number,
                                        int64_t header_field, int subcode = 0) {
  orc::TeletextPageSnapshot snapshot;
  snapshot.magazine = magazine;
  snapshot.page_number = page_number;
  snapshot.subcode = subcode;
  snapshot.header_field_index = header_field;
  snapshot.last_field_index = header_field + 4;
  return snapshot;
}

// Write |text| into row 1 of a snapshot so a merge can be seen to replace the
// page's content.
void set_row_1(orc::TeletextPageSnapshot& snapshot, const std::string& text) {
  snapshot.row_received[1] = true;
  for (size_t column = 0; column < text.size(); ++column) {
    snapshot.cells[1][column].character = static_cast<uint8_t>(text[column]);
  }
}

std::string row_1_of(const orc::TeletextPageSnapshot& snapshot, size_t length) {
  std::string text;
  for (size_t column = 0; column < length; ++column) {
    text.push_back(static_cast<char>(snapshot.cells[1][column].character));
  }
  return text;
}

// The page of a catalogue entry that is not a multi-page set.
const orc::TeletextPageSnapshot& only_subpage(
    const orc::TeletextCataloguedPage& page) {
  EXPECT_EQ(page.subpages.size(), 1u);
  return page.subpages.at(0).page;
}

// A run's per-field packet record: |counts[i]| packets recovered in field i.
std::vector<uint8_t> field_counts(std::initializer_list<int> counts) {
  std::vector<uint8_t> out;
  out.reserve(counts.size());
  for (const int count : counts) {
    out.push_back(static_cast<uint8_t>(count));
  }
  return out;
}

// The one sub-page of a page that is not a multi-page set.
const orc::TeletextCataloguedSubPage& only_sub_entry(
    const orc::TeletextCataloguedPage& page) {
  EXPECT_EQ(page.subpages.size(), 1u);
  return page.subpages.at(0);
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////////////////

TEST(TeletextPageCatalogue, Merge_RecordsAPagesIdentityAndFirstFrame) {
  orc::TeletextPageCatalogue catalogue;
  catalogue.merge(make_snapshot(1, 0x23, 40), /*frame_id=*/20);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].magazine, 1);
  EXPECT_EQ(pages[0].page_number, 0x23);
  EXPECT_EQ(pages[0].first_seen_frame, 20u);
  EXPECT_EQ(pages[0].last_seen_frame, 20u);
  EXPECT_EQ(pages[0].times_seen, 1u);
  EXPECT_FALSE(pages[0].subtitle);
  EXPECT_FALSE(catalogue.truncated());
}

// The carousel brings a page round repeatedly; each distinct header is another
// appearance, and the newest assembly replaces the entry's content because a
// squasher-backed decoder renders it from every copy recovered so far.
TEST(TeletextPageCatalogue, Merge_CountsCarouselRepeatsAndTakesTheNewestPage) {
  orc::TeletextPageCatalogue catalogue;

  auto first = make_snapshot(1, 0x00, 10);
  set_row_1(first, "HELLO");
  catalogue.merge(first, /*frame_id=*/5);

  auto second = make_snapshot(1, 0x00, 600);
  set_row_1(second, "WORLD");
  catalogue.merge(second, /*frame_id=*/300);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].times_seen, 2u);
  EXPECT_EQ(pages[0].first_seen_frame, 5u);
  EXPECT_EQ(pages[0].last_seen_frame, 300u);
  EXPECT_EQ(row_1_of(only_subpage(pages[0]), 5), "WORLD");
}

// A rolling header (ETSI EN 300 706 §9.3.1.4) re-emits the page mid
// transmission under its original header field index. That is the same
// appearance, not another one.
TEST(TeletextPageCatalogue, Merge_DoesNotCountARollingHeaderTwice) {
  orc::TeletextPageCatalogue catalogue;

  auto partial = make_snapshot(1, 0x00, 10);
  partial.transmission_complete = false;
  set_row_1(partial, "HALF ");
  catalogue.merge(partial, /*frame_id=*/5);

  auto complete = make_snapshot(1, 0x00, 10);
  set_row_1(complete, "WHOLE");
  catalogue.merge(complete, /*frame_id=*/6);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].times_seen, 1u);
  // The completed assembly still replaces the fragment.
  EXPECT_EQ(row_1_of(only_subpage(pages[0]), 5), "WHOLE");
  EXPECT_TRUE(only_subpage(pages[0]).transmission_complete);
}

// C6 is the service saying which page carries the subtitles. A service may
// drop it between captions, so the flag is sticky: a marker that blinked out
// whenever nothing was on screen would be worse than useless for finding it.
TEST(TeletextPageCatalogue, Merge_KeepsTheSubtitleFlagOnceSeen) {
  orc::TeletextPageCatalogue catalogue;

  auto with_c6 = make_snapshot(8, 0x88, 10);
  with_c6.subtitle = true;
  catalogue.merge(with_c6, /*frame_id=*/5);

  catalogue.merge(make_snapshot(8, 0x88, 600), /*frame_id=*/300);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_TRUE(pages[0].subtitle);
}

TEST(TeletextPageCatalogue, Pages_AreOrderedByAddress) {
  orc::TeletextPageCatalogue catalogue;
  catalogue.merge(make_snapshot(8, 0x88, 10), 1);
  catalogue.merge(make_snapshot(1, 0x00, 20), 2);
  catalogue.merge(make_snapshot(1, 0x0F, 30), 3);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 3u);
  EXPECT_EQ(pages[0].magazine, 1);
  EXPECT_EQ(pages[0].page_number, 0x00);
  EXPECT_EQ(pages[1].magazine, 1);
  EXPECT_EQ(pages[1].page_number, 0x0F);
  EXPECT_EQ(pages[2].magazine, 8);
  EXPECT_EQ(pages[2].page_number, 0x88);
}

// The cap is what bounds the run's memory by the size of the service rather
// than by the length of the recording; the least recently seen page goes.
TEST(TeletextPageCatalogue, Merge_DropsTheLeastRecentlySeenPageAtTheCap) {
  orc::TeletextPageCatalogue catalogue(/*max_subpages=*/2);

  catalogue.merge(make_snapshot(1, 0x01, 10), 1);
  catalogue.merge(make_snapshot(1, 0x02, 20), 2);
  // Touch page 101 so page 102 becomes the oldest.
  catalogue.merge(make_snapshot(1, 0x01, 30), 3);
  catalogue.merge(make_snapshot(1, 0x03, 40), 4);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0].page_number, 0x01);
  EXPECT_EQ(pages[1].page_number, 0x03);
  EXPECT_TRUE(catalogue.truncated());
}

TEST(TeletextPageCatalogue, Truncated_StaysFalseWhileUnderTheCap) {
  orc::TeletextPageCatalogue catalogue(/*max_subpages=*/2);
  catalogue.merge(make_snapshot(1, 0x01, 10), 1);
  catalogue.merge(make_snapshot(1, 0x02, 20), 2);

  EXPECT_EQ(catalogue.size(), 2u);
  EXPECT_FALSE(catalogue.truncated());
}

// ---------------------------------------------------------------------------
// Multi-page sets (ETSI EN 300 706 Annex A.1)
// ---------------------------------------------------------------------------

// A page number can carry a sequence of sub-pages the service cycles through,
// told apart by the sub-code. Keyed on the page number alone the catalogue
// would hold whichever one was transmitted last and discard the rest of the
// sequence as if they had been retransmissions of the same page.
TEST(TeletextPageCatalogue, Merge_CataloguesEachSubpageOfAMultiPageSet) {
  orc::TeletextPageCatalogue catalogue;

  auto one = make_snapshot(1, 0x50, 10, /*subcode=*/0x0001);
  set_row_1(one, "ONE  ");
  catalogue.merge(one, /*frame_id=*/5);

  auto two = make_snapshot(1, 0x50, 20, /*subcode=*/0x0002);
  set_row_1(two, "TWO  ");
  catalogue.merge(two, /*frame_id=*/6);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u) << "sub-pages share one page number";
  ASSERT_EQ(pages[0].subpages.size(), 2u);
  EXPECT_EQ(pages[0].subpages[0].subcode, 0x0001);
  EXPECT_EQ(row_1_of(pages[0].subpages[0].page, 5), "ONE  ");
  EXPECT_EQ(pages[0].subpages[1].subcode, 0x0002);
  EXPECT_EQ(row_1_of(pages[0].subpages[1].page, 5), "TWO  ");
  EXPECT_EQ(catalogue.subpage_count(), 2u);
}

// Annex A.1 numbers the sub-pages of a display page sequentially from Mxx-0001,
// so ascending sub-code is the sequence the service cycles through — whatever
// order the recording happened to catch them in.
TEST(TeletextPageCatalogue, Subpages_AreOrderedBySubcode) {
  orc::TeletextPageCatalogue catalogue;
  catalogue.merge(make_snapshot(1, 0x50, 10, 0x0003), 1);
  catalogue.merge(make_snapshot(1, 0x50, 20, 0x0001), 2);
  catalogue.merge(make_snapshot(1, 0x50, 30, 0x0002), 3);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  ASSERT_EQ(pages[0].subpages.size(), 3u);
  EXPECT_EQ(pages[0].subpages[0].subcode, 0x0001);
  EXPECT_EQ(pages[0].subpages[1].subcode, 0x0002);
  EXPECT_EQ(pages[0].subpages[2].subcode, 0x0003);
}

// Each sub-page is a page in its own right, so it counts its own appearances
// and the frames it — not its siblings — was seen at. The page-level figures
// cover the whole set, so a table of page numbers reads the same whether or
// not a page turns out to be a multi-page set.
TEST(TeletextPageCatalogue, Merge_CountsAppearancesPerSubpageAndPerPage) {
  orc::TeletextPageCatalogue catalogue;

  catalogue.merge(make_snapshot(1, 0x50, 10, 0x0001), /*frame_id=*/5);
  catalogue.merge(make_snapshot(1, 0x50, 20, 0x0002), /*frame_id=*/6);
  // A second cycle of the carousel: sub-page 1 comes round again.
  catalogue.merge(make_snapshot(1, 0x50, 200, 0x0001), /*frame_id=*/100);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  ASSERT_EQ(pages[0].subpages.size(), 2u);
  EXPECT_EQ(pages[0].subpages[0].times_seen, 2u);
  EXPECT_EQ(pages[0].subpages[0].first_seen_frame, 5u);
  EXPECT_EQ(pages[0].subpages[0].last_seen_frame, 100u);
  EXPECT_EQ(pages[0].subpages[1].times_seen, 1u);
  EXPECT_EQ(pages[0].subpages[1].first_seen_frame, 6u);
  EXPECT_EQ(pages[0].subpages[1].last_seen_frame, 6u);

  EXPECT_EQ(pages[0].times_seen, 3u);
  EXPECT_EQ(pages[0].first_seen_frame, 5u);
  EXPECT_EQ(pages[0].last_seen_frame, 100u);
}

// C6 says which page carries the subtitles, and the service is free to set it
// on whichever sub-page happens to be carrying a caption.
TEST(TeletextPageCatalogue, Merge_SubtitleFlagBelongsToThePageNotTheSubpage) {
  orc::TeletextPageCatalogue catalogue;

  catalogue.merge(make_snapshot(8, 0x88, 10, 0x0001), 1);
  auto with_c6 = make_snapshot(8, 0x88, 20, 0x0002);
  with_c6.subtitle = true;
  catalogue.merge(with_c6, 2);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_TRUE(pages[0].subtitle);
}

// The cap bounds sub-pages, because that is what holds a page grid each. A
// page number stays in the catalogue only for the sub-pages it still has.
TEST(TeletextPageCatalogue, Merge_DropsTheLeastRecentlySeenSubpageAtTheCap) {
  orc::TeletextPageCatalogue catalogue(/*max_subpages=*/2);

  catalogue.merge(make_snapshot(1, 0x01, 10, 0x0001), 1);
  catalogue.merge(make_snapshot(1, 0x02, 20, 0x0001), 2);
  // Touch 101 so 102's only sub-page becomes the oldest, then add a third.
  catalogue.merge(make_snapshot(1, 0x01, 30, 0x0001), 3);
  catalogue.merge(make_snapshot(1, 0x01, 40, 0x0002), 4);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u) << "page 102 lost its last sub-page";
  EXPECT_EQ(pages[0].page_number, 0x01);
  EXPECT_EQ(pages[0].subpages.size(), 2u);
  EXPECT_EQ(catalogue.subpage_count(), 2u);
  EXPECT_TRUE(catalogue.truncated());
}

// A page whose sub-codes churn — a misdecoded header degrades its sub-code
// nibbles to zero — must lose its own oldest sub-pages rather than crowd the
// rest of the service out of the catalogue.
// ETSI EN 300 706 Annex A.2 item 5: a page whose content the service replaced
// has a previous version, and the recording holds both. The decoder puts each
// in its own content run, and the catalogue keeps the one replaced rather
// than letting the newer overwrite it.
TEST(TeletextPageCatalogue, Merge_KeepsTheVersionAPageShowedBeforeItChanged) {
  orc::TeletextPageCatalogue catalogue;

  auto first = make_snapshot(1, 0x00, 10);
  set_row_1(first, "RAIN");
  catalogue.merge(first, /*frame_id=*/5);
  auto repeat = make_snapshot(1, 0x00, 100);
  set_row_1(repeat, "RAIN");
  catalogue.merge(repeat, /*frame_id=*/50);
  auto update = make_snapshot(1, 0x00, 400);
  update.content_run = 1;
  set_row_1(update, "SNOW");
  catalogue.merge(update, /*frame_id=*/200);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  const auto& subpage = only_sub_entry(pages[0]);
  EXPECT_EQ(subpage.times_seen, 3u);
  EXPECT_EQ(row_1_of(subpage.page, 4), "SNOW");
  EXPECT_EQ(subpage.version_times_seen, 1u);
  EXPECT_EQ(subpage.version_first_seen_frame, 200u);
  ASSERT_EQ(subpage.earlier_versions.size(), 1u);
  const auto& earlier = subpage.earlier_versions[0];
  EXPECT_EQ(row_1_of(earlier.page, 4), "RAIN");
  EXPECT_EQ(earlier.times_seen, 2u);
  EXPECT_EQ(earlier.first_seen_frame, 5u);
  EXPECT_EQ(earlier.last_seen_frame, 50u);
  EXPECT_EQ(subpage.versions_dropped, 0u);
}

// A service rotating a page through several screens under one sub-code brings
// each screen round again, and the decoder files a returning screen under the
// run it started. It is the same version seen again, not another one.
TEST(TeletextPageCatalogue, Merge_FilesAReturningScreenUnderItsOwnVersion) {
  orc::TeletextPageCatalogue catalogue;
  const char* screens[] = {"RAIN", "SNOW"};
  for (int turn = 0; turn < 4; ++turn) {
    auto snapshot = make_snapshot(1, 0x50, turn * 100);
    snapshot.content_run = turn % 2;
    set_row_1(snapshot, screens[turn % 2]);
    catalogue.merge(snapshot, static_cast<uint64_t>(turn * 50));
  }
  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  const auto& subpage = only_sub_entry(pages[0]);
  EXPECT_EQ(subpage.times_seen, 4u);
  EXPECT_EQ(row_1_of(subpage.page, 4), "SNOW");
  EXPECT_EQ(subpage.version_times_seen, 2u);
  EXPECT_EQ(subpage.version_first_seen_frame, 50u);
  ASSERT_EQ(subpage.earlier_versions.size(), 1u);
  EXPECT_EQ(row_1_of(subpage.earlier_versions[0].page, 4), "RAIN");
  EXPECT_EQ(subpage.earlier_versions[0].times_seen, 2u);
  EXPECT_EQ(subpage.earlier_versions[0].first_seen_frame, 0u);
  EXPECT_EQ(subpage.earlier_versions[0].last_seen_frame, 100u);
}

// Read by transmission, a slideshow's screens each come round again; a
// version one transmission showed, of a page another version of which came
// round, is not a screen. It goes, whether it was shown last or between the
// screens, and the page's appearances still count its header.
TEST(TeletextPageCatalogue, DropUnrepeatedVersions_KeepsTheScreensThatRecur) {
  orc::TeletextPageCatalogue catalogue;
  const char* shown[] = {"RAIN", "SNOW", "RAIN", "JUNK",
                         "SNOW", "RAIN", "ODDS"};
  const int runs[] = {0, 1, 0, 2, 1, 0, 3};
  for (int turn = 0; turn < 7; ++turn) {
    auto snapshot = make_snapshot(1, 0x12, turn * 100);
    snapshot.content_run = runs[turn];
    set_row_1(snapshot, shown[turn]);
    catalogue.merge(snapshot, static_cast<uint64_t>(turn * 50));
  }

  EXPECT_EQ(catalogue.drop_unrepeated_versions(), 2u);
  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  const auto& subpage = only_sub_entry(pages[0]);
  EXPECT_EQ(subpage.times_seen, 7u);
  EXPECT_EQ(row_1_of(subpage.page, 4), "RAIN") << "the screen shown last";
  EXPECT_EQ(subpage.version_times_seen, 3u);
  ASSERT_EQ(subpage.earlier_versions.size(), 1u);
  EXPECT_EQ(row_1_of(subpage.earlier_versions[0].page, 4), "SNOW");
}

// A page every version of which one transmission showed keeps them: there is
// no screen that came round to prefer.
TEST(TeletextPageCatalogue, DropUnrepeatedVersions_KeepsAPageNothingRepeated) {
  orc::TeletextPageCatalogue catalogue;
  const char* shown[] = {"RAIN", "SNOW"};
  for (int turn = 0; turn < 2; ++turn) {
    auto snapshot = make_snapshot(1, 0x12, turn * 100);
    snapshot.content_run = turn;
    set_row_1(snapshot, shown[turn]);
    catalogue.merge(snapshot, static_cast<uint64_t>(turn * 50));
  }

  EXPECT_EQ(catalogue.drop_unrepeated_versions(), 0u);
  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(only_sub_entry(pages[0]).earlier_versions.size(), 1u);
}

// A page that never changed has no earlier versions, however often it came
// round.
TEST(TeletextPageCatalogue, Merge_KeepsNoVersionsOfAnUnchangedPage) {
  orc::TeletextPageCatalogue catalogue;
  for (int appearance = 0; appearance < 5; ++appearance) {
    catalogue.merge(make_snapshot(1, 0x00, appearance * 100),
                    static_cast<uint64_t>(appearance * 50));
  }
  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_TRUE(only_sub_entry(pages[0]).earlier_versions.empty());
  EXPECT_EQ(only_sub_entry(pages[0]).version_times_seen, 5u);
}

// A subtitle page replaces its content with every caption, so the versions one
// sub-page keeps are bounded, the oldest going first and being counted.
TEST(TeletextPageCatalogue, Merge_BoundsTheVersionsOneSubPageKeeps) {
  orc::TeletextPageCatalogue catalogue;
  constexpr int kVersions = 20;
  for (int version = 0; version < kVersions; ++version) {
    auto snapshot = make_snapshot(8, 0x88, version * 100);
    snapshot.content_run = version;
    set_row_1(snapshot, std::string(1, static_cast<char>('A' + version)));
    catalogue.merge(snapshot, static_cast<uint64_t>(version * 50));
  }
  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  const auto& subpage = only_sub_entry(pages[0]);
  constexpr auto kKept =
      orc::TeletextPageCatalogue::kMaxEarlierVersionsPerSubPage;
  ASSERT_EQ(subpage.earlier_versions.size(), kKept);
  EXPECT_EQ(subpage.versions_dropped, kVersions - 1 - kKept);
  // The newest earlier version is the one just replaced.
  EXPECT_EQ(row_1_of(subpage.earlier_versions.back().page, 1),
            std::string(1, static_cast<char>('A' + kVersions - 2)));
  EXPECT_EQ(row_1_of(subpage.page, 1),
            std::string(1, static_cast<char>('A' + kVersions - 1)));
}

TEST(TeletextPageCatalogue, Merge_CapsTheSubpagesOfOnePage) {
  orc::TeletextPageCatalogue catalogue(/*max_subpages=*/16,
                                       /*max_subpages_per_page=*/2);

  catalogue.merge(make_snapshot(1, 0x01, 10, 0x0001), 1);
  catalogue.merge(make_snapshot(1, 0x01, 20, 0x0002), 2);
  catalogue.merge(make_snapshot(1, 0x01, 30, 0x0003), 3);
  catalogue.merge(make_snapshot(1, 0x02, 40, 0x0000), 4);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 2u) << "the other page kept its place";
  ASSERT_EQ(pages[0].subpages.size(), 2u);
  EXPECT_EQ(pages[0].subpages[0].subcode, 0x0002);
  EXPECT_EQ(pages[0].subpages[1].subcode, 0x0003);
  EXPECT_TRUE(catalogue.truncated());
  EXPECT_EQ(catalogue.subpages_dropped(), 1u);
  EXPECT_EQ(catalogue.pages_dropped(), 0u);
}

// A page whose sub-codes are a clock carries dozens of real sub-pages, and a
// damaged recording adds misread ones beside them. The cap drops a sub-page no
// appearance attested before one that was named as transmitted, however much
// longer ago that one was seen: the unattested one is most likely a misreading.
TEST(TeletextPageCatalogue, Merge_CapDropsAnUnattestedSubpageFirst) {
  orc::TeletextPageCatalogue catalogue(/*max_subpages=*/16,
                                       /*max_subpages_per_page=*/2);
  auto attested = [](int subcode, int64_t field) {
    auto snapshot = make_snapshot(3, 0x60, field, subcode);
    snapshot.identity_attested = true;
    return snapshot;
  };
  catalogue.merge(attested(0x0216, 10), 1);
  catalogue.merge(make_snapshot(3, 0x60, 20, 0x0217), 2);  // misread
  catalogue.merge(attested(0x0300, 30), 3);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  ASSERT_EQ(pages[0].subpages.size(), 2u);
  EXPECT_EQ(pages[0].subpages[0].subcode, 0x0216);
  EXPECT_EQ(pages[0].subpages[1].subcode, 0x0300);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Per-page lost-packet count
//
// A row the carousel brought round N times should have arrived N times; the
// copies actually combined say how many did. This is the only sound per-page
// count available: a service inserting on several VBI lines interleaves its
// magazines, so a field that came back short is short for whichever page that
// packet belonged to, and which one is not knowable.

namespace {

// A catalogued sub-page seen |times_seen| times, whose display rows arrived
// |copies| times each (index 0 of |copies| is display row 1).
orc::TeletextCataloguedSubPage seen_page(uint64_t times_seen,
                                         std::initializer_list<int> copies) {
  orc::TeletextCataloguedSubPage subpage;
  subpage.times_seen = times_seen;
  subpage.version_times_seen = times_seen;
  size_t row = 1;
  for (const int count : copies) {
    subpage.page.row_copies[row] = count;
    subpage.page.row_arrivals[row] = count;
    subpage.page.row_received[row] = count > 0;
    ++row;
  }
  return subpage;
}

}  // namespace

// Every row arrived on every appearance: nothing was lost.
TEST(TeletextSubPageLostPackets, AreZeroWhenEveryRowArrivedEveryTime) {
  const auto subpage = seen_page(/*times_seen=*/8, {8, 8, 8});
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

// A row that arrived on six of eight appearances was lost twice, and the
// shortfalls sum over the page's rows.
TEST(TeletextSubPageLostPackets, CountTheShortfallAgainstTheAppearances) {
  const auto subpage = seen_page(/*times_seen=*/8, {8, 6, 5});
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 5u);
}

// A row that never arrived at all is either one the service never sent — which
// most pages do, to space themselves out — or one lost every single time, and
// nothing here can tell those apart. Counting it would accuse every page of
// losing the blank rows it never carried.
TEST(TeletextSubPageLostPackets, IgnoreRowsThatNeverArrived) {
  const auto subpage = seen_page(/*times_seen=*/8, {8, 0, 0, 8});
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

// A page seen once has nothing to compare against: every row it carried
// arrived once, and the rows it did not are unknowable. Silence is the only
// honest answer, and it is exactly where a gap is most likely to be a loss —
// so the figure is a floor, not a measurement.
TEST(TeletextSubPageLostPackets, AreZeroForAPageSeenOnce) {
  const auto subpage = seen_page(/*times_seen=*/1, {1, 0, 1});
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

// More copies than appearances is not negative loss: a row re-sent inside one
// transmission is squashed as another copy.
TEST(TeletextSubPageLostPackets, ClampAtZeroOnMoreCopiesThanAppearances) {
  const auto subpage = seen_page(/*times_seen=*/4, {9, 4});
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

// The squasher keeps only a row's latest copies, so how many it combined is not
// how many arrived. A page the carousel brought round a hundred times with
// every row every time lost nothing, although each row rests on sixteen
// copies.
TEST(TeletextSubPageLostPackets, CountArrivalsBeyondTheCopiesKept) {
  auto subpage = seen_page(/*times_seen=*/100, {100, 97});
  subpage.page.row_copies[1] = 16;
  subpage.page.row_copies[2] = 16;
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 3u);
}

// The assembly on show is the current version's, so its rows are measured
// against that version's appearances: the appearances of versions it
// replaced carried other rows.
TEST(TeletextSubPageLostPackets, MeasureTheCurrentVersionOnly) {
  auto subpage = seen_page(/*times_seen=*/10, {4, 4});
  subpage.times_seen = 30;
  subpage.version_times_seen = 4;
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

// The header row carries a live clock and is never squashed, so its copy count
// is always zero and says nothing about loss.
TEST(TeletextSubPageLostPackets, IgnoreTheHeaderRow) {
  auto subpage = seen_page(/*times_seen=*/8, {8, 8});
  subpage.page.row_copies[0] = 0;
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

// ---------------------------------------------------------------------------
// Identity attestation (issue #267)
// ---------------------------------------------------------------------------
//
// ETSI EN 300 706 §8.2 gives Hamming 8/4 minimum distance 4, so a three-bit
// burst in a page-number byte resolves silently to a neighbouring codeword. The
// page it happens to is not damaged — it is duplicated, at a number the service
// never sent — and only whether the identity arrived as transmitted tells the
// copy from the original.

namespace {

// A snapshot whose header named the page as transmitted, or did not.
orc::TeletextPageSnapshot attested_snapshot(int magazine, int page_number,
                                            int64_t header_field, bool attested,
                                            int subcode = 0) {
  auto snapshot = make_snapshot(magazine, page_number, header_field, subcode);
  snapshot.identity_attested = attested;
  return snapshot;
}

// |appearances| appearances of one page, each with its own header field.
void carousel(orc::TeletextPageCatalogue& catalogue, int magazine,
              int page_number, int appearances, bool attested, int subcode = 0,
              int64_t first_field = 0) {
  for (int index = 0; index < appearances; ++index) {
    catalogue.merge(
        attested_snapshot(magazine, page_number, first_field + index * 10,
                          attested, subcode),
        static_cast<uint64_t>(first_field + index * 10));
  }
}

}  // namespace

TEST(TeletextPageCatalogueIdentities, AttestedAppearancesAreCounted) {
  orc::TeletextPageCatalogue catalogue;
  carousel(catalogue, 1, 0x20, /*appearances=*/3, /*attested=*/true);
  catalogue.merge(attested_snapshot(1, 0x20, 400, /*attested=*/false), 400);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].times_seen, 4u);
  EXPECT_EQ(pages[0].times_attested, 3u);
}

// The shape of the reported bug: one page seen many times with its number
// received as transmitted, and a copy of it at a number one digit away that
// was never once received cleanly.
TEST(TeletextPageCatalogueIdentities, FoldUnattestedPageIntoItsOneNeighbour) {
  orc::TeletextPageCatalogue catalogue;
  carousel(catalogue, 1, 0x09, /*appearances=*/40, /*attested=*/true);
  carousel(catalogue, 1, 0x79, /*appearances=*/2, /*attested=*/false,
           /*subcode=*/0, /*first_field=*/5000);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_seen, 2u);
  EXPECT_EQ(reconciliation.identities_unattested, 1u);
  EXPECT_EQ(reconciliation.identities_folded, 1u);
  EXPECT_EQ(reconciliation.identities_dropped, 0u);
  EXPECT_EQ(reconciliation.appearances_folded, 2u);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].page_number, 0x09);
  // The appearances were appearances of 1/09: the carousel brought it round
  // and the recording misread its number on the way past.
  EXPECT_EQ(pages[0].times_seen, 42u);
  EXPECT_EQ(pages[0].last_seen_frame, 5010u);
}

// A service numbering its pages densely offers two equally good explanations
// for a misreading, and neither is evidence of anything.
TEST(TeletextPageCatalogueIdentities, LeaveAnAmbiguousMisreadingUnfolded) {
  orc::TeletextPageCatalogue catalogue;
  carousel(catalogue, 1, 0x03, /*appearances=*/20, /*attested=*/true);
  carousel(catalogue, 1, 0x07, /*appearances=*/20, /*attested=*/true,
           /*subcode=*/0, /*first_field=*/1000);
  // 1/05 is one digit from both.
  carousel(catalogue, 1, 0x05, /*appearances=*/2, /*attested=*/false,
           /*subcode=*/0, /*first_field=*/5000);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_folded, 0u);
  EXPECT_EQ(reconciliation.identities_dropped, 1u);
  EXPECT_EQ(reconciliation.appearances_dropped, 2u);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0].times_seen, 20u);
  EXPECT_EQ(pages[1].times_seen, 20u);
}

// A page whose number is corrupted the same way on every appearance recurs
// under that one number with content nothing else carries.
TEST(TeletextPageCatalogueIdentities, KeepARecurringPageOfItsOwn) {
  orc::TeletextPageCatalogue catalogue;
  for (int index = 0; index < 20; ++index) {
    auto snapshot = attested_snapshot(1, 0x50, index * 10, true);
    set_row_1(snapshot, "NEWS HEADLINES");
    catalogue.merge(snapshot, static_cast<uint64_t>(index * 10));
  }
  for (int index = 0; index < 3; ++index) {
    auto snapshot = attested_snapshot(1, 0x60, 5000 + index * 10, false);
    set_row_1(snapshot, "WEATHER TODAY");
    catalogue.merge(snapshot, static_cast<uint64_t>(5000 + index * 10));
  }

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_kept, 1u);
  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[1].page_number, 0x60);
  EXPECT_EQ(pages[1].times_attested, 0u);
}

// Of two neighbours a misreading could have come from, the content says which.
TEST(TeletextPageCatalogueIdentities,
     AMisreadingFoldsIntoTheNeighbourItMatches) {
  orc::TeletextPageCatalogue catalogue;
  const auto page = [&](int number, int64_t field, bool attested,
                        const std::string& text) {
    auto snapshot = attested_snapshot(1, number, field, attested);
    set_row_1(snapshot, text);
    catalogue.merge(snapshot, static_cast<uint64_t>(field));
  };
  for (int index = 0; index < 10; ++index) {
    page(0x03, index * 10, true, "SPORT RESULTS");
    page(0x07, 1000 + index * 10, true, "TV LISTINGS");
  }
  page(0x05, 5000, false, "TV LISTINGS");
  page(0x05, 5010, false, "TV LISTINGS");

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_folded, 1u);
  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[1].page_number, 0x07);
  EXPECT_EQ(pages[1].times_seen, 12u);
}

// A false lock on noise names a page nothing else in the service is near.
TEST(TeletextPageCatalogueIdentities, DropAMisreadingWithNoNeighbour) {
  orc::TeletextPageCatalogue catalogue;
  carousel(catalogue, 1, 0x20, /*appearances=*/20, /*attested=*/true);
  carousel(catalogue, 7, 0xFE, /*appearances=*/1, /*attested=*/false,
           /*subcode=*/0, /*first_field=*/5000);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_dropped, 1u);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].magazine, 1);
  EXPECT_EQ(pages[0].page_number, 0x20);
}

// A misread sub-code duplicates a sub-page inside a page number that is itself
// perfectly attested, so the identity has to carry the sub-code too.
TEST(TeletextPageCatalogueIdentities, FoldAMisreadSubCode) {
  orc::TeletextPageCatalogue catalogue;
  carousel(catalogue, 1, 0x20, /*appearances=*/20, /*attested=*/true,
           /*subcode=*/0x0001);
  carousel(catalogue, 1, 0x20, /*appearances=*/1, /*attested=*/false,
           /*subcode=*/0x0009, /*first_field=*/5000);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_folded, 1u);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  ASSERT_EQ(pages[0].subpages.size(), 1u);
  EXPECT_EQ(pages[0].subpages[0].subcode, 0x0001);
  EXPECT_EQ(pages[0].subpages[0].times_seen, 21u);
}

// On an undamaged recording every page number arrives as transmitted, so the
// pass has nothing to do and must not disturb the catalogue.
TEST(TeletextPageCatalogueIdentities, LeaveAnUndamagedCatalogueAlone) {
  orc::TeletextPageCatalogue catalogue;
  carousel(catalogue, 1, 0x03, /*appearances=*/5, /*attested=*/true);
  carousel(catalogue, 1, 0x07, /*appearances=*/5, /*attested=*/true,
           /*subcode=*/0, /*first_field=*/1000);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_FALSE(reconciliation.acted());
  EXPECT_FALSE(reconciliation.withheld);
  EXPECT_TRUE(reconciliation.summary("page").empty());
  EXPECT_EQ(catalogue.pages().size(), 2u);
}

// A recording so damaged that nothing at all was received as transmitted
// offers no baseline to judge the rest against. Emptying the catalogue would be
// the confident answer rather than the honest one.
TEST(TeletextPageCatalogueIdentities, WithholdWhenNothingWasAttested) {
  orc::TeletextPageCatalogue catalogue;
  carousel(catalogue, 1, 0x03, /*appearances=*/5, /*attested=*/false);
  carousel(catalogue, 1, 0x07, /*appearances=*/5, /*attested=*/false,
           /*subcode=*/0, /*first_field=*/1000);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_TRUE(reconciliation.withheld);
  EXPECT_FALSE(reconciliation.acted());
  EXPECT_EQ(catalogue.pages().size(), 2u);
}

}  // namespace orc_unit_test
