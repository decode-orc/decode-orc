/*
 * File:        teletext_catalogue_view_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the teletext catalogue view the host draws
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_catalogue_view.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>

#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {
namespace {

// A dataset holding one page whose row 1 carries |codes| in |g0_set|. This is
// what the stage hands the host: the catalogue view is the last thing between
// a decoded page and the character-cell grid drawn on screen, so a set that
// stops here never reaches the reader however right the decoder was.
TeletextAnalysisDataset make_dataset(const std::string& codes,
                                     TeletextG0Set g0_set,
                                     int national_option_subset = 0) {
  TeletextPageSnapshot snapshot;
  snapshot.magazine = 1;
  snapshot.page_number = 0x00;
  snapshot.g0_set = g0_set;
  snapshot.national_option_subset = national_option_subset;
  snapshot.row_received[1] = true;
  for (size_t column = 0; column < codes.size(); ++column) {
    snapshot.cells[1][column].character = static_cast<uint8_t>(codes[column]);
    // The decoder stamps every cell with the set its code is to be read in, so
    // that a row switched between two alphabets by an ESC still converts (see
    // TeletextPageCell::g0_set). A hand-built snapshot has to do the same.
    snapshot.cells[1][column].g0_set = g0_set;
    snapshot.cells[1][column].national_option_subset = national_option_subset;
  }

  TeletextCataloguedSubPage subpage;
  subpage.page = snapshot;

  TeletextCataloguedPage page;
  page.magazine = snapshot.magazine;
  page.page_number = snapshot.page_number;
  page.times_seen = 1;
  page.subpages.push_back(std::move(subpage));

  TeletextAnalysisDataset dataset;
  dataset.pages.push_back(std::move(page));
  return dataset;
}

// UTF-8 of the drawn row, which is what a reader sees. The grid is on whichever
// item carries the page: a catalogued page is a parent whose sub-pages are its
// children, and the payload runs parallel to the item list.
std::string drawn_row(const CatalogueDataset& catalogue, int row,
                      size_t count) {
  const CatalogueCellGrid* grid = nullptr;
  for (const CataloguePayload& payload : catalogue.payloads) {
    if (payload.kind == CataloguePayload::Kind::kCellGrid) {
      grid = &payload.grid;
      break;
    }
  }
  EXPECT_NE(grid, nullptr);
  if (grid == nullptr) {
    return {};
  }
  std::string text;
  for (size_t column = 0; column < count; ++column) {
    const char32_t cp = grid->at(row, static_cast<int>(column)).character;
    // Only the ranges these tests reach; enough for Latin and Cyrillic.
    if (cp < 0x80) {
      text.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return text;
}

// A dataset holding one page whose rows rest on |copies| copies each, indexed
// by row. Rows given a non-zero count are marked received.
TeletextAnalysisDataset make_dataset_with_copies(
    const std::map<int, int>& copies) {
  TeletextPageSnapshot snapshot;
  snapshot.magazine = 1;
  snapshot.page_number = 0x64;
  for (const auto& [row, count] : copies) {
    snapshot.row_copies[static_cast<size_t>(row)] = count;
    snapshot.row_received[static_cast<size_t>(row)] = count > 0;
  }

  TeletextCataloguedSubPage subpage;
  subpage.page = snapshot;

  TeletextCataloguedPage page;
  page.magazine = snapshot.magazine;
  page.page_number = snapshot.page_number;
  page.times_seen = 1;
  page.subpages.push_back(std::move(subpage));

  TeletextAnalysisDataset dataset;
  dataset.pages.push_back(std::move(page));
  return dataset;
}

bool row_is_unconfirmed(const CatalogueDataset& catalogue, int row) {
  for (const CataloguePayload& payload : catalogue.payloads) {
    if (payload.kind == CataloguePayload::Kind::kCellGrid) {
      EXPECT_LT(static_cast<size_t>(row), payload.grid.row_status.size());
      return payload.grid.row_status[static_cast<size_t>(row)].unconfirmed;
    }
  }
  ADD_FAILURE() << "no cell grid in the catalogue";
  return false;
}

// A row carried onto this page by a burst can arrive two or three times over a
// long recording while the page's own rows have hundreds. Marking only the rows
// that rest on a single copy left exactly that row drawn as ordinary content,
// which is what a reader has no other way to notice.
TEST(TeletextCatalogueView, ARowFarBelowThePagesOwnEvidenceIsUnconfirmed) {
  const CatalogueDataset catalogue = build_teletext_catalogue(
      make_dataset_with_copies({{1, 500}, {2, 480}, {3, 500}, {24, 3}}));
  EXPECT_FALSE(row_is_unconfirmed(catalogue, 1));
  EXPECT_FALSE(row_is_unconfirmed(catalogue, 2));
  EXPECT_TRUE(row_is_unconfirmed(catalogue, 24));
}

// The unevenness of a real recovery is not suspicion: a row the dropouts took
// more often than its neighbours still belongs to the page.
TEST(TeletextCatalogueView, ARowMerelyLessLuckyThanItsNeighboursIsNot) {
  const CatalogueDataset catalogue = build_teletext_catalogue(
      make_dataset_with_copies({{1, 500}, {2, 480}, {24, 120}}));
  EXPECT_FALSE(row_is_unconfirmed(catalogue, 24));
}

// The label needs something to compare against: where every row rests on one
// copy nothing distinguishes them, and marking the whole page says nothing.
TEST(TeletextCatalogueView, APageSeenOnceHasNoUnconfirmedRows) {
  const CatalogueDataset catalogue = build_teletext_catalogue(
      make_dataset_with_copies({{1, 1}, {2, 1}, {24, 1}}));
  EXPECT_FALSE(row_is_unconfirmed(catalogue, 1));
  EXPECT_FALSE(row_is_unconfirmed(catalogue, 24));
}

// A page whose content the service replaced during the recording is marked in
// the list, and every version it went through can be stepped to under the
// page display, oldest first, each drawn from its own assembly.
TEST(TeletextCatalogueView, APageThatChangedIsMarkedAndItsVersionsListed) {
  TeletextAnalysisDataset dataset = make_dataset("SNOW", TeletextG0Set::Latin);
  TeletextCataloguedSubPage& subpage = dataset.pages[0].subpages[0];
  subpage.times_seen = 5;
  subpage.first_seen_frame = 10;
  subpage.last_seen_frame = 90;
  subpage.version_first_seen_frame = 60;
  subpage.version_times_seen = 2;
  TeletextPageVersion earlier;
  earlier.first_seen_frame = 10;
  earlier.last_seen_frame = 50;
  earlier.times_seen = 3;
  earlier.page =
      make_dataset("RAIN", TeletextG0Set::Latin).pages[0].subpages[0].page;
  subpage.earlier_versions.push_back(earlier);

  const CatalogueDataset catalogue = build_teletext_catalogue(dataset);
  ASSERT_EQ(catalogue.items.size(), 3u);
  const CatalogueItem& page_row = catalogue.items[0];
  ASSERT_EQ(page_row.badges.size(), 1u);
  EXPECT_EQ(page_row.badges[0], "2 versions");
  EXPECT_NE(page_row.tooltip.find("changed during the recording"),
            std::string::npos);
  EXPECT_NE(page_row.tooltip.find("frames 11-51 and 61-91"), std::string::npos);

  const CatalogueItem& first = catalogue.items[1];
  const CatalogueItem& second = catalogue.items[2];
  EXPECT_EQ(first.parent_id, page_row.id);
  EXPECT_EQ(second.parent_id, page_row.id);
  EXPECT_EQ(first.variant_label, "0000, version 1 of 2");
  EXPECT_EQ(second.variant_label, "0000, version 2 of 2");
  // The current version keeps the sub-page's own id.
  EXPECT_EQ(second.id, page_row.id + "/0000");
  EXPECT_NE(first.id, second.id);

  EXPECT_EQ(catalogue.payloads[1].headline,
            "Page 100 version 1 of 2 seen 3 times, frames 11-51");
  EXPECT_EQ(catalogue.payloads[2].headline,
            "Page 100 version 2 of 2 seen 2 times, frames 61-91");
  EXPECT_EQ(catalogue.payloads[1].grid.at(1, 0).character, U'R');
  EXPECT_EQ(catalogue.payloads[2].grid.at(1, 0).character, U'S');
}

// A page that held still is listed as before: no mark, one variant per
// sub-page, labelled with its sub-code alone.
TEST(TeletextCatalogueView, APageThatHeldStillHasNoVersions) {
  const CatalogueDataset catalogue =
      build_teletext_catalogue(make_dataset("SAME", TeletextG0Set::Latin));
  ASSERT_EQ(catalogue.items.size(), 2u);
  EXPECT_TRUE(catalogue.items[0].badges.empty());
  EXPECT_EQ(catalogue.items[1].variant_label, "0000");
}

// A page kept although its number never arrived undamaged says so.
TEST(TeletextCatalogueView, APageKeptWithoutAnAttestedNumberIsMarked) {
  TeletextAnalysisDataset dataset = make_dataset("KEPT", TeletextG0Set::Latin);
  dataset.summary.identity_reconciliation.identities_kept = 1;
  const CatalogueDataset kept = build_teletext_catalogue(dataset);
  ASSERT_FALSE(kept.items.empty());
  EXPECT_NE(std::find(kept.items[0].badges.begin(), kept.items[0].badges.end(),
                      "unconfirmed"),
            kept.items[0].badges.end());
  EXPECT_NE(kept.items[0].tooltip.find("never arrived with its number "
                                       "undamaged"),
            std::string::npos);

  dataset.pages[0].times_attested = 1;
  const CatalogueDataset attested = build_teletext_catalogue(dataset);
  EXPECT_TRUE(attested.items[0].badges.empty());
}

// The reader is told which alphabet the pages were read in, because it is the
// one thing about a rendered page that cannot be checked by looking at it.
TEST(TeletextCatalogueView, TheSummarySaysWhichCharacterSetWasUsed) {
  TeletextAnalysisDataset dataset = make_dataset("A", TeletextG0Set::Cyrillic2);
  dataset.summary.frames_analysed = 10;
  dataset.summary.packets_recovered = 20;
  dataset.summary.character_set = TeletextG0Set::Cyrillic2;

  const CatalogueDataset catalogue = build_teletext_catalogue(dataset);
  EXPECT_NE(
      catalogue.summary.headline.find("read as Cyrillic (Russian/Bulgarian)"),
      std::string::npos)
      << catalogue.summary.headline;

  dataset.summary.character_set = TeletextG0Set::Latin;
  const CatalogueDataset latin = build_teletext_catalogue(dataset);
  EXPECT_NE(latin.summary.headline.find("read as Latin"), std::string::npos)
      << latin.summary.headline;
  // With one alphabet the headline says nothing about a second, so a page that
  // holds one set is not described as if it might hold two.
  EXPECT_EQ(latin.summary.headline.find("switching to"), std::string::npos)
      << latin.summary.headline;
}

// A page holding two alphabets looks like a page holding one that has gone
// wrong in places, so the pairing has to be stated for that reading to be
// available to a reader at all.
TEST(TeletextCatalogueView, TheSummaryNamesTheSecondCharacterSetToo) {
  TeletextAnalysisDataset dataset = make_dataset("A", TeletextG0Set::Cyrillic2);
  dataset.summary.frames_analysed = 10;
  dataset.summary.packets_recovered = 20;
  dataset.summary.character_set = TeletextG0Set::Cyrillic2;
  dataset.summary.second_character_set =
      TeletextG0Designation{TeletextG0Set::Latin, 0};

  const CatalogueDataset catalogue = build_teletext_catalogue(dataset);
  EXPECT_NE(catalogue.summary.headline.find(
                "read as Cyrillic (Russian/Bulgarian), switching to Latin"),
            std::string::npos)
      << catalogue.summary.headline;
}

TEST(TeletextCatalogueView, DrawsThePageInItsOwnG0Set) {
  // The same seven transmitted codes, drawn twice. This is the whole point of
  // the character set reaching the grid: nothing about the page changes, only
  // the alphabet its codes are read in.
  const std::string codes = "Wtornik";

  const CatalogueDataset latin =
      build_teletext_catalogue(make_dataset(codes, TeletextG0Set::Latin));
  ASSERT_FALSE(latin.items.empty());
  EXPECT_EQ(drawn_row(latin, 1, codes.size()), "Wtornik");

  const CatalogueDataset cyrillic =
      build_teletext_catalogue(make_dataset(codes, TeletextG0Set::Cyrillic2));
  ASSERT_FALSE(cyrillic.items.empty());
  EXPECT_EQ(drawn_row(cyrillic, 1, codes.size()), "Вторник");
}

TEST(TeletextCatalogueView, DrawsTheLatinNationalOptionSubset) {
  // The sub-set still applies within Latin, and must not have been lost when
  // the G0 set joined it: 2/3 is "£" for a UK page and "#" for a German one.
  const std::string codes = "#";

  const CatalogueDataset uk = build_teletext_catalogue(
      make_dataset(codes, TeletextG0Set::Latin,
                   static_cast<int>(TeletextNationalOption::English)));
  EXPECT_EQ(drawn_row(uk, 1, 1), "£");

  const CatalogueDataset german = build_teletext_catalogue(
      make_dataset(codes, TeletextG0Set::Latin,
                   static_cast<int>(TeletextNationalOption::German)));
  EXPECT_EQ(drawn_row(german, 1, 1), "#");
}

TEST(TeletextCatalogueView, TheCyrillicSetsIgnoreTheHeadersSubsetBits) {
  // A Cyrillic page whose header happens to carry a national option — which is
  // exactly what the reference Russian broadcast sends, C12-C14 = 100 — must
  // still draw as Cyrillic. 2/6 is the position the sub-set logic would most
  // easily corrupt: "&" in Latin, a letter here.
  const CatalogueDataset cyrillic = build_teletext_catalogue(
      make_dataset("&", TeletextG0Set::Cyrillic2,
                   static_cast<int>(TeletextNationalOption::French)));
  EXPECT_EQ(drawn_row(cyrillic, 1, 1), "ы");
}

// The variant item carrying the page's grid, and its payload.
const CatalogueItem* page_variant(const CatalogueDataset& catalogue) {
  for (size_t i = 0; i < catalogue.items.size(); ++i) {
    if (catalogue.payloads[i].kind == CataloguePayload::Kind::kCellGrid) {
      return &catalogue.items[i];
    }
  }
  return nullptr;
}

TEST(TeletextCatalogueView, OffersAPagesFlofLinksAsItsKeys) {
  // EN 300 706 Annex H.2: the first four links of X/27/0 are the coloured
  // keys, the sixth the index key; the fifth is for the receiver alone.
  TeletextAnalysisDataset dataset = make_dataset("INDEX", TeletextG0Set::Latin);
  TeletextPageSnapshot& page = dataset.pages.front().subpages.front().page;
  page.links[0] = TeletextPageLink{1, 0x02, kTeletextAnySubcode};
  page.links[3] = TeletextPageLink{2, 0x50, kTeletextAnySubcode};
  page.links[4] = TeletextPageLink{1, 0x99, kTeletextAnySubcode};
  page.links[5] = TeletextPageLink{1, 0x00, kTeletextAnySubcode};

  const CatalogueDataset catalogue = build_teletext_catalogue(dataset);
  const CatalogueItem* item = page_variant(catalogue);
  ASSERT_NE(item, nullptr);
  ASSERT_EQ(item->links.size(), 3u);
  for (const CatalogueLink& link : item->links) {
    EXPECT_EQ(link.role, CatalogueLink::Role::kKey) << link.label;
  }
  EXPECT_EQ(item->links[0].label, "Red");
  EXPECT_EQ(item->links[0].target_id, "102");
  EXPECT_EQ(item->links[1].label, "Cyan");
  EXPECT_EQ(item->links[1].target_id, "250");
  EXPECT_EQ(item->links[2].label, "Index");
  EXPECT_EQ(item->links[2].target_id, "100");
  // The page's own row carries them too, for a reader who selects the page.
  EXPECT_EQ(catalogue.items.front().links.size(), 3u);
}

TEST(TeletextCatalogueView, DrawsALevel15CharacterInPlaceOfTheLevel1One) {
  TeletextAnalysisDataset dataset = make_dataset("CAFE", TeletextG0Set::Latin);
  TeletextPageSnapshot& page = dataset.pages.front().subpages.front().page;
  page.cells[1][3].enhanced = U'é';
  page.cells[1][2].mosaic = true;  // even in a mosaic run
  page.cells[1][2].character = 0x7F;
  page.cells[1][2].enhanced = char32_t{0x2592};
  page.enhanced_characters = 2;

  const CatalogueDataset catalogue = build_teletext_catalogue(dataset);
  EXPECT_EQ(drawn_row(catalogue, 1, 2), "CA");
  const CataloguePayload* payload = nullptr;
  for (const CataloguePayload& candidate : catalogue.payloads) {
    if (candidate.kind == CataloguePayload::Kind::kCellGrid) {
      payload = &candidate;
    }
  }
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(payload->grid.at(1, 3).character, U'é');
  EXPECT_EQ(payload->grid.at(1, 2).character, char32_t{0x2592});
  EXPECT_FALSE(payload->grid.at(1, 2).mosaic);
  EXPECT_NE(payload->condition.find("2 characters added by packets X/26"),
            std::string::npos);
}

TEST(TeletextCatalogueView, LeavesRow24BlankWhereThePageSaysNotToShowIt) {
  TeletextAnalysisDataset dataset = make_dataset("", TeletextG0Set::Latin);
  TeletextPageSnapshot& page = dataset.pages.front().subpages.front().page;
  page.cells[24][0].character = 'P';
  page.row_received[24] = true;
  page.display_row_24 = false;
  const CatalogueCellGrid grid = teletext_page_grid(page);
  EXPECT_EQ(grid.at(24, 0).character, U' ');
  page.display_row_24 = true;
  EXPECT_EQ(teletext_page_grid(page).at(24, 0).character, U'P');
}

TEST(TeletextCatalogueView, ListsTheDataSentOutsideThePagesAfterThem) {
  TeletextAnalysisDataset dataset = make_dataset("NEWS", TeletextG0Set::Latin);
  dataset.service_data_packets = 12;
  dataset.service_data = {"packet 8/30: 12 of Format 1", "initial page 100"};
  TeletextDataLine line;
  line.channel = 9;
  line.format = "IDL Format A";
  line.tag = "9-2A";
  line.packets = 3;
  line.summary = "data channel 9, IDL Format A, address 2A";
  line.payload = {'H', 'E', 'L', 'L', 'O'};
  dataset.data_lines.push_back(line);
  TeletextDataLine binary = line;
  binary.tag = "9-2B";
  binary.payload = {0x00, 0x01, 0xFE, 0xFF};
  dataset.data_lines.push_back(binary);
  TeletextDataLine page_data = line;
  page_data.channel = -1;
  page_data.format = "Page Format - Clear";
  page_data.tag = "1F0-s1-a3";
  dataset.data_lines.push_back(page_data);

  const CatalogueDataset catalogue = build_teletext_catalogue(dataset);
  ASSERT_EQ(catalogue.items.size(), catalogue.payloads.size());
  ASSERT_GE(catalogue.items.size(), 6u);
  const size_t first = catalogue.items.size() - 4;
  EXPECT_EQ(catalogue.items[first].values.front(), "8/30");
  EXPECT_EQ(catalogue.items[first].values[1], "12");
  EXPECT_EQ(catalogue.payloads[first].kind, CataloguePayload::Kind::kText);
  EXPECT_EQ(catalogue.payloads[first].document.text,
            "packet 8/30: 12 of Format 1\ninitial page 100");
  EXPECT_EQ(catalogue.items[first + 1].values.front(), "Data 9-2A");
  EXPECT_NE(catalogue.payloads[first + 1].document.text.find("HELLO"),
            std::string::npos);
  // Bytes that are not text are dumped.
  EXPECT_NE(catalogue.payloads[first + 2].document.text.find("00 01 FE FF"),
            std::string::npos);
  const auto& notices = catalogue.summary.notices;
  EXPECT_NE(std::find(notices.begin(), notices.end(),
                      "Broadcast service data (packet 8/30)"),
            notices.end());
  EXPECT_NE(
      std::find(notices.begin(), notices.end(), "2 independent data lines"),
      notices.end());
  EXPECT_NE(
      std::find(notices.begin(), notices.end(), "1 data page application"),
      notices.end());
  EXPECT_NE(catalogue.items[first + 3].tooltip.find("Page Format - Clear"),
            std::string::npos);
}

}  // namespace
}  // namespace orc
