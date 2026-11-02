/*
 * File:        nabts_network_messages_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for a network's data services read out of
 *              private-use data groups
 *
 * Covers: the 4-for-3 coding, ANPA 1312's heading fields, the binary heading
 * block, and the log: where a header ends, a service's groups put back in
 * sequence order with copies merged and lost groups marked, the items cut and
 * read by their framing, carousel copies merged, a status signal, a page, and
 * the groups it must leave alone.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_network_messages.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nabts_test_builders.h"

namespace orc_unit_test {
namespace {

using nabts::hamming;
using nabts::parity;
using orc::NabtsDataGroup;
using orc::NabtsNetworkMessage;
using orc::NabtsNetworkMessageLog;

////////////////////////////////////////////////////////////////////////////////
// Builders
////////////////////////////////////////////////////////////////////////////////

constexpr char kSoh = 0x01;
constexpr char kStx = 0x02;
constexpr char kEtx = 0x03;
constexpr char kEot = 0x04;
constexpr char kSyn = 0x16;
constexpr char kUs = 0x1F;

/// A provider header: each nibble as its Hamming 8/4 codeword.
std::vector<uint8_t> header(const std::vector<uint8_t>& nibbles) {
  std::vector<uint8_t> out;
  for (const uint8_t nibble : nibbles) {
    out.push_back(hamming(nibble));
  }
  return out;
}

/// The nibbles of a header for every station: kind, service, sequence, then
/// the addressing field "7 F F".
std::vector<uint8_t> to_all(uint8_t kind, uint16_t service, uint8_t sequence) {
  return {kind,
          static_cast<uint8_t>((service >> 8) & 0xF),
          static_cast<uint8_t>((service >> 4) & 0xF),
          static_cast<uint8_t>(service & 0xF),
          sequence,
          0x7,
          0xF,
          0xF};
}

/// Seven-bit text with odd parity, as the services send their characters.
std::vector<uint8_t> seven_bit(const std::string& text) {
  std::vector<uint8_t> out;
  for (const char c : text) {
    out.push_back(parity(static_cast<uint8_t>(c)));
  }
  return out;
}

NabtsDataGroup group(uint16_t channel, const std::vector<uint8_t>& head,
                     const std::vector<uint8_t>& data) {
  NabtsDataGroup g;
  g.channel = channel;
  g.channel_attested = true;
  g.header.valid = true;
  g.header.attested = true;
  g.header.type_attested = true;
  g.header.type = orc::kNabtsPrivateGroupType;
  g.data = head;
  g.data.insert(g.data.end(), data.begin(), data.end());
  g.present.assign(g.data.size(), orc::kNabtsByteArrived);
  return g;
}

/// |bytes| in the 4-for-3 coding, a space after each four characters.
std::string coded(std::vector<uint8_t> bytes) {
  while (bytes.size() % 3 != 0) {
    bytes.push_back(0);
  }
  std::string out;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    uint8_t high = 0;
    for (size_t k = 0; k < 3; ++k) {
      out += static_cast<char>(0x30 + (bytes[i + k] & 0x3F));
      high = static_cast<uint8_t>(high | ((bytes[i + k] >> 6) << (2 * k)));
    }
    out += static_cast<char>(0x30 + high);
    out += ' ';
  }
  return out;
}

std::vector<uint8_t> bytes_of(const std::string& text) {
  return {text.begin(), text.end()};
}

std::string field(const NabtsNetworkMessage& message, const std::string& name) {
  for (const auto& [key, value] : message.fields) {
    if (key == name) {
      return value;
    }
  }
  return {};
}

////////////////////////////////////////////////////////////////////////////////
// The coding and the headings
////////////////////////////////////////////////////////////////////////////////

TEST(NabtsNetworkMessagesTest, FourCharactersCarryThreeBytes) {
  // A run of a recorded message's text: "ad " — the high bits of 'a' and 'd'
  // in the fourth character, and the space's none.
  const auto bytes = orc::nabts_decode_4for3("QTP5");
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ((*bytes)[0], 'a');
  EXPECT_EQ((*bytes)[1], 'd');
  EXPECT_EQ((*bytes)[2], ' ');
  EXPECT_FALSE(orc::nabts_decode_4for3("QT P").has_value());
}

TEST(NabtsNetworkMessagesTest, ACodedRunDecodesAndALostTokenIsLostBytes) {
  std::vector<int> characters;
  for (const char c : coded(bytes_of("CR LF\r\n"))) {
    characters.push_back(c);
  }
  std::vector<int> decoded = orc::nabts_decode_4for3_run(characters);
  ASSERT_EQ(decoded.size(), 9U);
  EXPECT_EQ(decoded[0], 'C');
  EXPECT_EQ(decoded[5], '\r');
  EXPECT_EQ(decoded[6], '\n');

  characters[1] = -1;  // a character of the first token never arrived
  decoded = orc::nabts_decode_4for3_run(characters);
  ASSERT_EQ(decoded.size(), 9U);
  EXPECT_EQ(decoded[0], -1);
  EXPECT_EQ(decoded[2], -1);
  EXPECT_EQ(decoded[3], 'L');
}

TEST(NabtsNetworkMessagesTest, AnAnpaHeadingReadsAsItsFields) {
  const std::string heading = std::string("v0474") + kUs + "cbs-0\nr v" +
                              "\x13\x11" +
                              " BC-DNA27-Bush,DNA27 g0474 a0000 04-30 0205";
  const auto fields = orc::nabts_anpa_heading_fields(heading);
  ASSERT_GE(fields.size(), 10U);
  EXPECT_EQ(fields[0].second, "v");
  EXPECT_EQ(fields[1].second, "0474");
  EXPECT_EQ(fields[2].second, "cbs-0");
  EXPECT_EQ(fields[3].second, "r");
  EXPECT_EQ(fields[4].second, "v");
  EXPECT_EQ(fields[5].second, "<DC3><DC1>");
  EXPECT_EQ(fields[6].second, "BC-DNA27-Bush,DNA27");
  EXPECT_EQ(fields[9].first, "Filing date");
  EXPECT_EQ(fields[9].second, "04-30");
  EXPECT_TRUE(orc::nabts_anpa_heading_fields("no heading here").empty());
}

TEST(NabtsNetworkMessagesTest, AHeadingBlockReadsItsFixedFields) {
  std::vector<uint8_t> block(0x102, 0);
  block[10] = 0xCD;
  block[11] = 0xAB;
  const auto put = [&block](size_t at, const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
      block[at + i] = static_cast<uint8_t>(text[i]);
    }
  };
  put(12, "v48080wi--WI-Dahmer(Tops)");
  for (size_t i = 22 + 15; i < 44; ++i) {
    block[i] = 0xB0;  // the field's padding
  }
  put(64, "rnap");
  put(77, "WI");
  put(124, "AP");
  const auto fields = orc::nabts_heading_block_fields(block);
  ASSERT_FALSE(fields.empty());
  EXPECT_EQ(fields[0].second, "v");
  EXPECT_EQ(fields[1].second, "4808");
  EXPECT_EQ(fields[2].second, "0wi--");
  EXPECT_EQ(fields[3].first, "Keyword");
  EXPECT_EQ(fields[3].second, "WI-Dahmer(Tops)");
  EXPECT_EQ(fields.back().second, "AP");

  block[10] = 0;  // no magic: not such a block
  EXPECT_TRUE(orc::nabts_heading_block_fields(block).empty());
}

////////////////////////////////////////////////////////////////////////////////
// The log
////////////////////////////////////////////////////////////////////////////////

TEST(NabtsNetworkMessagesTest, AWireMessageRunsAcrossItsServicesGroups) {
  // An ANPA 1312 message in two groups of consecutive sequence numbers, each
  // followed by a fill group under the same header, which carries nothing.
  const std::string message = std::string(2, kSyn) + kSoh + "v0001" + kUs +
                              "cbs-1\nr v\x14\x12 BC-FEEDTXT NET-NEWS " +
                              "v0000 00-00 0000\r\n" + kStx +
                              "LINEUP FOR TONIGHT\r\nITEM ONE\r\nITEM TWO\r\n" +
                              kEtx + "\r\n" + kEot;
  const std::vector<uint8_t> text = seven_bit(message);
  // Split before a line end, as the service's groups end where they are full;
  // a CR is no Hamming codeword, so the second group proves its header too.
  const size_t half = message.find("\r\nITEM TWO");
  NabtsNetworkMessageLog log;
  log.add_group(group(0xCFD, header(to_all(0x1, 0xCF0, 0x8)),
                      {text.begin(), text.begin() + static_cast<long>(half)}),
                100);
  log.add_group(group(0xCFD, header(to_all(0xF, 0xCF0, 0x8)),
                      std::vector<uint8_t>(64, 0x80)),
                101);
  log.add_group(group(0xCFD, header(to_all(0x1, 0xCF0, 0x9)),
                      {text.begin() + static_cast<long>(half), text.end()}),
                102);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  const NabtsNetworkMessage& m = messages[0];
  EXPECT_EQ(m.record_address, "CF0");
  EXPECT_EQ(m.kind, "message");
  EXPECT_EQ(m.form, "ANPA 1312");
  EXPECT_EQ(m.addressing, "all stations");
  EXPECT_EQ(m.title, "BC-FEEDTXT");
  EXPECT_EQ(field(m, "Version"), "NET-NEWS");
  EXPECT_EQ(m.groups, 2U);
  EXPECT_TRUE(m.complete);
  EXPECT_EQ(m.lost_bytes, 0U);
  EXPECT_NE(m.text.find("ITEM TWO"), std::string::npos);
}

TEST(NabtsNetworkMessagesTest, AStationMessageSentTwiceIsOneMessage) {
  // A station message under a bitmap of stations, sent twice; the first copy
  // lost a stretch the second has.
  std::vector<uint8_t> nibbles = {0x1, 0xC, 0xB, 0x2, 0x3, 0x3, 0x0, 0x0};
  nibbles.insert(nibbles.end(), 16, 0x0);
  nibbles[10] = 0x8;  // one station
  const std::string message =
      std::string(1, kSoh) +
      "CALL\r\nFM DESK 4\r\n\r\nRE: SCHEDULE CHANGE\r\n" +
      "THE PROGRAM MOVES TO 9PM.\r\n" + kEtx + "N G KXXX 27 10203 1522 2001" +
      "\r\n\n\n" + kEot;
  NabtsDataGroup first = group(0xCD0, header(nibbles), seven_bit(message));
  for (size_t i = 40; i < 50; ++i) {
    first.present[i] = orc::kNabtsByteLost;
  }
  NabtsNetworkMessageLog log;
  log.add_group(first, 10);
  log.add_group(group(0xCD0, header(nibbles), seven_bit(message)), 20);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  const NabtsNetworkMessage& m = messages[0];
  EXPECT_EQ(m.form, "station message");
  EXPECT_EQ(m.title, "SCHEDULE CHANGE");
  EXPECT_EQ(m.trailer, "N G KXXX 27 10203 1522 2001");
  EXPECT_EQ(m.copies, 1U);
  EXPECT_EQ(m.lost_bytes, 0U);
  EXPECT_EQ(m.addressing.rfind("1 of 72 stations by bitmap", 0), 0U)
      << m.addressing;
}

TEST(NabtsNetworkMessagesTest, AGroupSentAgainUnderItsSequenceNumberFillsIt) {
  // A group repeated at once under the same sequence number is a copy of it,
  // not the next part of the stream: its bytes fill those the first lost.
  const std::string message = std::string(1, kSoh) +
                              "RE: REPEATED GROUP\r\nTHE TEXT OF IT.\r\n" +
                              kEtx + "N G DESK 12 10203 0900 0901\r\n" + kEot;
  NabtsDataGroup first =
      group(0xCD0, header(to_all(0x1, 0xCB2, 0x6)), seven_bit(message));
  const size_t lost_from = 8 + 10;
  for (size_t i = lost_from; i < lost_from + 6; ++i) {
    first.present[i] = orc::kNabtsByteLost;
  }
  NabtsNetworkMessageLog log;
  log.add_group(first, 1);
  log.add_group(
      group(0xCD0, header(to_all(0x1, 0xCB2, 0x6)), seven_bit(message)), 2);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].groups, 1U);
  EXPECT_EQ(messages[0].copies, 1U);
  EXPECT_EQ(messages[0].lost_bytes, 0U);
  EXPECT_EQ(messages[0].title, "REPEATED GROUP");
  EXPECT_NE(messages[0].text.find("THE TEXT OF IT."), std::string::npos);
}

// A control among the text, or DEL, is shown by its name.
TEST(NabtsNetworkMessagesTest, AControlAmongTheTextIsShownByItsName) {
  const std::string message = std::string(1, kSoh) +
                              "RE: BELL\r\nRING \x07 TWICE \x7F.\r\n" + kEtx +
                              "N G DESK 12 10203 0900 0901\r\n" + kEot;
  NabtsNetworkMessageLog log;
  log.add_group(
      group(0xCD0, header(to_all(0x1, 0xCB2, 0x6)), seven_bit(message)), 1);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_NE(messages[0].text.find("RING <BEL> TWICE <7F>."), std::string::npos)
      << messages[0].text;
}

TEST(NabtsNetworkMessagesTest, AHeaderEndsWhereTheServicesProvenHeadersDo) {
  // The second group's text opens with 'P', whose byte with parity is the
  // Hamming codeword of 8: its header must not take it in.
  NabtsNetworkMessageLog log;
  const std::vector<uint8_t> head = to_all(0x1, 0xCF0, 0x2);
  log.add_group(
      group(0xCFD, header(head),
            seven_bit(std::string(1, kSoh) + "FIRST ITEM\r\n" + kEtx + kEot)),
      1);
  log.add_group(
      group(0xCFD, header(to_all(0x1, 0xCF0, 0x3)),
            seven_bit(std::string(1, kSoh) + "SECOND\r\n" + kEtx + kEot)),
      2);
  log.add_group(group(0xCFD, header(to_all(0x1, 0xCF0, 0x4)),
                      seven_bit("PART TWO HERE\r\n")),
                3);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 3U);
  // Plain text, though two of its words are four of the coding's characters.
  EXPECT_EQ(messages[2].form, "text");
  EXPECT_EQ(messages[2].text.substr(0, 8), "PART TWO");
}

TEST(NabtsNetworkMessagesTest, ASkippedSequenceNumberMarksLostGroups) {
  NabtsNetworkMessageLog log;
  log.add_group(group(0xCFD, header(to_all(0x1, 0xCF0, 0x4)),
                      seven_bit(std::string(1, kSoh) + "BEGINS HERE AND")),
                1);
  log.add_group(
      group(0xCFD, header(to_all(0x1, 0xCF0, 0x6)),
            seven_bit(" ENDS THERE\r\n" + std::string(1, kEtx) + kEot)),
      2);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_GE(messages[0].lost_bytes, 1U);
  EXPECT_NE(messages[0].text.find("AND[...] ENDS"), std::string::npos)
      << messages[0].text;
}

TEST(NabtsNetworkMessagesTest, ACodedMessageDecodesItsBlockAndText) {
  // A relayed message: SOH and a plain heading, SYN SYN SOH, CR LF, the coded
  // heading block, CR LF STX CR LF, the coded text, which ends ETX EOT.
  std::vector<uint8_t> block(0x102, 0);
  block[10] = 0xCD;
  block[11] = 0xAB;
  const std::string keyword = "v12340ks--KS-Weather";
  for (size_t i = 0; i < keyword.size(); ++i) {
    block[12 + i] = static_cast<uint8_t>(keyword[i]);
  }
  const std::string body = "^AP-KS-Weather\r\n\tSunny and warm.\r\n";
  std::vector<uint8_t> text = bytes_of(body);
  text.insert(text.end(), {0xFF, 0xFF, 0x0D, 0x0A, 0x03, 0x04});
  const std::string stream = std::string(1, kSoh) + "W APKS 101r Maaa AAAA " +
                             kSyn + kSyn + kSoh + "\r\n" + coded(block) +
                             "\r\n" + kStx + "\r\n" + coded(text);
  NabtsNetworkMessageLog log;
  log.add_group(
      group(0xCFD, header(to_all(0x1, 0xCF0, 0x0)), seven_bit(stream)), 5);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  const NabtsNetworkMessage& m = messages[0];
  EXPECT_EQ(m.form, "ANPA 1312, 4-for-3 coded");
  EXPECT_EQ(field(m, "Relay heading"), "W APKS 101r Maaa AAAA");
  EXPECT_EQ(field(m, "Number"), "1234");
  EXPECT_EQ(m.title, "KS-Weather");
  EXPECT_NE(m.text.find("Sunny and warm."), std::string::npos) << m.text;
  EXPECT_TRUE(m.complete);
  EXPECT_GE(m.frame.size(), 0x100U);
}

TEST(NabtsNetworkMessagesTest, ACodedMessagesRestIsDecodedWithoutItsStart) {
  // The recording begins inside a coded message: its text runs on to a
  // plain ETX and EOT, with no SOH or heading before it. Its first character,
  // 'd', is also a Hamming codeword, which the header for every station must
  // not take in although no other group proves where it ends.
  const std::string stream =
      coded(bytes_of("the rest of the story.\r\n")) + "\r\n" + kEtx + kEot;
  NabtsNetworkMessageLog log;
  log.add_group(
      group(0xCFD, header(to_all(0x1, 0xCF0, 0x0)), seven_bit(stream)), 1);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].form,
            "ANPA 1312, 4-for-3 coded, its start not received");
  EXPECT_EQ(messages[0].title, "the rest of the story.");
  EXPECT_TRUE(messages[0].complete);
}

TEST(NabtsNetworkMessagesTest, ACarouselsCopiesOfAnItemAreOneItem) {
  // A dated alert sent on each turn of a carousel under the next sequence
  // number, padded out with NUL.
  std::vector<uint8_t> alert = seven_bit(
      "01-02-1980  08:33:06\r\n\r\nSTAND BY FOR A SPECIAL REPORT\r\n");
  alert.insert(alert.end(), 16, 0x80);
  NabtsNetworkMessageLog log;
  for (uint8_t sequence = 0; sequence < 3; ++sequence) {
    log.add_group(group(0x002, header(to_all(0x5, 0xCBA, sequence)), alert),
                  sequence * 100U);
  }
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].kind, "urgent message");
  EXPECT_EQ(messages[0].title, "STAND BY FOR A SPECIAL REPORT");
  EXPECT_EQ(field(messages[0], "Sent"), "01-02-1980  08:33:06");
  EXPECT_EQ(messages[0].copies, 2U);
}

TEST(NabtsNetworkMessagesTest, AStatusSignalIsItsValuesOverTheRecording) {
  // Five nibbles and padding: the fifth is the value.
  NabtsNetworkMessageLog log;
  const std::vector<uint8_t> padding(14, 0x80);
  uint64_t frame = 0;
  for (const uint8_t value : {0, 0, 0, 9, 9, 0}) {
    log.add_group(group(0xA00, header({0x1, 0x0, 0xC, 0xC, value}), padding),
                  frame += 30);
  }
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].kind, "status");
  EXPECT_EQ(messages[0].record_address, "0CC");
  EXPECT_EQ(field(messages[0], "Values"), "0, 9");
  EXPECT_EQ(messages[0].text,
            "0  frames 30-90, 3 groups\n9  frames 120-150, 2 groups\n"
            "0  frames 180-180, 1 groups\n");
}

TEST(NabtsNetworkMessagesTest, APageIsKeptAsItsNaplpsCode) {
  std::vector<uint8_t> code =
      seven_bit(std::string("\x18\x1b") + "E" + "\x0e" + " ABC" + "\x0f" +
                "Commences operation soon");
  code.insert(code.end(), 8, 0x80);
  NabtsNetworkMessageLog log;
  log.add_group(group(0x002, header(to_all(0x0, 0xCBA, 0x1)), code), 7);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].form, "NAPLPS");
  EXPECT_EQ(messages[0].kind, "page");
  // The code as it arrived, parity and all, as a record's is kept.
  EXPECT_EQ(messages[0].code.front(), parity(0x18));
  EXPECT_EQ(messages[0].title, "Commences operation soon");
}

// A page's byte that failed its parity in every copy keeps the parity it
// failed, so the interpreter takes it for no control, as the receivers did.
TEST(NabtsNetworkMessagesTest, APagesFailingByteKeepsTheParityItFailed) {
  std::vector<uint8_t> code =
      seven_bit(std::string("\x0e") + "hGGFxxhGGD@p" + "\x0f" + "WEATHER MAP");
  code.insert(code.end(), 8, 0x80);
  code[0] ^= 0x80;  // SO, failing
  NabtsNetworkMessageLog log;
  log.add_group(group(0x002, header(to_all(0x0, 0xCBA, 0x1)), code), 1);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].code.front(), code[0]);
  EXPECT_EQ(messages[0].damaged_bytes, 1U);
}

// Copies vote byte by byte: a character one copy received failing its parity
// is taken from another that holds it clean, however many hold it failing.
TEST(NabtsNetworkMessagesTest, AFailingCharacterIsTakenFromACleanCopy) {
  const std::string message = std::string(1, kSoh) +
                              "RE: VOTED\r\nTHE ITEM'S TEXT HERE.\r\n" + kEtx +
                              "N G DESK 12 10203 0900 0901\r\n" + kEot;
  const std::vector<uint8_t> clean = seven_bit(message);
  std::vector<uint8_t> failing = clean;
  const size_t at = message.find("ITEM'S");
  failing[at] ^= 0x01;  // 'I' read as 'H', failing its parity
  for (const bool group_copies : {true, false}) {
    NabtsNetworkMessageLog log;
    // A group sent again at once under its sequence number, or the item sent
    // again under the next as a carousel does.
    for (uint8_t copy = 0; copy < 3; ++copy) {
      log.add_group(
          group(0xCD0,
                header(to_all(0x1, 0xCB2, group_copies ? 0x6 : 0x6 + copy)),
                copy < 2 ? failing : clean),
          copy);
    }
    const std::vector<NabtsNetworkMessage> messages = log.messages();
    ASSERT_EQ(messages.size(), 1U) << group_copies;
    EXPECT_NE(messages[0].text.find("THE ITEM'S TEXT"), std::string::npos)
        << group_copies << ": " << messages[0].text;
    EXPECT_EQ(messages[0].damaged_bytes, 0U) << group_copies;
  }
}

// Among copies that hold a byte clean, the most of them decide it.
TEST(NabtsNetworkMessagesTest, TheCopiesThatAgreeOutvoteOneThatDiffers) {
  std::vector<uint8_t> page;
  for (int i = 0; i < 120; ++i) {
    page.push_back(parity(static_cast<uint8_t>('A' + i % 26)));
  }
  page.insert(page.end(), 8, 0x80);
  std::vector<uint8_t> misread = page;
  misread[50] = parity('#');  // passes its parity, and is wrong
  NabtsNetworkMessageLog log;
  log.add_group(group(0x002, header(to_all(0x0, 0xCBA, 0x1)), misread), 1);
  log.add_group(group(0x002, header(to_all(0x0, 0xCBA, 0x2)), page), 2);
  log.add_group(group(0x002, header(to_all(0x0, 0xCBA, 0x3)), page), 3);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].copies, 2U);
  EXPECT_EQ(messages[0].code[50], page[50]);
}

// A character that failed its parity in every copy is shown as U+FFFD and
// counted, and a control that failed is taken for none: here the ETX, so the
// item does not split into text and trailer.
TEST(NabtsNetworkMessagesTest, ACharacterFailingInEveryCopyIsMarkedDamaged) {
  const std::string message = std::string(1, kSoh) +
                              "RE: DAMAGED\r\nONE BAD LETTER.\r\n" + kEtx +
                              "N G DESK 12 10203 0900 0901\r\n" + kEot;
  std::vector<uint8_t> bytes = seven_bit(message);
  bytes[message.find("BAD")] ^= 0x02;
  NabtsNetworkMessageLog log;
  log.add_group(group(0xCD0, header(to_all(0x1, 0xCB2, 0x6)), bytes), 1);
  std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_NE(messages[0].text.find("ONE \xEF\xBF\xBD"
                                  "AD LETTER."),
            std::string::npos)
      << messages[0].text;
  EXPECT_EQ(messages[0].damaged_bytes, 1U);
  EXPECT_EQ(messages[0].form, "station message");
  EXPECT_NE(orc::nabts_network_message_document(messages[0])
                .find("; 1 character failed its parity in every copy"),
            std::string::npos);

  bytes = seven_bit(message);
  bytes[message.find(kEtx)] ^= 0x80;
  NabtsNetworkMessageLog failing_etx;
  failing_etx.add_group(group(0xCD0, header(to_all(0x1, 0xCB2, 0x6)), bytes),
                        1);
  messages = failing_etx.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].form, "text");
  EXPECT_TRUE(messages[0].trailer.empty());
  EXPECT_EQ(messages[0].damaged_bytes, 1U);
}

// A coded token's damaged character spoils only the bytes it carries part of:
// one of the first three its own byte's low six bits, the fourth the top two
// bits of all three.
TEST(NabtsNetworkMessagesTest, ADamagedCodedCharacterSpoilsOnlyItsBytes) {
  const auto run = [](size_t damaged_at) {
    std::vector<int> characters;
    for (const char c : std::string("QTP5 ")) {
      characters.push_back(c);
    }
    characters[damaged_at] |= orc::kNabtsDamagedCharacter;
    return orc::nabts_decode_4for3_run(characters);
  };
  const std::vector<int> second = run(1);
  ASSERT_EQ(second.size(), 3U);
  EXPECT_EQ(second[0], 'a');
  EXPECT_EQ(second[1], 'd' | orc::kNabtsDamagedCharacter);
  EXPECT_EQ(second[2], ' ');
  const std::vector<int> fourth = run(3);
  ASSERT_EQ(fourth.size(), 3U);
  EXPECT_EQ(fourth[0], 'a' | orc::kNabtsDamagedCharacter);
  EXPECT_EQ(fourth[1], 'd' | orc::kNabtsDamagedCharacter);
  EXPECT_EQ(fourth[2], ' ' | orc::kNabtsDamagedCharacter);
}

TEST(NabtsNetworkMessagesTest, GroupsOfOtherTypesOrUnattestedAreLeftAlone) {
  NabtsNetworkMessageLog log;
  NabtsDataGroup other = group(0xCFD, header(to_all(0x1, 0xCF0, 0x0)),
                               seven_bit("TEXT OF ANOTHER TYPE"));
  other.header.type = 1;
  log.add_group(other, 1);
  NabtsDataGroup corrected = group(0xCFD, header(to_all(0x1, 0xCF0, 0x0)),
                                   seven_bit("TEXT UNDER A CORRECTED HEADER"));
  corrected.header.attested = false;
  log.add_group(corrected, 2);
  // Data with no provider header at all: text from its first byte.
  log.add_group(group(0xCFD, {}, seven_bit("NO HEADER HERE")), 3);
  EXPECT_EQ(log.groups(), 0U);
  EXPECT_TRUE(log.messages().empty());
}

TEST(NabtsNetworkMessagesTest, AnItemRunsAcrossGroupsThatEachOpenWithSoh) {
  // A service that puts SOH at the head of every group: an item's later
  // groups open with one too, and only the first is the item's own.
  NabtsNetworkMessageLog log;
  const std::vector<std::string> parts = {
      std::string(1, kSoh) + "CALL\r\nRE: LISTINGS\r\nLINE ONE\r\n",
      std::string(1, kSoh) + "LINE TWO\r\n",
      std::string(1, kSoh) + "LINE THREE\r\n" + kEtx + "N G BROAD 1\r\n" + kEot,
      std::string(1, kSoh) + "RE: NEXT ONE\r\nBODY\r\n" + kEtx +
          "N G KXXX 2\r\n" + kEot};
  uint8_t sequence = 0;
  for (const std::string& part : parts) {
    log.add_group(
        group(0xCD0, header(to_all(0x1, 0xCB2, sequence)), seven_bit(part)),
        sequence * 10U);
    ++sequence;
  }
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 2U);
  EXPECT_EQ(messages[0].title, "LISTINGS");
  EXPECT_NE(messages[0].text.find("LINE ONE\nLINE TWO\nLINE THREE"),
            std::string::npos)
      << messages[0].text;
  EXPECT_EQ(messages[0].text.find("<SOH>"), std::string::npos);
  EXPECT_EQ(messages[0].groups, 3U);
  EXPECT_EQ(messages[1].title, "NEXT ONE");
}

TEST(NabtsNetworkMessagesTest, AHeaderALostByteCutShortTakesItsProvenLength) {
  // A header with a long bitmap of NUL nibbles, whose codeword is a control
  // character: where a byte of it was lost, the rest of it must not be read
  // as the message's text.
  std::vector<uint8_t> nibbles = {0x1, 0xC, 0xB, 0x2, 0x0, 0x3, 0x0};
  nibbles.insert(nibbles.end(), 20, 0x0);
  NabtsNetworkMessageLog log;
  for (uint8_t sequence = 0; sequence < 3; ++sequence) {
    nibbles[4] = sequence;
    NabtsDataGroup g =
        group(0xCD0, header(nibbles),
              seven_bit(std::string(1, kSoh) + "RE: ITEM " +
                        std::to_string(sequence) + "\r\nTEXT\r\n" + kEtx +
                        "TRAILER" + kEot));
    if (sequence == 2) {
      g.present[10] = orc::kNabtsByteLost;  // inside the bitmap
    }
    log.add_group(g, sequence);
  }
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 3U);
  EXPECT_EQ(messages[2].title, "ITEM 2");
  EXPECT_EQ(messages[2].text.find("<NAK>"), std::string::npos)
      << messages[2].text;
  // The bitmap is read as far as it arrived, and no further.
  EXPECT_EQ(messages[2].addressing,
            "at least 0 of 84 stations by bitmap (30000" +
                std::string(17, '-') + ")");
  EXPECT_EQ(messages[1].addressing,
            "0 of 84 stations by bitmap (3" + std::string(21, '0') + ")");
}

TEST(NabtsNetworkMessagesTest, CopiesThatDifferInAByteAreOneItem) {
  // A carousel page sent twice, a byte of the second misread.
  std::vector<uint8_t> page;
  for (int i = 0; i < 120; ++i) {
    page.push_back(parity(static_cast<uint8_t>('A' + i % 26)));
  }
  page.insert(page.end(), 8, 0x80);
  std::vector<uint8_t> misread = page;
  misread[50] = parity('#');
  NabtsNetworkMessageLog log;
  log.add_group(group(0x002, header(to_all(0x0, 0xCBA, 0x1)), page), 1);
  log.add_group(group(0x002, header(to_all(0x0, 0xCBA, 0x2)), misread), 2);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].copies, 1U);
}

TEST(NabtsNetworkMessagesTest, APagesTitleIsTextItDrawsNotItsOperands) {
  // After SO the bytes are drawing operands, printable as they are; only
  // what follows SI is text.
  std::vector<uint8_t> code =
      seven_bit(std::string("\x0e") + "hGGFxxhGGD@p" + "\x0f" + "WEATHER MAP");
  code.insert(code.end(), 8, 0x80);
  NabtsNetworkMessageLog log;
  log.add_group(group(0x002, header(to_all(0x0, 0xCBA, 0x1)), code), 1);
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].title, "WEATHER MAP");
}

TEST(NabtsNetworkMessagesTest, AMessageForOneStationNamesItsAddress) {
  // The addressing field's short form: mode E, a nibble, then the station's
  // address in four nibbles. Sent twice, it is one message and a copy.
  const std::vector<uint8_t> nibbles = {0x1, 0xC, 0xF, 0x0, 0x4, 0xE,
                                        0xA, 0x0, 0x0, 0x0, 0xB};
  NabtsNetworkMessageLog log;
  for (uint8_t sequence = 4; sequence < 6; ++sequence) {
    std::vector<uint8_t> head = nibbles;
    head[4] = sequence;
    log.add_group(group(0xCFD, header(head),
                        seven_bit(std::string(1, kSoh) + "RE: FOR ONE\r\n" +
                                  kEtx + kEot)),
                  sequence);
  }
  const std::vector<NabtsNetworkMessage> messages = log.messages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].copies, 1U);
  EXPECT_EQ(messages[0].addressing, "station address 11 (EA000B)");
}

TEST(NabtsNetworkMessagesTest, AMessagesDocumentSaysWhatItIs) {
  NabtsNetworkMessage message;
  message.channel = 0xCFD;
  message.record_address = "CF0";
  message.kind = "message";
  message.form = "ANPA 1312";
  message.title = "BC-FEEDTXT";
  message.groups = 2;
  message.fields = {{"Keyword", "BC-FEEDTXT"}};
  message.text = "LINE ONE\n";
  message.trailer = "END";
  message.complete = true;
  const std::string document = orc::nabts_network_message_document(message);
  EXPECT_EQ(
      document.rfind("BC-FEEDTXT\n\nmessage on CFD, record address CF0", 0), 0U)
      << document;
  EXPECT_NE(document.find("Keyword: BC-FEEDTXT\n"), std::string::npos);
  EXPECT_NE(document.find("\nLINE ONE\n"), std::string::npos);
  EXPECT_NE(document.find("\n-- END\n"), std::string::npos);
  EXPECT_EQ(orc::nabts_network_message_file_stem(message, 12),
            "0012-0-BC-FEEDTXT");
}

}  // namespace
}  // namespace orc_unit_test
