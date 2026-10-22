/*
 * File:        json_writer.h
 * Module:      orc-cli
 * Purpose:     Minimal JSON emitter for the machine-readable command output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CLI_JSON_WRITER_H
#define ORC_CLI_JSON_WRITER_H

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace orc {
namespace cli {

// JSON is the CLI's query output: a script parses it and Orc never reads it
// back. That direction is why the emitter lives here rather than reusing the
// YAML writer orc-core links — the paste-ready forms Orc does read back
// (`stages info --yaml`, `--filtergraph`) stay in the formats their readers
// accept, and this side needs one small fully-specified escaping function and
// no new link dependency.
//
// Values are written in the order the caller emits them; the caller is
// responsible for balancing every begin/end pair.

/**
 * @brief Keep stdout clear for a JSON document.
 *
 * The plugin runtime logs to stdout, and one log line in the middle of a
 * document makes it unparseable — so a command about to emit JSON claims the
 * runtime's shared "core" logger first and points it at stderr. The
 * diagnostics still reach the user; they just stop sharing the stream the
 * document is on. Call once, before the presenter calls that gather the data.
 */
void reserve_stdout_for_json();

/// Pass only warnings and errors from the runtime's "core" logger, for a
/// command asked to be quiet. Call after reserve_stdout_for_json().
void quiet_runtime_logging();

/// Escape a UTF-8 string into a JSON string body, without the quotes. Bytes
/// above 0x7f are already valid JSON when the input is UTF-8, so they pass
/// through unchanged rather than being re-encoded as escapes.
std::string json_escape(const std::string& text);

/// True when @p text is already valid JSON number syntax, so it can be written
/// verbatim instead of being re-rendered (and rounded) on the way out.
bool is_json_number(const std::string& text);

/**
 * @brief Emit a JSON document to a stream, indented two spaces per level.
 *
 * Deliberately smaller than a JSON library: it writes, it never parses, and
 * every value it can write is one a presenter type already holds.
 */
class JsonWriter {
 public:
  explicit JsonWriter(std::ostream& out) : out_(out) {}

  void begin_object();
  void end_object();
  void begin_array();
  void end_array();

  /// Name the next value. The value call that follows becomes this member.
  void key(const std::string& name);

  void member(const std::string& name, const std::string& value);
  void member(const std::string& name, const char* value);
  void member_bool(const std::string& name, bool value);
  void member_int(const std::string& name, std::int64_t value);
  /// A member whose value is already in JSON number syntax. Written verbatim,
  /// so a stored "1.500000" reaches a script as the number it was.
  void member_number(const std::string& name, const std::string& token);
  void member_null(const std::string& name);
  void member_strings(const std::string& name,
                      const std::vector<std::string>& values);

  /// One string element of an array.
  void value(const std::string& text);
  /// A null in value position: an array element, or the value a key names when
  /// the object it would have introduced does not exist.
  void value_null();

  /// Terminate the document. Call once, after the outermost container closes.
  void finish();

 private:
  void prefix();
  std::string indent() const;

  std::ostream& out_;
  /// One flag per open container: true until it holds something, so the
  /// emitter knows whether a separator is due.
  std::vector<bool> empty_;
  /// Set between key() and the value it names, which share a line.
  bool pending_key_ = false;
};

}  // namespace cli
}  // namespace orc

#endif  // ORC_CLI_JSON_WRITER_H
