/*
 * File:        nabts_framed_containers_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for unpacking the containers a framed datacast
 *              service sends its files in
 *
 * Covers: a file container's zlib stream unpacked and named by its address;
 * an archive container's imploded stream unpacked, named by its name and
 * checked by its CRC-32, one of the short kind, one whose path's length is a
 * single digit, one whose path has bytes received wrong — its cache folder's
 * put right, two others read as '_', a third too many — and one whose CRC-32
 * fails; a file cached as a web server's response taken out of
 * it, named by the address the response gives; an archive whose bytes went
 * missing unpacked as far as they go; and a file that is no container left
 * alone. The containers are built here. Deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_framed_containers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orc_unit_test {
namespace {

// "hello hello hello hello, framed world", a zlib stream of the fixed codes.
const std::vector<uint8_t> kZlib = {
    0x78, 0xDA, 0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40,
    0x27, 0x75, 0x14, 0xD2, 0x8A, 0x12, 0x73, 0x53, 0x53, 0x14,
    0xCA, 0xF3, 0x8B, 0x72, 0x52, 0x00, 0x04, 0x5A, 0x0D, 0xB4};
// "AIAIAIAIAIAIA" imploded, and its CRC-32.
const std::vector<uint8_t> kImploded = {0x00, 0x04, 0x82, 0x24,
                                        0x25, 0x8F, 0x80, 0x7F};
constexpr uint32_t kImplodedCrc = 0x506B8690;

std::string text_of(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

orc::NabtsCarriedFile carried(std::vector<uint8_t> data) {
  orc::NabtsCarriedFile file;
  file.channel = 0x010;
  file.last_channel = 0x01F;
  file.transfer = "framed service";
  file.name = "container";
  file.time = "1999-01-02 03:04:05 UTC";
  file.data = std::move(data);
  return file;
}

/// |text| as a zlib stream of one stored block.
std::vector<uint8_t> stored_zlib(const std::string& text) {
  std::vector<uint8_t> out = {0x78, 0x01, 0x01};
  const auto length = static_cast<uint16_t>(text.size());
  const auto inverse = static_cast<uint16_t>(~length);
  for (const uint16_t half : {length, inverse}) {
    out.push_back(static_cast<uint8_t>(half & 0xFFU));
    out.push_back(static_cast<uint8_t>(half >> 8U));
  }
  out.insert(out.end(), text.begin(), text.end());
  uint32_t a = 1;
  uint32_t b = 0;
  for (const char c : text) {
    a = (a + static_cast<uint8_t>(c)) % 65521U;
    b = (b + a) % 65521U;
  }
  const uint32_t adler = (b << 16U) | a;
  for (const uint32_t shift : {24U, 16U, 8U, 0U}) {
    out.push_back(static_cast<uint8_t>(adler >> shift));
  }
  return out;
}

/// A file container of |stream| fetched from |address|.
std::vector<uint8_t> file_container(const std::string& address,
                                    const std::vector<uint8_t>& stream) {
  std::vector<uint8_t> out = {'W', 'T', 'F', '1'};
  out.resize(orc::kNabtsFileContainerHead - 1, 0x01);
  out.push_back(static_cast<uint8_t>(address.size()));
  out.insert(out.end(), address.begin(), address.end());
  out.insert(out.end(), orc::kNabtsFileContainerTail, 0x02);
  out.insert(out.end(), stream.begin(), stream.end());
  return out;
}

/// An archive container of |stream| named |name|, cached under |path|, its
/// record giving |crc|; of the short kind with |short_record|.
std::vector<uint8_t> archive_container(const std::string& path,
                                       const std::string& name,
                                       const std::vector<uint8_t>& stream,
                                       uint32_t crc,
                                       bool short_record = false) {
  std::vector<uint8_t> out = {'W', 'T', 'Z',
                              static_cast<uint8_t>(short_record ? 'X' : 'Y')};
  if (path.size() < 10) {
    // One digit and its zero, the field's last byte left as it was.
    out.push_back(static_cast<uint8_t>('0' + path.size()));
    out.push_back(0);
    out.push_back(0x5A);
  } else {
    out.push_back(static_cast<uint8_t>('0' + path.size() / 10));
    out.push_back(static_cast<uint8_t>('0' + path.size() % 10));
    out.push_back(0);
  }
  out.insert(out.end(), path.begin(), path.end());
  std::vector<uint8_t> record(short_record ? orc::kNabtsShortArchiveRecordBytes
                                           : orc::kNabtsArchiveRecordBytes,
                              0);
  for (size_t i = 0; i < 4; ++i) {
    record[i] = static_cast<uint8_t>(stream.size() >> (8 * i));
    record[4 + i] = static_cast<uint8_t>(crc >> (8 * i));
  }
  if (!short_record) {
    record[8] = 2;
  }
  std::copy(name.begin(), name.end(),
            record.begin() + (short_record ? orc::kNabtsShortArchiveNameAt
                                           : orc::kNabtsArchiveNameAt));
  out.insert(out.end(), record.begin(), record.end());
  out.insert(out.end(), stream.begin(), stream.end());
  return out;
}

TEST(NabtsFramedContainers, UnpacksAFileContainer) {
  const auto file =
      carried(file_container("http://example.invalid/pages\\notes.txt", kZlib));
  ASSERT_TRUE(orc::nabts_framed_container(file));
  const auto content = orc::nabts_framed_container_content(file);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(content->name, "notes.txt");
  EXPECT_EQ(content->source, "http://example.invalid/pages\\notes.txt");
  EXPECT_EQ(text_of(content->data), "hello hello hello hello, framed world");
  EXPECT_EQ(content->format, "text");
  EXPECT_FALSE(content->cut_short);
  EXPECT_EQ(content->checksum_holds, std::optional<bool>(true));
  EXPECT_EQ(content->time, file.time);
  EXPECT_EQ(content->last_channel, 0x01F);
}

TEST(NabtsFramedContainers, UnpacksAnArchiveContainer) {
  const auto file = carried(
      archive_container("cache\\site", "aia.txt", kImploded, kImplodedCrc));
  const auto content = orc::nabts_framed_container_content(file);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(content->name, "aia.txt");
  EXPECT_EQ(content->source, "cache\\site\\aia.txt");
  EXPECT_EQ(text_of(content->data), "AIAIAIAIAIAIA");
  EXPECT_EQ(content->checksum, std::optional<uint32_t>(kImplodedCrc));
  EXPECT_EQ(content->checksum_holds, std::optional<bool>(true));
  EXPECT_FALSE(content->cut_short);
}

TEST(NabtsFramedContainers, UnpacksAShortArchiveContainer) {
  const auto file = carried(archive_container(
      "www.example.invalid\\pics", "aia.txt", kImploded, kImplodedCrc, true));
  ASSERT_TRUE(orc::nabts_framed_container(file));
  const auto content = orc::nabts_framed_container_content(file);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(content->name, "aia.txt");
  EXPECT_EQ(content->source, "www.example.invalid\\pics\\aia.txt");
  EXPECT_EQ(text_of(content->data), "AIAIAIAIAIAIA");
  EXPECT_EQ(content->checksum_holds, std::optional<bool>(true));
}

TEST(NabtsFramedContainers, ReadsAPathLengthOfOneDigit) {
  const auto file = carried(
      archive_container("cache\\abc", "aia.txt", kImploded, kImplodedCrc));
  ASSERT_TRUE(orc::nabts_framed_container(file));
  const auto content = orc::nabts_framed_container_content(file);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(content->source, "cache\\abc\\aia.txt");
  EXPECT_EQ(content->checksum_holds, std::optional<bool>(true));
}

TEST(NabtsFramedContainers, ReadsAnArchivesPathThroughBytesReceivedWrong) {
  auto file = carried(
      archive_container("cache\\site", "aia.txt", kImploded, kImplodedCrc));
  // The cache folder's name with a byte received wrong is put right, a
  // character or none.
  for (const uint8_t wrong : {uint8_t{'k'}, uint8_t{0x7F}}) {
    file.data[8] = wrong;
    const auto content = orc::nabts_framed_container_content(file);
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(content->source, "cache\\site\\aia.txt");
    EXPECT_FALSE(content->source_misread);
  }
  // Bytes past it that are no character are read as '_', up to two of them.
  file.data[8] = 'a';
  file.data[13] = 0x00;
  file.data[14] = 0x9F;
  const auto content = orc::nabts_framed_container_content(file);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(content->source, "cache\\__te\\aia.txt");
  EXPECT_TRUE(content->source_misread);
  EXPECT_EQ(text_of(content->data), "AIAIAIAIAIAIA");
  file.data[15] = 0x01;
  EXPECT_FALSE(orc::nabts_framed_container_content(file).has_value());
}

TEST(NabtsFramedContainers, TakesTheFileOutOfTheResponseItWasCachedAs) {
  const std::string head =
      "HTTP/1.0 200 OK\r\nContent-type: text/plain\r\n"
      "CachedURL: http://www.example.invalid/notes/today.txt\r\n\r\n";
  const auto file = carried(file_container(
      "www.example.invalid\\notes\\today.txt", stored_zlib(head + "the body")));
  const auto content = orc::nabts_framed_container_content(file);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(text_of(content->data), "the body");
  EXPECT_EQ(content->response_head, head);
  EXPECT_EQ(content->source, "http://www.example.invalid/notes/today.txt");
  EXPECT_EQ(content->name, "today.txt");
  EXPECT_EQ(content->format, "text");
  EXPECT_EQ(content->checksum_holds, std::optional<bool>(true));
}

TEST(NabtsFramedContainers, SaysWhenAnArchivesCrcFails) {
  const auto file = carried(archive_container("cache\\site", "aia.txt",
                                              kImploded, kImplodedCrc ^ 1U));
  const auto content = orc::nabts_framed_container_content(file);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(content->checksum_holds, std::optional<bool>(false));
}

TEST(NabtsFramedContainers, UnpacksAsFarAsTheBytesThatArrived) {
  // The archive's last three bytes lost: its stream unpacks as far as they.
  auto file = carried(
      archive_container("cache\\site", "aia.txt", kImploded, kImplodedCrc));
  file.data_present.assign(file.data.size(), 1);
  for (size_t i = file.data.size() - 3; i < file.data.size(); ++i) {
    file.data_present[i] = 0;
    file.data[i] = 0;
  }
  const auto content = orc::nabts_framed_container_content(file);
  ASSERT_TRUE(content.has_value());
  EXPECT_TRUE(content->cut_short);
  EXPECT_FALSE(content->checksum_holds.has_value());
  EXPECT_EQ(std::string("AIAIAIAIAIAIA").rfind(text_of(content->data), 0), 0u);
}

TEST(NabtsFramedContainers, LeavesAFileThatIsNoContainerAlone) {
  const auto file = carried({'G', 'I', 'F', '8', '9', 'a', 0, 0});
  EXPECT_FALSE(orc::nabts_framed_container(file));
  EXPECT_FALSE(orc::nabts_framed_container_content(file).has_value());
}

}  // namespace
}  // namespace orc_unit_test
