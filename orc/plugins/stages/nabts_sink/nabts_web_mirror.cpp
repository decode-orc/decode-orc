/*
 * File:        nabts_web_mirror.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The pages and files framed datacast services sent, laid out as
 *              the web sites they came from
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_web_mirror.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <utility>

namespace orc {

namespace {

// Where a page's bytes as received are kept, beside the copy whose links point
// into the mirror.
constexpr const char* kReceivedFolder = "_received";

// The attributes whose values are addresses a browser fetches or follows.
constexpr std::array<const char*, 6> kLinkAttributes = {
    "href", "src", "background", "lowsrc", "action", "codebase"};
// The attribute site-copying tools of the time kept a copy's original address
// in, which no browser follows: left as it is, a record of where it came from.
constexpr const char* kRecordAttribute = "tppabs";
// The page a folder's address is served as.
constexpr const char* kFolderPage = "index.html";

// Schemes a link leaves the mirror's reach with: scripts, mail, the
// browser's own.
constexpr std::array<const char*, 9> kForeignSchemes = {
    "javascript:", "vbscript:", "mailto:", "news:", "data:",
    "about:",      "file:",     "res:",    "ftp:"};

std::string lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

/// |part| as a file system takes it: a character none would, '_'; trailing
/// dots and spaces, which one drops, gone; a device name one reserves,
/// prefixed.
std::string part_of(const std::string& part) {
  std::string out;
  for (const char c : part) {
    const auto byte = static_cast<unsigned char>(c);
    const bool kept =
        std::isalnum(byte) != 0 ||
        std::string("._~!$&'()+,;=@-").find(c) != std::string::npos;
    out.push_back(kept ? c : '_');
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
    out.pop_back();
  }
  if (out.empty()) {
    return "_";
  }
  static const std::set<std::string> kReserved = {
      "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
      "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
      "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  if (kReserved.count(lower(out.substr(0, out.find('.')))) > 0) {
    out.insert(out.begin(), '_');
  }
  return out;
}

/// The parts of |path|: '\' read as '/', empty parts and "." dropped, ".."
/// taking the part before it away.
std::vector<std::string> parts_of(const std::string& path) {
  std::vector<std::string> parts;
  std::string part;
  const auto close = [&]() {
    if (part == "..") {
      if (!parts.empty()) {
        parts.pop_back();
      }
    } else if (!part.empty() && part != ".") {
      parts.push_back(part);
    }
    part.clear();
  };
  for (const char c : path) {
    if (c == '/' || c == '\\') {
      close();
    } else {
      part.push_back(c);
    }
  }
  close();
  return parts;
}

std::string joined(const std::vector<std::string>& parts, size_t from = 0) {
  std::string out;
  for (size_t i = from; i < parts.size(); ++i) {
    out += (out.empty() ? "" : "/") + parts[i];
  }
  return out;
}

/// The scheme's length, "://" with it, where |text| opens with one: letters
/// — read as whatever a misread made of them — then "://".
size_t scheme_length(const std::string& text) {
  const size_t at = text.find("://");
  if (at == std::string::npos || at == 0 || at > 8) {
    return 0;
  }
  for (size_t i = 0; i < at; ++i) {
    if (std::isalpha(static_cast<unsigned char>(text[i])) == 0) {
      return 0;
    }
  }
  return at + 3;
}

/// host/path of |rest| — an address with its scheme gone — as the mirror's
/// parts: the host in lower case, "index.html" for a folder, each part as a
/// file system takes it. Nothing for an address with no host.
std::optional<std::vector<std::string>> mirror_parts(std::string rest) {
  for (char& c : rest) {
    c = c == '\\' ? '/' : c;
  }
  const bool folder = !rest.empty() && rest.back() == '/';
  // The host is the first part, and the path's ".." stops at it.
  const size_t slash = rest.find('/');
  const std::string host = lower(rest.substr(0, slash));
  if (host.empty() || host == "." || host == "..") {
    return std::nullopt;
  }
  std::vector<std::string> parts{host};
  if (slash != std::string::npos) {
    const std::vector<std::string> path = parts_of(rest.substr(slash + 1));
    parts.insert(parts.end(), path.begin(), path.end());
  }
  if (folder || parts.size() == 1) {
    parts.emplace_back("index.html");
  }
  for (std::string& part : parts) {
    part = part_of(part);
  }
  return parts;
}

/// |address| with its query and fragment cut off.
std::string without_query(const std::string& address) {
  return address.substr(0, address.find_first_of("?#"));
}

/// The parts of a source address in the mirror: a URL's scheme, query and
/// fragment, or the "cache" folder the client kept its pages in, dropped — a
/// path's '?' and '#' being characters of it.
std::optional<std::vector<std::string>> source_parts(
    const std::string& source) {
  std::string rest = source;
  for (char& c : rest) {
    c = c == '\\' ? '/' : c;
  }
  if (const size_t scheme = scheme_length(rest); scheme > 0) {
    rest = without_query(rest.substr(scheme));
  } else if (lower(rest.substr(0, 6)) == "cache/") {
    rest = rest.substr(6);
  } else if (starts_with(rest, "//")) {
    rest = without_query(rest.substr(2));
  }
  return mirror_parts(rest);
}

std::string addresses_of(const NabtsCarriedFile& file) {
  return file.last_channel > file.channel
             ? fmt::format("{:03X}-{:03X}", file.channel, file.last_channel)
             : fmt::format("{:03X}", file.channel);
}

/// |to| relative to the folder that holds |from|, both mirror paths.
std::string relative_path(const std::string& from, const std::string& to) {
  std::vector<std::string> folder = parts_of(from);
  if (!folder.empty()) {
    folder.pop_back();
  }
  const std::vector<std::string> target = parts_of(to);
  size_t common = 0;
  while (common < folder.size() && common + 1 < target.size() &&
         folder[common] == target[common]) {
    ++common;
  }
  std::string out;
  for (size_t i = common; i < folder.size(); ++i) {
    out += "../";
  }
  return out + joined(target, common);
}

std::string escape_html(const std::string& text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

/// |path| with "~|n|" before its extension, the extension being what follows
/// the last part's last dot.
std::string version_path(const std::string& path, size_t n) {
  const size_t slash = path.rfind('/');
  const size_t dot = path.rfind('.');
  const std::string suffix = "~" + std::to_string(n);
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash) ||
      dot == slash + 1) {
    return path + suffix;
  }
  return path.substr(0, dot) + suffix + path.substr(dot);
}

/// Whether a file's bytes should have their links pointed at the mirror: a
/// page, or a style sheet.
bool links_are_rewritten(const NabtsCarriedFile& file,
                         const std::string& path) {
  const std::string name = lower(path.substr(path.rfind('/') + 1));
  const size_t dot = name.rfind('.');
  const std::string extension =
      dot == std::string::npos ? "" : name.substr(dot + 1);
  return file.format == "html" || extension == "htm" || extension == "html" ||
         extension == "shtml" || extension == "asp" || extension == "css";
}

bool whole(const NabtsCarriedFile& file) {
  return file.data_present.empty() && !file.cut_short;
}

bool holds(const NabtsCarriedFile& file, size_t i) {
  return file.data_present.empty() || file.data_present[i] != 0;
}

size_t bytes_held(const NabtsCarriedFile& file) {
  return file.data.size() - file.bytes_lost();
}

/// Whether damaged copy |damaged| is a copy of |other|: they agree, on every
/// byte both hold, over at least the first part of |damaged|
/// (kNabtsMirrorAgreeingQuarters).
bool agrees(const NabtsCarriedFile& damaged, const NabtsCarriedFile& other) {
  if (damaged.data.empty()) {
    return false;
  }
  const size_t common = std::min(damaged.data.size(), other.data.size());
  size_t agreeing = common;
  for (size_t i = 0; i < common; ++i) {
    if (holds(damaged, i) && holds(other, i) &&
        damaged.data[i] != other.data[i]) {
      agreeing = i;
      break;
    }
  }
  return agreeing * 4 >= damaged.data.size() * kNabtsMirrorAgreeingQuarters &&
         agreeing > 0;
}

bool same_bytes(const NabtsCarriedFile& a, const NabtsCarriedFile& b) {
  return a.data == b.data && a.data_present == b.data_present &&
         a.cut_short == b.cut_short;
}

/// Whether |file| arrived sound: whole, its check holding where it has one,
/// and its address read.
bool sound(const NabtsCarriedFile& file) {
  return whole(file) && file.checksum_holds.value_or(true) &&
         !file.source_misread;
}

/// The bytes two strings as long as each other differ in, ignoring case.
size_t differing(const std::string& a, const std::string& b) {
  size_t count = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    count += std::tolower(static_cast<unsigned char>(a[i])) !=
                     std::tolower(static_cast<unsigned char>(b[i]))
                 ? 1
                 : 0;
  }
  return count;
}

/// |places|, each file's place in the mirror, with the folders a misread put
/// files in put right (nabts_web_mirror()); returns how many files moved.
size_t place_misreads(const std::vector<NabtsCarriedFile>& files,
                      std::vector<std::optional<std::string>>& places) {
  const std::string unaddressed = std::string(kNabtsMirrorUnaddressed) + "/";
  // The folders sound files are in or under, by their length.
  std::set<std::string> vouched;
  std::map<size_t, std::vector<std::string>> vouched_by_length;
  for (size_t i = 0; i < files.size(); ++i) {
    const std::optional<std::string>& place = places[i];
    if (!place || starts_with(*place, unaddressed) || !sound(files[i])) {
      continue;
    }
    const std::vector<std::string> parts = parts_of(*place);
    std::string folder;
    for (size_t p = 0; p + 1 < parts.size(); ++p) {
      folder += (p == 0 ? "" : "/") + parts[p];
      if (vouched.insert(lower(folder)).second) {
        vouched_by_length[folder.size()].push_back(folder);
      }
    }
  }
  // Whether |place|'s folder is one a sound file vouches for.
  const auto vouched_for = [&vouched](const std::string& place) {
    return vouched.count(lower(place.substr(0, place.rfind('/')))) > 0;
  };
  // The address a file was sent under, as compared: '\' read as '/', in
  // lower case.
  const auto sent_as = [](const NabtsCarriedFile& file) {
    std::string text = lower(file.source);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
  };
  size_t moved = 0;
  // Sound files by their names.
  std::map<std::string, std::vector<size_t>> sound_by_name;
  for (size_t i = 0; i < files.size(); ++i) {
    const std::optional<std::string>& place = places[i];
    if (place && !starts_with(*place, unaddressed) && sound(files[i])) {
      sound_by_name[lower(place->substr(place->rfind('/') + 1))].push_back(i);
    }
  }
  // A file named as a sound file is, whose address as sent differs from that
  // file's in few enough bytes, is a misread copy of it.
  std::vector<bool> placed(files.size(), false);
  for (size_t i = 0; i < files.size(); ++i) {
    const std::optional<std::string>& place = places[i];
    if (!place || starts_with(*place, unaddressed) || sound(files[i]) ||
        files[i].source.empty() || vouched_for(*place)) {
      continue;
    }
    const auto named =
        sound_by_name.find(lower(place->substr(place->rfind('/') + 1)));
    if (named == sound_by_name.end()) {
      continue;
    }
    const std::string sent = sent_as(files[i]);
    const size_t allowed = std::max(kNabtsMirrorMisreadBytes + 1,
                                    sent.size() / kNabtsMirrorMisreadShare);
    std::optional<std::string> best;
    size_t fewest = allowed + 1;
    bool tied = false;
    for (const size_t k : named->second) {
      const std::string other = sent_as(files[k]);
      if (other.size() != sent.size()) {
        continue;
      }
      const size_t count = differing(sent, other);
      if (count < fewest) {
        best = places[k];
        fewest = count;
        tied = false;
      } else if (count == fewest && best != places[k]) {
        tied = true;
      }
    }
    if (best && !tied) {
      places[i] = *best;
      placed[i] = true;
      ++moved;
    }
  }
  for (size_t i = 0; i < files.size(); ++i) {
    const std::optional<std::string>& place = places[i];
    if (!place || placed[i] || starts_with(*place, unaddressed) ||
        sound(files[i])) {
      continue;
    }
    const std::vector<std::string> parts = parts_of(*place);
    for (size_t level = parts.size() - 1; level > 0; --level) {
      const std::string folder = joined(std::vector<std::string>(
          parts.begin(), parts.begin() + static_cast<ptrdiff_t>(level)));
      if (vouched.count(lower(folder)) > 0) {
        break;
      }
      const auto same_length = vouched_by_length.find(folder.size());
      if (same_length == vouched_by_length.end()) {
        continue;
      }
      const std::string* best = nullptr;
      size_t fewest = kNabtsMirrorMisreadBytes + 1;
      bool tied = false;
      for (const std::string& candidate : same_length->second) {
        const size_t count = differing(folder, candidate);
        if (count < fewest) {
          best = &candidate;
          fewest = count;
          tied = false;
        } else if (count == fewest) {
          tied = true;
        }
      }
      if (best != nullptr && !tied) {
        places[i] = *best + "/" + joined(parts, level);
        ++moved;
        break;
      }
    }
  }
  return moved;
}

/// A version being gathered: the file standing for it, and its copies.
struct Gathering {
  size_t file = 0;
  uint32_t copies = 0;
};

}  // namespace

std::optional<std::string> nabts_mirror_path(const NabtsCarriedFile& file) {
  if (file.transfer != "framed service") {
    return std::nullopt;
  }
  if (!file.source.empty()) {
    if (const std::optional<std::vector<std::string>> parts =
            source_parts(file.source)) {
      return joined(*parts);
    }
  }
  return std::string(kNabtsMirrorUnaddressed) + "/" + addresses_of(file) + "/" +
         part_of(file.name.empty() ? "file" : file.name);
}

std::string nabts_mirror_rewrite_links(
    const std::string& page, const std::string& page_path,
    const std::function<std::optional<std::string>(const std::string&)>&
        lookup) {
  const std::vector<std::string> page_parts = parts_of(page_path);
  const std::string host = page_parts.empty() ? "" : page_parts.front();
  std::string folder;
  for (size_t i = 0; i + 1 < page_parts.size(); ++i) {
    folder += page_parts[i] + "/";
  }

  // The link as the mirror can serve it, or as the web address it named. A
  // folder's address a script joins more onto (|prefix|) is pointed at the
  // folder, where the mirror holds anything in it.
  const auto rewritten = [&](const std::string& link,
                             bool prefix) -> std::string {
    size_t lead = 0;
    while (lead < link.size() &&
           std::isspace(static_cast<unsigned char>(link[lead])) != 0) {
      ++lead;
    }
    const std::string trimmed = link.substr(lead);
    const std::string low = lower(trimmed);
    if (trimmed.empty() || trimmed.front() == '#' ||
        std::any_of(
            kForeignSchemes.begin(), kForeignSchemes.end(),
            [&low](const char* scheme) { return starts_with(low, scheme); })) {
      return link;
    }
    const size_t hash = trimmed.find('#');
    const std::string fragment =
        hash == std::string::npos ? "" : trimmed.substr(hash);
    // The query stays: a page can read it, as the client's guide pages do.
    const std::string address = trimmed.substr(0, hash);
    const size_t ask = address.find('?');
    const std::string query =
        ask == std::string::npos ? "" : address.substr(ask);
    std::string rest = address.substr(0, ask);
    for (char& c : rest) {
      c = c == '\\' ? '/' : c;
    }
    bool absolute = false;
    if (const size_t scheme = scheme_length(rest); scheme > 0) {
      rest = rest.substr(scheme);
      absolute = true;
    } else if (starts_with(rest, "//")) {
      rest = rest.substr(2);
      absolute = true;
    } else if (starts_with(rest, "/")) {
      rest = host + rest;
    } else {
      rest = folder + rest;
    }
    const std::optional<std::vector<std::string>> parts = mirror_parts(rest);
    if (!parts) {
      return link;
    }
    if (prefix) {
      // mirror_parts() gave the folder its page; the folder is what is asked.
      const std::optional<std::string> found = lookup(
          joined(std::vector<std::string>(parts->begin(), parts->end() - 1)) +
          "/");
      if (!found) {
        return link;
      }
      const std::string to_page =
          relative_path(page_path, *found + kFolderPage);
      const std::string to_folder =
          to_page.substr(0, to_page.size() - std::string(kFolderPage).size());
      return to_folder.empty() ? "./" : to_folder;
    }
    if (const std::optional<std::string> found = lookup(joined(*parts))) {
      return relative_path(page_path, *found) + query + fragment;
    }
    // Where the mirror holds nothing, a link of the page's own is made the
    // address it named on the web.
    return absolute ? link : "http://" + rest + query + fragment;
  };

  const std::string low = lower(page);
  std::string out;
  out.reserve(page.size());
  size_t i = 0;
  // Copies |link|'s value from |from| to |to|, rewritten.
  const auto take = [&](size_t from, size_t to, bool prefix = false) {
    out += rewritten(page.substr(from, to - from), prefix);
    i = to;
  };
  // Where the value of the attribute |key| opening at |at| lies, if one does:
  // inside its quotes, or up to the tag's end or a space.
  const auto value_of =
      [&](size_t at,
          const std::string& key) -> std::optional<std::pair<size_t, size_t>> {
    if (low.compare(at, key.size(), key) != 0) {
      return std::nullopt;
    }
    at += key.size();
    while (at < page.size() && page[at] == ' ') {
      ++at;
    }
    if (at >= page.size() || page[at] != '=') {
      return std::nullopt;
    }
    ++at;
    while (at < page.size() && page[at] == ' ') {
      ++at;
    }
    if (at < page.size() && (page[at] == '"' || page[at] == '\'')) {
      const size_t close = page.find(page[at], at + 1);
      return std::make_pair(at + 1,
                            close == std::string::npos ? page.size() : close);
    }
    size_t to = at;
    while (to < page.size() && page[to] != '>' &&
           std::isspace(static_cast<unsigned char>(page[to])) == 0) {
      ++to;
    }
    return std::make_pair(at, to);
  };
  while (i < page.size()) {
    // A style's url( ... ).
    if (low.compare(i, 4, "url(") == 0) {
      out += page.substr(i, 4);
      size_t from = i + 4;
      while (from < page.size() &&
             (page[from] == ' ' || page[from] == '"' || page[from] == '\'')) {
        out.push_back(page[from]);
        ++from;
      }
      size_t to = from;
      while (to < page.size() && page[to] != ')' && page[to] != '"' &&
             page[to] != '\'' && page[to] != ' ') {
        ++to;
      }
      take(from, to);
      continue;
    }
    // A refresh's URL=.
    if (low.compare(i, 4, "url=") == 0 &&
        (i == 0 || std::isalnum(static_cast<unsigned char>(low[i - 1])) == 0)) {
      out += page.substr(i, 4);
      size_t to = i + 4;
      while (to < page.size() && page[to] != '"' && page[to] != '\'' &&
             page[to] != '>' && page[to] != ';' &&
             std::isspace(static_cast<unsigned char>(page[to])) == 0) {
        ++to;
      }
      take(i + 4, to);
      continue;
    }
    // An attribute naming an address, or keeping one as a record.
    if (i > 0 && std::isspace(static_cast<unsigned char>(page[i - 1])) != 0) {
      std::optional<std::pair<size_t, size_t>> value;
      for (const char* name : kLinkAttributes) {
        value = value_of(i, name);
        if (value) {
          break;
        }
      }
      const bool record = !value;
      if (record) {
        value = value_of(i, kRecordAttribute);
      }
      if (value) {
        out += page.substr(i, value->first - i);
        if (record) {
          out += page.substr(value->first, value->second - value->first);
          i = value->second;
        } else {
          take(value->first, value->second);
        }
        continue;
      }
    }
    // A full address in a quoted string — a script's, an option's value —
    // pointed at the mirror where it holds what the address names; one a
    // script joins more onto, at the folder it names.
    if (i > 0 && (page[i - 1] == '"' || page[i - 1] == '\'') &&
        (low.compare(i, 7, "http://") == 0 ||
         low.compare(i, 8, "https://") == 0)) {
      const char quote = page[i - 1];
      size_t to = i;
      while (to < page.size() && page[to] != quote && page[to] != '<' &&
             page[to] != '>' &&
             std::isspace(static_cast<unsigned char>(page[to])) == 0) {
        ++to;
      }
      size_t next = to + 1;
      while (next < page.size() && page[next] == ' ') {
        ++next;
      }
      const bool joined_onto = to < page.size() && page[to] == quote &&
                               next < page.size() && page[next] == '+';
      if (!joined_onto) {
        take(i, to);
      } else if (page[to - 1] == '/') {
        take(i, to, true);
      } else {
        out += page.substr(i, to - i);
        i = to;
      }
      continue;
    }
    out.push_back(page[i]);
    ++i;
  }
  return out;
}

NabtsWebMirror nabts_web_mirror(const std::vector<NabtsCarriedFile>& files) {
  NabtsWebMirror mirror;

  // Where each file goes, the folders a misread put files in put right.
  std::vector<std::optional<std::string>> places;
  places.reserve(files.size());
  for (const NabtsCarriedFile& file : files) {
    places.push_back(nabts_mirror_path(file));
  }
  mirror.misread = place_misreads(files, places);

  // Every address's versions, in the order received.
  std::map<std::string, std::vector<Gathering>> versions;
  std::map<std::string, size_t> first_file;
  for (size_t i = 0; i < files.size(); ++i) {
    const NabtsCarriedFile& file = files[i];
    const std::optional<std::string>& path = places[i];
    if (!path || file.data.empty()) {
      continue;
    }
    first_file.try_emplace(*path, i);
    std::vector<Gathering>& known = versions[*path];
    bool placed = false;
    for (Gathering& version : known) {
      const NabtsCarriedFile& held = files[version.file];
      if (same_bytes(held, file) ||
          (!whole(file) && agrees(file, held) &&
           (whole(held) || bytes_held(held) >= bytes_held(file)))) {
        // Another copy of what is held: the same bytes, or a damaged copy.
        version.copies += file.copies;
        placed = true;
      } else if (!whole(held) && agrees(held, file)) {
        // A better copy of what is held: whole, or holding more.
        version.file = i;
        version.copies += file.copies;
        placed = true;
      }
      if (placed) {
        break;
      }
    }
    if (!placed) {
      known.push_back(Gathering{i, file.copies});
    }
  }

  // Where each address's current version is written; and the same by the
  // address in lower case, for the pages' links, which a client that kept its
  // pages on a file system read without regard to case.
  std::map<std::string, std::string> written;
  std::map<std::string, std::string> by_lower;
  std::set<std::string> folders;
  for (const auto& [path, known] : versions) {
    const std::vector<std::string> parts = parts_of(path);
    std::string folder;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      folder += (folder.empty() ? "" : "/") + parts[i];
      folders.insert(folder);
    }
  }
  for (const auto& [path, known] : versions) {
    // A file whose path another address's folder takes is written beside it.
    written[path] = folders.count(path) > 0 ? path + "~file" : path;
    by_lower.try_emplace(lower(path), written[path]);
  }
  std::map<std::string, std::string> folder_by_lower;
  for (const std::string& folder : folders) {
    folder_by_lower.try_emplace(lower(folder) + "/", folder + "/");
  }
  const auto lookup =
      [&by_lower, &folder_by_lower](
          const std::string& path) -> std::optional<std::string> {
    if (!path.empty() && path.back() == '/') {
      const auto it = folder_by_lower.find(lower(path));
      if (it == folder_by_lower.end()) {
        return std::nullopt;
      }
      return it->second;
    }
    for (const char* name :
         {"", "/index.html", "/index.htm", "/default.htm", "/default.html"}) {
      std::string key = lower(path);
      if (*name != '\0') {
        if (key.size() > 11 &&
            key.compare(key.size() - 11, 11, "/index.html") == 0) {
          key.resize(key.size() - 11);
        }
        key += name;
      }
      const auto it = by_lower.find(key);
      if (it != by_lower.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  };

  std::set<std::string> hosts;
  for (const auto& [path, known] : versions) {
    const NabtsCarriedFile& first = files[first_file.at(path)];
    NabtsMirrorEntry entry;
    entry.path = written.at(path);
    entry.source = first.source;
    entry.addresses = addresses_of(first);
    if (!starts_with(path, std::string(kNabtsMirrorUnaddressed) + "/")) {
      hosts.insert(path.substr(0, path.find('/')));
    }
    entry.current = known.size() - 1;
    for (size_t v = known.size(); v-- > 0;) {
      if (whole(files[known[v].file])) {
        entry.current = v;
        break;
      }
    }
    for (size_t v = 0; v < known.size(); ++v) {
      const NabtsCarriedFile& file = files[known[v].file];
      NabtsMirrorVersion version;
      version.written =
          known.size() > 1 ? version_path(entry.path, v + 1) : entry.path;
      version.time = file.time;
      version.bytes = file.data.size();
      version.format = file.format;
      version.copies = known[v].copies;
      version.whole = whole(file);
      version.check = file.checksum_holds;
      std::vector<std::string> targets{version.written};
      if (v == entry.current && known.size() > 1) {
        targets.push_back(entry.path);
      }
      for (const std::string& target : targets) {
        std::vector<uint8_t> bytes = file.data;
        bool pointed_here = false;
        if (links_are_rewritten(file, target)) {
          const std::string page(file.data.begin(), file.data.end());
          const std::string pointed =
              nabts_mirror_rewrite_links(page, target, lookup);
          if (pointed != page) {
            bytes.assign(pointed.begin(), pointed.end());
            pointed_here = true;
            ++mirror.pages_rewritten;
          }
        }
        mirror.files.push_back(NabtsMirrorFile{target, std::move(bytes)});
        // As received: before its links were pointed, and with the head of
        // the response it was cached as.
        if (pointed_here || !file.response_head.empty()) {
          std::vector<uint8_t> received(file.response_head.begin(),
                                        file.response_head.end());
          received.insert(received.end(), file.data.begin(), file.data.end());
          mirror.files.push_back(
              NabtsMirrorFile{std::string(kReceivedFolder) + "/" + target,
                              std::move(received)});
        }
      }
      entry.versions.push_back(std::move(version));
    }
    mirror.replaced += known.size() > 1 ? 1 : 0;
    mirror.entries.push_back(std::move(entry));
  }
  mirror.hosts = hosts.size();

  // The listing, and the manifest the report reads.
  std::string index =
      "<!doctype html>\n<meta charset=\"utf-8\">\n<title>Website "
      "mirror</title>\n<style>body{font:14px/1.45 system-ui,sans-serif;"
      "margin:16px;max-width:1200px} table{border-collapse:collapse} "
      "td,th{padding:2px 12px 2px 0;text-align:left;border-bottom:1px solid "
      "#ccc;vertical-align:top} .dim{color:#777}</style>\n<h1>Website "
      "mirror</h1>\n";
  index += fmt::format(
      "<p>{} addresses on {} hosts, as the datacast services sent them, "
      "laid out by host and path as their client kept them; {} sent again "
      "changed, every version kept as NAME~1, NAME~2 and on, the last "
      "received whole at the address itself. A page's links point into the "
      "mirror where it holds what they name; its bytes as received — a file "
      "cached as a web server's response with the response's head — are "
      "under {}/. The pages were made for the browser of their day: one that "
      "draws with that browser's own object model, or asks the service's "
      "client for files from its cache, shows only in part in a browser of "
      "today.</p>\n",
      mirror.entries.size(), mirror.hosts, mirror.replaced, kReceivedFolder);
  if (mirror.misread > 0) {
    index += fmt::format(
        "<p>{} damaged files whose addresses were misread into a folder of "
        "their own are placed in the folder they differ from by a byte or "
        "two, one a file received sound is in; the manifest gives each "
        "address as sent.</p>\n",
        mirror.misread);
  }
  // The pages each host opens with: its shallowest index.
  std::map<std::string, std::string> starts;
  for (const NabtsMirrorEntry& entry : mirror.entries) {
    const std::string name =
        lower(entry.path.substr(entry.path.rfind('/') + 1));
    if (name != "index.html" && name != "index.htm") {
      continue;
    }
    const std::string host_of = entry.path.substr(0, entry.path.find('/'));
    if (host_of == kNabtsMirrorUnaddressed) {
      continue;
    }
    const auto it = starts.find(host_of);
    if (it == starts.end() ||
        std::count(entry.path.begin(), entry.path.end(), '/') <
            std::count(it->second.begin(), it->second.end(), '/')) {
      starts[host_of] = entry.path;
    }
  }
  if (!starts.empty()) {
    index += "<h2>Start pages</h2>\n<ul>\n";
    for (const auto& [host_of, path] : starts) {
      index +=
          fmt::format("<li><a href=\"{0}\">{0}</a></li>\n", escape_html(path));
    }
    index += "</ul>\n";
  }
  std::string manifest =
      "path\tversion\tversions\tcurrent\twritten\ttime\tbytes\tformat\twhole"
      "\tcheck\tsource\taddresses\tcopies\n";
  std::string open_host;
  for (const NabtsMirrorEntry& entry : mirror.entries) {
    const std::string host_of = entry.path.substr(0, entry.path.find('/'));
    if (host_of != open_host) {
      index += (open_host.empty() ? "" : "</table>\n") +
               fmt::format(
                   "<h2>{}</h2>\n<table><tr><th>Address</th><th>"
                   "Received</th><th>Bytes</th><th>Versions</th></tr>"
                   "\n",
                   escape_html(host_of));
      open_host = host_of;
    }
    const NabtsMirrorVersion& current = entry.versions[entry.current];
    std::string list;
    for (size_t v = 0; v < entry.versions.size(); ++v) {
      const NabtsMirrorVersion& version = entry.versions[v];
      if (entry.versions.size() > 1) {
        list += fmt::format(
            "{}<a href=\"{}\">{}</a> {}{}", list.empty() ? "" : " · ",
            escape_html(version.written), v + 1, escape_html(version.time),
            version.whole ? "" : " (incomplete)");
      }
      std::string clean_source = entry.source;
      std::replace_if(
          clean_source.begin(), clean_source.end(),
          [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
      manifest += fmt::format(
          "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", entry.path,
          v + 1, entry.versions.size(), v == entry.current ? 1 : 0,
          version.written, version.time, version.bytes, version.format,
          version.whole ? 1 : 0,
          version.check ? (*version.check ? "holds" : "fails") : "-",
          clean_source, entry.addresses, version.copies);
    }
    index += fmt::format(
        "<tr><td><a href=\"{0}\">{1}</a>{2}</td><td>{3}</td><td>{4}</td>"
        "<td>{5}</td></tr>\n",
        escape_html(entry.path),
        escape_html(entry.path.substr(entry.path.find('/') + 1)),
        current.whole ? "" : " <span class=dim>(incomplete)</span>",
        escape_html(current.time), current.bytes,
        entry.versions.size() > 1 ? fmt::format("{} versions, replaced: {}",
                                                entry.versions.size(), list)
                                  : "");
  }
  if (!open_host.empty()) {
    index += "</table>\n";
  }
  mirror.files.push_back(NabtsMirrorFile{
      "index.html", std::vector<uint8_t>(index.begin(), index.end())});
  mirror.files.push_back(NabtsMirrorFile{
      "manifest.tsv", std::vector<uint8_t>(manifest.begin(), manifest.end())});
  return mirror;
}

}  // namespace orc
