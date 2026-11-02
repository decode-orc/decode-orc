/*
 * File:        nabts_archive_builders.h
 * Module:      orc-tests
 * Purpose:     Builders of the ZIP archives and dBASE tables a framed
 *              datacast service sends, for the tests that read them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_TESTS_NABTS_ARCHIVE_BUILDERS_H
#define ORC_TESTS_NABTS_ARCHIVE_BUILDERS_H

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nabts_decompress.h"

namespace orc_unit_test {

// 1997-03-04 10:20:30 as MS-DOS writes a date and a time.
constexpr uint16_t kArchiveDate = ((1997 - 1980) << 9) | (3 << 5) | 4;
constexpr uint16_t kArchiveTime = (10 << 11) | (20 << 5) | (30 / 2);

inline void put16(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFFU));
  out.push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
}

inline void put32(std::vector<uint8_t>& out, uint32_t value) {
  put16(out, value & 0xFFFFU);
  put16(out, value >> 16U);
}

/// One member of a ZIP archive (PKWARE APPNOTE.TXT 4.3.7): its local file
/// header, then |stream| — |data| stored as it is (|method| 0), or the
/// deflate stream of it (method 8).
inline std::vector<uint8_t> zip_member(
    const std::string& name, const std::vector<uint8_t>& data,
    uint16_t method = 0, const std::vector<uint8_t>& stream = {}) {
  const std::vector<uint8_t>& sent = method == 0 ? data : stream;
  std::vector<uint8_t> out = {'P', 'K', 3, 4};
  put16(out, 20);  // version needed
  put16(out, 0);   // flags
  put16(out, method);
  put16(out, kArchiveTime);
  put16(out, kArchiveDate);
  put32(out, orc::nabts_crc32(data.data(), data.size()));
  put32(out, static_cast<uint32_t>(sent.size()));
  put32(out, static_cast<uint32_t>(data.size()));
  put16(out, static_cast<uint32_t>(name.size()));
  put16(out, 0);  // extra field
  out.insert(out.end(), name.begin(), name.end());
  out.insert(out.end(), sent.begin(), sent.end());
  return out;
}

/// A field of a dBASE table: its name, type and length.
struct TableField {
  std::string name;
  char type = 'C';
  uint8_t length = 0;
};

/// A dBASE III table of |fields| holding |records|, those numbered in
/// |deleted| marked deleted; written 1997-03-04. A character field's value is
/// padded after it, a number's before it.
inline std::vector<uint8_t> dbase_table(
    const std::vector<TableField>& fields,
    const std::vector<std::vector<std::string>>& records,
    const std::set<size_t>& deleted = {}) {
  size_t record_length = 1;
  for (const TableField& field : fields) {
    record_length += field.length;
  }
  std::vector<uint8_t> out = {0x03, 97, 3, 4};
  put32(out, static_cast<uint32_t>(records.size()));
  put16(out, static_cast<uint32_t>(32 + 32 * fields.size() + 1));
  put16(out, static_cast<uint32_t>(record_length));
  out.resize(32, 0);
  for (const TableField& field : fields) {
    std::vector<uint8_t> descriptor(32, 0);
    std::copy(field.name.begin(), field.name.end(), descriptor.begin());
    descriptor[11] = static_cast<uint8_t>(field.type);
    descriptor[16] = field.length;
    out.insert(out.end(), descriptor.begin(), descriptor.end());
  }
  out.push_back(0x0D);
  for (size_t r = 0; r < records.size(); ++r) {
    out.push_back(deleted.count(r) > 0 ? '*' : ' ');
    for (size_t f = 0; f < fields.size(); ++f) {
      std::string value = records[r][f];
      const size_t width = fields[f].length;
      if (fields[f].type == 'N') {
        value = std::string(width - value.size(), ' ') + value;
      } else {
        value.resize(width, ' ');
      }
      out.insert(out.end(), value.begin(), value.end());
    }
  }
  out.push_back(0x1A);
  return out;
}

/// The tables of a service catalogue: a tree of a group and the two
/// channels under it, and the services those channels stand for, on data
/// channels 15 and 1.
inline std::vector<std::pair<std::string, std::vector<uint8_t>>>
catalogue_tables() {
  const std::vector<uint8_t> tree =
      dbase_table({{"ID", 'N', 5},
                   {"PARENTID", 'N', 5},
                   {"NAME", 'C', 30},
                   {"SVCID", 'N', 5},
                   {"INFOFILE", 'C', 20}},
                  {{"1", "0", "Magazines", "0", ""},
                   {"2", "1", "Weekly", "26", ""},
                   {"3", "1", "Monthly", "27", ""},
                   {"4", "0", "Adverts", "5", ""}});
  const std::vector<uint8_t> services =
      dbase_table({{"ID", 'N', 5},
                   {"DATACHAN", 'N', 5},
                   {"ADDRESS", 'C', 30},
                   {"TYPE", 'N', 5},
                   {"DATAPATHID", 'C', 20}},
                  {{"26", "15", "\x11\x05\xE1\x08", "0", "1,"},
                   {"27", "15", "\x91\x07\x01\x0D", "0", "1,"},
                   {"5", "1", "\xF1\x04\x61\x0A", "0", "1,"}});
  const std::vector<uint8_t> broadcasters =
      dbase_table({{"ID", 'N', 5}, {"ACRONYM", 'C', 30}}, {{"1", "XYZ"}});
  return {{"CAT_0002.DBF", tree},
          {"SVC_0002.DBF", services},
          {"BRDC0002.DBF", broadcasters}};
}

/// A self-extracting archive of the catalogue's tables: the program that
/// unpacks it, then the archive.
inline std::vector<uint8_t> catalogue_archive() {
  std::vector<uint8_t> out = {'M', 'Z'};
  out.resize(300, 0x90);
  for (const auto& [name, data] : catalogue_tables()) {
    const std::vector<uint8_t> member = zip_member(name, data);
    out.insert(out.end(), member.begin(), member.end());
  }
  return out;
}

}  // namespace orc_unit_test

#endif  // ORC_TESTS_NABTS_ARCHIVE_BUILDERS_H
