/*
 * File:        cc_line21_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the Closed Caption Sink's XDS decoder, ATVEF
 *              trigger count and report
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../../../../orc/plugins/stages/cc_sink/cc_line21_report.h"
#include "../../../../orc/plugins/stages/cc_sink/cc_xds_decoder.h"

namespace orc_unit_test {
namespace {

using orc::CCXdsPacket;
using orc::EIA608Service;

////////////////////////////////////////////////////////////////////////////////
// Builders
////////////////////////////////////////////////////////////////////////////////

/// The pairs of one XDS packet: start code and type, the characters two to a
/// field, the end code and the checksum that zeroes the seven-bit sum.
std::vector<std::pair<uint8_t, uint8_t>> xds_packet(
    uint8_t xds_class, uint8_t type, const std::string& characters) {
  std::vector<std::pair<uint8_t, uint8_t>> pairs = {{xds_class, type}};
  unsigned sum = xds_class + type;
  for (size_t i = 0; i < characters.size(); i += 2) {
    const auto first = static_cast<uint8_t>(characters[i]);
    const auto second = i + 1 < characters.size()
                            ? static_cast<uint8_t>(characters[i + 1])
                            : uint8_t{0};
    pairs.emplace_back(first, second);
    sum += first + second;
  }
  sum += 0x0F;
  pairs.emplace_back(0x0F, static_cast<uint8_t>((128 - (sum % 128)) % 128));
  return pairs;
}

std::vector<CCXdsPacket> feed(
    orc::CCXdsDecoder& decoder,
    const std::vector<std::pair<uint8_t, uint8_t>>& pairs) {
  std::vector<CCXdsPacket> out;
  decoder.set_packet_callback(
      [&out](const CCXdsPacket& packet) { out.push_back(packet); });
  uint64_t field = 1;
  for (const auto& [first, second] : pairs) {
    decoder.accept(first, second, field);
    field += 2;
  }
  return out;
}

/// RFC 791's checksum of |text|, as an ATVEF trigger carries it.
std::string trigger_checksum(const std::string& text) {
  uint32_t sum = 0;
  for (size_t i = 0; i < text.size(); i += 2) {
    uint32_t word = static_cast<uint32_t>(static_cast<uint8_t>(text[i])) << 8U;
    if (i + 1 < text.size()) {
      word |= static_cast<uint8_t>(text[i + 1]);
    }
    sum += word;
  }
  while ((sum >> 16U) != 0) {
    sum = (sum & 0xFFFFU) + (sum >> 16U);
  }
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%04X", (~sum) & 0xFFFFU);
  return buffer;
}

////////////////////////////////////////////////////////////////////////////////
// The Extended Data Service
////////////////////////////////////////////////////////////////////////////////

TEST(CCXdsDecoder, AssemblesAPacketAndChecksItsChecksum) {
  orc::CCXdsDecoder decoder;
  const auto packets = feed(decoder, xds_packet(0x05, 0x01, "PBS "));
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].xds_class, 0x05);
  EXPECT_EQ(packets[0].type, 0x01);
  EXPECT_EQ(orc::cc_xds_value_text(packets[0]), "PBS");
  EXPECT_EQ(orc::cc_xds_type_name(0x05, 0x01), "Channel: network name");
  EXPECT_EQ(decoder.packets_good(), 1u);
  EXPECT_EQ(decoder.packets_bad(), 0u);
}

TEST(CCXdsDecoder, RefusesAPacketWhoseChecksumFails) {
  orc::CCXdsDecoder decoder;
  auto pairs = xds_packet(0x05, 0x02, "WXYZ");
  pairs.back().second = static_cast<uint8_t>((pairs.back().second + 1) & 0x7F);
  EXPECT_TRUE(feed(decoder, pairs).empty());
  EXPECT_EQ(decoder.packets_bad(), 1u);
}

// A caption control code takes the line back for its service; the packet
// resumes on its continue code, and the caption's pairs are not XDS.
TEST(CCXdsDecoder, ResumesAPacketACaptionInterrupted) {
  orc::CCXdsDecoder decoder;
  auto pairs = xds_packet(0x01, 0x03, "NOVA");
  std::vector<CCXdsPacket> packets;
  decoder.set_packet_callback(
      [&packets](const CCXdsPacket& packet) { packets.push_back(packet); });
  EXPECT_TRUE(decoder.accept(pairs[0].first, pairs[0].second, 1));
  EXPECT_TRUE(decoder.accept(pairs[1].first, pairs[1].second, 3));
  // CC3: resume caption loading, then two characters.
  EXPECT_FALSE(decoder.accept(0x15, 0x20, 5));
  EXPECT_FALSE(decoder.accept('H', 'I', 7));
  // Continue code for the current class, same type.
  EXPECT_TRUE(decoder.accept(0x02, 0x03, 9));
  EXPECT_TRUE(decoder.accept(pairs[2].first, pairs[2].second, 11));
  EXPECT_TRUE(decoder.accept(pairs[3].first, pairs[3].second, 13));
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(orc::cc_xds_value_text(packets[0]), "NOVA");
}

TEST(CCXdsDecoder, LeavesCaptionPairsOutsideAPacketAlone) {
  orc::CCXdsDecoder decoder;
  EXPECT_FALSE(decoder.accept(0x15, 0x2C, 1));
  EXPECT_FALSE(decoder.accept('O', 'K', 3));
  EXPECT_FALSE(decoder.accept(0x00, 0x00, 5));
}

// The time of day, UTC, and the time zone.
TEST(CCXdsDecoder, ReadsTheTimeOfDayAndTimeZone) {
  CCXdsPacket time;
  time.xds_class = 0x07;
  time.type = 0x01;
  // Minute 34, hour 3, date 8, month 11, Sunday, 1998 — bit 6 set in each.
  time.data = {0x40 | 34, 0x40 | 3, 0x40 | 8, 0x40 | 11, 0x40 | 1, 0x40 | 8};
  EXPECT_EQ(orc::cc_xds_value_text(time), "1998-11-08 03:34 UTC");

  CCXdsPacket zone;
  zone.xds_class = 0x07;
  zone.type = 0x04;
  zone.data = {0x40 | 0x20 | 5, 0x00};
  EXPECT_EQ(orc::cc_xds_value_text(zone), "UTC-5, daylight saving observed");
}

CCXdsPacket packet_of(uint8_t xds_class, uint8_t type,
                      std::vector<uint8_t> data) {
  CCXdsPacket packet;
  packet.xds_class = xds_class;
  packet.type = type;
  packet.data = std::move(data);
  return packet;
}

// Content advisory [CTA-608-E §9.5.1.5, Tables 18-23], as recordings send it:
// a TV rating with content flags (48h 6Ch), TV-PG and TV-G, an MPA "Not
// Rated", and the two Canadian systems.
TEST(CCXdsDecoder, ReadsTheContentAdvisory) {
  const auto advisory = [](uint8_t first, uint8_t second) {
    return orc::cc_xds_value_text(packet_of(0x01, 0x05, {first, second}));
  };
  EXPECT_EQ(advisory(0x48, 0x6C), "TV-PG (violence, adult language)");
  EXPECT_EQ(advisory(0x48, 0x44), "TV-PG");
  EXPECT_EQ(advisory(0x48, 0x43), "TV-G");
  EXPECT_EQ(advisory(0x68, 0x75),
            "TV-14 (violence, sexual situations, "
            "suggestive dialogue)");
  // Fantasy violence goes with TV-Y7 alone (Table 21).
  EXPECT_EQ(advisory(0x48, 0x62), "TV-Y7 (fantasy violence)");
  EXPECT_EQ(advisory(0x48, 0x63), "TV-G");
  EXPECT_EQ(advisory(0x47, 0x40), "MPA Not Rated");
  EXPECT_EQ(advisory(0x53, 0x40), "MPA PG-13");
  EXPECT_EQ(advisory(0x58, 0x45), "Canadian English 14+");
  EXPECT_EQ(advisory(0x78, 0x43), "Canadian French 13 ans +");
  EXPECT_EQ(advisory(0x78, 0x47), "Canadian French (invalid)");
  // A system reserved for other countries is left in hexadecimal.
  EXPECT_EQ(advisory(0x58, 0x4C), "58 4C");
}

// Length and time in show [CTA-608-E §9.5.1.2], programme type keywords
// [§9.5.1.4, Table 17], and the end of a programme [§9.5.1.1].
TEST(CCXdsDecoder, ReadsTheProgrammesLengthTypeAndEnd) {
  EXPECT_EQ(orc::cc_xds_value_text(packet_of(0x01, 0x02, {0x40 | 30, 0x40})),
            "00:30");
  EXPECT_EQ(orc::cc_xds_value_text(
                packet_of(0x01, 0x02, {0x40, 0x41, 0x7B, 0x40, 0x40 | 5})),
            "01:00, 00:59:05 elapsed");
  EXPECT_EQ(orc::cc_xds_value_text(packet_of(0x01, 0x04, {0x21, 0x3C})),
            "Entertainment, Drama");
  EXPECT_EQ(orc::cc_xds_value_text(packet_of(0x03, 0x04, {0x22, 0x7F})),
            "Movie, Western");
  EXPECT_EQ(
      orc::cc_xds_value_text(packet_of(0x01, 0x01, {0x7F, 0x7F, 0x7F, 0x7F})),
      "end of programme");
  EXPECT_EQ(orc::cc_xds_value_text(
                packet_of(0x01, 0x01, {0x40, 0x40 | 20, 0x40 | 13, 0x50 | 1})),
            "starts 01-13 20:00 UTC, tape delayed");
}

// The other binary packets [CTA-608-E §9.5.1.6-9.5.1.8, §9.5.3.3, §9.5.3.4]:
// audio and caption services, copy control, tape delay and the transmission
// signal identifier (4Ch 4Eh 43h 40h is 03ECh).
TEST(CCXdsDecoder, ReadsTheServicesCopyControlAndChannelIdentity) {
  EXPECT_EQ(orc::cc_xds_value_text(packet_of(0x01, 0x06, {0x4B, 0x51})),
            "main English, true stereo; second Spanish, mono");
  EXPECT_EQ(orc::cc_xds_value_text(packet_of(0x01, 0x07, {0x48, 0x49, 0x52})),
            "CC1 English, TEXT1 English, CC2 Spanish");
  EXPECT_EQ(orc::cc_xds_value_text(packet_of(0x01, 0x08, {0x52, 0x41})),
            "one copy permitted, PSP on, split burst off, redistribution "
            "controlled");
  EXPECT_EQ(orc::cc_xds_value_text(packet_of(0x05, 0x03, {0x40 | 30, 0x43})),
            "03:30");
  EXPECT_EQ(
      orc::cc_xds_value_text(packet_of(0x05, 0x04, {0x4C, 0x4E, 0x43, 0x40})),
      "03ECh (1004)");
  // Private data stays in hexadecimal.
  EXPECT_EQ(orc::cc_xds_value_text(packet_of(0x0D, 0x41, {0x41, 0x7F})),
            "41 7F");
}

TEST(CCXdsSummary, ListsEachTypesValuesInTheOrderTheyArrived) {
  orc::CCXdsSummary summary;
  CCXdsPacket name;
  name.xds_class = 0x01;
  name.type = 0x03;
  name.data = {'N', 'O', 'V', 'A'};
  summary.add(name);
  summary.add(name);
  name.data = {'F', 'R', 'O', 'N', 'T', 'L', 'I', 'N', 'E'};
  summary.add(name);
  const auto lines = summary.lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0],
            "Current programme: name: \"NOVA\" x2; \"FRONTLINE\" x1 (3 "
            "packets)");
}

////////////////////////////////////////////////////////////////////////////////
// ATVEF triggers
////////////////////////////////////////////////////////////////////////////////

TEST(CCAtvefTriggerScanner, CountsATriggerWhoseChecksumHolds) {
  orc::CCAtvefTriggerScanner scanner;
  const std::string body = "<http://www.example.com/news.html>[name:News]";
  const std::string trigger = body + "[" + trigger_checksum(body) + "]";
  for (const char c : "Some text " + trigger + " more text") {
    scanner.put(c);
  }
  scanner.finish();
  EXPECT_EQ(scanner.triggers(), 1u);
  EXPECT_EQ(scanner.unchecked(), 0u);
  ASSERT_EQ(scanner.examples().size(), 1u);
  EXPECT_EQ(scanner.examples()[0], trigger);
}

// CTA-608-E §7.12's own example spells the checksum "0x" and four digits.
TEST(CCAtvefTriggerScanner, ReadsAChecksumSpelledWithItsPrefix) {
  orc::CCAtvefTriggerScanner scanner;
  const std::string body = "<lid:show>[t:p]";
  for (const char c : body + "[0x" + trigger_checksum(body) + "]") {
    scanner.put(c);
  }
  scanner.finish();
  EXPECT_EQ(scanner.triggers(), 1u);
}

// Well formed, but the checksum fails or is missing: CTA-608-E §7.12 requires
// one, so the string is not counted as a trigger, only noted.
TEST(CCAtvefTriggerScanner, DoesNotCountATriggerWhoseChecksumFailsOrIsMissing) {
  orc::CCAtvefTriggerScanner scanner;
  for (const char c :
       std::string("<lid:show>[expires:20001231][0000] <http://a.tv/>[n:A]")) {
    scanner.put(c);
  }
  scanner.finish();
  EXPECT_EQ(scanner.triggers(), 0u);
  EXPECT_EQ(scanner.unchecked(), 2u);
  EXPECT_TRUE(scanner.examples().empty());
}

// A card dump can read "<B:rqX:C}s>" from noise: no URL a trigger carries,
// and no checksum.
TEST(CCAtvefTriggerScanner, CountsNothingThatIsNotAUrl) {
  orc::CCAtvefTriggerScanner scanner;
  for (const char c : std::string("<B:rqX:C}s> PRICES <UP> 3 [EST] <ftp:x>")) {
    scanner.put(c);
  }
  scanner.finish();
  EXPECT_EQ(scanner.triggers(), 0u);
  EXPECT_EQ(scanner.unchecked(), 0u);
}

// A text restart abandons a trigger being read [CTA-608-E §7.10].
TEST(CCAtvefTriggerScanner, AbandonsATriggerOnATextRestart) {
  orc::CCAtvefTriggerScanner scanner;
  const std::string body = "<http://a.tv/>";
  const std::string trigger = body + "[" + trigger_checksum(body) + "]";
  for (const char c : trigger.substr(0, 6)) {
    scanner.put(c);
  }
  scanner.restart();
  for (const char c : trigger.substr(6)) {
    scanner.put(c);
  }
  scanner.finish();
  EXPECT_EQ(scanner.triggers(), 0u);
}

////////////////////////////////////////////////////////////////////////////////
// The report
////////////////////////////////////////////////////////////////////////////////

/// A pair with both parities good that EIA-608 defines for television field
/// |field|, read amid line 21 data: one the report takes.
orc::EIA608Line21Run::Pair report_pair(int field, uint8_t data0,
                                       uint8_t data1) {
  orc::EIA608Line21Run::Pair pair;
  pair.television_field = field;
  pair.data0 = data0;
  pair.data1 = data1;
  pair.defined = true;
  return pair;
}

// TEXT2 is selected by a text restart on data channel 2 (1Ch 2Ah); its
// characters are counted and searched for triggers.
TEST(CCLine21Report, CountsEachServicesCharactersAndTheTriggersOnText2) {
  orc::CCLine21Report report;
  report.add_pair(report_pair(0, 0x14, 0x2C),
                  /*xds=*/false);  // CC1: erase displayed memory
  report.add_pair(report_pair(0, 'H', 'I'), /*xds=*/false);
  report.add_pair(report_pair(0, 0x1C, 0x2A),
                  /*xds=*/false);  // TEXT2: text restart
  const std::string body = "<http://a.tv/>";
  const std::string trigger = body + "[" + trigger_checksum(body) + "]";
  for (size_t i = 0; i < trigger.size(); i += 2) {
    report.add_pair(
        report_pair(
            0, static_cast<uint8_t>(trigger[i]),
            i + 1 < trigger.size() ? static_cast<uint8_t>(trigger[i + 1]) : 0),
        /*xds=*/false);
  }
  report.add_pair(report_pair(0, 0x1C, 0x2D),
                  /*xds=*/false);  // carriage return
  // The second field: an XDS pair is the Extended Data Service's.
  report.add_pair(report_pair(1, 'X', 'Y'), /*xds=*/true);
  report.finish();

  EXPECT_EQ(report.characters(EIA608Service::CC1), 2u);
  EXPECT_EQ(report.characters(EIA608Service::T2), trigger.size());
  EXPECT_EQ(report.characters(EIA608Service::CC3), 0u);
  EXPECT_EQ(report.atvef().triggers(), 1u);

  const std::string text = report.text(10);
  EXPECT_NE(text.find("ATVEF triggers:  1 on TEXT2"), std::string::npos)
      << text;
  EXPECT_NE(text.find("CC1 2"), std::string::npos) << text;
}

// A pair not taken — a parity failed, it stood alone, or it is no code —
// adds no characters.
TEST(CCLine21Report, CountsOnlyThePairsTaken) {
  orc::CCLine21Report report;
  report.add_lines(0, 4, 4);
  report.add_pair(report_pair(0, 0x14, 0x20), /*xds=*/false);
  auto bad_parity = report_pair(0, 'W', 'E');
  bad_parity.defined = false;
  bad_parity.first_only = true;
  report.add_pair(bad_parity, /*xds=*/false);
  auto alone = report_pair(0, 'D', 'I');
  alone.isolated = true;
  report.add_pair(alone, /*xds=*/false);
  auto undefined = report_pair(0, 0x16, 0x25);
  undefined.defined = false;
  report.add_pair(undefined, /*xds=*/false);
  report.finish();

  EXPECT_EQ(report.characters(EIA608Service::CC1), 0u);
  const std::string text = report.text(4);
  EXPECT_NE(text.find("4 of 4 lines carried line 21 data, 1 pairs taken"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("1 more refused as no code EIA-608 defines"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("1 refused as standing alone"), std::string::npos)
      << text;
}

TEST(CCLine21Report, SaysWhenTheFieldsWereReadTheOtherWayRound) {
  orc::CCLine21Report report;
  EXPECT_EQ(report.text(10).find("Field order:"), std::string::npos);
  report.set_fields_reversed(true);
  EXPECT_NE(report.text(10).find("Field order:     reversed"),
            std::string::npos);
}

}  // namespace
}  // namespace orc_unit_test
