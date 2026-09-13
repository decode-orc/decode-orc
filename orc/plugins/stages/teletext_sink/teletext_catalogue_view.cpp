/*
 * File:        teletext_catalogue_view.cpp
 * Module:      teletext_sink stage plugin
 * Purpose:     The page catalogue as an SDK CatalogueDataset the host can
 * browse
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_catalogue_view.h"

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace orc {

namespace {

// Level 1 display colours in spacing-attribute code order (ETSI EN 300 706
// §12.2 Table 26): black, red, green, yellow, blue, magenta, cyan, white.
// Fixed by the standard, so they travel with the page rather than being
// something the host is expected to know.
std::vector<CatalogueColour> level1_palette() {
  return {
      CatalogueColour{0, 0, 0},     CatalogueColour{255, 0, 0},
      CatalogueColour{0, 255, 0},   CatalogueColour{255, 255, 0},
      CatalogueColour{0, 0, 255},   CatalogueColour{255, 0, 255},
      CatalogueColour{0, 255, 255}, CatalogueColour{255, 255, 255},
  };
}

// Nominal shape of a teletext character rectangle (Fig. 8 of the BBC 1976
// specification), which is the aspect the grid must be drawn at whatever shape
// the host gives the view.
constexpr int kCellAspectWidth = 12;
constexpr int kCellAspectHeight = 20;

// How far below the page's best-attested row a row's copy count must fall
// before it is called unconfirmed (see where it is applied). Eight leaves ample
// room for the unevenness of a real capture — a row lost to dropouts more often
// than its neighbours still keeps a good fraction of their copies — while the
// rows that do not belong to the page at all sit one to three copies against
// hundreds.
constexpr int kUnconfirmedCopyRatio = 8;

// True when a 7-bit code selects a G1 block-mosaic character.
//
// With |blast_through|, codes 4/0-5/F remain alphanumeric capitals even in
// mosaic mode (EN 300 706 §15.7.1 Table 47 NOTE 1). Without it every code from
// 2/0 up is a mosaic — which is what the 525-line service's own graphics need
// (see TeletextPageSnapshot::mosaic_blast_through). Codes below 2/0 are
// spacing attributes and never reach here as characters.
bool is_mosaic_code(uint8_t code, bool blast_through) {
  if (code < 0x20) {
    return false;
  }
  if (!blast_through) {
    return true;
  }
  return code <= 0x3F || code >= 0x60;
}

// Extract the six sixel bits of a G1 mosaic code (EN 300 706 §15.7.1 Table 47,
// bit allocation as Table 35): character bits 1-5 select the top-left through
// bottom-left cells and bit 7 the bottom-right cell.
uint8_t mosaic_sixels(uint8_t code) {
  return static_cast<uint8_t>((code & 0x1F) | ((code >> 1) & 0x20));
}

// ETSI EN 300 706 §9.3.1.1: the page number is a two-digit hexadecimal field,
// but a receiver keypad can only select the decimal values, so viewable pages
// run 100-899. Numbers containing A-F address hidden data pages (and are what
// a misdecoded header usually produces).
bool is_non_selectable_page(int page_number) {
  return (page_number & 0x0F) > 9 || ((page_number >> 4) & 0x0F) > 9;
}

// Row order: the selectable pages 100-899 ascending, then the hex-digit pages
// (also ascending) below them. Magazine numbers are 1-8, so the key is just the
// page address with the hex pages biased past every decimal one.
int page_sort_key(int magazine, int page_number) {
  return (is_non_selectable_page(page_number) ? 0x1000 : 0) + magazine * 0x100 +
         page_number;
}

/// Conventional magazine + two-hex-digit page label, e.g. "100", "1F0".
std::string page_label(int magazine, int page_number) {
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%d%02X", magazine, page_number & 0xFF);
  return buffer;
}

/// The four sub-code digits S4 S3 S2 S1 as ETSI EN 300 706 Annex A.1 writes
/// them (e.g. "0002"), from the packed 13-bit field.
std::string subcode_label(int subcode) {
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%X%X%X%X", (subcode >> 11) & 0x3,
                (subcode >> 7) & 0xF, (subcode >> 4) & 0x7, subcode & 0xF);
  return buffer;
}

std::string frame_range(uint64_t first, uint64_t last) {
  // Frame numbers are 1-based in the UI. A page seen once has no range.
  const uint64_t from = first + 1;
  const uint64_t to = last + 1;
  if (from == to) {
    return std::to_string(from);
  }
  return std::to_string(from) + "-" + std::to_string(to);
}

std::string plural(uint64_t count, const char* singular, const char* many) {
  return std::to_string(count) + " " + (count == 1 ? singular : many);
}

/// "Page 100 seen 12 times, frames 5-4210", reporting the sub-page rather than
/// the set on a multi-page set: the line describes what is on screen.
std::string page_headline(const std::string& label,
                          const TeletextCataloguedSubPage& subpage,
                          bool multi_page_set) {
  std::string text = "Page " + label;
  if (multi_page_set) {
    text += " sub-page " + subcode_label(subpage.subcode);
  }
  text += " seen " + plural(subpage.times_seen, "time", "times") + ", frames " +
          frame_range(subpage.first_seen_frame, subpage.last_seen_frame);
  return text;
}

/// How much of the page came back: a plain row count rather than a fraction of
/// the 24-row grid, because services leave rows out as a matter of course — the
/// blank lines that space a page out are simply not transmitted — so "rows
/// 21/24" reads as three rows missing when nothing was wrong at all.
std::string page_condition(const TeletextPageSnapshot& snapshot,
                           uint64_t lost_packets, int rows_received,
                           int damaged_bytes, int unconfirmed_rows) {
  const std::string rows =
      plural(static_cast<uint64_t>(rows_received), "row", "rows");

  // A page whose last transmission was still arriving when the range ran out
  // looks exactly like a finished one with rows missing.
  if (!snapshot.transmission_complete) {
    return "Partial - still arriving (" + rows + " so far)";
  }

  std::vector<std::string> faults;
  if (lost_packets > 0) {
    faults.push_back(plural(lost_packets, "packet", "packets") + " lost");
  }
  if (damaged_bytes > 0) {
    faults.push_back(plural(static_cast<uint64_t>(damaged_bytes),
                            "damaged byte", "damaged bytes"));
  }
  // Not damage, but not settled either: the carousel has corrected most of this
  // page against a repeat and these rows have not been checked by one.
  if (unconfirmed_rows > 0) {
    faults.push_back(
        plural(static_cast<uint64_t>(unconfirmed_rows), "row", "rows") +
        " seen only once");
  }
  if (faults.empty()) {
    return "Complete (" + rows + ")";
  }
  std::string text = rows;
  for (const auto& fault : faults) {
    text += ", " + fault;
  }
  return text;
}

std::string run_headline(const TeletextRecoverySummary& summary) {
  if (summary.frames_analysed == 0 && summary.packets_recovered == 0) {
    return {};
  }
  std::vector<std::string> parts;
  parts.push_back(std::to_string(summary.packets_recovered) +
                  " packets recovered from " +
                  std::to_string(summary.fields_with_data) + " fields over " +
                  std::to_string(summary.frames_analysed) + " frames");
  // Odd parity (§8.1) is the only damage measure available without the original
  // transmission, and it is a floor: a byte damaged in two bits passes it.
  if (summary.characters_written > 0) {
    parts.push_back(std::to_string(summary.characters_damaged) + " of " +
                    std::to_string(summary.characters_written) +
                    " characters known damaged");
  }
  if (summary.packets_corrected > 0) {
    parts.push_back(std::to_string(summary.packets_corrected) +
                    " rows corrected by repeats");
  }
  if (summary.bytes_repaired > 0) {
    parts.push_back(std::to_string(summary.bytes_repaired) +
                    " bytes parity-repaired");
  }
  if (summary.lost_packets_estimate > 0) {
    parts.push_back("about " + std::to_string(summary.lost_packets_estimate) +
                    " packets lost");
  }
  if (summary.pages_truncated) {
    parts.push_back("page list truncated at the catalogue limit");
  }
  // Last, because it is a statement about the run rather than a count from it —
  // and the one thing a reader cannot check by looking at the page (see
  // TeletextRecoverySummary::character_set).
  parts.push_back(
      "read as " + to_string(summary.character_set) +
      (summary.second_character_set.has_value()
           ? ", switching to " + to_string(summary.second_character_set->g0_set)
           : std::string()));
  std::string text = parts.front();
  for (size_t i = 1; i < parts.size(); ++i) {
    text += "; " + parts[i];
  }
  return text;
}

}  // namespace

CatalogueCellGrid teletext_page_grid(const TeletextPageSnapshot& snapshot,
                                     uint64_t lost_packets) {
  CatalogueCellGrid grid;
  grid.rows = TeletextPageSnapshot::kRows;
  // The page's own width rather than the grid constant: a page narrower than
  // the full 40 fills the view as a 40-column one does, rather than being drawn
  // with a blank margin it never had.
  grid.columns = snapshot.columns;
  grid.palette = level1_palette();
  grid.cell_aspect_width = kCellAspectWidth;
  grid.cell_aspect_height = kCellAspectHeight;
  // On newsflash (C5) and subtitle (C6) pages only the boxed area is displayed;
  // everything outside it is transparent to the video picture (§12.2 codes
  // 0/A-0/B).
  grid.boxed_only = snapshot.newsflash || snapshot.subtitle;
  // Only a page whose own transmissions came back short has rows worth
  // banding: a row the assembly never received is otherwise one the service
  // chose not to send, which most pages do to space themselves out.
  grid.data_lost = lost_packets > 0;
  if (grid.columns <= 0) {
    grid.rows = 0;
    return grid;
  }

  grid.cells.resize(static_cast<size_t>(grid.rows) *
                    static_cast<size_t>(grid.columns));
  for (int row = 0; row < grid.rows; ++row) {
    for (int col = 0; col < grid.columns; ++col) {
      const auto& cell =
          snapshot.cells[static_cast<size_t>(row)][static_cast<size_t>(col)];
      CatalogueCell& out = grid.cells[static_cast<size_t>(row) *
                                          static_cast<size_t>(grid.columns) +
                                      static_cast<size_t>(col)];

      out.foreground = static_cast<uint8_t>(cell.foreground);
      out.background = static_cast<uint8_t>(cell.background);
      out.double_height = cell.double_height;
      out.double_height_lower = cell.double_height_lower;
      out.flash = cell.flash;
      out.concealed = cell.conceal;
      out.boxed = cell.boxed;
      out.damaged = cell.parity_error;

      // Held-mosaic cells carry the held character; separated_mosaic then
      // reflects the held character's original mode.
      if ((cell.mosaic || cell.held_mosaic) &&
          is_mosaic_code(cell.character, snapshot.mosaic_blast_through)) {
        out.mosaic = true;
        out.mosaic_pattern = mosaic_sixels(cell.character);
        out.mosaic_separated = cell.separated_mosaic;
        out.character = U' ';
      } else {
        // The cell's own G0 set: the alphabet the service designated and the
        // national option sub-set its header selected, not a fixed English one
        // (§15.2, §15.6.2) — and where the page has a second set, whichever of
        // the two the row's ESC codes left in force at this column (§15.3).
        out.character = orc::teletext_g0_to_unicode(
            cell.character, cell.g0_set, cell.national_option_subset);
      }
    }
  }

  // A row is only worth calling unconfirmed where the page has something to
  // compare it against: on a page whose rows have all been seen once, every row
  // rests on one copy and the label would be noise. Where other rows *have*
  // been corrected by a repeat, a row that stands alone is the one the reader
  // should distrust.
  //
  // "Stands alone" is a share of the page's own evidence, not a fixed count. A
  // page cycles as a whole, so its rows are transmitted alike and their copy
  // counts land within a factor of two or so of each other; a row an order of
  // magnitude below the best-attested one was not transmitted with the page at
  // all. Testing only for a single copy missed exactly the case the reader most
  // needs marked — a row carried onto this page by a burst can arrive two or
  // three times over a long recording while the page's real rows have hundreds,
  // and at three copies it was drawn as ordinary content.
  const int most_copies =
      *std::max_element(snapshot.row_copies.begin(), snapshot.row_copies.end());

  grid.row_status.resize(static_cast<size_t>(grid.rows));
  for (int row = 0; row < grid.rows; ++row) {
    const int copies = snapshot.row_copies[static_cast<size_t>(row)];
    auto& status = grid.row_status[static_cast<size_t>(row)];
    status.received = snapshot.row_received[static_cast<size_t>(row)];
    status.unconfirmed =
        row > 0 && !grid.at(row, 0).double_height_lower && most_copies > 1 &&
        (copies == 1 || copies * kUnconfirmedCopyRatio <= most_copies);
  }
  return grid;
}

CatalogueDataset build_teletext_catalogue(const TeletextAnalysisDataset& data) {
  CatalogueDataset out;

  out.schema.columns = {
      CatalogueColumn{"page", "Page", false},
      CatalogueColumn{"seen", "Seen", true},
      CatalogueColumn{"frames", "Frames", true},
  };
  out.schema.item_noun = "Page";
  out.schema.variant_noun = "Sub-page";
  out.schema.find_label = "Page:";
  out.schema.find_placeholder = "e.g. 100, 888";
  out.schema.highlight_label = "Show data errors";
  out.schema.empty_message = "No teletext pages were recovered";

  // The catalogue arrives page-address ordered; re-sort so the pages a receiver
  // could select come first and the hex-digit ones settle below them. The new
  // order is carried as indices rather than pointers into data.pages: the
  // result is then plainly independent of where the pages happen to live.
  std::vector<size_t> order(data.pages.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(), [&data](size_t lhs, size_t rhs) {
    return page_sort_key(data.pages[lhs].magazine,
                         data.pages[lhs].page_number) <
           page_sort_key(data.pages[rhs].magazine, data.pages[rhs].page_number);
  });

  std::vector<std::string> subtitle_pages;

  for (const size_t index : order) {
    const TeletextCataloguedPage* page = &data.pages[index];
    const std::string label = page_label(page->magazine, page->page_number);
    const bool multi_page_set = page->subpages.size() > 1;

    CatalogueItem item;
    item.id = label;
    item.find_key = label;
    item.values = {
        label,
        std::to_string(page->times_seen),
        frame_range(page->first_seen_frame, page->last_seen_frame),
    };
    item.selectable = !is_non_selectable_page(page->page_number);

    std::vector<std::string> tips;
    if (!item.selectable) {
      // Kept listed — it is genuinely recovered data — but marked, so the pages
      // a viewer could actually tune to are the ones that stand out.
      tips.push_back("Page " + label +
                     " contains hexadecimal digits: a hidden data page, or a "
                     "misdecoded header. It cannot be selected on a receiver.");
    }
    if (page->subtitle) {
      // Spelled out rather than shown as a symbol because there is nowhere in
      // the table to put a legend.
      item.badges.push_back("subs");
      subtitle_pages.push_back(label);
      tips.push_back(
          "Page " + label +
          " is transmitted with the C6 subtitle control bit set: it is the "
          "page this service carries its subtitles on. Use it as the subtitle "
          "page when exporting subtitles.");
    }
    if (multi_page_set) {
      // Which pages are multi-page sets is not otherwise visible from the
      // table: their sub-pages are counted together in the Seen column,
      // because the row is about the page number.
      tips.push_back("Page " + label + " is a sequence of " +
                     std::to_string(page->subpages.size()) +
                     " sub-pages the service cycles through; step through them "
                     "under the page display.");
    }
    for (size_t i = 0; i < tips.size(); ++i) {
      item.tooltip += (i == 0 ? "" : "\n\n") + tips[i];
    }

    out.items.push_back(std::move(item));
    // The page itself draws nothing: its sub-pages carry the content, and the
    // host shows the first of them when the page is selected.
    out.payloads.emplace_back();

    for (const auto& subpage : page->subpages) {
      CatalogueItem variant;
      variant.id = label + "/" + subcode_label(subpage.subcode);
      variant.parent_id = label;
      variant.variant_label = subcode_label(subpage.subcode);

      CataloguePayload payload;
      payload.kind = CataloguePayload::Kind::kCellGrid;
      payload.grid = teletext_page_grid(subpage.page, subpage.lost_packets);
      payload.headline = page_headline(label, subpage, multi_page_set);

      int rows_received = 0;
      int damaged_bytes = 0;
      int unconfirmed_rows = 0;
      // Rows consumed by a double-height character above are excluded: their
      // transmitted content is ignored by definition (§12.2 code 0/D), so their
      // absence is not a gap.
      for (int row = 1; row < payload.grid.rows; ++row) {
        if (payload.grid.at(row, 0).double_height_lower) {
          continue;
        }
        const auto& status = payload.grid.row_status[static_cast<size_t>(row)];
        if (status.received) {
          ++rows_received;
        }
        if (status.unconfirmed) {
          ++unconfirmed_rows;
        }
        for (int col = 0; col < payload.grid.columns; ++col) {
          if (payload.grid.at(row, col).damaged) {
            ++damaged_bytes;
          }
        }
      }
      payload.condition =
          page_condition(subpage.page, subpage.lost_packets, rows_received,
                         damaged_bytes, unconfirmed_rows);

      out.items.push_back(std::move(variant));
      out.payloads.push_back(std::move(payload));
    }
  }

  out.summary.headline = run_headline(data.summary);
  if (!subtitle_pages.empty()) {
    // Plural in the general case: a multi-service recording, or a service
    // running subtitles in more than one language, declares C6 on each page it
    // uses for them.
    std::string notice = "Subtitles on " + subtitle_pages.front();
    for (size_t i = 1; i < subtitle_pages.size(); ++i) {
      notice += ", " + subtitle_pages[i];
    }
    out.summary.notices.push_back(std::move(notice));
  }

  return out;
}

}  // namespace orc
