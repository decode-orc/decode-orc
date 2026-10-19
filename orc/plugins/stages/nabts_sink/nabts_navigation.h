/*
 * File:        nabts_navigation.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     How a viewer moved between a teletext service's pages: the
 *              numbers a decoder's keypad took, where its keys led, the page
 *              numbers a page offers, and the page a viewer came from
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_NAVIGATION_H
#define ORC_NABTS_NAVIGATION_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vbi-services/nabts_page.h"

namespace orc {

// The pages a page may be presented over (nabts_presentation_contexts()), at
// most: enough for a board, a question on it and an answer to the question,
// with room to spare, and a bound on a walk through pages that lead to one
// another.
constexpr size_t kNabtsMaxContextPages = 4;

/**
 * @brief The number a viewer typed to reach a record, or empty
 *
 * The Panasonic TU-2000x takes a page number as
 * decimal digits and ENTER, and its ENTER ($D981 in its firmware) sets them
 * right-aligned at A7 of the nine-digit long address (§5.2.5), the two digits
 * after it zero, where at most seven were typed — so three digits are the
 * short address PQR, which §5.2.5 makes 0000PQR00, and four reach the long
 * addresses a service can give its larger pages (1985 is 000198500) — and
 * right-aligned
 * at A9 where eight or nine were. The number is the shortest that reaches the
 * address that way, never less than three digits.
 *
 * Empty where an address digit is not a decimal digit: the keypad has no key
 * for A to F, so those records (the support record at FFF among them) are not
 * pages a viewer could ask for.
 *
 * @param address The record address in long form (§5.2.5)
 */
std::string nabts_keypad_number(uint64_t address);

/**
 * @brief The record address a number typed at the keypad reaches
 *
 * The inverse of nabts_keypad_number(): one to seven digits right-aligned at
 * A7, eight or nine at A9.
 *
 * @param digits  Decimal digits, nothing else
 * @param address Set to the long address (§5.2.5) where the digits are valid
 * @return False for an empty string, more than nine digits or a non-digit
 */
bool nabts_keypad_address(const std::string& digits, uint64_t& address);

/**
 * @brief The record a decoder's NEXT key fetched when the page named none
 *
 * Where a page's header extension names no Next Record (§5.2.8.2 meaning 010),
 * the TU-2000x's NEXT ($D5D3) takes the next page by number: the two digits
 * after A7 cleared and A1 to A7 incremented as a decimal number, so 009 steps
 * to 010 and 000198500 to 000198600.
 *
 * @param address The record address in long form
 * @param next    Set to the next page's long address
 * @return False where A1 to A7 are not decimal digits or are already all nines
 */
bool nabts_next_page_address(uint64_t address, uint64_t& next);

/**
 * @brief The page's text as lines, one string a row of the screen
 *
 * The characters the page left on screen, grouped into rows by baseline and
 * read left to right. Unlike nabts_page_text(), a gap wider than a character
 * field between two runs on a row is read as a space, and a character written
 * over most of another — a title drawn twice, a pixel apart, for a shadow —
 * keeps only the one drawn last, which is the one on top. So "WRONG" with
 * "750" set beside it reads "WRONG 750", and a shadowed "150" reads "150"
 * rather than "115500". Mosaics and DRCS characters have no text and are left
 * out, as they are by nabts_page_text().
 */
std::vector<std::string> nabts_page_lines(const NabtsPageSnapshot& page);

/**
 * @brief The page numbers a page's text offers its viewer, in order
 *
 * A teletext page tells its viewer where to go in its own words: an index
 * lists its pages ("10.Weather", "NEWSFRONT.......2", "61 Basketball
 * tournament"), a story points on ("See page 500", "...p21", "See Pg.8"), a
 * quiz lists its answers by page number ("751 They're former first ladies")
 * and a wrong answer sends the viewer back ("WRONG 750"), and a game board is
 * a grid of numbers. What is read as a page number:
 *   - digits after "p", "pg", "page" or "pages" ("p21", "Pg.8", "page 500");
 *   - digits after "Press" ("Press 751");
 *   - the number a dot leader leads to from a title in capitals, where it ends
 *     the row ("NEWSFRONT.......2") — a table's leaders lead to scores and
 *     prices, with lower case before them or more of the row after them;
 *   - every number on a row that holds nothing else ("150   250   350");
 *   - the number at the end of a row of capitals ("BEIRUT BASE SEALED 4",
 *     "WRONG 750"), both ends of a range there ("HIGHLIGHTS 12-13"), where the
 *     row holds no other number (a row of results does);
 *   - the number a row starts with, followed by a full stop or a space and a
 *     word ("10.Weather", "151 silk"), or by a quantity and a word ("351 10
 *     votes"), where at least two rows of the page start so — a list, rather
 *     than a line of a story that happened to wrap before a number — and the
 *     first is not 1, which makes it a ranking; a row that goes on to more
 *     figures after its title is a row of a table, such as a ranking with
 *     its ratings;
 *   - every number followed by a colon and a title, as an index set in
 *     columns gives them ("24: Abbot-Anheuser   32: Kraft"), where the page
 *     has at least two and none is 1 — a legend of option keys numbers from 1
 *     ("1:Black stone", "9: Cursor").
 * Each number is given as it stands, once, in the order first met. Whether a
 * page of that number exists is the caller's business.
 */
std::vector<std::string> nabts_offered_numbers(
    const std::vector<std::string>& lines);

/**
 * @brief Whether a page starts on whatever was on screen before it
 *
 * A page that neither clears the display (CS, or RESET's display action,
 * §5.3.2.9.2 Table 15) nor fills the whole display area with a rectangle before
 * it draws anything else is drawn over the page the viewer came from — on the
 * Panasonic TU-2000x, which does not clear its screen when a new page is asked
 * for. Game pages were drawn that way on purpose: each question of a quiz can
 * put a mark on one square of the board the index page drew and its question
 * beneath, and each right answer one piece of a picture in
 * that square. (The Norpak TTX 650 does clear on a page request, so it showed
 * those pages on black.)
 */
bool nabts_draws_over_previous(const NabtsPageSnapshot& page);

/**
 * @brief One presentation record, as the search for the page a viewer came
 *        from needs it
 *
 * Addresses are long (§5.2.5) and in the record's own data channel.
 */
struct NabtsNavigationPage {
  uint16_t channel = 0;
  uint64_t address = 0;
  uint8_t version = 0;
  /// Frames the record was first and last seen on, to pair a page with the
  /// page it was sent beside when either was revised.
  uint64_t first_frame = 0;
  uint64_t last_frame = 0;

  /// Records its header extension names (§5.2.8.2 meanings 001, 010, 011):
  /// what MORE, NEXT and INDEX fetched from it.
  bool has_more = false;
  uint64_t more = 0;
  bool has_next = false;
  uint64_t next = 0;
  bool has_index = false;
  uint64_t index = 0;
  /// The next page by number (nabts_next_page_address()), which NEXT fetched
  /// where the header named no Next Record.
  bool has_next_by_number = false;
  uint64_t next_by_number = 0;
  /// Addresses of the pages its text offers (nabts_offered_numbers()).
  std::vector<uint64_t> offered;

  /// The record drew something a viewer could have seen.
  bool drawn = false;
  /// It starts on the screen it was asked from (nabts_draws_over_previous()).
  bool draws_over = false;
  /// A page a viewer came from may be looked for: it draws over, a copy of it
  /// arrived whole and checked and its start with it, a Full Service Standard
  /// receiver would show it, and it is not presented over a More Record
  /// predecessor already.
  bool wants_context = false;
};

/**
 * @brief The pages each page is presented over, oldest first
 *
 * For a page that draws over the screen it was asked from (wants_context), the
 * pages a viewer came to it through, found the way a TU-2000x viewer moved.
 * A step is surest from a page whose header extension points its MORE, NEXT
 * or INDEX at the next page, then from a page whose text offers the next
 * page's number, then from the page before it by number, whose NEXT would have
 * reached it; each step costs one more the less sure it is. The way taken
 * starts at a page that stands as it is — one that draws a whole screen of its
 * own, or wants no context — passes only through pages that draw over the page
 * before, holds at most kNabtsMaxContextPages pages, and is the cheapest such
 * way, then the shortest: so an answer of such a quiz comes out over its
 * question and the board, rather than over the copy of the question that the
 * board's NEXT led to. Among ways as cheap and as short, the last step's page
 * decides: one that draws a whole screen of its own over one that draws over
 * another, then the one whose time on air overlapped this page's most, then
 * the lowest address, then the highest version. A page no such way reaches is
 * presented on its own.
 *
 * @param pages Every presentation record of the catalogue
 * @return Parallel to |pages|: indices into it, oldest first; empty where a
 *         page is presented on its own. At most kNabtsMaxContextPages each.
 *
 * Complexity is O(W * N) in the pages that want a context W and the pages N;
 * a carousel keeps W to a few dozen and N to a few hundred.
 */
std::vector<std::vector<size_t>> nabts_presentation_contexts(
    const std::vector<NabtsNavigationPage>& pages);

}  // namespace orc

#endif  // ORC_NABTS_NAVIGATION_H
