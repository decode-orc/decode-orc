/*
 * File:        nabts_web_mirror.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The pages and files framed datacast services sent, laid out as
 *              the web sites they came from
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_WEB_MIRROR_H
#define ORC_NABTS_WEB_MIRROR_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "vbi-services/vbi_analysis_results.h"

namespace orc {

// Where a file the service named no address for goes, under the mirror's root.
constexpr const char* kNabtsMirrorUnaddressed = "_files";
// A copy of a file damaged in its sending — cut short, or with bytes missing —
// is a copy of a version received whole where it agrees with it on at least
// this share of its bytes, counted from its start, in parts per four.
constexpr size_t kNabtsMirrorAgreeingQuarters = 1;
// The most bytes a folder a file not received sound was placed in may differ
// from the folder a sound file vouches for that it is taken to be a misread
// of, ignoring case.
constexpr size_t kNabtsMirrorMisreadBytes = 2;
// A file not received sound, in a folder no sound file vouches for, named as
// a sound file is and sent under an address differing from that file's in at
// most one byte in this many — and in more than kNabtsMirrorMisreadBytes at
// least — is a misread copy of it.
constexpr size_t kNabtsMirrorMisreadShare = 8;

/// One file to write under the mirror's root.
struct NabtsMirrorFile {
  /// Relative to the mirror's root, '/' between its parts.
  std::string path;
  std::vector<uint8_t> data;
};

/// One version of one address of the mirror.
struct NabtsMirrorVersion {
  /// Where it was written, relative to the mirror's root.
  std::string written;
  /// When the service dated it, its bytes, and what they are
  /// (NabtsCarriedFile::format).
  std::string time;
  size_t bytes = 0;
  std::string format;
  /// Copies of it the recording held, the damaged copies it absorbed among
  /// them.
  uint32_t copies = 0;
  /// Whether it arrived whole — every byte, its container not broken off —
  /// and whether its service's check of it held, where it had one.
  bool whole = false;
  std::optional<bool> check;
};

/// One address of the mirror, with every version of it received.
struct NabtsMirrorEntry {
  /// Where it is, relative to the mirror's root: host/path.
  std::string path;
  /// The address or cache path as the service gave it.
  std::string source;
  /// The addresses that carried it.
  std::string addresses;
  /// In the order received; @ref current the one written at @ref path.
  std::vector<NabtsMirrorVersion> versions;
  size_t current = 0;
};

/// The mirror: every address, and every file to write.
struct NabtsWebMirror {
  std::vector<NabtsMirrorEntry> entries;
  std::vector<NabtsMirrorFile> files;
  /// Hosts, addresses whose later versions replaced earlier ones, files
  /// placed back in the folder a misread took them out of, and the pages and
  /// style sheets whose links were pointed at the mirror.
  size_t hosts = 0;
  size_t replaced = 0;
  size_t misread = 0;
  size_t pages_rewritten = 0;
};

/**
 * @brief Where |file| goes in the mirror: host/path, or nothing for a file no
 *        framed service sent
 *
 * The address the file's container gave (NabtsCarriedFile::source): a URL —
 * its scheme dropped, whatever it was read as — or a cache path, under the
 * "cache" folder the service's client kept its pages in, named for the host
 * and its path, which is dropped. '\' reads as '/'; a path that ends in a
 * folder gets "index.html"; each part keeps the characters a file system
 * takes, any other written as '_'; and "." and ".." go nowhere. A file the
 * service named no address for goes under kNabtsMirrorUnaddressed, in a
 * folder named for the addresses that carried it.
 */
std::optional<std::string> nabts_mirror_path(const NabtsCarriedFile& file);

/**
 * @brief |page|'s links — a page's or a style sheet's — pointed at the mirror
 *
 * Each address in an href, src, background, lowsrc, action or codebase
 * attribute, a style's url() and a refresh's URL= is read as a browser would
 * read it from |page_path|, the page's place in the mirror: a full URL, one
 * from the root of the page's host, or one relative to the page. Where
 * |lookup| finds the file it names in the mirror — given host/path, its query
 * dropped — the link is pointed at it, relative to the page, its query and
 * fragment kept; where it does not, a link that was not a full URL is made
 * one, to where it pointed on the web. Script, mail and in-page links are
 * left as they are, as is the original address a site-copying tool kept in
 * a tppabs attribute. A full URL in a quoted string elsewhere — a script's,
 * as the service's own front pages give their channels, or a list option's
 * value — is pointed at the mirror where it holds what the URL names; a
 * folder's URL a script joins more onto, at the folder, where |lookup| —
 * given host/path/, ending in '/' — finds the mirror holds anything in it.
 * A URL in the page's text is left as it is.
 */
std::string nabts_mirror_rewrite_links(
    const std::string& page, const std::string& page_path,
    const std::function<std::optional<std::string>(const std::string&)>&
        lookup);

/**
 * @brief The mirror of the files framed datacast services sent among |files|
 *
 * Every file a framed service sent goes to its address in the mirror
 * (nabts_mirror_path()) — save that a file not received sound (whole, its
 * check holding where it has one, its address read: not
 * NabtsCarriedFile::source_misread), in a folder no sound file is in or under,
 * goes where a misread in its sending took it from: to the sound file of its
 * name whose address as sent is as long as its own and differs from it least,
 * in at most one byte in kNabtsMirrorMisreadShare; or else to the one folder
 * a sound file vouches for that is as long as its own and differs from it in
 * at most kNabtsMirrorMisreadBytes bytes, looked for from its own folder up
 * to its host — case ignored, and a tie placing nothing. An address the
 * service sent more than once keeps
 * every version whose bytes differ, in the order received: the last version
 * received whole is written at the address, and with more than one version
 * each is written beside it too, as NAME~1.EXT, NAME~2.EXT and on. A damaged
 * copy — cut short, or with bytes missing — that agrees with a version on the
 * first part of its bytes (kNabtsMirrorAgreeingQuarters) is a copy of that
 * version. A page or style sheet is written with its links pointed at the
 * mirror (nabts_mirror_rewrite_links()), and as received under _received/;
 * so is a file cached as a web server's response, there with the response's
 * head (NabtsCarriedFile::response_head).
 *
 * The mirror's root also gets index.html, every address with its versions,
 * and manifest.tsv, a line for each version: its address, version number and
 * count, whether it is the current one, where it was written, its time,
 * bytes, kind, whether it arrived whole, its check, its address as sent, the
 * addresses that carried it and the copies of it the recording held.
 *
 * Pure: no I/O.
 */
NabtsWebMirror nabts_web_mirror(const std::vector<NabtsCarriedFile>& files);

}  // namespace orc

#endif  // ORC_NABTS_WEB_MIRROR_H
