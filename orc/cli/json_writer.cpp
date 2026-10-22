/*
 * File:        json_writer.cpp
 * Module:      orc-cli
 * Purpose:     Minimal JSON emitter for the machine-readable command output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "json_writer.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace orc {
namespace cli {

namespace {

/// Hex digit for one nibble, for the \uXXXX form control characters need.
char hex_digit(unsigned value) {
  return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

/// Name the plugin runtime registers its logger under. Its default sink is
/// stdout, which is the stream a JSON document needs to itself.
constexpr const char* kRuntimeLoggerName = "core";

}  // namespace

void reserve_stdout_for_json() {
  auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();

  // The runtime adopts an already-registered logger of this name rather than
  // creating its own, so registering first is enough when nothing has logged
  // yet; when something has, its sinks are repointed in place, because the
  // runtime caches the logger it found and re-registering would not reach it.
  if (auto existing = spdlog::get(kRuntimeLoggerName)) {
    existing->sinks() = std::vector<spdlog::sink_ptr>{sink};
    return;
  }

  auto logger =
      std::make_shared<spdlog::logger>(kRuntimeLoggerName, std::move(sink));
  logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
  logger->set_level(spdlog::level::info);
  spdlog::register_logger(std::move(logger));
}

void quiet_runtime_logging() {
  if (auto logger = spdlog::get(kRuntimeLoggerName)) {
    logger->set_level(spdlog::level::warn);
  }
}

std::string json_escape(const std::string& text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char raw : text) {
    const unsigned char ch = static_cast<unsigned char>(raw);
    switch (ch) {
      case '"':
        escaped += "\\\"";
        continue;
      case '\\':
        escaped += "\\\\";
        continue;
      case '\b':
        escaped += "\\b";
        continue;
      case '\f':
        escaped += "\\f";
        continue;
      case '\n':
        escaped += "\\n";
        continue;
      case '\r':
        escaped += "\\r";
        continue;
      case '\t':
        escaped += "\\t";
        continue;
      default:
        break;
    }
    if (ch < 0x20) {
      escaped += "\\u00";
      escaped += hex_digit((ch >> 4) & 0xf);
      escaped += hex_digit(ch & 0xf);
      continue;
    }
    escaped += raw;
  }
  return escaped;
}

bool is_json_number(const std::string& text) {
  std::size_t i = 0;
  if (i < text.size() && text[i] == '-') {
    ++i;
  }
  const std::size_t integer_start = i;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    ++i;
  }
  if (i == integer_start) {
    return false;
  }
  // JSON has no octal-looking numbers: a leading zero is only allowed as the
  // whole integer part, so "007" is text, not a number, and gets quoted.
  if (text[integer_start] == '0' && i - integer_start > 1) {
    return false;
  }
  if (i < text.size() && text[i] == '.') {
    ++i;
    const std::size_t fraction_start = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      ++i;
    }
    if (i == fraction_start) {
      return false;
    }
  }
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
      ++i;
    }
    const std::size_t exponent_start = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      ++i;
    }
    if (i == exponent_start) {
      return false;
    }
  }
  return i == text.size();
}

std::string JsonWriter::indent() const {
  return std::string(empty_.size() * 2, ' ');
}

void JsonWriter::prefix() {
  if (pending_key_) {
    // The key already wrote the separator and the space after the colon.
    pending_key_ = false;
    return;
  }
  if (empty_.empty()) {
    return;  // The document's outermost value.
  }
  if (!empty_.back()) {
    out_ << ",";
  }
  out_ << "\n" << indent();
  empty_.back() = false;
}

void JsonWriter::begin_object() {
  prefix();
  out_ << "{";
  empty_.push_back(true);
}

void JsonWriter::end_object() {
  const bool was_empty = empty_.back();
  empty_.pop_back();
  if (!was_empty) {
    out_ << "\n" << indent();
  }
  out_ << "}";
}

void JsonWriter::begin_array() {
  prefix();
  out_ << "[";
  empty_.push_back(true);
}

void JsonWriter::end_array() {
  const bool was_empty = empty_.back();
  empty_.pop_back();
  if (!was_empty) {
    out_ << "\n" << indent();
  }
  out_ << "]";
}

void JsonWriter::key(const std::string& name) {
  prefix();
  out_ << "\"" << json_escape(name) << "\": ";
  pending_key_ = true;
}

void JsonWriter::member(const std::string& name, const std::string& value) {
  key(name);
  prefix();
  out_ << "\"" << json_escape(value) << "\"";
}

void JsonWriter::member(const std::string& name, const char* value) {
  member(name, std::string(value == nullptr ? "" : value));
}

void JsonWriter::member_bool(const std::string& name, bool value) {
  key(name);
  prefix();
  out_ << (value ? "true" : "false");
}

void JsonWriter::member_int(const std::string& name, std::int64_t value) {
  key(name);
  prefix();
  out_ << value;
}

void JsonWriter::member_number(const std::string& name,
                               const std::string& token) {
  // A token that is not a number would produce a document no reader accepts,
  // so it is written as the string it is rather than corrupting the output.
  if (!is_json_number(token)) {
    member(name, token);
    return;
  }
  key(name);
  prefix();
  out_ << token;
}

void JsonWriter::member_null(const std::string& name) {
  key(name);
  value_null();
}

void JsonWriter::member_strings(const std::string& name,
                                const std::vector<std::string>& values) {
  key(name);
  begin_array();
  for (const auto& entry : values) {
    value(entry);
  }
  end_array();
}

void JsonWriter::value(const std::string& text) {
  prefix();
  out_ << "\"" << json_escape(text) << "\"";
}

void JsonWriter::value_null() {
  prefix();
  out_ << "null";
}

void JsonWriter::finish() { out_ << "\n"; }

}  // namespace cli
}  // namespace orc
