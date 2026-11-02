/*
 * File:        nabts_page.cpp
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     The NAPLPS default colour map and character repertoires
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_page.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace orc {

namespace {

// X3.110 §5.3.2.5.2 places the three primaries equidistant around a hue circle
// with blue at 0 degrees, red at 120 and green at 240.
constexpr double kBlueAngle = 0.0;
constexpr double kRedAngle = 120.0;
constexpr double kGreenAngle = 240.0;

/// Angular distance between |a| and |b| the short way round the circle.
double angular_distance(double a, double b) {
  double delta = std::fabs(a - b);
  if (delta > 180.0) {
    delta = 360.0 - delta;
  }
  return delta;
}

/**
 * @brief The hue at |angle| degrees, per the algorithm of X3.110 §5.3.2.5.2
 *
 * The closest primary to the angle is set full on, the furthest full off, and
 * the second closest gets the angular distance from the closest primary divided
 * by 60 degrees — which is the half-separation of two primaries, so the
 * fraction runs from 0 at a pure primary to 1 midway between two.
 *
 * The value is then "normalized by multiplying it by the maximum color value
 * which can be stored for that primary" — kNabtsGunMax — "and then rounded".
 * At three bits a gun this reproduces all eight hue entries of T.101 Table
 * II-3 exactly.
 */
NabtsColour hue_at(double angle) {
  struct Primary {
    double angle;
    uint8_t* component;
  };

  NabtsColour colour;
  Primary primaries[3] = {
      {kGreenAngle, &colour.green},
      {kRedAngle, &colour.red},
      {kBlueAngle, &colour.blue},
  };

  // Sort by distance from the hue: closest, second, furthest. Three elements,
  // so a couple of swaps rather than a sort.
  double distances[3];
  for (int i = 0; i < 3; ++i) {
    distances[i] = angular_distance(angle, primaries[i].angle);
  }
  for (int i = 0; i < 2; ++i) {
    for (int j = i + 1; j < 3; ++j) {
      if (distances[j] < distances[i]) {
        std::swap(distances[i], distances[j]);
        std::swap(primaries[i], primaries[j]);
      }
    }
  }

  // P1 full on, P3 off, P2 by the fraction above.
  *primaries[0].component = static_cast<uint8_t>(kNabtsGunMax);
  *primaries[2].component = 0;
  const double fraction = distances[0] / 60.0;
  *primaries[1].component = static_cast<uint8_t>(
      std::lround(fraction * static_cast<double>(kNabtsGunMax)));
  return colour;
}

}  // namespace

void nabts_default_colour_map(NabtsColour (&map)[kNabtsColourMapEntries]) {
  // §5.3.2.5.2: "The first half of the default color map is used to store a
  // complete, uniformly spaced grey scale", G = R = B, black through white.
  constexpr size_t kGreys = kNabtsColourMapEntries / 2;
  for (size_t i = 0; i < kGreys; ++i) {
    const auto level =
        static_cast<uint8_t>(std::lround(static_cast<double>(i * kNabtsGunMax) /
                                         static_cast<double>(kGreys - 1)));
    map[i] = NabtsColour{level, level, level, false};
  }

  // "The second half ... a full range of hues equally spaced around the
  // perimeter of the hue circle", starting at 0 degrees — blue — and proceeding
  // counterclockwise, which is the direction of increasing angle given where
  // §5.3.2.5.2 places the primaries.
  constexpr size_t kHues = kNabtsColourMapEntries - kGreys;
  for (size_t i = 0; i < kHues; ++i) {
    const double angle =
        360.0 * static_cast<double>(i) / static_cast<double>(kHues);
    map[kGreys + i] = hue_at(angle);
  }
}

namespace {

// The lowest and highest code positions a G-set occupies (X3.110 §4.3.1: a
// G-set is the 96 positions 2/0 to 7/15 of the in-use table).
constexpr uint8_t kFirstCode = 0x20;
constexpr uint8_t kLastCode = 0x7F;
constexpr size_t kSetSize = kLastCode - kFirstCode + 1;

// The supplementary set, code position 2/0 first (X3.110 §5.2, Tables 25 to
// 27). Verified position by position against the Coded Representation column
// of Tables 18 to 27, which name the code position of every character the set
// carries; the layout that falls out is ISO 6937-1982's, which §7.2 names as
// the source, plus X3.110's own additions at 5/6 to 5/11 and 6/5.
//
// Column 4 is Tables 26 and 27's non-spacing marks, given here as the Unicode
// combining marks. They are transmitted *before* the letter they modify, which
// is the reverse of Unicode's order — see nabts_supplementary_is_nonspacing().
constexpr char32_t kSupplementary[kSetSize] = {
    // 2/0 - 2/15
    U' ',
    U'¡',
    U'¢',
    U'£',
    U'$',
    U'¥',
    U'#',
    U'§',
    U'¤',
    U'‘',
    U'“',
    U'«',
    U'←',
    U'↑',
    U'→',
    U'↓',
    // 3/0 - 3/15
    U'°',
    U'±',
    U'²',
    U'³',
    U'×',
    U'µ',
    U'¶',
    U'·',
    U'÷',
    U'’',
    U'”',
    U'»',
    U'¼',
    U'½',
    U'¾',
    U'¿',
    // 4/0 - 4/15: the non-spacing marks. 4/0 is Table 27's "vector overbar",
    // whose graphic the note describes as an arrow above the letter; 4/9 its
    // "slant" and 4/12 its underline.
    U'⃗',
    U'̀',
    U'́',
    U'̂',
    U'̃',
    U'̄',
    U'̆',
    U'̇',
    U'̈',
    U'̸',
    U'̊',
    U'̧',
    U'̲',
    U'̋',
    U'̨',
    U'̌',
    // 5/0 - 5/15. 5/6 to 5/11 are X3.110's line and diagonal graphics (Table 25
    // notes 1 to 8), drawn corner to corner of the character field, so the box
    // and triangle characters are the closest Unicode has.
    U'―',
    U'¹',
    U'®',
    U'©',
    U'™',
    U'♪',
    U'─',
    U'│',
    U'╱',
    U'╲',
    U'◢',
    U'◣',
    U'⅛',
    U'⅜',
    U'⅝',
    U'⅞',
    // 6/0 - 6/15. 6/5 is Table 25's cross, "equivalent to overlaying the full
    // horizontal line character with the full vertical line character".
    U'Ω',
    U'Æ',
    U'Đ',
    U'ª',
    U'Ħ',
    U'┼',
    U'Ĳ',
    U'Ŀ',
    U'Ł',
    U'Ø',
    U'Œ',
    U'º',
    U'Þ',
    U'Ŧ',
    U'Ŋ',
    U'ŉ',
    // 7/0 - 7/15. 7/15 is unassigned.
    U'ĸ',
    U'æ',
    U'đ',
    U'ð',
    U'ħ',
    U'ı',
    U'ĳ',
    U'ŀ',
    U'ł',
    U'ø',
    U'œ',
    U'ß',
    U'þ',
    U'ŧ',
    U'ŋ',
    U' ',
};

}  // namespace

char32_t nabts_primary_to_unicode(uint8_t code) {
  // 7/15 is DELETE in ASCII rather than a graphic, and anything below 2/0 is a
  // control that never reaches a character primitive.
  if (code < kFirstCode || code >= kLastCode) {
    return U' ';
  }
  return static_cast<char32_t>(code);
}

char32_t nabts_supplementary_to_unicode(uint8_t code) {
  if (code < kFirstCode || code > kLastCode) {
    return U' ';
  }
  return kSupplementary[code - kFirstCode];
}

bool nabts_supplementary_is_nonspacing(uint8_t code) {
  // Column 4 exactly: Tables 26 and 27 assign every one of its sixteen
  // positions to a mark, and no other column carries one.
  return code >= 0x40 && code <= 0x4F;
}

bool nabts_same_character_field(const NabtsPrimitive& a,
                                const NabtsPrimitive& b) {
  constexpr double kSameField = 1e-6;
  return std::fabs(a.origin.x - b.origin.x) < kSameField &&
         std::fabs(a.origin.y - b.origin.y) < kSameField &&
         std::fabs(a.size.dx - b.size.dx) < kSameField &&
         std::fabs(a.size.dy - b.size.dy) < kSameField &&
         a.rotation == b.rotation;
}

bool nabts_is_mosaic_code(uint8_t code) {
  // Figure 62: b6 = 1 selects a mosaic, which is columns 2, 3, 6 and 7. §5.4
  // adds the second copy of the solid mosaic at 5/15.
  return (code & 0x20) != 0 || code == 0x5F;
}

uint8_t nabts_mosaic_sixels(uint8_t code) {
  // Figure 62 assigns b1 to b5 to the top-left through bottom-left elements and
  // b7 to the bottom-right one, leaving b6 as the marker bit and b8 free. That
  // is the same allocation World System Teletext uses for its G1 sixels, so the
  // packing is the same: bits 0 to 4 straight through, b7 folded down to bit 5.
  return static_cast<uint8_t>((code & 0x1F) | ((code >> 1) & 0x20));
}

std::string nabts_code_point_to_utf8(char32_t code) {
  std::string out;
  if (code < 0x80) {
    out.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
  return out;
}

std::string nabts_character_to_utf8(uint8_t code,
                                    NabtsPrimitive::Repertoire repertoire) {
  switch (repertoire) {
    case NabtsPrimitive::Repertoire::kPrimary:
      return nabts_code_point_to_utf8(nabts_primary_to_unicode(code));
    case NabtsPrimitive::Repertoire::kSupplementary:
      return nabts_code_point_to_utf8(nabts_supplementary_to_unicode(code));
    case NabtsPrimitive::Repertoire::kMosaic:
    case NabtsPrimitive::Repertoire::kDrcs:
      // Block graphics and a downloaded bitmap are shapes rather than
      // characters; a text reading of one would be an invention.
      break;
  }
  return " ";
}

namespace {

// Unit-space slack for deciding whether two characters share a baseline.
// Table D1 item 8 puts the nominal resolution at 256 x 200, so one pixel is
// about 1/256; this is far below that and absorbs only the arithmetic of
// resolving a relative coordinate.  Kept at namespace scope because MSVC
// rejects reading a function-local constexpr from a capture-less lambda.
constexpr double kEpsilon = 1e-6;

// A page's text is read in the order it was sent rather than the order it
// stands on the screen once at least this many characters, and at least one
// in this many of them, were written where another already stood.
constexpr size_t kOverprintedCharactersForSentOrder = 8;
constexpr size_t kOverprintedShareDivisor = 4;

// A run of characters drawn again, every one of them within about a quarter
// of its field of the same character, is read once: a title given a shadow by
// drawing it twice a pixel apart, or drawn again in place in another
// colour. The field is cut into this many cells each way to find them.
constexpr int64_t kRedrawnQuarters = 4;

// A run of characters a filled figure drawn after it covers — more than this
// share of the run's characters with their field's centre inside it — was
// taken off the screen by it; a figure over a letter or two is a picture
// beside the words, and leaves them where they are.
constexpr size_t kCoveredRunShareDivisor = 2;

/// Whether |primitive| paints over everything under its interior: a filled
/// rectangle or polygon in a colour that is not transparent.
bool paints_over(const NabtsPrimitive& primitive) {
  return primitive.filled && !primitive.colour.transparent &&
         (primitive.kind == NabtsPrimitiveKind::kRectangle ||
          (primitive.kind == NabtsPrimitiveKind::kPolygon &&
           primitive.points.size() >= 3));
}

/// Whether the figure |cover| (see paints_over()) holds |point|.
bool holds(const NabtsPrimitive& cover, NabtsPoint point) {
  if (cover.kind == NabtsPrimitiveKind::kRectangle) {
    const double x0 = std::min(cover.origin.x, cover.origin.x + cover.size.dx);
    const double x1 = std::max(cover.origin.x, cover.origin.x + cover.size.dx);
    const double y0 = std::min(cover.origin.y, cover.origin.y + cover.size.dy);
    const double y1 = std::max(cover.origin.y, cover.origin.y + cover.size.dy);
    return point.x >= x0 && point.x <= x1 && point.y >= y0 && point.y <= y1;
  }
  // Even-odd, the rule a filled polygon is filled by.
  bool inside = false;
  const std::vector<NabtsPoint>& v = cover.points;
  for (size_t i = 0, j = v.size() - 1; i < v.size(); j = i++) {
    if ((v[i].y > point.y) != (v[j].y > point.y) &&
        point.x < (v[j].x - v[i].x) * (point.y - v[i].y) / (v[j].y - v[i].y) +
                      v[i].x) {
      inside = !inside;
    }
  }
  return inside;
}

/// One character as it is read: where its field stands, its place in the
/// display list, and its text.
struct Placed {
  double y = 0.0;
  double x = 0.0;
  double width = 0.0;
  double height = 0.0;
  size_t index = 0;
  std::string text;
};

/// The characters of one screen as text: in the order the screen holds them,
/// or in the order they were sent where they were written over each other.
std::string read_screen(std::vector<Placed> placed) {
  // A page that writes over itself has no reading order on its screen: a
  // message longer than the display, which SCROLL OFF (§6.2.7.14, the default)
  // wraps back to the top, puts the characters of every pass on the same rows.
  // Its text is read in the order it was sent instead, a line wherever the
  // cursor started one, which is how the message reads.
  size_t overprinted = 0;
  {
    std::set<std::pair<int64_t, int64_t>> used;
    for (const Placed& entry : placed) {
      const auto cell = std::make_pair(
          static_cast<int64_t>(std::llround(entry.x / kEpsilon)),
          static_cast<int64_t>(std::llround(entry.y / kEpsilon)));
      overprinted += used.insert(cell).second ? 0 : 1;
    }
  }
  if (overprinted >= kOverprintedCharactersForSentOrder &&
      overprinted * kOverprintedShareDivisor >= placed.size()) {
    std::string out = placed.front().text;
    for (size_t i = 1; i < placed.size(); ++i) {
      const Placed& before = placed[i - 1];
      const Placed& now = placed[i];
      const double tolerance =
          std::max(std::max(before.height, now.height) / 2.0, kEpsilon);
      if (std::fabs(now.y - before.y) > tolerance || now.x < before.x) {
        out += '\n';
      }
      out += now.text;
    }
    return out;
  }

  std::stable_sort(placed.begin(), placed.end(),
                   [](const Placed& lhs, const Placed& rhs) {
                     if (std::fabs(lhs.y - rhs.y) > kEpsilon) {
                       return lhs.y > rhs.y;  // top of the screen first
                     }
                     return lhs.x < rhs.x;
                   });

  std::string out;
  double line_y = placed.front().y;
  double line_height = placed.front().height;
  for (size_t i = 0; i < placed.size(); ++i) {
    if (i > 0) {
      const double tolerance =
          std::max(std::max(line_height, placed[i].height) / 2.0, kEpsilon);
      if (std::fabs(placed[i].y - line_y) > tolerance) {
        out += '\n';
        line_y = placed[i].y;
        line_height = placed[i].height;
      }
    }
    out += placed[i].text;
  }
  return out;
}

}  // namespace

std::string nabts_page_text(const NabtsPageSnapshot& page) {
  std::vector<Placed> placed;
  // §7.2: a non-spacing mark is transmitted before the letter it modifies and
  // shares its character field, so it is held until that letter arrives — and
  // composed onto it only if the letter really is in that field. A mark with
  // no letter of its own stands alone, which Unicode writes as the combining
  // mark over a no-break space.
  const NabtsPrimitive* mark_field = nullptr;
  size_t mark_index = 0;
  std::string pending_marks;
  const auto place = [&placed](const NabtsPrimitive& field, size_t index,
                               std::string text) {
    Placed entry;
    entry.y = field.origin.y;
    entry.x = field.origin.x;
    entry.width = std::fabs(field.size.dx);
    entry.height = std::fabs(field.size.dy);
    entry.index = index;
    entry.text = std::move(text);
    placed.push_back(std::move(entry));
  };
  const auto place_marks_alone = [&] {
    if (pending_marks.empty()) {
      return;
    }
    place(*mark_field, mark_index, " " + pending_marks);
    pending_marks.clear();
  };

  for (size_t index = 0; index < page.primitives.size(); ++index) {
    const NabtsPrimitive& primitive = page.primitives[index];
    if (primitive.kind != NabtsPrimitiveKind::kCharacter) {
      continue;
    }
    if (primitive.repertoire == NabtsPrimitive::Repertoire::kMosaic ||
        primitive.repertoire == NabtsPrimitive::Repertoire::kDrcs) {
      // A block graphic or a downloaded bitmap has no text form; reading one as
      // a space would put gaps in the text that the record never had.
      continue;
    }
    const bool same_field = mark_field != nullptr &&
                            nabts_same_character_field(primitive, *mark_field);
    if (!same_field) {
      place_marks_alone();
    }
    if (primitive.repertoire == NabtsPrimitive::Repertoire::kSupplementary &&
        nabts_supplementary_is_nonspacing(primitive.character)) {
      pending_marks +=
          nabts_character_to_utf8(primitive.character, primitive.repertoire);
      mark_field = &primitive;
      mark_index = index;
      continue;
    }
    place(primitive, index,
          nabts_character_to_utf8(primitive.character, primitive.repertoire) +
              pending_marks);
    pending_marks.clear();
  }
  place_marks_alone();
  if (placed.empty()) {
    return {};
  }

  // The characters as the record wrote them: runs, each the characters sent
  // one after another along one baseline, left to right without a gap wider
  // than two fields — a word, a line, a title.
  std::vector<std::pair<size_t, size_t>> runs;  // [first, last) of |placed|
  for (size_t i = 0; i < placed.size(); ++i) {
    const bool continues =
        i > 0 &&
        std::fabs(placed[i].y - placed[i - 1].y) <=
            std::max(placed[i - 1].height / 2.0, kEpsilon) &&
        placed[i].x > placed[i - 1].x &&
        placed[i].x - placed[i - 1].x <= 2.0 * placed[i - 1].width + kEpsilon;
    if (continues) {
      runs.back().second = i + 1;
    } else {
      runs.emplace_back(i, i + 1);
    }
  }

  // A run a filled figure drawn after it takes off the screen had gone by
  // then: a timed page can blank a box with a filled rectangle to write the
  // next message in it, an index can take its opening headlines away under the
  // menu, and a More Record
  // wrote its page over its predecessor's (§5.2.7.8). Read where they stood,
  // the words that went and the words that replaced them interleave letter by
  // letter. So the text is read a screen at a time: the runs each such figure
  // took away, in the order the figures were drawn, then what the display
  // holds once the record is done. (What a clear removed is gone from
  // |primitives| altogether.)
  std::vector<size_t> covers;
  for (size_t index = 0; index < page.primitives.size(); ++index) {
    if (paints_over(page.primitives[index])) {
      covers.push_back(index);
    }
  }
  std::map<size_t, std::vector<Placed>> screens;  // by the figure that ended it
  std::set<std::tuple<std::string, int64_t, int64_t>> drawn;
  const auto quarter = [](double at, double field) {
    return static_cast<int64_t>(
        std::floor(at / std::max(field / static_cast<double>(kRedrawnQuarters),
                                 kEpsilon)));
  };
  for (const auto& [first, last] : runs) {
    // A run whose every character stands where the same one already does —
    // within the neighbouring quarter-field cells, so never as far as half a
    // field off — is drawn again, and read once. A space has nothing to show.
    bool redrawn = false;
    for (size_t i = first; i < last; ++i) {
      if (placed[i].text == " ") {
        continue;
      }
      const int64_t qx = quarter(placed[i].x, placed[i].width);
      const int64_t qy = quarter(placed[i].y, placed[i].height);
      bool found = false;
      for (int64_t dx = -1; dx <= 1 && !found; ++dx) {
        for (int64_t dy = -1; dy <= 1 && !found; ++dy) {
          found = drawn.count(
                      std::make_tuple(placed[i].text, qx + dx, qy + dy)) > 0;
        }
      }
      if (!found) {
        redrawn = false;
        break;
      }
      redrawn = true;
    }
    if (redrawn) {
      continue;
    }
    for (size_t i = first; i < last; ++i) {
      drawn.emplace(placed[i].text, quarter(placed[i].x, placed[i].width),
                    quarter(placed[i].y, placed[i].height));
    }

    size_t screen = page.primitives.size();
    for (auto cover = std::upper_bound(covers.begin(), covers.end(),
                                       placed[last - 1].index);
         cover != covers.end(); ++cover) {
      size_t held = 0;
      for (size_t i = first; i < last; ++i) {
        const NabtsPoint centre{placed[i].x + placed[i].width / 2.0,
                                placed[i].y + placed[i].height / 2.0};
        held += holds(page.primitives[*cover], centre) ? 1 : 0;
      }
      if (held * kCoveredRunShareDivisor > last - first) {
        screen = *cover;
        break;
      }
    }
    std::vector<Placed>& into = screens[screen];
    into.insert(into.end(), placed.begin() + static_cast<std::ptrdiff_t>(first),
                placed.begin() + static_cast<std::ptrdiff_t>(last));
  }

  std::string out;
  for (auto& [figure, characters] : screens) {
    (void)figure;
    if (!out.empty()) {
      out += "\n\n";
    }
    out += read_screen(std::move(characters));
  }
  return out;
}

}  // namespace orc
