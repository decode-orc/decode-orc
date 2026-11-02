/*
 * File:        nabts_web_mirror_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for laying out the files framed datacast services
 *              sent as the web sites they came from
 *
 * Covers: a URL, a cache path, a folder, a misread scheme and a file with no
 * address placed in the mirror, and a file no framed service sent left out;
 * a page's links pointed at the files the mirror holds — from the root, from
 * the page, a full URL, a style's url(), a refresh, a script's string, a
 * folder a script joins onto and a list option — and at the web where it
 * holds none, queries kept, script links, a copying tool's record of the
 * address and the page's text left alone; a file cached as a web server's
 * response kept as received with its head; damaged files a misread put in a
 * folder of their own placed back in the one they were taken out of, and
 * sound files and those too far from any left; every version of an address
 * kept, the last received whole at the address; a damaged copy folded into the
 * version it agrees with; and the index and manifest. The files are built
 * here. Deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_web_mirror.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

orc::NabtsCarriedFile framed(const std::string& source, const std::string& text,
                             const std::string& time = "1999-01-02 03:04:05") {
  orc::NabtsCarriedFile file;
  file.channel = 0x010;
  file.last_channel = 0x01F;
  file.transfer = "framed service";
  file.name = source.substr(source.find_last_of("/\\") + 1);
  file.source = source;
  file.time = time;
  file.copies = 1;
  file.data.assign(text.begin(), text.end());
  file.format = "text";
  return file;
}

std::string text_of(const std::vector<uint8_t>& data) {
  return std::string(data.begin(), data.end());
}

/// The mirror's files by path.
std::map<std::string, std::string> written(const orc::NabtsWebMirror& mirror) {
  std::map<std::string, std::string> out;
  for (const auto& file : mirror.files) {
    out[file.path] = text_of(file.data);
  }
  return out;
}

TEST(NabtsWebMirror, PlacesEachFileAtItsHostAndPath) {
  EXPECT_EQ(orc::nabts_mirror_path(
                framed("http://home.example.net/gfx\\providers\\logo.gif", "")),
            "home.example.net/gfx/providers/logo.gif");
  EXPECT_EQ(orc::nabts_mirror_path(
                framed("cache\\www.example.com\\ads\\banner.gif", "")),
            "www.example.com/ads/banner.gif");
  EXPECT_EQ(orc::nabts_mirror_path(framed("http://Host.Example.COM/dir/", "")),
            "host.example.com/dir/index.html");
  EXPECT_EQ(orc::nabts_mirror_path(framed("htpp://host.example.com/a.txt", "")),
            "host.example.com/a.txt");
  EXPECT_EQ(orc::nabts_mirror_path(
                framed("http://host.example.com/../../etc/x?y=1", "")),
            "host.example.com/etc/x");
  EXPECT_EQ(
      orc::nabts_mirror_path(framed("cache\\www.example.com\\a#b?.gif", "")),
      "www.example.com/a_b_.gif");
  orc::NabtsCarriedFile unaddressed = framed("", "");
  unaddressed.name = "TOOL.EXE";
  EXPECT_EQ(orc::nabts_mirror_path(unaddressed), "_files/010-01F/TOOL.EXE");
  orc::NabtsCarriedFile carousel = framed("http://host.example.com/a", "");
  carousel.transfer = "file carousel";
  EXPECT_FALSE(orc::nabts_mirror_path(carousel).has_value());
}

TEST(NabtsWebMirror, PointsAPagesLinksAtTheFilesTheMirrorHolds) {
  const std::set<std::string> held = {
      "host.example.com/img/a.gif", "host.example.com/dir/b.gif",
      "other.example.com/x.html", "host.example.com/dir/sub/index.html"};
  const auto lookup =
      [&held](const std::string& path) -> std::optional<std::string> {
    if (path.back() == '/') {
      const auto it = held.lower_bound(path);
      if (it != held.end() && it->rfind(path, 0) == 0) {
        return path;
      }
      return std::nullopt;
    }
    for (const std::string& candidate :
         {path, path.substr(0, path.rfind('/')) + "/index.html"}) {
      if (held.count(candidate) > 0) {
        return candidate;
      }
    }
    return std::nullopt;
  };
  const std::string page =
      "<a href=\"/img/a.gif\">a</a> <img src=b.gif> "
      "<a href='http://other.example.com/x.html#top'>x</a> "
      "<a href=\"missing.html?q=1\">m</a> <a href=\"javascript:go()\">j</a> "
      "<a href=\"sub/\">s</a>\n"
      "<style>body { background: url(/img/a.gif) }</style>\n"
      "<meta http-equiv=refresh content=\"0; URL=http://host.example.com/dir/"
      "b.gif\">\n"
      "<script>var next = \"http://other.example.com/x.html?home\"; var far = "
      "\"http://far.example.com/y.html\"; var pic = "
      "\"http://host.example.com/img/\" + name + \".gif\";</script>\n"
      "<img src=\"b.gif\" tppabs=\"http://host.example.com/dir/b.gif\">\n"
      "<option value=\"http://host.example.com/dir/sub/\">sub</option>\n"
      "As at http://host.example.com/img/a.gif here.";
  const std::string pointed = orc::nabts_mirror_rewrite_links(
      page, "host.example.com/dir/p.html", lookup);
  EXPECT_NE(pointed.find("href=\"../img/a.gif\""), std::string::npos)
      << pointed;
  EXPECT_NE(pointed.find("src=b.gif"), std::string::npos) << pointed;
  EXPECT_NE(pointed.find("href='../../other.example.com/x.html#top'"),
            std::string::npos)
      << pointed;
  EXPECT_NE(
      pointed.find("href=\"http://host.example.com/dir/missing.html?q=1\""),
      std::string::npos)
      << pointed;
  EXPECT_NE(pointed.find("href=\"javascript:go()\""), std::string::npos)
      << pointed;
  EXPECT_NE(pointed.find("href=\"sub/index.html\""), std::string::npos)
      << pointed;
  EXPECT_NE(pointed.find("url(../img/a.gif)"), std::string::npos) << pointed;
  EXPECT_NE(pointed.find("URL=b.gif"), std::string::npos) << pointed;
  EXPECT_NE(pointed.find("var next = \"../../other.example.com/x.html?home\""),
            std::string::npos)
      << pointed;
  EXPECT_NE(pointed.find("\"http://far.example.com/y.html\""),
            std::string::npos)
      << pointed;
  EXPECT_NE(pointed.find("var pic = \"../img/\" + name"), std::string::npos)
      << pointed;
  EXPECT_NE(pointed.find("tppabs=\"http://host.example.com/dir/b.gif\""),
            std::string::npos)
      << pointed;
  EXPECT_NE(pointed.find("<option value=\"sub/index.html\">"),
            std::string::npos)
      << pointed;
  EXPECT_NE(pointed.find("As at http://host.example.com/img/a.gif here."),
            std::string::npos)
      << pointed;
}

TEST(NabtsWebMirror, KeepsEveryVersionAndWritesTheLastWholeAtItsAddress) {
  const auto mirror =
      orc::nabts_web_mirror({framed("http://host.example.com/news.txt",
                                    "first edition", "1999-01-02 03:00:00"),
                             framed("http://host.example.com/news.txt",
                                    "second edition", "1999-01-02 04:00:00")});
  ASSERT_EQ(mirror.entries.size(), 1u);
  const auto& entry = mirror.entries[0];
  ASSERT_EQ(entry.versions.size(), 2u);
  EXPECT_EQ(entry.current, 1u);
  EXPECT_EQ(mirror.replaced, 1u);
  const auto files = written(mirror);
  EXPECT_EQ(files.at("host.example.com/news.txt"), "second edition");
  EXPECT_EQ(files.at("host.example.com/news~1.txt"), "first edition");
  EXPECT_EQ(files.at("host.example.com/news~2.txt"), "second edition");
}

TEST(NabtsWebMirror, FoldsADamagedCopyIntoTheVersionItAgreesWith) {
  orc::NabtsCarriedFile whole =
      framed("http://host.example.com/page.txt", "the whole of the page");
  orc::NabtsCarriedFile cut =
      framed("http://host.example.com/page.txt", "the whole of th#");
  cut.cut_short = true;
  const auto mirror = orc::nabts_web_mirror({cut, whole});
  ASSERT_EQ(mirror.entries.size(), 1u);
  ASSERT_EQ(mirror.entries[0].versions.size(), 1u);
  EXPECT_TRUE(mirror.entries[0].versions[0].whole);
  EXPECT_EQ(mirror.entries[0].versions[0].copies, 2u);
  EXPECT_EQ(written(mirror).at("host.example.com/page.txt"),
            "the whole of the page");
}

TEST(NabtsWebMirror, WritesAPageWithItsLinksPointedAndAsReceived) {
  orc::NabtsCarriedFile page =
      framed("http://host.example.com/index.html", "<img src=\"/logo.gif\">");
  page.format = "html";
  const auto mirror = orc::nabts_web_mirror(
      {page, framed("http://host.example.com/logo.gif", "GIF89a")});
  const auto files = written(mirror);
  EXPECT_EQ(files.at("host.example.com/index.html"), "<img src=\"logo.gif\">");
  EXPECT_EQ(files.at("_received/host.example.com/index.html"),
            "<img src=\"/logo.gif\">");
  EXPECT_EQ(mirror.pages_rewritten, 1u);
  EXPECT_EQ(mirror.hosts, 1u);
  const std::string& index = files.at("index.html");
  EXPECT_NE(index.find("<a href=\"host.example.com/index.html\">"),
            std::string::npos)
      << index;
  const std::string& manifest = files.at("manifest.tsv");
  EXPECT_EQ(manifest.rfind("path\tversion\tversions\tcurrent\twritten", 0), 0u);
  EXPECT_NE(manifest.find("host.example.com/logo.gif\t1\t1\t1\t"
                          "host.example.com/logo.gif\t"),
            std::string::npos)
      << manifest;
}

TEST(NabtsWebMirror, PlacesAMisreadFileInTheFolderItWasTakenOutOf) {
  const auto damaged = [](const std::string& source) {
    orc::NabtsCarriedFile file = framed(source, "damaged " + source);
    file.cut_short = true;
    return file;
  };
  orc::NabtsCarriedFile failing =
      framed("http://www.exbmple.com/news/f.gif", "check fails");
  failing.checksum_holds = false;
  // Whole, but its address read in part.
  orc::NabtsCarriedFile misread =
      framed("cache\\www.exa_ple.com\\sport\\m.gif", "m");
  misread.source_misread = true;
  const auto mirror = orc::nabts_web_mirror(
      {framed("http://www.example.com/news/a.gif", "a"),
       framed("http://www.example.com/sport/b.gif", "b"),
       // Damaged, in a folder no sound file vouches for: put back.
       damaged("http://www.exbmple.com/news/c.gif"), failing, misread,
       damaged("http://www.example.com/nuws/d.gif"),
       // Damaged, but in a folder a sound file is in: left.
       damaged("http://www.example.com/news/e.gif"),
       // Sound, though a byte from a vouched folder: left.
       framed("http://www.examplf.com/news/g.gif", "g"),
       // Three bytes from the nearest: left.
       damaged("http://wwx.exbmplg.com/news/h.gif"),
       // Named as a sound file is, its address three bytes from that file's
       // and two from another folder: placed with the file.
       framed("http://www.example.com/news/story.htm", "story"),
       damaged("http://wxw.exbmplf.com/news/story.htm")});
  const auto files = written(mirror);
  EXPECT_EQ(mirror.misread, 5u);
  EXPECT_EQ(files.count("www.examplf.com/news/story.htm"), 0u);
  EXPECT_EQ(files.count("www.example.com/news/story~2.htm"), 1u);
  EXPECT_EQ(files.count("www.example.com/sport/m.gif"), 1u);
  EXPECT_EQ(files.count("www.example.com/news/c.gif"), 1u);
  EXPECT_EQ(files.count("www.example.com/news/f.gif"), 1u);
  EXPECT_EQ(files.count("www.example.com/news/d.gif"), 1u);
  EXPECT_EQ(files.count("www.example.com/news/e.gif"), 1u);
  EXPECT_EQ(files.count("www.examplf.com/news/g.gif"), 1u);
  EXPECT_EQ(files.count("wwx.exbmplg.com/news/h.gif"), 1u);
  const auto entry = std::find_if(
      mirror.entries.begin(), mirror.entries.end(),
      [](const auto& e) { return e.path == "www.example.com/news/c.gif"; });
  ASSERT_NE(entry, mirror.entries.end());
  EXPECT_EQ(entry->source, "http://www.exbmple.com/news/c.gif");
}

TEST(NabtsWebMirror, KeepsTheResponseAFileWasCachedAsUnderReceived) {
  orc::NabtsCarriedFile picture =
      framed("http://host.example.com/pics/a.gif", "GIF89a");
  picture.format = "gif";
  picture.response_head = "HTTP/1.0 200 OK\r\nContent-type: image/gif\r\n\r\n";
  const auto files = written(orc::nabts_web_mirror({picture}));
  EXPECT_EQ(files.at("host.example.com/pics/a.gif"), "GIF89a");
  EXPECT_EQ(files.at("_received/host.example.com/pics/a.gif"),
            "HTTP/1.0 200 OK\r\nContent-type: image/gif\r\n\r\nGIF89a");
}

}  // namespace
}  // namespace orc_unit_test
