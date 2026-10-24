/*
 * File:        nabts_navigation_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for how a viewer moved between a NABTS service's
 *              pages
 *
 * Covers: the number a decoder's keypad took for a record address and the
 * address it reached, the next page by number, the page's text read row by
 * row, the page numbers a page's text offers, whether a page starts on the
 * screen it was asked from, and the pages a page is presented over.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_navigation.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::NabtsNavigationPage;
using orc::NabtsPageSnapshot;
using orc::NabtsPrimitive;
using orc::NabtsPrimitiveKind;

// The default character field of the reference receiver (X3.110 Table D1).
constexpr double kFieldWidth = 1.0 / 40.0;
constexpr double kFieldHeight = 0.0390625;

/// |text| as characters of the default field from |x|, on the row at |y|.
void place_text(NabtsPageSnapshot& page, const std::string& text, double x,
                double y) {
  for (const char c : text) {
    NabtsPrimitive character;
    character.kind = NabtsPrimitiveKind::kCharacter;
    character.character = static_cast<uint8_t>(c);
    character.origin = {x, y};
    character.size = {kFieldWidth, kFieldHeight};
    page.primitives.push_back(character);
    x += kFieldWidth;
  }
}

NabtsPrimitive rectangle(double x, double y, double width, double height,
                         bool filled) {
  NabtsPrimitive out;
  out.kind = NabtsPrimitiveKind::kRectangle;
  out.origin = {x, y};
  out.size = {width, height};
  out.filled = filled;
  return out;
}

NabtsNavigationPage page_at(uint64_t address) {
  NabtsNavigationPage page;
  page.address = address;
  page.drawn = true;
  page.first_frame = 0;
  page.last_frame = 1000;
  return page;
}

/// A page that starts on the screen it was asked from and wants the page a
/// viewer came from.
NabtsNavigationPage partial_page_at(uint64_t address) {
  NabtsNavigationPage page = page_at(address);
  page.draws_over = true;
  page.wants_context = true;
  return page;
}

// ---- The keypad -------------------------------------------------------------

TEST(NabtsKeypadNumber, AShortAddressIsItsThreeDigits) {
  EXPECT_EQ(orc::nabts_keypad_number(0x000015000ULL), "150");
  EXPECT_EQ(orc::nabts_keypad_number(0x000000100ULL), "001");
  EXPECT_EQ(orc::nabts_keypad_number(0x000000000ULL), "000");
}

TEST(NabtsKeypadNumber, ALongPageNumberIsTheDigitsUpToA7) {
  // A carousel's long-address pages: typed as four digits on the TU-2000x.
  EXPECT_EQ(orc::nabts_keypad_number(0x000198500ULL), "1985");
  EXPECT_EQ(orc::nabts_keypad_number(0x000500100ULL), "5001");
}

TEST(NabtsKeypadNumber, AMoreRecordIsReachedByItsWholeAddress) {
  EXPECT_EQ(orc::nabts_keypad_number(0x000000201ULL), "000000201");
}

TEST(NabtsKeypadNumber, AHexadecimalDigitHasNoKey) {
  EXPECT_EQ(orc::nabts_keypad_number(0x0000FFF00ULL), "");
  EXPECT_EQ(orc::nabts_keypad_number(0x0000F0000ULL), "");
}

TEST(NabtsKeypadAddress, DigitsAreRightAlignedAtA7OrA9) {
  uint64_t address = 0;
  ASSERT_TRUE(orc::nabts_keypad_address("150", address));
  EXPECT_EQ(address, 0x000015000ULL);
  ASSERT_TRUE(orc::nabts_keypad_address("1985", address));
  EXPECT_EQ(address, 0x000198500ULL);
  ASSERT_TRUE(orc::nabts_keypad_address("21", address));
  EXPECT_EQ(address, 0x000002100ULL);
  ASSERT_TRUE(orc::nabts_keypad_address("000000201", address));
  EXPECT_EQ(address, 0x000000201ULL);
  ASSERT_TRUE(orc::nabts_keypad_address("00000201", address));
  EXPECT_EQ(address, 0x000000201ULL);
}

TEST(NabtsKeypadAddress, RefusesWhatIsNotANumber) {
  uint64_t address = 7;
  EXPECT_FALSE(orc::nabts_keypad_address("", address));
  EXPECT_FALSE(orc::nabts_keypad_address("1A4", address));
  EXPECT_FALSE(orc::nabts_keypad_address("1234567890", address));
  EXPECT_EQ(address, 7U);
}

TEST(NabtsKeypadNumber, RoundTripsThroughTheKeypad) {
  for (const uint64_t address :
       {0x000015000ULL, 0x000198500ULL, 0x000000201ULL, 0x000000000ULL}) {
    uint64_t back = 0;
    ASSERT_TRUE(
        orc::nabts_keypad_address(orc::nabts_keypad_number(address), back));
    EXPECT_EQ(back, address);
  }
}

TEST(NabtsNextPage, CountsInDecimal) {
  uint64_t next = 0;
  ASSERT_TRUE(orc::nabts_next_page_address(0x000000900ULL, next));
  EXPECT_EQ(next, 0x000001000ULL);  // 009 -> 010
  ASSERT_TRUE(orc::nabts_next_page_address(0x000009900ULL, next));
  EXPECT_EQ(next, 0x000010000ULL);  // 099 -> 100
  ASSERT_TRUE(orc::nabts_next_page_address(0x000198500ULL, next));
  EXPECT_EQ(next, 0x000198600ULL);
}

TEST(NabtsNextPage, FromAMoreRecordGoesToTheNextPage) {
  uint64_t next = 0;
  ASSERT_TRUE(orc::nabts_next_page_address(0x000000201ULL, next));
  EXPECT_EQ(next, 0x000000300ULL);  // 000000201 -> 003
}

TEST(NabtsNextPage, EndsAtTheLastNumberAndAtHexadecimal) {
  uint64_t next = 0;
  EXPECT_FALSE(orc::nabts_next_page_address(0x999999900ULL, next));
  EXPECT_FALSE(orc::nabts_next_page_address(0x0000FFF00ULL, next));
}

// ---- The page's text
// ---------------------------------------------------------

TEST(NabtsPageLines, ReadsRowsDownTheScreen) {
  NabtsPageSnapshot page;
  place_text(page, "Try again", 0.1, 0.40);
  place_text(page, "RIGHT!", 0.1, 0.60);
  EXPECT_EQ(orc::nabts_page_lines(page),
            (std::vector<std::string>{"RIGHT!", "Try again"}));
}

TEST(NabtsPageLines, AGapBetweenRunsIsASpace) {
  // A quiz's wrong answers set the question's number beside "WRONG".
  NabtsPageSnapshot page;
  place_text(page, "WRONG", 0.1, 0.60);
  place_text(page, "750", 0.1 + 7 * kFieldWidth, 0.60);
  EXPECT_EQ(orc::nabts_page_lines(page),
            (std::vector<std::string>{"WRONG 750"}));
}

TEST(NabtsPageLines, AShadowedTitleIsReadOnce) {
  NabtsPageSnapshot page;
  place_text(page, "150", 0.1, 0.60);
  place_text(page, "150", 0.1 + 1.0 / 256.0, 0.60 + 1.0 / 256.0);
  EXPECT_EQ(orc::nabts_page_lines(page), (std::vector<std::string>{"150"}));
}

TEST(NabtsOfferedNumbers, AGameBoardOffersEveryNumber) {
  const std::vector<std::string> board = {"PUZZLE STUMPERS", "150 250 350",
                                          "450 550 650", "750 850 950"};
  EXPECT_EQ(orc::nabts_offered_numbers(board),
            (std::vector<std::string>{"150", "250", "350", "450", "550", "650",
                                      "750", "850", "950"}));
}

TEST(NabtsOfferedNumbers, AQuestionOffersItsAnswers) {
  const std::vector<std::string> question = {
      "What's the traditional wedding gift", "after 1 yr. of marriage?",
      "151 silk", "152 emerald", "153 paper"};
  EXPECT_EQ(orc::nabts_offered_numbers(question),
            (std::vector<std::string>{"151", "152", "153"}));
}

TEST(NabtsOfferedNumbers, AnswersThatAreQuantitiesAreOfferedToo) {
  const std::vector<std::string> question = {
      "How many electoral votes did Walter",
      "Mondale win in the 1984 election?", "351 10 votes", "352 13 votes",
      "353 45 votes"};
  EXPECT_EQ(orc::nabts_offered_numbers(question),
            (std::vector<std::string>{"351", "352", "353"}));
  // A row of figures is no list of pages.
  EXPECT_TRUE(
      orc::nabts_offered_numbers({"1984 12 3 .800", "1985 14 2 .875"}).empty());
}

TEST(NabtsOfferedNumbers, ATablesRowsOfferNothing) {
  // Nielsen's ranking, whose first row a tape lost, and a poll in two columns:
  // figures after the title.
  EXPECT_TRUE(orc::nabts_offered_numbers({"3 Growing Pains 4 Little Match Girl",
                                          "6 Murder, She Wrote(R) CBS 18.4 30",
                                          "10 Matlock NBC 17.1 29"})
                  .empty());
  EXPECT_TRUE(
      orc::nabts_offered_numbers({"5 LSU 15 UCLA", "6 Nebraska 16 Arizona"})
          .empty());
}

TEST(NabtsOfferedNumbers, AWrongAnswerOffersTheQuestion) {
  EXPECT_EQ(orc::nabts_offered_numbers({"WRONG 750", "Try again"}),
            (std::vector<std::string>{"750"}));
}

TEST(NabtsOfferedNumbers, PageWordsAndDotLeadersPointAtPages) {
  EXPECT_EQ(
      orc::nabts_offered_numbers(
          {"at new record high.................p21", "See page 500 for details",
           "THE GREAT JAMAICA TAX BREAK. See Pg.8", "NEWSFRONT.......2",
           "SATURDAY'S HIGHLIGHTS 12-13"}),
      (std::vector<std::string>{"21", "500", "8", "2", "12", "13"}));
}

TEST(NabtsOfferedNumbers, AnIndexListsItsPages) {
  EXPECT_EQ(orc::nabts_offered_numbers({"Page", "11.US Weather Conditions",
                                        "12.US Temperature Map",
                                        "13.Travelers' Advisory"}),
            (std::vector<std::string>{"11", "12", "13"}));
}

TEST(NabtsOfferedNumbers, AnIndexInColumnsNumbersItsEntriesWithAColon) {
  // A stock index.
  EXPECT_EQ(orc::nabts_offered_numbers({"As of: 9:31 EST", "PAGE: PAGE:",
                                        "24: Abbot-Anheuser 32: Kraft -McDerm",
                                        "25: Apple-Black & 33: McDona-NBD Ba"}),
            (std::vector<std::string>{"24", "32", "25", "33"}));
  EXPECT_TRUE(
      orc::nabts_offered_numbers({"Kickoff at 1:05", "Final 10:30"}).empty());
  // A film's title in a listing, beside a list of another kind.
  EXPECT_TRUE(orc::nabts_offered_numbers({"9:30 Police Academy 5: Miami Beach",
                                          "11:00 Action Jackson",
                                          "00 Smothers Brothers"})
                  .empty());
  // A game of Go: a legend of option keys.
  EXPECT_TRUE(orc::nabts_offered_numbers({"7 9: Cursor", "1:Black stone",
                                          "2:White stone", "3:Erase stone"})
                  .empty());
}

TEST(NabtsOfferedNumbers, ATablesLeadersLeadToValuesNotPages) {
  EXPECT_TRUE(orc::nabts_offered_numbers({"Indianapolis...10NY Giants.......35",
                                          "RCA......... 48 7/8 47 5/8  10,529",
                                          "AMEX Ind.......1.,244927..0092"})
                  .empty());
}

TEST(NabtsOfferedNumbers, ARankingAndAStoryOfferNothing) {
  EXPECT_TRUE(
      orc::nabts_offered_numbers({"1 Penn St.", "2 Miami", "3 Iowa"}).empty());
  EXPECT_TRUE(orc::nabts_offered_numbers({"1.Penn St. (47)    11-0-0",
                                          "2.Miami, FL (3)    10-1-0",
                                          "9.BYU              11-2-0"})
                  .empty());
  EXPECT_TRUE(orc::nabts_offered_numbers(
                  {"innocent to 13 charges that she fur-",
                   "WINNIPEG 7 NYRNGRS 5", "Press NEXT for more"})
                  .empty());
}

// ---- Drawing over the screen
// ----------------------------------------------------

TEST(NabtsDrawsOverPrevious, APageThatClearsFirstDoesNot) {
  NabtsPageSnapshot page;
  place_text(page, "INDEX", 0.1, 0.6);
  page.timeline.clears.push_back(orc::NabtsDisplayClear{0, 0, 0});
  EXPECT_FALSE(orc::nabts_draws_over_previous(page));
}

TEST(NabtsDrawsOverPrevious, AGroundOverTheDisplayAreaHidesThePageBefore) {
  NabtsPageSnapshot page;
  page.primitives.push_back(rectangle(0.0, 0.0, 0.996, 0.777, true));
  place_text(page, "News", 0.1, 0.6);
  EXPECT_FALSE(orc::nabts_draws_over_previous(page));
}

TEST(NabtsDrawsOverPrevious, ASquareOfTheBoardDrawsOverIt) {
  NabtsPageSnapshot page;
  page.primitives.push_back(rectangle(0.078, 0.383, 0.27, -0.148, true));
  place_text(page, "751 silk", 0.1, 0.2);
  EXPECT_TRUE(orc::nabts_draws_over_previous(page));
}

TEST(NabtsDrawsOverPrevious, APageThatDrawsNothingDoesNot) {
  EXPECT_FALSE(orc::nabts_draws_over_previous(NabtsPageSnapshot{}));
}

// ---- The page a viewer came from
// ----------------------------------------------

TEST(NabtsPresentationContexts, AnAnswerIsPresentedOverItsQuestionAndBoard) {
  NabtsNavigationPage board = page_at(0x000071100ULL);
  board.offered = {0x000015000ULL, 0x000075000ULL};
  NabtsNavigationPage question = partial_page_at(0x000075000ULL);
  question.offered = {0x000075100ULL, 0x000075200ULL, 0x000075300ULL};
  const NabtsNavigationPage answer = partial_page_at(0x000075300ULL);
  const auto contexts =
      orc::nabts_presentation_contexts({board, question, answer});
  EXPECT_TRUE(contexts[0].empty());
  EXPECT_EQ(contexts[1], (std::vector<size_t>{0}));
  EXPECT_EQ(contexts[2], (std::vector<size_t>{0, 1}));
}

TEST(NabtsPresentationContexts, NextByNumberReachesAPageNothingOffers) {
  NabtsNavigationPage board = page_at(0x000071100ULL);
  board.has_next_by_number = true;
  board.next_by_number = 0x000071200ULL;
  const auto contexts = orc::nabts_presentation_contexts(
      {board, partial_page_at(0x000071200ULL)});
  EXPECT_EQ(contexts[1], (std::vector<size_t>{0}));
}

TEST(NabtsPresentationContexts, AKeyTheHeaderNamesComesBeforeAnOfferedNumber) {
  // Rotating highlights: the News card's MORE names the Finance card.
  NabtsNavigationPage news = page_at(0x000003909ULL);
  news.has_more = true;
  news.more = 0x000000109ULL;
  NabtsNavigationPage elsewhere = page_at(0x000000100ULL);
  elsewhere.offered = {0x000000109ULL};
  const auto contexts = orc::nabts_presentation_contexts(
      {elsewhere, news, partial_page_at(0x000000109ULL)});
  EXPECT_EQ(contexts[2], (std::vector<size_t>{1}));
}

TEST(NabtsPresentationContexts, AWholePageIsPreferredToOneThatDrawsOver) {
  NabtsNavigationPage partial = partial_page_at(0x000071200ULL);
  partial.offered = {0x000015000ULL};
  partial.wants_context = false;
  NabtsNavigationPage whole = page_at(0x000071100ULL);
  whole.offered = {0x000015000ULL};
  const auto contexts = orc::nabts_presentation_contexts(
      {partial, whole, partial_page_at(0x000015000ULL)});
  EXPECT_EQ(contexts[2], (std::vector<size_t>{1}));
}

TEST(NabtsPresentationContexts, TheVersionOnAirAtTheTimeIsPreferred) {
  NabtsNavigationPage early = page_at(0x000071100ULL);
  early.offered = {0x000015000ULL};
  early.first_frame = 0;
  early.last_frame = 100;
  NabtsNavigationPage late = early;
  late.version = 1;
  late.first_frame = 200;
  late.last_frame = 900;
  NabtsNavigationPage question = partial_page_at(0x000015000ULL);
  question.first_frame = 300;
  question.last_frame = 800;
  const auto contexts =
      orc::nabts_presentation_contexts({early, late, question});
  EXPECT_EQ(contexts[2], (std::vector<size_t>{1}));
}

TEST(NabtsPresentationContexts, APageThatWantsNoneGetsNone) {
  NabtsNavigationPage board = page_at(0x000071100ULL);
  board.offered = {0x000015000ULL};
  NabtsNavigationPage question = partial_page_at(0x000015000ULL);
  question.wants_context = false;
  EXPECT_TRUE(orc::nabts_presentation_contexts({board, question})[1].empty());
}

TEST(NabtsPresentationContexts, PagesThatOnlyLeadToEachOtherStandAlone) {
  // Neither draws a screen of its own, so there is nothing to start from.
  NabtsNavigationPage a = partial_page_at(0x000002000ULL);
  a.offered = {0x000003000ULL};
  NabtsNavigationPage b = partial_page_at(0x000003000ULL);
  b.offered = {0x000002000ULL};
  const auto contexts = orc::nabts_presentation_contexts({a, b});
  EXPECT_TRUE(contexts[0].empty());
  EXPECT_TRUE(contexts[1].empty());
}

TEST(NabtsPresentationContexts, AnAnswerIsDrawnOverTheQuestionTheBoardOffers) {
  // A quiz board (711) offers its questions, and its NEXT reaches 712, a copy
  // of question 150 that offers the same answers: the answer comes out over
  // the question the board offers, the surer and as short a way.
  NabtsNavigationPage board = page_at(0x000071100ULL);
  board.offered = {0x000015000ULL};
  board.has_next_by_number = true;
  board.next_by_number = 0x000071200ULL;
  NabtsNavigationPage copy = partial_page_at(0x000071200ULL);
  copy.offered = {0x000015100ULL};
  copy.first_frame = 0;
  copy.last_frame = 5000;
  NabtsNavigationPage question = partial_page_at(0x000015000ULL);
  question.offered = {0x000015100ULL};
  NabtsNavigationPage answer = partial_page_at(0x000015100ULL);
  answer.last_frame = 5000;
  const auto contexts =
      orc::nabts_presentation_contexts({board, copy, question, answer});
  EXPECT_EQ(contexts[1], (std::vector<size_t>{0}));
  EXPECT_EQ(contexts[2], (std::vector<size_t>{0}));
  EXPECT_EQ(contexts[3], (std::vector<size_t>{0, 2}));
}

TEST(NabtsPresentationContexts, AWayThroughMorePagesThanTheLimitIsNotTaken) {
  // A board and five pages, each reached by NEXT from the one before.
  std::vector<NabtsNavigationPage> pages = {page_at(0x000071100ULL)};
  for (uint64_t number = 0x712; number <= 0x716; ++number) {
    pages.push_back(partial_page_at(number << 8));
  }
  for (size_t i = 0; i + 1 < pages.size(); ++i) {
    pages[i].has_next_by_number = true;
    pages[i].next_by_number = pages[i + 1].address;
  }
  const auto contexts = orc::nabts_presentation_contexts(pages);
  EXPECT_EQ(contexts[4], (std::vector<size_t>{0, 1, 2, 3}));
  EXPECT_TRUE(contexts[5].empty());
}

TEST(NabtsPresentationContexts, AnotherVersionOfThePageIsNoWayToIt) {
  NabtsNavigationPage earlier = page_at(0x000015000ULL);
  earlier.offered = {0x000015000ULL};
  const auto contexts = orc::nabts_presentation_contexts(
      {earlier, partial_page_at(0x000015000ULL)});
  EXPECT_TRUE(contexts[1].empty());
}

}  // namespace
}  // namespace orc_unit_test
