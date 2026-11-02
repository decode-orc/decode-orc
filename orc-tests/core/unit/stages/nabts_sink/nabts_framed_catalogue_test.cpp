/*
 * File:        nabts_framed_catalogue_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for reading the archives a framed datacast service
 *              sends, the dBASE tables in them, and the catalogue of services
 *              those tables make up
 *
 * Covers: a ZIP archive's members stored and deflated, dated and checked by
 * their CRC-32; an archive found after the program of a self-extracting one;
 * a member whose CRC-32 fails, and one broken off; bytes holding no archive;
 * a dBASE table's fields and records, a record marked deleted left out, a
 * field of bytes that are no text read as hexadecimal, and bytes that are no
 * table; and the catalogue of services the tables make up — its tree, its
 * services by data channel, the broadcasters, and the names it gives the
 * services of a data channel — and no catalogue without its tables. The
 * archives and tables are built here. Deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_framed_catalogue.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nabts_archive_builders.h"

namespace orc_unit_test {
namespace {

std::vector<uint8_t> bytes_of(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

// "MEMBER ONE: the catalogue of services, the catalogue of services",
// deflated with no zlib wrapping.
const std::string kDeflatedText =
    "MEMBER ONE: the catalogue of services, the catalogue of services";
const std::vector<uint8_t> kDeflated = {
    0xF3, 0x75, 0xF5, 0x75, 0x72, 0x0D, 0x52, 0xF0, 0xF7, 0x73, 0xB5,
    0x52, 0x28, 0xC9, 0x48, 0x55, 0x48, 0x4E, 0x2C, 0x49, 0xCC, 0xC9,
    0x4F, 0x2F, 0x4D, 0x55, 0xC8, 0x4F, 0x53, 0x28, 0x4E, 0x2D, 0x2A,
    0xCB, 0x4C, 0x4E, 0x2D, 0xD6, 0xC1, 0x2D, 0x05, 0x00};

TEST(NabtsFramedCatalogue, ReadsTheMembersOfAnArchive) {
  std::vector<uint8_t> archive = zip_member("NOTES.TXT", bytes_of("stored"));
  const std::vector<uint8_t> second =
      zip_member("BOOK.TXT", bytes_of(kDeflatedText), 8, kDeflated);
  archive.insert(archive.end(), second.begin(), second.end());
  const auto members = orc::nabts_archive_members(archive);
  ASSERT_EQ(members.size(), 2u);
  EXPECT_EQ(members[0].name, "NOTES.TXT");
  EXPECT_EQ(members[0].time, "1997-03-04 10:20:30");
  EXPECT_EQ(members[0].method, 0);
  EXPECT_EQ(members[0].data, bytes_of("stored"));
  EXPECT_TRUE(members[0].crc_holds);
  EXPECT_EQ(members[1].name, "BOOK.TXT");
  EXPECT_EQ(members[1].method, 8);
  EXPECT_EQ(members[1].data, bytes_of(kDeflatedText));
  EXPECT_TRUE(members[1].unpacked);
  EXPECT_TRUE(members[1].crc_holds);
}

TEST(NabtsFramedCatalogue, FindsTheArchiveAfterTheProgramThatUnpacksIt) {
  std::vector<uint8_t> self_extracting = {'M', 'Z'};
  self_extracting.resize(500, 0x4E);
  const std::vector<uint8_t> member =
      zip_member("BOOK.TXT", bytes_of(kDeflatedText), 8, kDeflated);
  self_extracting.insert(self_extracting.end(), member.begin(), member.end());
  const auto members = orc::nabts_archive_members(self_extracting);
  ASSERT_EQ(members.size(), 1u);
  EXPECT_EQ(members[0].data, bytes_of(kDeflatedText));
  EXPECT_TRUE(members[0].crc_holds);
}

TEST(NabtsFramedCatalogue, SaysWhenAMembersCrcFailsOrItBrokeOff) {
  std::vector<uint8_t> stored = zip_member("A.TXT", bytes_of("stored bytes"));
  stored[stored.size() - 3] ^= 0x20;  // a byte received wrong
  auto members = orc::nabts_archive_members(stored);
  ASSERT_EQ(members.size(), 1u);
  EXPECT_TRUE(members[0].unpacked);
  EXPECT_FALSE(members[0].crc_holds);

  std::vector<uint8_t> deflated =
      zip_member("BOOK.TXT", bytes_of(kDeflatedText), 8, kDeflated);
  deflated.resize(deflated.size() - 20);  // broken off
  members = orc::nabts_archive_members(deflated);
  ASSERT_EQ(members.size(), 1u);
  EXPECT_FALSE(members[0].crc_holds);
}

TEST(NabtsFramedCatalogue, FindsNoArchiveInBytesThatHoldNone) {
  EXPECT_TRUE(orc::nabts_archive_members(bytes_of("PK no archive here, just "
                                                  "text that mentions PK"))
                  .empty());
}

TEST(NabtsFramedCatalogue, ReadsADbaseTable) {
  const std::vector<uint8_t> data =
      dbase_table({{"ID", 'N', 4}, {"NAME", 'C', 10}, {"CODE", 'C', 6}},
                  {{"7", "Weekly", "\x11\x05\xE1"},
                   {"12", "Monthly", "ab"},
                   {"3", "x", ""}},
                  {2});
  const auto table = orc::nabts_table(data);
  ASSERT_TRUE(table.has_value());
  EXPECT_EQ(table->updated, "1997-03-04");
  EXPECT_EQ(table->fields, (std::vector<std::string>{"ID", "NAME", "CODE"}));
  EXPECT_EQ(table->types, (std::vector<char>{'N', 'C', 'C'}));
  EXPECT_EQ(table->field("NAME"), 1);
  EXPECT_EQ(table->field("NONE"), -1);
  ASSERT_EQ(table->records.size(), 2u);
  EXPECT_EQ(table->deleted, 1u);
  EXPECT_EQ(table->records[0],
            (std::vector<std::string>{"7", "Weekly", "1105e1"}));
  EXPECT_EQ(table->records[1],
            (std::vector<std::string>{"12", "Monthly", "ab"}));
  EXPECT_EQ(orc::nabts_table_text(*table),
            "ID | NAME | CODE\n7 | Weekly | 1105e1\n12 | Monthly | ab\n");
}

TEST(NabtsFramedCatalogue, ReadsNoTableFromBytesThatAreNone) {
  std::vector<uint8_t> data =
      dbase_table({{"ID", 'N', 4}, {"NAME", 'C', 10}}, {{"7", "Weekly"}});
  data[10] = 99;  // a record length its fields do not add up to
  EXPECT_FALSE(orc::nabts_table(data).has_value());
  EXPECT_FALSE(orc::nabts_table(bytes_of("not a table at all, only text "
                                         "long enough to hold a header"))
                   .has_value());
}

TEST(NabtsFramedCatalogue, ReadsTheCatalogueOfServicesItsTablesMakeUp) {
  const auto members = orc::nabts_archive_members(catalogue_archive());
  ASSERT_EQ(members.size(), 3u);
  const auto catalogue = orc::nabts_service_catalogue(members);
  ASSERT_TRUE(catalogue.has_value());
  EXPECT_EQ(catalogue->updated, "1997-03-04");
  ASSERT_EQ(catalogue->entries.size(), 4u);
  EXPECT_EQ(catalogue->entries[1].name, "Weekly");
  EXPECT_EQ(catalogue->entries[1].parent, 1u);
  EXPECT_EQ(catalogue->entries[1].service, 26u);
  ASSERT_EQ(catalogue->services.size(), 3u);
  EXPECT_EQ(catalogue->services[0].data_channel, 15u);
  EXPECT_EQ(catalogue->services[0].address, "1105e108");
  EXPECT_EQ(catalogue->broadcasters, (std::vector<std::string>{"XYZ"}));
  EXPECT_EQ(orc::nabts_service_catalogue_names(*catalogue, 15),
            (std::vector<std::string>{"Weekly", "Monthly"}));
  EXPECT_EQ(orc::nabts_service_catalogue_names(*catalogue, 1),
            (std::vector<std::string>{"Adverts"}));
  EXPECT_TRUE(orc::nabts_service_catalogue_names(*catalogue, 7).empty());
  const std::string text = orc::nabts_service_catalogue_text(*catalogue);
  EXPECT_NE(text.find("Magazines\n  Weekly: service 26, data channel 15 "
                      "(0F0-0FF)\n  Monthly: service 27"),
            std::string::npos);
  EXPECT_NE(text.find("Adverts: service 5, data channel 1 (010-01F)"),
            std::string::npos);
  EXPECT_NE(text.find("carried by XYZ"), std::string::npos);
  EXPECT_NE(text.find(" 15 (0F0-0FF): Weekly, Monthly"), std::string::npos);
}

TEST(NabtsFramedCatalogue, FindsNoCatalogueWithoutItsTables) {
  const auto tables = catalogue_tables();
  // The tree alone, with no table of services.
  const auto members =
      orc::nabts_archive_members(zip_member(tables[0].first, tables[0].second));
  ASSERT_EQ(members.size(), 1u);
  EXPECT_FALSE(orc::nabts_service_catalogue(members).has_value());
}

}  // namespace
}  // namespace orc_unit_test
