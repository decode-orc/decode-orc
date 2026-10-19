/*
 * File:        cc_line21_report.cpp
 * Module:      orc-core
 * Purpose:     What a recording's line 21 carried, for the Closed Caption
 *              Sink's report: services, XDS, ATVEF triggers and field order
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "cc_line21_report.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace orc {

namespace {

// Longest trigger read before it is given up as not a trigger at all: ATVEF
// triggers are short, and TEXT2 is a narrow channel.
constexpr size_t kMaxTriggerCharacters = 512;
// Triggers quoted in the report.
constexpr size_t kMaxExamples = 3;
// A checksum attribute is four hexadecimal digits [CTA-608-E §7.12]; its
// example spells them with a "0x" before them.
constexpr size_t kChecksumDigits = 4;

// Miscellaneous control code that restarts a text service [CTA-608-E §7].
constexpr uint8_t kTextRestart = 0x2A;

// The checksum's four hex digits, with or without the "0x" CTA-608-E §7.12's
// example puts before them, as a number; -1 for any other attribute.
int32_t checksum_value(const std::string& attribute) {
  std::string digits = attribute;
  if (digits.size() == kChecksumDigits + 2 && digits[0] == '0' &&
      (digits[1] == 'x' || digits[1] == 'X')) {
    digits = digits.substr(2);
  }
  if (digits.size() != kChecksumDigits) {
    return -1;
  }
  for (const char c : digits) {
    if (std::isxdigit(static_cast<unsigned char>(c)) == 0) {
      return -1;
    }
  }
  return static_cast<int32_t>(std::strtol(digits.c_str(), nullptr, 16));
}

// RFC 791 / RFC 1071: the ones' complement sum of sixteen-bit words, the
// first byte of each the more significant, an odd byte padded with zero.
uint16_t ones_complement_sum(const std::string& text) {
  uint32_t sum = 0;
  for (size_t i = 0; i < text.size(); i += 2) {
    uint32_t word = static_cast<uint32_t>(static_cast<uint8_t>(text[i])) << 8U;
    if (i + 1 < text.size()) {
      word |= static_cast<uint8_t>(text[i + 1]);
    }
    sum += word;
    sum = (sum & 0xFFFFU) + (sum >> 16U);
  }
  return static_cast<uint16_t>(sum);
}

// A URL a trigger may carry: an http:, https: or lid: URL (ATVEF 1.1) made
// of the characters RFC 1738 §2.2 allows in one — letters, digits, the safe
// and extra characters, the reserved ones and "%" escapes — and "~" and "#",
// which URLs carry in practice. |url| is the text between the angle brackets.
bool is_trigger_url(const std::string& url) {
  const size_t colon = url.find(':');
  if (colon == std::string::npos || colon + 1 >= url.size()) {
    return false;
  }
  std::string scheme = url.substr(0, colon);
  for (char& c : scheme) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (scheme != "http" && scheme != "https" && scheme != "lid") {
    return false;
  }
  static constexpr const char* kUrlPunctuation = "$-_.+!*'(),;/?:@=&%~#";
  for (const char c : url) {
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 &&
        std::strchr(kUrlPunctuation, c) == nullptr) {
      return false;
    }
  }
  return true;
}

}  // namespace

const char* cc_service_label(EIA608Service service) {
  switch (service) {
    case EIA608Service::T1:
      return "TEXT1";
    case EIA608Service::T2:
      return "TEXT2";
    case EIA608Service::T3:
      return "TEXT3";
    case EIA608Service::T4:
      return "TEXT4";
    default:
      return eia608_service_name(service);
  }
}

void CCAtvefTriggerScanner::reset() {
  state_ = State::kIdle;
  trigger_.clear();
  last_attribute_.clear();
  last_attribute_at_ = 0;
}

void CCAtvefTriggerScanner::restart() { reset(); }

void CCAtvefTriggerScanner::complete() {
  // The checksum is over everything before its own "[".
  const int32_t value = checksum_value(last_attribute_);
  bool holds = false;
  if (value >= 0 && last_attribute_at_ > 0) {
    uint32_t total =
        ones_complement_sum(trigger_.substr(0, last_attribute_at_)) +
        static_cast<uint32_t>(value);
    total = (total & 0xFFFFU) + (total >> 16U);
    holds = total == 0xFFFFU;
  }
  if (holds) {
    ++triggers_;
    if (examples_.size() < kMaxExamples) {
      examples_.push_back(trigger_);
    }
  } else {
    ++unchecked_;
  }
  reset();
}

void CCAtvefTriggerScanner::put(char character) {
  const auto byte = static_cast<uint8_t>(character);
  if (byte < 0x20 || byte >= 0x7F) {
    // Not a character of a URL [CTA-608-E §7.11].
    return;
  }
  if (trigger_.size() >= kMaxTriggerCharacters) {
    reset();
  }

  switch (state_) {
    case State::kIdle:
      if (character == '<') {
        trigger_ = "<";
        state_ = State::kUrl;
      }
      return;
    case State::kUrl:
      if (character == '<') {
        trigger_ = "<";
        return;
      }
      if (character == '>') {
        if (is_trigger_url(trigger_.substr(1))) {
          trigger_ += character;
          state_ = State::kAfterUrl;
        } else {
          reset();
        }
        return;
      }
      trigger_ += character;
      return;
    case State::kAfterUrl:
      if (character == '[') {
        last_attribute_at_ = trigger_.size();
        last_attribute_.clear();
        trigger_ += character;
        state_ = State::kAttribute;
        return;
      }
      complete();
      put(character);
      return;
    case State::kAttribute:
      if (character == '<' || character == '>' || character == '[') {
        // Brackets do not appear inside an attribute [CTA-608-E §7.12].
        reset();
        put(character);
        return;
      }
      trigger_ += character;
      if (character == ']') {
        state_ = State::kAfterUrl;
      } else {
        last_attribute_ += character;
      }
      return;
  }
}

void CCAtvefTriggerScanner::finish() {
  if (state_ == State::kAfterUrl) {
    complete();
  }
  reset();
}

CCLine21Report::CCLine21Report()
    : demux_(EIA608Service::CC1, /*suppress_repeated_controls=*/true) {}

void CCLine21Report::add_lines(int field, uint64_t lines, uint64_t carried) {
  FieldCounts& counts = fields_[field == 0 ? 0 : 1];
  counts.lines += lines;
  counts.carried += carried;
}

void CCLine21Report::add_neighbour_line(int field, int line, uint64_t pairs) {
  FieldCounts& counts = fields_[field == 0 ? 0 : 1];
  (line == 20 ? counts.line20 : counts.line22) += pairs;
}

void CCLine21Report::add_pair(const EIA608Line21Run::Pair& pair, bool xds) {
  const int field = pair.television_field == 0 ? 0 : 1;
  FieldCounts& counts = fields_[static_cast<size_t>(field)];
  const auto byte0 = static_cast<uint8_t>(pair.data0 & 0x7F);
  const auto byte1 = static_cast<uint8_t>(pair.data1 & 0x7F);
  if (!pair.taken()) {
    // A pair is read only where its first byte's parity held, so one that is
    // no code EIA-608 defines is counted where its second byte's held too.
    if (pair.isolated) {
      ++counts.isolated;
    } else if (!pair.first_only) {
      ++counts.undefined;
    }
    return;
  }
  ++counts.taken;
  if (byte0 != 0 || byte1 != 0) {
    ++counts.not_null;
  }
  if (pair.first_only) {
    ++counts.first_only;
  }
  if (xds) {
    return;
  }

  demux_.accept(field, byte0, byte1);
  const auto service = demux_.last_service();
  if (!service.has_value()) {
    return;
  }
  const bool to_text2 = *service == EIA608Service::T2;
  if (byte0 >= 0x10 && byte0 <= 0x1F) {
    if (to_text2 && (byte0 & 0xF6) == 0x14 && byte1 == kTextRestart) {
      atvef_.restart();
    }
    return;
  }
  for (const uint8_t byte : {byte0, byte1}) {
    if (byte >= 0x20 && byte < 0x7F) {
      ++characters_[static_cast<size_t>(*service)];
      if (to_text2) {
        atvef_.put(static_cast<char>(byte));
      }
    }
  }
}

void CCLine21Report::add_xds_packet(const CCXdsPacket& packet) {
  xds_.add(packet);
}

std::string CCLine21Report::text(uint64_t frames) const {
  std::string out = "Line 21 report\n";
  out += "  Frames:          " + std::to_string(frames) + "\n";
  const std::array<std::array<EIA608Service, 4>, 2> services = {{
      {EIA608Service::CC1, EIA608Service::CC2, EIA608Service::T1,
       EIA608Service::T2},
      {EIA608Service::CC3, EIA608Service::CC4, EIA608Service::T3,
       EIA608Service::T4},
  }};
  for (size_t field = 0; field < 2; ++field) {
    const FieldCounts& counts = fields_[field];
    char line[240];
    std::snprintf(line, sizeof(line),
                  "  Field %zu:         %llu of %llu lines carried line 21 "
                  "data, %llu pairs taken, %llu of them not null padding",
                  field + 1, static_cast<unsigned long long>(counts.carried),
                  static_cast<unsigned long long>(counts.lines),
                  static_cast<unsigned long long>(counts.taken),
                  static_cast<unsigned long long>(counts.not_null));
    out += line;
    if (counts.first_only > 0) {
      out += " (" + std::to_string(counts.first_only) +
             " by their first character alone, the second byte's parity "
             "failed)";
    }
    if (counts.undefined > 0) {
      out += ", " + std::to_string(counts.undefined) +
             " more refused as no code EIA-608 defines";
    }
    if (counts.isolated > 0) {
      out += ", " + std::to_string(counts.isolated) +
             " refused as standing alone (no line 21 data around them)";
    }
    out += "\n";
    if (counts.line22 > 0 || counts.line20 > 0) {
      out +=
          "                   read on the line next to line 21 where it "
          "carried none (the time base slipped a line): " +
          std::to_string(counts.line22) + " on line 22, " +
          std::to_string(counts.line20) + " on line 20\n";
    }
    std::string characters = "                   characters:";
    for (const EIA608Service service : services[field]) {
      characters += std::string(" ") + cc_service_label(service) + " " +
                    std::to_string(characters_[static_cast<size_t>(service)]);
    }
    out += characters + "\n";
  }

  if (fields_reversed_) {
    out +=
        "  Field order:     reversed — the capture's first stored field is "
        "television field 2 (its line 21 spells the control codes as field 2 "
        "does, or the second stored field's as field 1 does), so CC1, CC2, "
        "TEXT1 and TEXT2 were read from the second stored field\n";
  }
  out += "  XDS:             " + std::to_string(xds_good_) + " packets";
  if (xds_bad_ > 0) {
    out += ", " + std::to_string(xds_bad_) + " more that failed their checksum";
  }
  out += "\n";
  for (const std::string& line : xds_.lines()) {
    out += "    " + line + "\n";
  }

  out += "  ATVEF triggers:  " + std::to_string(atvef_.triggers()) +
         " on TEXT2 (Transport A: <url>[attribute]...[checksum], the "
         "checksum holding)";
  if (atvef_.unchecked() > 0) {
    out += ", " + std::to_string(atvef_.unchecked()) +
           " more well formed whose checksum is missing or fails";
  }
  out += "\n";
  for (const std::string& example : atvef_.examples()) {
    out += "    " + example + "\n";
  }
  return out;
}

}  // namespace orc
