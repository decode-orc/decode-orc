/*
 * File:        nabts_framed_catalogue.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The archives a framed datacast service sends programs and
 *              tables in, the dBASE tables they hold, and the catalogue of
 *              services those tables make up
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_FRAMED_CATALOGUE_H
#define ORC_NABTS_FRAMED_CATALOGUE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// Members of one archive read, and the bytes they may unpack to between them.
constexpr size_t kNabtsArchiveMembersMax = 256;
constexpr size_t kNabtsArchiveBytesMax = size_t{16} << 20;
// Records of one table read.
constexpr size_t kNabtsTableRecordsMax = 65536;

/// One member of a ZIP archive.
struct NabtsArchiveMember {
  std::string name;
  /// When the archive dates it (its MS-DOS date and time), as YYYY-MM-DD
  /// hh:mm:ss in the time of the machine that made it.
  std::string time;
  /// How it is stored: 0 as it is, 8 deflated; any other is listed and not
  /// unpacked.
  uint16_t method = 0;
  /// The CRC-32 and size the archive gives it.
  uint32_t crc = 0;
  uint32_t size = 0;
  /// Its bytes as far as they unpacked.
  std::vector<uint8_t> data;
  /// Its stream reached its end, and its bytes have the CRC-32 the archive
  /// gives.
  bool unpacked = false;
  bool crc_holds = false;
};

/**
 * @brief The members of the ZIP archive |data| holds
 *
 * PKWARE APPNOTE.TXT 4.3.7: each member a local file header (its signature
 * "PK\3\4", the method, MS-DOS time and date, CRC-32, sizes and name), then
 * its bytes. The archive may follow a program — a self-extracting archive is
 * the program that unpacks it, the archive after — so the headers are looked
 * for anywhere in |data|; one is believed where its name is a file name. A
 * member stored (method 0) or deflated (8, nabts_inflate_raw()) is unpacked
 * and checked against its CRC-32 — an archive broken off giving its last
 * member as far as its bytes go, its check failing. Empty where |data| holds
 * no archive.
 *
 * Pure: no I/O. Bounded by kNabtsArchiveMembersMax and kNabtsArchiveBytesMax.
 */
std::vector<NabtsArchiveMember> nabts_archive_members(
    const std::vector<uint8_t>& data);

/// One dBASE table: its fields and records.
struct NabtsTable {
  /// When the table was last written, as its header gives it: YYYY-MM-DD.
  std::string updated;
  /// The fields' names and types (dBASE's letters: C characters, N numbers,
  /// D dates, L logical and the rest).
  std::vector<std::string> fields;
  std::vector<char> types;
  /// The records not marked deleted, a value a field: characters with their
  /// padding dropped, and a field holding bytes that are no text as their
  /// hexadecimal.
  std::vector<std::vector<std::string>> records;
  /// Records marked deleted, and left out.
  size_t deleted = 0;

  /// The column of field |name|, or -1 where the table has none.
  int field(const std::string& name) const;
};

/**
 * @brief The table a dBASE file holds (dBASE III and the FoxPro files after
 *        it)
 *
 * Its header — the version, the date it was last written, how many records,
 * how long the header and each record are — then a descriptor a field (its
 * name, type and length) ended by 0x0D, then the records, each a byte marking
 * it deleted or not and its fields' values as text. Nothing where |data| is
 * no such table: a header whose fields do not add up to its record, or whose
 * records do not fit.
 *
 * Pure: no I/O. Bounded by kNabtsTableRecordsMax.
 */
std::optional<NabtsTable> nabts_table(const std::vector<uint8_t>& data);

/// |table| as text: a line naming its fields, then a line a record, its
/// values separated by " | ".
std::string nabts_table_text(const NabtsTable& table);

/// One entry of a service catalogue's tree.
struct NabtsCatalogueEntry {
  uint32_t id = 0;
  /// The entry it is listed under; 0 for one at the top.
  uint32_t parent = 0;
  std::string name;
  /// The service it stands for, 0 for a group of entries.
  uint32_t service = 0;
};

/// One service a service catalogue lists.
struct NabtsCatalogueService {
  uint32_t id = 0;
  /// The data channel that carries it: the first two hexadecimal digits of
  /// the family of sixteen addresses its frames are sent on.
  uint32_t data_channel = 0;
  uint32_t type = 0;
  /// Its address as the table stores it, in hexadecimal.
  std::string address;
};

/// The catalogue of services a framed datacast sent its receivers.
struct NabtsServiceCatalogue {
  /// When its tables were last written.
  std::string updated;
  std::vector<NabtsCatalogueEntry> entries;
  std::vector<NabtsCatalogueService> services;
  /// The broadcasters its paths name.
  std::vector<std::string> broadcasters;
};

/**
 * @brief The service catalogue the tables of an archive make up, where they
 *        do
 *
 * The framed services measured send their receivers a catalogue of what they
 * carry, as a self-extracting archive of dBASE tables: a tree of channels and
 * the groups they are listed under (a table "CAT_", its fields ID, PARENTID,
 * NAME and SVCID), the services those channels stand for (a table "SVC_": ID,
 * DATACHAN — the data channel that carries the service — ADDRESS and TYPE),
 * and the broadcasters that carry them ("BRDC_": ID, ACRONYM). Nothing where
 * |members| holds no such tables, whole.
 *
 * Pure: no I/O.
 */
std::optional<NabtsServiceCatalogue> nabts_service_catalogue(
    const std::vector<NabtsArchiveMember>& members);

/// |catalogue| as text: its tree, each channel with the service it stands
/// for and the addresses of its data channel, then the services by data
/// channel.
std::string nabts_service_catalogue_text(
    const NabtsServiceCatalogue& catalogue);

/// The names |catalogue| gives the services on |data_channel|, in the order
/// of its tree.
std::vector<std::string> nabts_service_catalogue_names(
    const NabtsServiceCatalogue& catalogue, uint32_t data_channel);

}  // namespace orc

#endif  // ORC_NABTS_FRAMED_CATALOGUE_H
