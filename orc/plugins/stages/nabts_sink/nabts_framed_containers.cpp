/*
 * File:        nabts_framed_containers.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The containers a framed datacast service sends its pages,
 *              pictures and other files in, unpacked
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_framed_containers.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>

#include "nabts_bytes.h"
#include "nabts_data_group.h"
#include "nabts_datacast_files.h"
#include "nabts_decompress.h"

namespace orc {

namespace {

constexpr char kFileMagic[] = "WTF1";
constexpr char kArchiveMagic[] = "WTZY";
constexpr char kShortArchiveMagic[] = "WTZX";
constexpr size_t kMagicBytes = 4;
// An archive's magic and the field of its path's length.
constexpr size_t kArchiveHead = kMagicBytes + 3;
// The folder the service's client cached its files in, which an archive's
// path opens with.
constexpr char kCacheFolder[] = "cache\\";

bool opens_with(const std::vector<uint8_t>& data, const char* magic) {
  return data.size() >= kMagicBytes &&
         std::memcmp(data.data(), magic, kMagicBytes) == 0;
}

bool is_digit(uint8_t byte) { return byte >= '0' && byte <= '9'; }

/// |length| bytes of |data| at |at| as text, if all of them arrived and are
/// printable.
std::optional<std::string> printable(const NabtsCarriedFile& file, size_t at,
                                     size_t length) {
  if (at + length > file.data.size()) {
    return std::nullopt;
  }
  std::string text;
  for (size_t i = at; i < at + length; ++i) {
    const uint8_t byte = file.data[i];
    if (byte < 0x20 || byte >= 0x7F ||
        (!file.data_present.empty() && file.data_present[i] == 0)) {
      return std::nullopt;
    }
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

/// The last part of an address or path: after its last '/' or '\'.
std::string last_part(const std::string& address) {
  const size_t slash = address.find_last_of("/\\");
  std::string part =
      slash == std::string::npos ? address : address.substr(slash + 1);
  return part.empty() ? "index" : part;
}

/// Where |file|'s bytes from |at| on stop being whole: its first byte no copy
/// delivered, or its end.
size_t whole_to(const NabtsCarriedFile& file, size_t at) {
  if (file.data_present.empty()) {
    return file.data.size();
  }
  const auto lost =
      std::find(file.data_present.begin() + static_cast<ptrdiff_t>(at),
                file.data_present.end(), uint8_t{0});
  return static_cast<size_t>(lost - file.data_present.begin());
}

/// The length of an archive's path: one or two decimal digits, ended by a
/// zero.
std::optional<size_t> archive_path_length(const std::vector<uint8_t>& data) {
  if (data.size() <= kArchiveHead || !is_digit(data[4])) {
    return std::nullopt;
  }
  const auto digit = [&data](size_t at) {
    return static_cast<size_t>(data[at] - '0');
  };
  if (is_digit(data[5]) && data[6] == 0) {
    return digit(4) * 10 + digit(5);
  }
  if (data[5] == 0) {
    return digit(4);
  }
  return std::nullopt;
}

/// An archive's path of |length| bytes, read where at most
/// kNabtsArchivePathUnreadable of its bytes did not arrive or are not
/// printable, each of those written '_' and |misread| set. An archive that
/// caches under the client's cache folder (|cached|) has the folder's name put
/// right where one of its bytes was received wrong.
std::optional<std::string> archive_path(const NabtsCarriedFile& file,
                                        size_t length, bool cached,
                                        bool& misread) {
  if (kArchiveHead + length > file.data.size()) {
    return std::nullopt;
  }
  std::string path;
  size_t unreadable = 0;
  for (size_t i = kArchiveHead; i < kArchiveHead + length; ++i) {
    const uint8_t byte = file.data[i];
    const bool arrived = file.data_present.empty() || file.data_present[i] != 0;
    if (!arrived || byte < 0x20 || byte >= 0x7F) {
      ++unreadable;
      path.push_back('_');
    } else {
      path.push_back(static_cast<char>(byte));
    }
  }
  const std::string folder(kCacheFolder);
  if (cached && length > folder.size()) {
    size_t wrong = 0;
    size_t wrong_unreadable = 0;
    for (size_t i = 0; i < folder.size(); ++i) {
      if (path[i] != folder[i]) {
        ++wrong;
        const size_t at = kArchiveHead + i;
        const bool arrived =
            file.data_present.empty() || file.data_present[at] != 0;
        wrong_unreadable +=
            !arrived || file.data[at] < 0x20 || file.data[at] >= 0x7F ? 1 : 0;
      }
    }
    if (wrong == 1) {
      path.replace(0, folder.size(), folder);
      unreadable -= wrong_unreadable;
    }
  }
  if (unreadable > kNabtsArchivePathUnreadable) {
    return std::nullopt;
  }
  misread = unreadable > 0;
  return path;
}

/// |file| as the body of the web server's response it holds, if it holds
/// one whose head ends within kNabtsResponseHeadMax bytes: the head kept
/// aside, and the file named and placed by its "CachedURL" header.
void take_response_head(NabtsCarriedFile& file) {
  const std::vector<uint8_t>& data = file.data;
  static constexpr char kStatus[] = "HTTP/";
  if (data.size() < sizeof(kStatus) ||
      std::memcmp(data.data(), kStatus, sizeof(kStatus) - 1) != 0) {
    return;
  }
  const std::string head(
      data.begin(), data.begin() + static_cast<ptrdiff_t>(std::min(
                                       data.size(), kNabtsResponseHeadMax)));
  size_t end = head.find("\r\n\r\n");
  end = end == std::string::npos ? head.find("\n\n") : end + 2;
  if (end == std::string::npos) {
    return;
  }
  end += 2;
  file.response_head = head.substr(0, end);
  file.data.erase(file.data.begin(),
                  file.data.begin() + static_cast<ptrdiff_t>(end));
  file.format = nabts_file_format(file.data, file.data_present);
  // The address the response was fetched from, as the header names it.
  static constexpr char kCachedUrl[] = "cachedurl:";
  size_t line = 0;
  while (line < end) {
    const size_t next = file.response_head.find('\n', line);
    std::string text = file.response_head.substr(
        line, (next == std::string::npos ? end : next) - line);
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
      text.pop_back();
    }
    std::string name = text.substr(0, sizeof(kCachedUrl) - 1);
    std::transform(name.begin(), name.end(), name.begin(), [](char c) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    if (name == kCachedUrl) {
      const size_t value = text.find_first_not_of(' ', sizeof(kCachedUrl) - 1);
      if (value != std::string::npos) {
        file.source = text.substr(value);
        file.name = last_part(file.source);
        file.source_misread = false;
      }
      break;
    }
    if (next == std::string::npos) {
      break;
    }
    line = next + 1;
  }
}

/// |container|'s file from what its stream unpacked to, |source| where it
/// came from and |name| its name.
NabtsCarriedFile content_of(const NabtsCarriedFile& container,
                            NabtsDecompressed unpacked, std::string source,
                            std::string name) {
  NabtsCarriedFile file;
  file.channel = container.channel;
  file.last_channel = container.last_channel;
  file.transfer = container.transfer;
  file.time = container.time;
  file.copies = container.copies;
  file.name = std::move(name);
  file.source = std::move(source);
  file.data = std::move(unpacked.bytes);
  file.cut_short = !unpacked.ended;
  file.format = nabts_file_format(file.data, file.data_present);
  return file;
}

}  // namespace

bool nabts_framed_container(const NabtsCarriedFile& file) {
  return opens_with(file.data, kFileMagic) ||
         ((opens_with(file.data, kArchiveMagic) ||
           opens_with(file.data, kShortArchiveMagic)) &&
          archive_path_length(file.data).has_value());
}

std::optional<NabtsCarriedFile> nabts_framed_container_content(
    const NabtsCarriedFile& file) {
  if (!nabts_framed_container(file)) {
    return std::nullopt;
  }
  const std::vector<uint8_t>& data = file.data;
  if (opens_with(data, kFileMagic)) {
    if (data.size() < kNabtsFileContainerHead) {
      return std::nullopt;
    }
    const size_t address_length = data[kNabtsFileContainerHead - 1];
    const std::optional<std::string> address =
        printable(file, kNabtsFileContainerHead, address_length);
    const size_t at =
        kNabtsFileContainerHead + address_length + kNabtsFileContainerTail;
    if (!address || at >= data.size()) {
      return std::nullopt;
    }
    // The stream as far as its bytes are whole: a hole's zeros are no
    // stream's.
    NabtsDecompressed unpacked = nabts_inflate(
        data.data() + at, whole_to(file, at) - at, kNabtsCarriedBytesMax);
    if (unpacked.bytes.empty()) {
      return std::nullopt;
    }
    const bool ended = unpacked.ended;
    const bool complete = unpacked.complete;
    NabtsCarriedFile content =
        content_of(file, std::move(unpacked), *address, last_part(*address));
    if (ended) {
      content.checksum_holds = complete;  // the stream's own Adler-32
    }
    take_response_head(content);
    return content;
  }

  const bool short_record = opens_with(data, kShortArchiveMagic);
  const size_t path_length = archive_path_length(data).value_or(0);
  const size_t record = kArchiveHead + path_length;
  const size_t at = record + (short_record ? kNabtsShortArchiveRecordBytes
                                           : kNabtsArchiveRecordBytes);
  bool misread = false;
  const std::optional<std::string> path =
      archive_path(file, path_length, !short_record, misread);
  if (!path || at >= data.size()) {
    return std::nullopt;
  }
  // The file's name, ended by a zero within its field.
  const size_t name_at =
      record + (short_record ? kNabtsShortArchiveNameAt : kNabtsArchiveNameAt);
  const size_t name_bytes =
      short_record ? kNabtsShortArchiveNameBytes : kNabtsArchiveNameBytes;
  size_t name_length = 0;
  while (name_length < name_bytes && data[name_at + name_length] != 0) {
    ++name_length;
  }
  const std::optional<std::string> name = printable(file, name_at, name_length);
  if (!name || name->empty()) {
    return std::nullopt;
  }
  const size_t packed = std::min<size_t>(nabts_le32(data.data() + record),
                                         whole_to(file, at) - at);
  NabtsDecompressed unpacked =
      nabts_explode(data.data() + at, packed, kNabtsCarriedBytesMax);
  if (unpacked.bytes.empty()) {
    return std::nullopt;
  }
  const bool complete = unpacked.complete;
  const std::string source =
      *path + (path->empty() || path->back() == '\\' ? "" : "\\") + *name;
  NabtsCarriedFile content =
      content_of(file, std::move(unpacked), source, *name);
  content.source_misread = misread;
  if (complete) {
    content.checksum = nabts_le32(data.data() + record + 4);
    content.checksum_holds =
        nabts_crc32(content.data.data(), content.data.size()) ==
        *content.checksum;
  }
  take_response_head(content);
  return content;
}

std::optional<NabtsCarriedFile> nabts_framed_container_reopened(
    const NabtsCarriedFile& file) {
  // The bytes lost from its opening on.
  if (file.data_present.empty() || file.data_present[0] != 0) {
    return std::nullopt;
  }
  const auto first_arrived =
      std::find_if(file.data_present.begin(), file.data_present.end(),
                   [](uint8_t present) { return present != 0; });
  const auto lost =
      static_cast<size_t>(first_arrived - file.data_present.begin());
  if (lost > kNabtsContainerOpeningLost || lost == file.data.size()) {
    return std::nullopt;
  }
  std::optional<NabtsCarriedFile> found;
  for (const char* magic : {kArchiveMagic, kShortArchiveMagic}) {
    const bool cached = std::strcmp(magic, kArchiveMagic) == 0;
    for (size_t length = 10; length <= 99; ++length) {
      // The opening: the magic, the path's length in two digits ended by a
      // zero, and — an archive's path being in the cache folder — the
      // folder's name.
      std::string opening(magic);
      opening += std::to_string(length);
      opening.push_back('\0');
      if (cached) {
        opening += kCacheFolder;
      }
      if (lost > opening.size() || opening.size() > file.data.size()) {
        continue;
      }
      // What arrived of the opening has to be the opening's.
      bool agrees = true;
      for (size_t i = lost; i < opening.size() && agrees; ++i) {
        agrees = file.data_present[i] == 0 ||
                 file.data[i] == static_cast<uint8_t>(opening[i]);
      }
      if (!agrees) {
        continue;
      }
      NabtsCarriedFile trial = file;
      for (size_t i = 0; i < lost; ++i) {
        trial.data[i] = static_cast<uint8_t>(opening[i]);
        trial.data_present[i] = kNabtsByteArrived;
      }
      if (std::find(trial.data_present.begin(), trial.data_present.end(),
                    uint8_t{0}) == trial.data_present.end()) {
        trial.data_present.clear();
      }
      std::optional<NabtsCarriedFile> content =
          nabts_framed_container_content(trial);
      if (!content || content->cut_short ||
          !content->checksum_holds.value_or(false)) {
        continue;
      }
      // Two openings that give the same file are one answer.
      if (found &&
          (found->data != content->data || found->source != content->source)) {
        return std::nullopt;
      }
      if (!found) {
        found = std::move(content);
      }
    }
  }
  return found;
}

}  // namespace orc
