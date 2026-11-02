/*
 * File:        naplps_listing.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     A NAPLPS record's codes, listed for a reader
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_listing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "naplps_code_env.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

/// The picture description instructions of X3.110 Figure 13, by position 2/0
/// to 3/15.
constexpr std::array<const char*, 32> kPdiNames{"RESET",
                                                "DOMAIN",
                                                "TEXT",
                                                "TEXTURE",
                                                "POINT SET ABS",
                                                "POINT SET REL",
                                                "POINT ABS",
                                                "POINT REL",
                                                "LINE ABS",
                                                "LINE REL",
                                                "SET LINE ABS",
                                                "SET LINE REL",
                                                "ARC OUTLINED",
                                                "ARC FILLED",
                                                "SET ARC OUTLINED",
                                                "SET ARC FILLED",
                                                "RECT OUTLINED",
                                                "RECT FILLED",
                                                "SET RECT OUTLINED",
                                                "SET RECT FILLED",
                                                "POLY OUTLINED",
                                                "POLY FILLED",
                                                "SET POLY OUTLINED",
                                                "SET POLY FILLED",
                                                "FIELD",
                                                "INCREMENTAL POINT",
                                                "INCREMENTAL LINE",
                                                "INCREMENTAL POLY FILLED",
                                                "SET COLOR",
                                                "WAIT",
                                                "SELECT COLOR",
                                                "BLINK"};

/// The C1 controls of X3.110 Figure 65, by position 4/0 to 5/15.
constexpr std::array<const char*, 32> kC1Names{
    "DEF MACRO",      "DEFP MACRO",
    "DEFT MACRO",     "DEF DRCS",
    "DEF TEXTURE",    "END",
    "REPEAT",         "REPEAT TO EOL",
    "REVERSE VIDEO",  "NORMAL VIDEO",
    "SMALL TEXT",     "MEDIUM TEXT",
    "NORMAL TEXT",    "DOUBLE HEIGHT",
    "BLINK START",    "DOUBLE SIZE",
    "PROTECT",        "EDC1",
    "EDC2",           "EDC3",
    "EDC4",           "WORD WRAP ON",
    "WORD WRAP OFF",  "SCROLL ON",
    "SCROLL OFF",     "UNDERLINE START",
    "UNDERLINE STOP", "FLASH CURSOR",
    "STEADY CURSOR",  "CURSOR OFF",
    "BLINK STOP",     "UNPROTECT"};

/// The first of the C1 controls (DEF MACRO, 4/0).
constexpr uint8_t kFirstC1 = 0x40;
/// The first numeric byte of a 96-position set, an operand or a cursor
/// address (§5.3.1, §6.1.6.5).
constexpr uint8_t kFirstNumeric = 0x40;
/// The first code of the 94 or 96 positions a G-set occupies.
constexpr uint8_t kFirstGraphic = 0x20;

/// A C0 control with an effect on the presentation (X3.110 §6.1), by name;
/// nullptr for one without.
const char* c0_name(uint8_t code) {
  switch (code) {
    case 0x07:
      return "BEL";
    case kNaplpsApb:
      return "APB";
    case kNaplpsApf:
      return "APF";
    case kNaplpsApd:
      return "APD";
    case kNaplpsApu:
      return "APU";
    case kNaplpsCs:
      return "CS";
    case kNaplpsApr:
      return "APR";
    case kNaplpsSo:
      return "SO";
    case kNaplpsSi:
      return "SI";
    case kNaplpsCan:
      return "CAN";
    case kNaplpsSs2:
      return "SS2";
    case kNaplpsSdc:
      return "SDC";
    case kNaplpsAps:
      return "APS";
    case kNaplpsSs3:
      return "SS3";
    case kNaplpsAph:
      return "APH";
    case kNaplpsNsr:
      return "NSR";
    default:
      return nullptr;
  }
}

const char* set_name(NaplpsGSet set) {
  switch (set) {
    case NaplpsGSet::kPrimary:
      return "primary";
    case NaplpsGSet::kSupplementary:
      return "supplementary";
    case NaplpsGSet::kPdi:
      return "PDI";
    case NaplpsGSet::kMosaic:
      return "mosaic";
    case NaplpsGSet::kMacro:
      return "macro";
    case NaplpsGSet::kDrcs:
      return "DRCS";
    case NaplpsGSet::kNull:
      break;
  }
  return "null";
}

std::string hex_code(uint8_t code) {
  constexpr const char* kDigits = "0123456789ABCDEF";
  return std::string{kDigits[code >> 4], kDigits[code & 0x0F]};
}

std::string count_of(size_t count, const char* one, const char* many) {
  return std::to_string(count) + " " + (count == 1 ? one : many);
}

/// Collects the listing: a line per command, controls gathered on a line of
/// their own until something else comes, characters into a quoted run.
class Listing {
 public:
  void control(const std::string& name) {
    end_text();
    if (!controls_.empty()) {
      controls_ += ' ';
    }
    controls_ += name;
  }

  void line(const std::string& text) {
    end_controls();
    end_text();
    out_ += text + '\n';
  }

  void character(const std::string& glyph) {
    end_controls();
    text_ += glyph;
  }

  std::string finish() {
    end_controls();
    end_text();
    return out_;
  }

 private:
  void end_controls() {
    if (!controls_.empty()) {
      out_ += controls_ + '\n';
      controls_.clear();
    }
  }

  void end_text() {
    if (!text_.empty()) {
      out_ += '"' + text_ + "\"\n";
      text_.clear();
    }
  }

  std::string out_;
  std::string controls_;
  std::string text_;
};

}  // namespace

std::string naplps_code_listing(const std::vector<uint8_t>& data,
                                const std::vector<uint8_t>& present) {
  std::vector<uint8_t> codes(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    codes[i] = static_cast<uint8_t>(data[i] & 0x7F);
  }
  const auto arrived = [&present](size_t i) {
    return i >= present.size() || present[i] != 0;
  };
  // A control failing its parity is skipped as the record is read (the
  // interpreter's rule, after the Panasonic TU-2000x's).
  const auto mark = [&data](size_t i) {
    return teletext_odd_parity_valid(data[i]) ? "" : "?";
  };
  // The bytes from |i| on that a command takes as numeric data, transmission
  // controls among them aside (§5.3.1): one past the last.
  const auto numeric_end = [&](size_t i) {
    while (i < codes.size() && arrived(i) &&
           (codes[i] >= kFirstNumeric ||
            naplps_is_transparent_control(codes[i]))) {
      ++i;
    }
    return i;
  };

  Listing out;
  NaplpsCodeEnvironment env;
  size_t i = 0;
  while (i < codes.size()) {
    if (!arrived(i)) {
      size_t end = i;
      while (end < codes.size() && !arrived(end)) {
        ++end;
      }
      out.line("[" + count_of(end - i, "byte", "bytes") + " lost]");
      i = end;
      continue;
    }
    const uint8_t code = codes[i];
    if (naplps_is_transparent_control(code)) {
      ++i;
      continue;
    }

    if (code == kNaplpsEsc) {
      const NaplpsEscape escape =
          naplps_parse_escape(codes.data() + i + 1, codes.size() - i - 1);
      const std::string at = mark(i);
      size_t next = i + escape.length;
      switch (escape.kind) {
        case NaplpsEscapeKind::kDesignation:
          env.designate(escape.slot, escape.set);
          out.control("G" + std::to_string(static_cast<int>(escape.slot)) +
                      "=" + set_name(escape.set) + at);
          break;
        case NaplpsEscapeKind::kLockingShift:
          env.invoke_locking(escape.slot);
          out.control("LS" + std::to_string(static_cast<int>(escape.slot)) +
                      at);
          break;
        case NaplpsEscapeKind::kControl: {
          const std::string name =
              kC1Names[static_cast<uint8_t>(escape.c1) - kFirstC1];
          if (escape.c1 == NaplpsC1::kRepeat && next < codes.size() &&
              codes[next] >= kFirstNumeric) {
            out.control(name + " " + std::to_string(codes[next] & 0x3F) + at);
            ++next;
          } else if (naplps_terminates_definition(escape.c1) &&
                     escape.c1 != NaplpsC1::kEnd) {
            // The code it defines, then the body up to the next control that
            // ends a definition (§6.2.2.1, §6.2.3, §6.2.4).
            std::string defined;
            if (next < codes.size() && codes[next] >= kFirstGraphic) {
              defined = " " + hex_code(codes[next]);
              ++next;
            }
            size_t end = next;
            while (end + 1 < codes.size() &&
                   !(codes[end] == kNaplpsEsc &&
                     naplps_is_c1_final(codes[end + 1]) &&
                     naplps_terminates_definition(
                         static_cast<NaplpsC1>(codes[end + 1])))) {
              ++end;
            }
            if (end + 1 >= codes.size()) {
              end = codes.size();
            }
            out.line(name + defined + at + " (" +
                     count_of(end - next, "byte", "bytes") + ")");
            next = end;
          } else {
            out.control(name + at);
          }
          break;
        }
        case NaplpsEscapeKind::kUnsupported:
        case NaplpsEscapeKind::kMalformed:
        case NaplpsEscapeKind::kTruncated:
          out.control("ESC" + at);
          break;
      }
      i = next;
      continue;
    }

    if (code < kFirstGraphic) {
      const std::string at = mark(i);
      if (code == kNaplpsSo) {
        env.invoke_locking(NaplpsGSlot::kG1);
      } else if (code == kNaplpsSi) {
        env.invoke_locking(NaplpsGSlot::kG0);
      } else if (code == kNaplpsSs2) {
        env.invoke_single_shift(NaplpsGSlot::kG2);
      } else if (code == kNaplpsSs3) {
        env.invoke_single_shift(NaplpsGSlot::kG3);
      }
      const char* name = c0_name(code);
      std::string text = name != nullptr ? name : "<" + hex_code(code) + ">";
      // NSR and APS take a row and a column after themselves where both
      // bytes are numeric (§6.1.2.4, §6.1.6.5).
      if ((code == kNaplpsNsr || code == kNaplpsAps) && i + 2 < codes.size() &&
          codes[i + 1] >= kFirstNumeric && codes[i + 2] >= kFirstNumeric) {
        text += " row " + std::to_string(codes[i + 1] & 0x3F) + " column " +
                std::to_string(codes[i + 2] & 0x3F);
        i += 2;
      }
      if (code == kNaplpsNsr) {
        env.reset();
      }
      out.control(text + at);
      ++i;
      continue;
    }

    const NaplpsGSet set = env.in_use();
    env.consume_character();
    switch (set) {
      case NaplpsGSet::kPdi: {
        const size_t end = numeric_end(i + 1);
        if (code < kFirstNumeric) {
          out.line(std::string(kPdiNames[code - kFirstGraphic]) + mark(i) +
                   " (" + count_of(end - i - 1, "byte", "bytes") + ")");
          i = end;
        } else {
          const size_t run = numeric_end(i);
          out.line("operand bytes with no instruction (" +
                   count_of(run - i, "byte", "bytes") + ")");
          i = run;
        }
        continue;
      }
      case NaplpsGSet::kPrimary:
        if (!teletext_odd_parity_valid(data[i])) {
          out.character("\xEF\xBF\xBD");
        } else if (code == '"') {
          out.character("\\\"");
        } else {
          out.character(std::string(1, static_cast<char>(code)));
        }
        break;
      case NaplpsGSet::kSupplementary:
        out.character("<S" + hex_code(code) + ">");
        break;
      case NaplpsGSet::kMosaic:
        out.character("<M" + hex_code(code) + ">");
        break;
      case NaplpsGSet::kDrcs:
        out.character("<D" + hex_code(code) + ">");
        break;
      case NaplpsGSet::kMacro:
        out.control("macro " + hex_code(code) + mark(i));
        break;
      case NaplpsGSet::kNull:
        break;
    }
    ++i;
  }
  return out.finish();
}

}  // namespace orc
