/*
 * File:        nabts_framed_catalogue.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The archives a framed datacast service sends programs and
 *              tables in, the dBASE tables they hold, and the catalogue of
 *              services those tables make up
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_framed_catalogue.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>

#include "nabts_bytes.h"
#include "nabts_decompress.h"

namespace orc {

namespace {

// PKWARE APPNOTE.TXT 4.3.7: the local file header, its fixed part's length,
// and the flag saying its sizes follow the member's bytes instead.
constexpr uint32_t kLocalHeaderSignature = 0x04034B50;
constexpr size_t kLocalHeaderBytes = 30;
constexpr uint16_t kSizesAfterData = 0x0008;
constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflated = 8;

// dBASE: the header's fixed part, a field descriptor, and the byte that ends
// the descriptors; a record's opening byte when it is deleted.
constexpr size_t kTableHeaderBytes = 32;
constexpr size_t kFieldDescriptorBytes = 32;
constexpr uint8_t kFieldsEnd = 0x0D;
constexpr uint8_t kRecordDeleted = '*';
// The years a table's header counts from.
constexpr int kTableYearBase = 1900;
// The deepest the channel tree is walked: the catalogues sent hold a handful
// of levels, and a table read wrong could hold a chain as long as itself.
constexpr size_t kCatalogueTreeDepthMax = 32;

/// MS-DOS's date and time as YYYY-MM-DD hh:mm:ss: the date's year from 1980
/// in bits 9-15, month 5-8 and day 0-4; the time's hours in bits 11-15,
/// minutes 5-10 and seconds halved 0-4.
std::string dos_time(uint16_t date, uint16_t time) {
  return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", 1980 + (date >> 9U),
                     (date >> 5U) & 0x0FU, date & 0x1FU, time >> 11U,
                     (time >> 5U) & 0x3FU, (time & 0x1FU) * 2U);
}

/// Whether |name| is a member's name: printable, no control character.
bool file_name(const uint8_t* p, size_t length) {
  if (length == 0 || length > 255) {
    return false;
  }
  return std::all_of(p, p + length,
                     [](uint8_t c) { return c >= 0x20 && c < 0x7F; });
}

/// |text| with the spaces padding it either side dropped.
std::string trimmed(const std::string& text) {
  const size_t from = text.find_first_not_of(' ');
  if (from == std::string::npos) {
    return {};
  }
  return text.substr(from, text.find_last_not_of(' ') - from + 1);
}

/// A field's bytes as text: characters as they are, padding dropped; bytes
/// that are no text in hexadecimal.
std::string field_value(const uint8_t* p, size_t length) {
  size_t end = length;
  while (end > 0 && (p[end - 1] == ' ' || p[end - 1] == 0)) {
    --end;
  }
  const bool text =
      std::all_of(p, p + end, [](uint8_t c) { return c >= 0x20 && c < 0x7F; });
  if (text) {
    return trimmed(std::string(p, p + end));
  }
  std::string hex;
  for (size_t i = 0; i < end; ++i) {
    hex += fmt::format("{:02x}", p[i]);
  }
  return hex;
}

/// The member named like |stem| ("CAT_", "SVC_") with a table of its own
/// whose fields include |required|.
std::optional<NabtsTable> table_named(
    const std::vector<NabtsArchiveMember>& members, const std::string& stem,
    const std::vector<std::string>& required) {
  for (const NabtsArchiveMember& member : members) {
    std::string upper = member.name;
    for (char& c : upper) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const size_t slash = upper.find_last_of("/\\");
    const std::string base =
        slash == std::string::npos ? upper : upper.substr(slash + 1);
    if (base.compare(0, stem.size(), stem) != 0 || base.size() < 4 ||
        base.compare(base.size() - 4, 4, ".DBF") != 0 || !member.crc_holds) {
      continue;
    }
    std::optional<NabtsTable> table = nabts_table(member.data);
    if (table && std::all_of(required.begin(), required.end(),
                             [&table](const std::string& field) {
                               return table->field(field) >= 0;
                             })) {
      return table;
    }
  }
  return std::nullopt;
}

uint32_t number(const std::vector<std::string>& record, int column) {
  if (column < 0 || static_cast<size_t>(column) >= record.size()) {
    return 0;
  }
  const std::string& text = record[static_cast<size_t>(column)];
  uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return 0;
    }
    value = value * 10 + static_cast<uint32_t>(c - '0');
  }
  return value;
}

/// The addresses of data channel |channel|: its sixteen, "010-01F".
std::string channel_addresses(uint32_t channel) {
  return fmt::format("{:03X}-{:03X}", (channel << 4U) & 0xFFFU,
                     ((channel << 4U) | 0x0FU) & 0xFFFU);
}

}  // namespace

std::vector<NabtsArchiveMember> nabts_archive_members(
    const std::vector<uint8_t>& data) {
  std::vector<NabtsArchiveMember> members;
  size_t unpacked = 0;
  size_t at = 0;
  while (at + kLocalHeaderBytes <= data.size() &&
         members.size() < kNabtsArchiveMembersMax) {
    if (nabts_le32(&data[at]) != kLocalHeaderSignature) {
      ++at;
      continue;
    }
    const uint8_t* p = &data[at];
    const uint16_t flags = nabts_le16(p + 6);
    const uint16_t name_length = nabts_le16(p + 26);
    const uint16_t extra_length = nabts_le16(p + 28);
    const size_t start = at + kLocalHeaderBytes + name_length + extra_length;
    const uint32_t packed = nabts_le32(p + 18);
    if (start > data.size() || !file_name(p + kLocalHeaderBytes, name_length)) {
      ++at;
      continue;
    }
    NabtsArchiveMember member;
    member.method = nabts_le16(p + 8);
    member.time = dos_time(nabts_le16(p + 12), nabts_le16(p + 10));
    member.crc = nabts_le32(p + 14);
    member.size = nabts_le32(p + 22);
    member.name.assign(p + kLocalHeaderBytes,
                       p + kLocalHeaderBytes + name_length);
    // A member whose sizes follow its bytes is bounded by the archive's end:
    // a deflated stream ends itself, and a stored one cannot be read so — the
    // next header is looked for after its own.
    const bool sizes_after = (flags & kSizesAfterData) != 0;
    // An archive broken off gives its last member as far as it goes.
    const size_t available =
        sizes_after ? data.size() - start
                    : std::min<size_t>(packed, data.size() - start);
    const size_t limit =
        std::min<size_t>(kNabtsArchiveBytesMax - unpacked,
                         sizes_after ? kNabtsArchiveBytesMax : member.size);
    size_t used = sizes_after ? 0 : packed;
    if (member.method == kMethodStored && !sizes_after) {
      const size_t take = std::min<size_t>(available, limit);
      member.data.assign(data.begin() + static_cast<ptrdiff_t>(start),
                         data.begin() + static_cast<ptrdiff_t>(start + take));
      member.unpacked = take == member.size;
    } else if (member.method == kMethodDeflated) {
      NabtsDecompressed out =
          nabts_inflate_raw(data.data() + start, available, limit);
      member.unpacked = out.ended;
      member.data = std::move(out.bytes);
      used = sizes_after ? out.used : packed;
    }
    if (member.unpacked && sizes_after) {
      // The sizes and CRC-32 after the bytes, the descriptor's own signature
      // ahead of them or not.
      const size_t after = start + used;
      const size_t descriptor =
          after + 4 <= data.size() && nabts_le32(&data[after]) == 0x08074B50
              ? after + 4
              : after;
      if (descriptor + 12 <= data.size()) {
        member.crc = nabts_le32(&data[descriptor]);
        member.size = nabts_le32(&data[descriptor + 8]);
        used = descriptor + 12 - start;
      }
    }
    member.crc_holds =
        member.unpacked && member.data.size() == member.size &&
        nabts_crc32(member.data.data(), member.data.size()) == member.crc;
    unpacked += member.data.size();
    members.push_back(std::move(member));
    at = start + std::max<size_t>(used, 1);
  }
  return members;
}

int NabtsTable::field(const std::string& name) const {
  const auto it = std::find(fields.begin(), fields.end(), name);
  return it == fields.end() ? -1 : static_cast<int>(it - fields.begin());
}

std::optional<NabtsTable> nabts_table(const std::vector<uint8_t>& data) {
  if (data.size() < kTableHeaderBytes + 1) {
    return std::nullopt;
  }
  // dBASE III and the FoxPro files after it, with or without a memo file.
  static const std::set<uint8_t> kVersions = {0x03, 0x30, 0x31, 0x32,
                                              0x83, 0x8B, 0xF5};
  if (kVersions.count(data[0]) == 0 || data[2] < 1 || data[2] > 12 ||
      data[3] < 1 || data[3] > 31) {
    return std::nullopt;
  }
  const uint32_t records = nabts_le32(&data[4]);
  const size_t header = nabts_le16(&data[8]);
  const size_t record = nabts_le16(&data[10]);
  NabtsTable table;
  table.updated = fmt::format("{:04}-{:02}-{:02}", kTableYearBase + data[1],
                              data[2], data[3]);
  std::vector<size_t> lengths;
  size_t at = kTableHeaderBytes;
  size_t total = 1;  // the byte that marks a record deleted
  while (at < header && at < data.size() && data[at] != kFieldsEnd) {
    if (at + kFieldDescriptorBytes > data.size()) {
      return std::nullopt;
    }
    const uint8_t* p = &data[at];
    const size_t name_end =
        static_cast<size_t>(std::find(p, p + 11, uint8_t{0}) - p);
    if (name_end == 0 || !file_name(p, name_end)) {
      return std::nullopt;
    }
    table.fields.emplace_back(p, p + name_end);
    table.types.push_back(static_cast<char>(p[11]));
    lengths.push_back(p[16]);
    total += p[16];
    at += kFieldDescriptorBytes;
  }
  if (table.fields.empty() || at >= data.size() || data[at] != kFieldsEnd ||
      total != record || header > data.size() ||
      records > (data.size() - header) / record) {
    return std::nullopt;
  }
  for (uint32_t r = 0;
       r < records && table.records.size() < kNabtsTableRecordsMax; ++r) {
    const uint8_t* p = &data[header + r * record];
    if (p[0] == kRecordDeleted) {
      ++table.deleted;
      continue;
    }
    std::vector<std::string> values;
    size_t offset = 1;
    for (const size_t length : lengths) {
      values.push_back(field_value(p + offset, length));
      offset += length;
    }
    table.records.push_back(std::move(values));
  }
  return table;
}

std::string nabts_table_text(const NabtsTable& table) {
  std::string out;
  for (size_t i = 0; i < table.fields.size(); ++i) {
    out += (i == 0 ? "" : " | ") + table.fields[i];
  }
  out += "\n";
  for (const std::vector<std::string>& record : table.records) {
    for (size_t i = 0; i < record.size(); ++i) {
      out += (i == 0 ? "" : " | ") + record[i];
    }
    out += "\n";
  }
  return out;
}

std::optional<NabtsServiceCatalogue> nabts_service_catalogue(
    const std::vector<NabtsArchiveMember>& members) {
  const std::optional<NabtsTable> tree =
      table_named(members, "CAT", {"ID", "PARENTID", "NAME", "SVCID"});
  const std::optional<NabtsTable> services =
      table_named(members, "SVC", {"ID", "DATACHAN"});
  if (!tree || !services) {
    return std::nullopt;
  }
  NabtsServiceCatalogue catalogue;
  catalogue.updated = std::max(tree->updated, services->updated);
  for (const std::vector<std::string>& record : tree->records) {
    NabtsCatalogueEntry entry;
    entry.id = number(record, tree->field("ID"));
    entry.parent = number(record, tree->field("PARENTID"));
    entry.name = record[static_cast<size_t>(tree->field("NAME"))];
    entry.service = number(record, tree->field("SVCID"));
    catalogue.entries.push_back(std::move(entry));
  }
  for (const std::vector<std::string>& record : services->records) {
    NabtsCatalogueService service;
    service.id = number(record, services->field("ID"));
    service.data_channel = number(record, services->field("DATACHAN"));
    service.type = number(record, services->field("TYPE"));
    if (const int address = services->field("ADDRESS"); address >= 0) {
      service.address = record[static_cast<size_t>(address)];
    }
    catalogue.services.push_back(std::move(service));
  }
  if (const std::optional<NabtsTable> broadcasters =
          table_named(members, "BRDC", {"ID", "ACRONYM"})) {
    for (const std::vector<std::string>& record : broadcasters->records) {
      catalogue.broadcasters.push_back(
          record[static_cast<size_t>(broadcasters->field("ACRONYM"))]);
    }
  }
  return catalogue;
}

std::string nabts_service_catalogue_text(
    const NabtsServiceCatalogue& catalogue) {
  std::map<uint32_t, const NabtsCatalogueService*> by_id;
  for (const NabtsCatalogueService& service : catalogue.services) {
    by_id.try_emplace(service.id, &service);
  }
  std::map<uint32_t, std::vector<const NabtsCatalogueEntry*>> children;
  std::set<uint32_t> ids;
  for (const NabtsCatalogueEntry& entry : catalogue.entries) {
    ids.insert(entry.id);
  }
  for (const NabtsCatalogueEntry& entry : catalogue.entries) {
    // An entry under one the tree lacks is listed at the top.
    children[ids.count(entry.parent) > 0 ? entry.parent : 0].push_back(&entry);
  }
  std::string out = fmt::format(
      "The catalogue of services, its tables last written {}: {} entries, {} "
      "services",
      catalogue.updated, catalogue.entries.size(), catalogue.services.size());
  if (!catalogue.broadcasters.empty()) {
    std::string names;
    for (const std::string& name : catalogue.broadcasters) {
      names += (names.empty() ? "" : ", ") + name;
    }
    out += ", carried by " + names;
  }
  out +=
      ". Each service is sent on a data channel: the first two digits of the "
      "family of sixteen addresses its frames take turns on.\n\n";
  std::set<uint32_t> shown;
  const std::function<void(uint32_t, size_t)> walk = [&](uint32_t parent,
                                                         size_t depth) {
    const auto it = children.find(parent);
    if (it == children.end() || depth > kCatalogueTreeDepthMax) {
      return;
    }
    for (const NabtsCatalogueEntry* entry : it->second) {
      if (!shown.insert(entry->id).second) {
        continue;  // a loop in the tree
      }
      out += std::string(2 * depth, ' ') + entry->name;
      if (entry->service != 0) {
        const auto service = by_id.find(entry->service);
        if (service == by_id.end()) {
          out += fmt::format(": service {}, not in the table of services",
                             entry->service);
        } else {
          out += fmt::format(": service {}, data channel {} ({})",
                             entry->service, service->second->data_channel,
                             channel_addresses(service->second->data_channel));
          if (service->second->type != 0) {
            out += fmt::format(", type {}", service->second->type);
          }
        }
      }
      out += "\n";
      walk(entry->id, depth + 1);
    }
  };
  walk(0, 0);
  std::map<uint32_t, std::vector<std::string>> by_channel;
  for (const NabtsCatalogueService& service : catalogue.services) {
    std::vector<std::string> names =
        nabts_service_catalogue_names(catalogue, service.data_channel);
    by_channel.try_emplace(service.data_channel, std::move(names));
  }
  out += "\nBy data channel:\n";
  for (const auto& [channel, names] : by_channel) {
    std::string list;
    for (const std::string& name : names) {
      list += (list.empty() ? "" : ", ") + name;
    }
    out +=
        fmt::format("  {:3} ({}): {}\n", channel, channel_addresses(channel),
                    list.empty() ? std::string("no channel names it") : list);
  }
  return out;
}

std::vector<std::string> nabts_service_catalogue_names(
    const NabtsServiceCatalogue& catalogue, uint32_t data_channel) {
  std::set<uint32_t> on_channel;
  for (const NabtsCatalogueService& service : catalogue.services) {
    if (service.data_channel == data_channel) {
      on_channel.insert(service.id);
    }
  }
  std::vector<std::string> names;
  for (const NabtsCatalogueEntry& entry : catalogue.entries) {
    if (entry.service != 0 && on_channel.count(entry.service) > 0 &&
        std::find(names.begin(), names.end(), entry.name) == names.end()) {
      names.push_back(entry.name);
    }
  }
  return names;
}

}  // namespace orc
