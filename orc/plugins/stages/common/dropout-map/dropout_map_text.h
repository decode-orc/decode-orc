/*
 * File:        dropout_map_text.h
 * Module:      decode-orc stage plugins and orc-view_core
 * Purpose:     The dropout_map stage's "dropout_map" parameter text, read and
 *              written by the stage and by its Dropout Editor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 decode-orc contributors
 */

#pragma once

// The stage stores its per-frame dropout edits as one string parameter:
//
//   [{frame:N,add:[{line:L,start:S,end:E},...],remove:[...]},...]
//
// frame is 0-based (the parameter editor shows it 1-based); line is the
// frame-flat 0-based line and start/end are samples within that line. The
// editor reads the stored text with IStageToolSession::parameters() and
// writes it back with apply_parameters(), so both must read it the same way:
// this header is the one copy.
//
// Standard library only: the stage includes it beside the stage tier and the
// view beside the view tier, and neither tier's types may cross.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace orc::dropout_map_text {

constexpr char kDropoutEditorContractId[] =
    "decode-orc.stage-tools.dropout-editor.v1";

/// The stage's one parameter.
constexpr char kParameterName[] = "dropout_map";

/// One dropout extent on one line.
struct Span {
  uint32_t line = 0;          ///< Frame-flat 0-based line
  uint32_t start_sample = 0;  ///< Sample within the line
  uint32_t end_sample = 0;    ///< Sample within the line
};

inline bool operator==(const Span& a, const Span& b) {
  return a.line == b.line && a.start_sample == b.start_sample &&
         a.end_sample == b.end_sample;
}
inline bool operator!=(const Span& a, const Span& b) { return !(a == b); }

/// What one frame adds to and removes from its input's dropouts.
struct FrameEdits {
  uint64_t frame_id = 0;
  std::vector<Span> additions;
  std::vector<Span> removals;
};

/// The edits by frame.
using EditMap = std::map<uint64_t, FrameEdits>;

namespace detail {

class Reader {
 public:
  explicit Reader(const std::string& text) : text_(text) {}

  bool at_end() const { return pos_ >= text_.size(); }
  char peek() const { return at_end() ? '\0' : text_[pos_]; }

  void skip_space() {
    while (!at_end() &&
           std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
      ++pos_;
    }
  }

  bool expect(char c) {
    skip_space();
    if (peek() == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  std::string key() {
    skip_space();
    std::string out;
    while (!at_end() &&
           std::isalpha(static_cast<unsigned char>(text_[pos_])) != 0) {
      out += text_[pos_++];
    }
    return out;
  }

  uint64_t number() {
    skip_space();
    uint64_t value = 0;
    while (!at_end() &&
           std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
      value = value * 10 + static_cast<uint64_t>(text_[pos_++] - '0');
    }
    return value;
  }

 private:
  const std::string& text_;
  size_t pos_ = 0;
};

inline Span read_span(Reader& in) {
  Span span;
  if (!in.expect('{')) return span;
  while (!in.at_end() && in.peek() != '}') {
    const std::string key = in.key();
    if (!in.expect(':')) break;
    const auto value = static_cast<uint32_t>(in.number());
    if (key == "line") {
      span.line = value;
    } else if (key == "start") {
      span.start_sample = value;
    } else if (key == "end") {
      span.end_sample = value;
    }
    in.expect(',');
    in.skip_space();
  }
  in.expect('}');
  return span;
}

inline std::vector<Span> read_spans(Reader& in) {
  std::vector<Span> spans;
  if (!in.expect('[')) return spans;
  in.skip_space();
  while (!in.at_end() && in.peek() != ']') {
    if (in.peek() == '{') {
      spans.push_back(read_span(in));
    } else if (!in.expect(',')) {
      break;
    }
    in.expect(',');
    in.skip_space();
  }
  in.expect(']');
  return spans;
}

inline void write_spans(std::ostringstream& out, const char* name,
                        const std::vector<Span>& spans) {
  if (spans.empty()) return;
  out << ',' << name << ":[";
  bool first = true;
  for (const auto& span : spans) {
    if (!first) out << ',';
    first = false;
    out << "{line:" << span.line << ",start:" << span.start_sample
        << ",end:" << span.end_sample << '}';
  }
  out << ']';
}

}  // namespace detail

/// True when @p text does not start with '[' once leading spaces are
/// skipped (and is not empty), so parse() reads nothing from it.
inline bool is_malformed(const std::string& text) {
  detail::Reader in(text);
  in.skip_space();
  return !in.at_end() && in.peek() != '[';
}

/// Read the parameter text. Empty text, "[]" and malformed text give an
/// empty map; unknown keys are skipped. A frame named twice keeps its last
/// entry.
inline EditMap parse(const std::string& text) {
  EditMap result;
  detail::Reader in(text);
  if (!in.expect('[')) return result;
  in.skip_space();
  while (!in.at_end() && in.peek() != ']') {
    if (!in.expect('{')) break;
    FrameEdits edits;
    while (!in.at_end() && in.peek() != '}') {
      const std::string key = in.key();
      if (!in.expect(':')) break;
      if (key == "frame") {
        edits.frame_id = in.number();
      } else if (key == "add") {
        edits.additions = detail::read_spans(in);
      } else if (key == "remove") {
        edits.removals = detail::read_spans(in);
      } else {
        // A key this version does not know: skip its value, a list or a
        // number, so the keys after it are still read.
        in.skip_space();
        if (in.peek() == '[') {
          (void)detail::read_spans(in);
        } else {
          (void)in.number();
        }
      }
      in.expect(',');
      in.skip_space();
    }
    in.expect('}');
    result[edits.frame_id] = edits;
    in.expect(',');
    in.skip_space();
  }
  return result;
}

/// Write the parameter text, frames in order. Frames with neither
/// additions nor removals are still written; an empty map is "[]".
inline std::string encode(const EditMap& map) {
  if (map.empty()) return "[]";
  std::ostringstream out;
  out << '[';
  bool first = true;
  for (const auto& [frame_id, edits] : map) {
    if (!first) out << ',';
    first = false;
    out << "{frame:" << frame_id;
    detail::write_spans(out, "add", edits.additions);
    detail::write_spans(out, "remove", edits.removals);
    out << '}';
  }
  out << ']';
  return out.str();
}

}  // namespace orc::dropout_map_text
