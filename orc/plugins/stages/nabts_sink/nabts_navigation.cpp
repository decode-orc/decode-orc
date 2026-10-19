/*
 * File:        nabts_navigation.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     How a viewer moved between a teletext service's pages: the
 *              numbers a decoder's keypad took, where its keys led, the page
 *              numbers a page offers, and the page a viewer came from
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_navigation.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <utility>

namespace orc {

namespace {

// CEA-516 §5.2.5: a long record address is nine hexadecimal digits, A1 to A9.
constexpr size_t kAddressDigits = 9;
// The TU-2000x's ENTER sets up to seven typed digits right-aligned at A7, the
// last of the digits a page number is made of; A8 and A9 number a page's More
// Records (§5.2.7.6).
constexpr size_t kPageNumberDigits = 7;
// A short address PQR is three digits (§5.2.5), the shortest number a page has.
constexpr size_t kShortestPageNumber = 3;
constexpr unsigned kHighestDecimalDigit = 9;
constexpr unsigned kNibbleBits = 4;
// The two More Record digits, A8 and A9, as one field of the long address.
constexpr unsigned kMoreDigitsBits = 8;

// A character drawn over another of the same code at less than this share of
// its field's width is the same character drawn twice — a title's shadow —
// and one written this far beyond the end of the field before it starts a new
// run of text.
constexpr double kSameFieldShare = 0.5;
constexpr double kGapShare = 0.5;
// Two characters share a row where their baselines are within this share of
// the taller character field, as nabts_page_text() groups them.
constexpr double kRowShare = 0.5;

// How far short of each edge of the display area a rectangle filling it may
// stop: a pixel and a half of the reference resolution (Table D1 item 10).
// Services fill their grounds from 0 to 255/256 across and 199/256 up, which a
// receiver drawing both edges of a rectangle lays over every pixel.
constexpr double kGroundTolerance = 1.5 / 256.0;

// Rows of a page that start with a number and a word before those numbers are
// taken for a list of pages (nabts_offered_numbers()).
constexpr size_t kListRowsAtLeast = 2;
// Numbers on a row of nothing else before they are taken for a board of pages.
constexpr size_t kGridNumbersAtLeast = 2;
// The most digits a page number has (a long address typed in full).
constexpr size_t kMostNumberDigits = kAddressDigits;

unsigned address_digit(uint64_t address, size_t index) {
  return static_cast<unsigned>(
             (address >> (kNibbleBits * (kAddressDigits - 1 - index)))) &
         0xFu;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
// A UTF-8 byte above ASCII is part of a letter of the supplementary set.
bool is_letter(char c) {
  return is_upper(c) || is_lower(c) || static_cast<unsigned char>(c) >= 0x80;
}

char lower(char c) {
  return is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

/// The run of digits starting at |at|, or empty.
std::string digits_at(const std::string& text, size_t at) {
  size_t end = at;
  while (end < text.size() && is_digit(text[end])) {
    ++end;
  }
  return text.substr(at, end - at);
}

std::vector<std::string> split_words(const std::string& line) {
  std::vector<std::string> words;
  size_t at = 0;
  while (at < line.size()) {
    while (at < line.size() && line[at] == ' ') {
      ++at;
    }
    size_t end = at;
    while (end < line.size() && line[end] != ' ') {
      ++end;
    }
    if (end > at) {
      words.push_back(line.substr(at, end - at));
    }
    at = end;
  }
  return words;
}

bool all_digits(const std::string& word) {
  return !word.empty() && std::all_of(word.begin(), word.end(),
                                      [](char c) { return is_digit(c); });
}

/// A figure: digits, with the full stops and commas of "19.6" or "1,500".
bool is_figure(const std::string& word) {
  return !word.empty() && is_digit(word.front()) &&
         std::all_of(word.begin(), word.end(), [](char c) {
           return is_digit(c) || c == '.' || c == ',';
         });
}

/// A number, or a range of two ("12-13"), as its numbers; empty where |word|
/// is neither.
std::vector<std::string> numbers_of(const std::string& word) {
  const size_t dash = word.find('-');
  if (dash == std::string::npos) {
    return all_digits(word) ? std::vector<std::string>{word}
                            : std::vector<std::string>{};
  }
  const std::string first = word.substr(0, dash);
  const std::string second = word.substr(dash + 1);
  if (all_digits(first) && all_digits(second)) {
    return {first, second};
  }
  return {};
}

/// Words that name a page number after them: "p21", "pg.61", "page 500",
/// "pages 12-13", "Press 751".
bool names_a_page(const std::string& word) {
  return word == "p" || word == "pg" || word == "page" || word == "pages" ||
         word == "press";
}

/// The numbers |line| offers after a page word, or after the dot leader of an
/// index entry: a title in capitals, dots, and a number that ends the row
/// ("NEWSFRONT.......2"). A table's dot leaders lead to values — a team's score
/// ("Indianapolis...10"), a share price ("RCA......... 48 7/8") — which have
/// lower case before them or more of the row after them.
void find_pointed_numbers(const std::string& line,
                          std::vector<std::string>& out) {
  for (size_t at = 0; at < line.size(); ++at) {
    size_t after = std::string::npos;
    bool must_end_row = false;
    if (is_letter(line[at]) && (at == 0 || !is_letter(line[at - 1]))) {
      size_t end = at;
      std::string word;
      while (end < line.size() && is_letter(line[end])) {
        word.push_back(lower(line[end]));
        ++end;
      }
      if (names_a_page(word)) {
        after = end;
      }
      at = end - 1;
    } else if (line[at] == '.' && at + 1 < line.size() && line[at + 1] == '.') {
      size_t end = at;
      while (end < line.size() && line[end] == '.') {
        ++end;
      }
      if (at > 0 && is_upper(line[at - 1])) {
        after = end;
        must_end_row = true;
      }
      at = end - 1;
    }
    if (after == std::string::npos) {
      continue;
    }
    if (after < line.size() && line[after] == '.') {
      ++after;
    }
    while (after < line.size() && line[after] == ' ') {
      ++after;
    }
    const std::string number = digits_at(line, after);
    if (number.empty() || number.size() > kMostNumberDigits) {
      continue;
    }
    // "p21" yes, "p21a" no: a page number stands on its own.
    const size_t end = after + number.size();
    if (end < line.size() && is_letter(line[end])) {
      continue;
    }
    if (must_end_row && line.find_first_not_of(' ', end) != std::string::npos) {
      continue;
    }
    out.push_back(number);
    if (end + 1 < line.size() && line[end] == '-') {
      const std::string last = digits_at(line, end + 1);
      if (!last.empty() && last.size() <= kMostNumberDigits) {
        out.push_back(last);
      }
    }
  }
}

/// A row of capitals ending in a number — an index entry ("BEIRUT BASE SEALED
/// 4"), a way back ("WRONG 750") — gives the number, both ends of a range.
void find_entry_number(const std::vector<std::string>& words,
                       std::vector<std::string>& out) {
  if (words.empty()) {
    return;
  }
  std::string last = words.back();
  std::vector<std::string> title(words.begin(), words.end() - 1);
  // A number set straight after the title, with no gap the row could show:
  // "RESULTS14-15".
  if (numbers_of(last).empty()) {
    size_t split = 0;
    while (split < last.size() && !is_digit(last[split])) {
      ++split;
    }
    if (split == 0 || split == last.size()) {
      return;
    }
    title.push_back(last.substr(0, split));
    last = last.substr(split);
  }
  const std::vector<std::string> numbers = numbers_of(last);
  if (numbers.empty() || title.empty()) {
    return;
  }
  bool has_letter = false;
  for (const std::string& word : title) {
    for (const char c : word) {
      // Not a row of capitals; or a row of results ("WINNIPEG 7 NYRNGRS 5"),
      // whose last number is a score.
      if (is_lower(c) || is_digit(c) || static_cast<unsigned char>(c) >= 0x80) {
        return;
      }
      has_letter = has_letter || is_upper(c);
    }
  }
  if (!has_letter) {
    return;
  }
  for (const std::string& number : numbers) {
    if (number.size() <= kMostNumberDigits) {
      out.push_back(number);
    }
  }
}

/// The number a list row starts with — "10.Weather", "151 silk", "253 "Oh,
/// Calcutta!"", "351 10 votes" — or empty.
std::string list_row_number(const std::vector<std::string>& words) {
  if (words.empty()) {
    return {};
  }
  const std::string& first = words.front();
  const std::string number = digits_at(first, 0);
  if (number.empty() || number.size() > kMostNumberDigits) {
    return {};
  }
  if (number.size() < first.size()) {
    // "10.Weather": a full stop, then the entry's title.
    return first[number.size()] == '.' && number.size() + 1 < first.size() &&
                   is_letter(first[number.size() + 1])
               ? number
               : std::string{};
  }
  if (words.size() < 2) {
    return {};
  }
  const auto starts_words = [](const std::string& word) {
    const char c = word.front();
    return is_letter(c) || c == '"' || c == '\'';
  };
  // An entry's title starts with a word, or with a quantity and a word — a
  // quiz's answers "351 10 votes", "653 102 feet".
  size_t title = 1;
  if (!starts_words(words[1])) {
    if (words.size() < 3 || !is_figure(words[1]) || !starts_words(words[2])) {
      return {};
    }
    title = 2;
  }
  // A row of a table goes on to more figures after its title — a ranking's
  // ratings, a poll's second column — where a list of pages names each page
  // once.
  for (size_t i = title + 1; i < words.size(); ++i) {
    if (is_figure(words[i])) {
      return {};
    }
  }
  return number;
}

/// The numbers of the entries of an index set in columns, each a number and a
/// colon before its title, as many to a row as there are columns — a stock
/// index: "24: Abbot-Anheuser   32: Kraft-McDerm". A time
/// ("9:31") has no title after its colon.
std::vector<std::string> colon_entries(const std::vector<std::string>& words) {
  std::vector<std::string> out;
  for (size_t i = 0; i < words.size(); ++i) {
    const std::string& word = words[i];
    const std::string number = digits_at(word, 0);
    if (number.empty() || number.size() > kMostNumberDigits ||
        number.size() >= word.size() || word[number.size()] != ':') {
      continue;
    }
    // The title follows the colon in the same word or starts the next one.
    const bool titled =
        number.size() + 1 < word.size()
            ? is_letter(word[number.size() + 1])
            : i + 1 < words.size() && is_letter(words[i + 1].front());
    if (titled) {
      out.push_back(number);
    }
  }
  return out;
}

}  // namespace

std::string nabts_keypad_number(uint64_t address) {
  std::string digits;
  digits.reserve(kAddressDigits);
  for (size_t index = 0; index < kAddressDigits; ++index) {
    const unsigned digit = address_digit(address, index);
    if (digit > kHighestDecimalDigit) {
      return {};
    }
    digits.push_back(static_cast<char>('0' + digit));
  }
  if (digits.compare(kPageNumberDigits, kAddressDigits - kPageNumberDigits,
                     "00") != 0) {
    // A More Record number of its own: only the whole address reaches it.
    return digits;
  }
  const std::string page = digits.substr(0, kPageNumberDigits);
  const size_t first = page.find_first_not_of('0');
  const size_t keep =
      first == std::string::npos
          ? kShortestPageNumber
          : std::max(kShortestPageNumber, kPageNumberDigits - first);
  return page.substr(kPageNumberDigits - keep);
}

bool nabts_keypad_address(const std::string& digits, uint64_t& address) {
  if (digits.empty() || digits.size() > kAddressDigits) {
    return false;
  }
  uint64_t value = 0;
  for (const char c : digits) {
    if (!is_digit(c)) {
      return false;
    }
    value = (value << kNibbleBits) | static_cast<uint64_t>(c - '0');
  }
  address =
      digits.size() <= kPageNumberDigits ? value << kMoreDigitsBits : value;
  return true;
}

bool nabts_next_page_address(uint64_t address, uint64_t& next) {
  const uint64_t page = address >> kMoreDigitsBits;
  uint64_t result = 0;
  unsigned carry = 1;
  for (size_t index = 0; index < kPageNumberDigits; ++index) {
    unsigned digit =
        static_cast<unsigned>(page >> (kNibbleBits * index)) & 0xFu;
    if (digit > kHighestDecimalDigit) {
      return false;
    }
    digit += carry;
    carry = digit > kHighestDecimalDigit ? 1 : 0;
    digit = carry != 0 ? 0 : digit;
    result |= static_cast<uint64_t>(digit) << (kNibbleBits * index);
  }
  if (carry != 0) {
    return false;  // every digit a nine: no page after it
  }
  next = result << kMoreDigitsBits;
  return true;
}

std::vector<std::string> nabts_page_lines(const NabtsPageSnapshot& page) {
  struct Placed {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    size_t order = 0;
    std::string text;
  };
  std::vector<Placed> placed;
  for (size_t index = 0; index < page.primitives.size(); ++index) {
    const NabtsPrimitive& primitive = page.primitives[index];
    if (primitive.kind != NabtsPrimitiveKind::kCharacter ||
        primitive.repertoire == NabtsPrimitive::Repertoire::kMosaic ||
        primitive.repertoire == NabtsPrimitive::Repertoire::kDrcs ||
        primitive.rotation != NabtsCharRotation::kNone ||
        primitive.path != NabtsCharPath::kRight) {
      // Shapes, and text set on its side or along another path, which a row
      // read left to right would only scramble.
      continue;
    }
    if (primitive.repertoire == NabtsPrimitive::Repertoire::kSupplementary &&
        nabts_supplementary_is_nonspacing(primitive.character)) {
      continue;  // an accent shares its letter's field
    }
    Placed entry;
    entry.x = primitive.origin.x;
    entry.y = primitive.origin.y;
    entry.width = std::fabs(primitive.size.dx);
    entry.height = std::fabs(primitive.size.dy);
    entry.order = index;
    entry.text =
        nabts_character_to_utf8(primitive.character, primitive.repertoire);
    placed.push_back(std::move(entry));
  }

  // Rows down the screen (unit y runs upwards), each read left to right.
  std::stable_sort(placed.begin(), placed.end(),
                   [](const Placed& a, const Placed& b) { return a.y > b.y; });
  std::vector<std::string> lines;
  size_t row_start = 0;
  while (row_start < placed.size()) {
    size_t row_end = row_start + 1;
    while (row_end < placed.size() &&
           std::fabs(placed[row_end].y - placed[row_start].y) <=
               kRowShare *
                   std::max(placed[row_start].height, placed[row_end].height)) {
      ++row_end;
    }
    std::vector<Placed> row(
        placed.begin() + static_cast<std::ptrdiff_t>(row_start),
        placed.begin() + static_cast<std::ptrdiff_t>(row_end));
    std::stable_sort(
        row.begin(), row.end(),
        [](const Placed& a, const Placed& b) { return a.x < b.x; });
    std::vector<const Placed*> kept;
    for (const Placed& entry : row) {
      if (!kept.empty()) {
        const Placed& before = *kept.back();
        if (before.text == entry.text &&
            entry.x - before.x <
                kSameFieldShare * std::max(before.width, entry.width)) {
          // The same character a pixel or two along: what shows is the one
          // drawn last.
          if (entry.order > before.order) {
            kept.back() = &entry;
          }
          continue;
        }
      }
      kept.push_back(&entry);
    }
    std::string line;
    for (size_t i = 0; i < kept.size(); ++i) {
      if (i > 0) {
        const Placed& before = *kept[i - 1];
        const double gap = kept[i]->x - (before.x + before.width);
        if (gap > kGapShare * std::max(before.width, kept[i]->width) &&
            line.back() != ' ' && kept[i]->text != " ") {
          line.push_back(' ');
        }
      }
      line += kept[i]->text;
    }
    // Trailing and leading spaces say nothing about the text.
    const size_t first = line.find_first_not_of(' ');
    const size_t last = line.find_last_not_of(' ');
    if (first != std::string::npos) {
      lines.push_back(line.substr(first, last - first + 1));
    }
    row_start = row_end;
  }
  return lines;
}

std::vector<std::string> nabts_offered_numbers(
    const std::vector<std::string>& lines) {
  std::vector<std::string> found;
  std::vector<std::string> listed;
  size_t list_rows = 0;
  std::vector<std::string> columned;
  for (const std::string& line : lines) {
    const std::vector<std::string> words = split_words(line);
    find_pointed_numbers(line, found);

    // A row of nothing but numbers: a board of pages.
    std::vector<std::string> grid;
    bool only_numbers = !words.empty();
    for (const std::string& word : words) {
      const std::vector<std::string> numbers = numbers_of(word);
      if (numbers.empty()) {
        only_numbers = false;
        break;
      }
      grid.insert(grid.end(), numbers.begin(), numbers.end());
    }
    if (only_numbers && grid.size() >= kGridNumbersAtLeast) {
      found.insert(found.end(), grid.begin(), grid.end());
      continue;
    }

    find_entry_number(words, found);

    const std::string number = list_row_number(words);
    if (!number.empty()) {
      ++list_rows;
      listed.push_back(number);
    }
    for (std::string& entry : colon_entries(words)) {
      columned.push_back(std::move(entry));
    }
  }

  // A list rather than a story that wrapped before a number, and not a
  // ranking or a numbered list, whose numbers start at 1 and are no pages.
  const bool numbered_from_one = !listed.empty() && listed.front() == "1";
  if (list_rows >= kListRowsAtLeast && !numbered_from_one) {
    found.insert(found.end(), listed.begin(), listed.end());
  }
  // An index in columns on the same terms, counted apart: a title in a
  // listing ("Police Academy 5: Miami Beach") is one entry alone, and a legend
  // of option keys ("1:Black stone", "9: Cursor", in a game of Go) numbers
  // from 1 wherever its rows put the 1.
  const bool keys_from_one =
      std::find(columned.begin(), columned.end(), "1") != columned.end();
  if (columned.size() >= kListRowsAtLeast && !keys_from_one) {
    found.insert(found.end(), columned.begin(), columned.end());
  }

  std::vector<std::string> out;
  std::set<std::string> seen;
  for (std::string& number : found) {
    // "007" and "7" are the same page.
    const size_t first = number.find_first_not_of('0');
    std::string key =
        first == std::string::npos ? std::string("0") : number.substr(first);
    if (seen.insert(key).second) {
      out.push_back(std::move(number));
    }
  }
  return out;
}

bool nabts_draws_over_previous(const NabtsPageSnapshot& page) {
  const std::vector<NabtsPrimitive>& before = page.timeline.superseded;
  if (before.empty() && page.primitives.empty()) {
    return false;  // it draws nothing at all
  }
  if (!page.timeline.clears.empty() &&
      page.timeline.clears.front().stream_index == 0) {
    return false;  // cleared before it drew
  }
  const NabtsPrimitive& first =
      before.empty() ? page.primitives.front() : before.front();
  if (first.kind == NabtsPrimitiveKind::kRectangle && first.filled) {
    const double left =
        std::min(first.origin.x, first.origin.x + first.size.dx);
    const double right =
        std::max(first.origin.x, first.origin.x + first.size.dx);
    const double bottom =
        std::min(first.origin.y, first.origin.y + first.size.dy);
    const double top = std::max(first.origin.y, first.origin.y + first.size.dy);
    if (left <= kGroundTolerance && right >= 1.0 - kGroundTolerance &&
        bottom <= kGroundTolerance &&
        top >= kNabtsDisplayAreaHeight - kGroundTolerance) {
      return false;  // a ground over the whole display area
    }
  }
  return true;
}

std::vector<std::vector<size_t>> nabts_presentation_contexts(
    const std::vector<NabtsNavigationPage>& pages) {
  // How strongly a page leads to another: a key the header points at it,
  // its number offered on the page, the next page by number. A step costs its
  // rank plus one, so a way through fewer pages costs less.
  enum Rank : int { kKeyed = 0, kOffered = 1, kNumbered = 2, kNone = 3 };
  const auto rank_of = [](const NabtsNavigationPage& from,
                          const NabtsNavigationPage& to) {
    if ((from.has_more && from.more == to.address) ||
        (from.has_next && from.next == to.address) ||
        (from.has_index && from.index == to.address)) {
      return kKeyed;
    }
    if (std::find(from.offered.begin(), from.offered.end(), to.address) !=
        from.offered.end()) {
      return kOffered;
    }
    if (!from.has_next && from.has_next_by_number &&
        from.next_by_number == to.address) {
      return kNumbered;
    }
    return kNone;
  };
  const auto overlap = [](const NabtsNavigationPage& a,
                          const NabtsNavigationPage& b) -> uint64_t {
    const uint64_t first = std::max(a.first_frame, b.first_frame);
    const uint64_t last = std::min(a.last_frame, b.last_frame);
    return last >= first ? last - first + 1 : 0;
  };

  // The pages each page that wants a context can be reached from.
  struct Lead {
    size_t from = 0;
    Rank rank = kNone;
  };
  std::vector<std::vector<Lead>> leads(pages.size());
  for (size_t to = 0; to < pages.size(); ++to) {
    if (!pages[to].wants_context) {
      continue;
    }
    for (size_t from = 0; from < pages.size(); ++from) {
      const NabtsNavigationPage& source = pages[from];
      if (from == to || !source.drawn || source.channel != pages[to].channel ||
          source.address == pages[to].address) {
        continue;  // another version of the same page is no way to it
      }
      const Rank rank = rank_of(source, pages[to]);
      if (rank != kNone) {
        leads[to].push_back(Lead{from, rank});
      }
    }
  }

  // The cheapest way to each page over exactly |depth| others, depth by
  // depth: a way starts at a page that stands as it is — one that draws a
  // whole screen of its own, or one that wants no context — and passes only
  // through pages that draw over the one before. Lower is better throughout:
  // the cost, the number of pages, and then the last step — its rank, a page
  // of its own over one that draws over another, the longest time on air
  // beside the page (negated), the lowest address, the highest version
  // (negated).
  using Key = std::tuple<int, size_t, int, int, uint64_t, uint64_t, int>;
  struct Way {
    bool found = false;
    Key key{};
    std::vector<size_t> under;  // oldest first
  };
  std::vector<std::vector<Way>> ways(kNabtsMaxContextPages + 1,
                                     std::vector<Way>(pages.size()));
  for (size_t page = 0; page < pages.size(); ++page) {
    if (pages[page].drawn &&
        (!pages[page].draws_over || !pages[page].wants_context)) {
      ways[0][page].found = true;
    }
  }
  for (size_t depth = 1; depth <= kNabtsMaxContextPages; ++depth) {
    for (size_t to = 0; to < pages.size(); ++to) {
      for (const Lead& lead : leads[to]) {
        const Way& before = ways[depth - 1][lead.from];
        if (!before.found) {
          continue;
        }
        const NabtsNavigationPage& source = pages[lead.from];
        const Key key{std::get<0>(before.key) + lead.rank + 1,
                      depth,
                      lead.rank,
                      source.draws_over ? 1 : 0,
                      UINT64_MAX - overlap(source, pages[to]),
                      source.address,
                      -static_cast<int>(source.version)};
        Way& way = ways[depth][to];
        if (!way.found || key < way.key) {
          way.found = true;
          way.key = key;
          way.under = before.under;
          way.under.push_back(lead.from);
        }
      }
    }
  }

  // The cheapest way over any number of pages.
  std::vector<std::vector<size_t>> contexts(pages.size());
  for (size_t to = 0; to < pages.size(); ++to) {
    const Way* best = nullptr;
    for (size_t depth = 1; depth <= kNabtsMaxContextPages; ++depth) {
      const Way& way = ways[depth][to];
      if (way.found && (best == nullptr || way.key < best->key)) {
        best = &way;
      }
    }
    if (best != nullptr) {
      contexts[to] = best->under;
    }
  }
  return contexts;
}

}  // namespace orc
