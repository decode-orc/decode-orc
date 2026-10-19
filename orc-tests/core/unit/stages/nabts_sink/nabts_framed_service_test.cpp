/*
 * File:        nabts_framed_service_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the reader of datacast services sent in
 *              counted frames across a family of addresses, under a pad
 *
 * Covers: a service's frames from two addresses put back in the order their
 * count gives; frames the count says were sent and not read; a frame with a
 * row missing read in part, and one whose header row was lost or failed its
 * code not read; a frame looked for only where a bundle starts; a count that
 * wraps, and one that falls back and starts a new run; too few frames to list
 * a service, or too scattered, and frames read on a block of addresses not
 * the service's left out; the pad taken off; an object put together as a
 * file — named and dated by its header, its last frame marked and cut short
 * by filler, a lost frame a hole, a lost row a hole only where no copy
 * brought it, a lost first frame naming it by its number; an object sent
 * again changed kept as a version of its own; a file in one of the service's
 * containers unpacked; a file sent in numbered parts joined; and a copy with
 * a row taken as received passed over for one without. The datacasts are built
 * here, frame by frame. Deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_framed_service.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::kNabtsFrameBytes;
using orc::kNabtsFrameHeaderBytes;
using orc::kNabtsFramePayloadBytes;
using orc::kNabtsFrameRowBytes;
using orc::kNabtsFrameRows;
using orc::NabtsDatacastChannel;

// The service's name, kept as sent.
constexpr uint32_t kService = 0x95A17491;
constexpr uint16_t kObject = 0x0A05;
// 1998-07-09 16:00:00 UTC.
constexpr uint32_t kTime = 900000000;

/// Frame |count| of object |object| at |place|, the pad not yet on it: its
/// header, then |payload|.
std::vector<uint8_t> plain_frame(uint16_t count, uint16_t object,
                                 uint32_t place,
                                 const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out(kNabtsFrameHeaderBytes, 0);
  out[5] = static_cast<uint8_t>(count & 0xFF);
  out[6] = static_cast<uint8_t>(count >> 8);
  out[9] = static_cast<uint8_t>(kService >> 24);
  out[10] = static_cast<uint8_t>(kService >> 16);
  out[11] = static_cast<uint8_t>(kService >> 8);
  out[12] = static_cast<uint8_t>(kService);
  out[13] = static_cast<uint8_t>(object & 0xFF);
  out[14] = static_cast<uint8_t>(object >> 8);
  for (size_t i = 0; i < 4; ++i) {
    out[15 + i] = static_cast<uint8_t>(place >> (8 * i));
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

/// Frame |count|, whole, of an object of its own, its bytes saying which
/// frame they belong to.
std::vector<uint8_t> plain_frame(uint16_t count) {
  std::vector<uint8_t> payload(kNabtsFramePayloadBytes);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(count + i);
  }
  return plain_frame(count, static_cast<uint16_t>(0x7000 + count), 397,
                     payload);
}

/// |plain| as sent: under the pad.
std::vector<uint8_t> sent(std::vector<uint8_t> plain) {
  const auto& pad = orc::nabts_frame_pad();
  for (size_t i = 0; i < plain.size(); ++i) {
    plain[i] ^= pad[i];
  }
  return plain;
}

/// A datacast on |channel| of |frames| as sent, the rows numbered in
/// |missing| lost and those in |damaged| taken as received; frame k arrives
/// in frame first + k * step of the recording. A frame shorter than
/// kNabtsFrameBytes has its last rows short and then empty, as a frame cut
/// short by filler arrives.
NabtsDatacastChannel datacast_of(
    uint16_t channel, const std::vector<std::vector<uint8_t>>& frames,
    const std::set<size_t>& missing = {}, const std::set<size_t>& damaged = {},
    uint32_t first = 0, uint32_t step = 1) {
  NabtsDatacastChannel datacast;
  datacast.channel = channel;
  size_t row = 0;
  for (size_t k = 0; k < frames.size(); ++k) {
    const std::vector<uint8_t>& f = frames[k];
    for (size_t r = 0; r < kNabtsFrameRows; ++r, ++row) {
      orc::NabtsDatacastRow entry;
      entry.offset = static_cast<uint32_t>(datacast.payload.size());
      entry.frame = static_cast<uint32_t>(first + k * step);
      if (missing.count(row) != 0) {
        entry.missing = true;
      } else {
        const size_t from = std::min(r * kNabtsFrameRowBytes, f.size());
        const size_t to = std::min((r + 1) * kNabtsFrameRowBytes, f.size());
        entry.length = static_cast<uint16_t>(to - from);
        entry.damaged = damaged.count(row) != 0;
        datacast.payload.insert(datacast.payload.end(),
                                f.begin() + static_cast<ptrdiff_t>(from),
                                f.begin() + static_cast<ptrdiff_t>(to));
      }
      datacast.rows.push_back(entry);
    }
  }
  return datacast;
}

/// The frames, as sent, of object |object| holding |data| named |name|: its
/// header and the first of its bytes, then a frame of them at a time.
std::vector<std::vector<uint8_t>> object_frames(
    const std::vector<uint8_t>& data, const std::string& name,
    uint16_t first_count, uint16_t object = kObject) {
  std::vector<uint8_t> head(orc::kNabtsFrameObjectHeaderBytes, 0);
  head[2] = static_cast<uint8_t>(object & 0xFF);
  head[3] = static_cast<uint8_t>(object >> 8);
  for (size_t i = 0; i < 4; ++i) {
    head[4 + i] = static_cast<uint8_t>(kTime >> (8 * i));
    head[8 + i] = static_cast<uint8_t>(data.size() >> (8 * i));
  }
  head[12] = static_cast<uint8_t>(name.size());
  head.insert(head.end(), name.begin(), name.end());
  std::vector<std::vector<uint8_t>> frames;
  size_t at = std::min(data.size(), kNabtsFramePayloadBytes - head.size());
  head.insert(head.end(), data.begin(),
              data.begin() + static_cast<ptrdiff_t>(at));
  uint16_t count = first_count;
  frames.push_back(sent(plain_frame(count++, object, 0, head)));
  while (at < data.size()) {
    const size_t length = std::min(kNabtsFramePayloadBytes, data.size() - at);
    std::vector<uint8_t> plain =
        plain_frame(count++, object, static_cast<uint32_t>(at),
                    std::vector<uint8_t>(
                        data.begin() + static_cast<ptrdiff_t>(at),
                        data.begin() + static_cast<ptrdiff_t>(at + length)));
    at += length;
    if (at == data.size()) {
      plain[7] = 0x45;  // the frame that ends the object
      plain[8] = 0x01;
    }
    frames.push_back(sent(std::move(plain)));
  }
  return frames;
}

/// |size| bytes of text.
std::vector<uint8_t> text_of(size_t size) {
  std::vector<uint8_t> out;
  for (size_t i = 0; out.size() < size; ++i) {
    const std::string line = "line " + std::to_string(i) + " of the file\n";
    out.insert(out.end(), line.begin(), line.end());
  }
  out.resize(size);
  return out;
}

TEST(NabtsFramedService, PutsAServicesFramesFromItsAddressesBackInOrder) {
  // Frames 0x100 to 0x113, the even ones on 0F0 and the odd on 0F1, taking
  // turns.
  std::vector<std::vector<uint8_t>> even;
  std::vector<std::vector<uint8_t>> odd;
  for (uint16_t n = 0x100; n < 0x114; ++n) {
    ((n & 1) != 0 ? odd : even).push_back(sent(plain_frame(n)));
  }
  const auto read =
      orc::nabts_framed_services({datacast_of(0x0F1, odd, {}, {}, 1, 2),
                                  datacast_of(0x0F0, even, {}, {}, 0, 2)});
  ASSERT_EQ(read.services.size(), 1u);
  const orc::NabtsFramedService& service = read.services[0];
  EXPECT_EQ(service.identifier, kService);
  EXPECT_EQ(service.channels, (std::vector<uint16_t>{0x0F0, 0x0F1}));
  EXPECT_EQ(service.frames, 20u);
  EXPECT_EQ(service.frames_lost, 0u);
  EXPECT_EQ(service.first_frame, 0x100);
  EXPECT_EQ(service.last_frame, 0x113);
  ASSERT_EQ(service.stream.size(), 20 * kNabtsFrameBytes);
  for (size_t i = 0; i < 20; ++i) {
    const uint8_t* f = &service.stream[i * kNabtsFrameBytes];
    EXPECT_EQ(f[5] | (f[6] << 8), 0x100 + static_cast<int>(i));
  }
  EXPECT_NE(service.summary.find("0F0-0F1"), std::string::npos);
}

TEST(NabtsFramedService, TakesThePadOffTheFrames) {
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t n = 0; n < 20; ++n) {
    frames.push_back(sent(plain_frame(n)));
  }
  const auto read = orc::nabts_framed_services({datacast_of(0x0E0, frames)});
  ASSERT_EQ(read.services.size(), 1u);
  const std::vector<uint8_t> plain = plain_frame(7);
  EXPECT_TRUE(std::equal(plain.begin(), plain.end(),
                         read.services[0].stream.begin() +
                             static_cast<ptrdiff_t>(7 * kNabtsFrameBytes)));
}

TEST(NabtsFramedService, CountsTheFramesSentAndNotRead) {
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t n = 0; n < 24; ++n) {
    if (n != 5 && n != 6) {
      frames.push_back(sent(plain_frame(n)));
    }
  }
  frames.push_back(sent(plain_frame(21)));  // sent again
  const auto read = orc::nabts_framed_services({datacast_of(0x0E0, frames)});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].frames, 22u);
  EXPECT_EQ(read.services[0].frames_lost, 2u);
  EXPECT_EQ(read.services[0].repeats, 1u);
}

TEST(NabtsFramedService, ReadsAFrameWithARowMissingInPart) {
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t n = 0; n < 20; ++n) {
    frames.push_back(sent(plain_frame(n)));
  }
  // A row of frame 2 lost: it is read without it, and left out of the
  // frames written whole.
  const auto read = orc::nabts_framed_services(
      {datacast_of(0x0E0, frames, {2 * kNabtsFrameRows + 7})});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].frames, 20u);
  EXPECT_EQ(read.services[0].frames_in_part, 1u);
  EXPECT_EQ(read.services[0].frames_lost, 0u);
  EXPECT_EQ(read.services[0].stream.size(), 19 * kNabtsFrameBytes);
}

TEST(NabtsFramedService, ReadsNoFrameWhoseHeaderRowWasLost) {
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t n = 0; n < 20; ++n) {
    frames.push_back(sent(plain_frame(n)));
  }
  const auto read = orc::nabts_framed_services(
      {datacast_of(0x0E0, frames, {2 * kNabtsFrameRows})});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].frames, 19u);
  EXPECT_EQ(read.services[0].frames_lost, 1u);
}

TEST(NabtsFramedService, ReadsNoFrameWhoseHeaderRowFailedItsCode) {
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t n = 0; n < 20; ++n) {
    frames.push_back(sent(plain_frame(n)));
  }
  const auto read = orc::nabts_framed_services(
      {datacast_of(0x0E0, frames, {}, {3 * kNabtsFrameRows})});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].frames, 19u);
}

TEST(NabtsFramedService, OpensAFrameOnlyWhereABundleStarts) {
  // A datacast in bundles of sixteen rows; frame 3's header row lost, and a
  // header row of another frame filed among its rows: no frame is read there,
  // and frame 4 still is.
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t n = 0; n < 20; ++n) {
    frames.push_back(sent(plain_frame(n)));
  }
  const std::vector<uint8_t> stray = sent(plain_frame(0x500));
  std::copy(stray.begin(), stray.begin() + kNabtsFrameRowBytes,
            frames[3].begin() + 7 * kNabtsFrameRowBytes);
  NabtsDatacastChannel datacast =
      datacast_of(0x0E0, frames, {3 * kNabtsFrameRows});
  datacast.layout.data_rows = kNabtsFrameRows;
  const auto read = orc::nabts_framed_services({datacast});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].frames, 19u);
  EXPECT_EQ(read.services[0].frames_lost, 1u);
  EXPECT_EQ(read.services[0].last_frame, 19);
}

TEST(NabtsFramedService, FollowsTheCountAcrossItsWrap) {
  std::vector<std::vector<uint8_t>> frames;
  for (uint32_t n = 0xFFF6; n < 0x10008; ++n) {
    frames.push_back(sent(plain_frame(static_cast<uint16_t>(n))));
  }
  const auto read = orc::nabts_framed_services({datacast_of(0x010, frames)});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].first_frame, 0xFFF6);
  EXPECT_EQ(read.services[0].last_frame, 0x0007);
  EXPECT_EQ(read.services[0].frames_lost, 0u);
}

TEST(NabtsFramedService, StartsANewRunWhereTheCountFallsBack) {
  // The count runs to 0x113, then starts again from zero: the frames between
  // are no frames lost.
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t n = 0x100; n < 0x114; ++n) {
    frames.push_back(sent(plain_frame(n)));
  }
  for (uint16_t n = 0; n < 20; ++n) {
    frames.push_back(sent(plain_frame(n)));
  }
  const auto read = orc::nabts_framed_services({datacast_of(0x010, frames)});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].frames, 40u);
  EXPECT_EQ(read.services[0].repeats, 0u);
  EXPECT_EQ(read.services[0].frames_lost, 0u);
  EXPECT_NE(read.services[0].summary.find("in 2 runs"), std::string::npos);
}

TEST(NabtsFramedService, ListsNoServiceFromAFewFrames) {
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t n = 0; n < orc::kNabtsFramedMinFrames - 1; ++n) {
    frames.push_back(sent(plain_frame(n)));
  }
  EXPECT_TRUE(orc::nabts_framed_services({datacast_of(0x0E0, frames)})
                  .services.empty());
}

TEST(NabtsFramedService, ListsNoServiceWhoseFramesAreScattered) {
  // Twenty frames, each 40 counts after the one before: no service sends so.
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t n = 0; n < 20; ++n) {
    frames.push_back(sent(plain_frame(static_cast<uint16_t>(40 * n))));
  }
  EXPECT_TRUE(orc::nabts_framed_services({datacast_of(0x0E0, frames)})
                  .services.empty());
}

TEST(NabtsFramedService, LeavesOutFramesReadOnAnotherBlockOfAddresses) {
  // The service on 0E0-0E1, and two of its frames read on 4E0 as well: an
  // address misread.
  std::vector<std::vector<uint8_t>> even;
  std::vector<std::vector<uint8_t>> odd;
  for (uint16_t n = 0x100; n < 0x114; ++n) {
    ((n & 1) != 0 ? odd : even).push_back(sent(plain_frame(n)));
  }
  const std::vector<std::vector<uint8_t>> stray = {sent(plain_frame(0x120)),
                                                   sent(plain_frame(0x121))};
  const auto read =
      orc::nabts_framed_services({datacast_of(0x0E0, even, {}, {}, 0, 2),
                                  datacast_of(0x0E1, odd, {}, {}, 1, 2),
                                  datacast_of(0x4E0, stray, {}, {}, 41, 1)});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].channels, (std::vector<uint16_t>{0x0E0, 0x0E1}));
  EXPECT_EQ(read.services[0].frames, 20u);
}

TEST(NabtsFramedService, PutsAnObjectTogetherAsTheFileItHolds) {
  // A file of 1000 bytes in three frames, the last cut short, sent six times.
  const std::vector<uint8_t> data = text_of(1000);
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t copy = 0; copy < 6; ++copy) {
    for (auto& f : object_frames(data, "TEST.TXT",
                                 static_cast<uint16_t>(0x40 + 3 * copy))) {
      frames.push_back(std::move(f));
    }
  }
  ASSERT_LT(frames.back().size(), kNabtsFrameBytes);
  const auto read = orc::nabts_framed_services({datacast_of(0x0F0, frames)});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].frames, 18u);
  EXPECT_EQ(read.services[0].objects, 1u);
  EXPECT_EQ(read.services[0].versions_whole, 1u);
  ASSERT_EQ(read.files.size(), 1u);
  const orc::NabtsCarriedFile& file = read.files[0];
  EXPECT_EQ(file.name, "TEST.TXT");
  EXPECT_EQ(file.time, "1998-07-09 16:00:00 UTC");
  EXPECT_EQ(file.transfer, "framed service");
  EXPECT_EQ(file.channel, 0x0F0);
  EXPECT_EQ(file.copies, 6u);
  EXPECT_EQ(file.format, "text");
  EXPECT_TRUE(file.data_present.empty());
  EXPECT_EQ(file.data, data);
}

TEST(NabtsFramedService, LeavesAHoleWhereAFrameOfAnObjectWasLost) {
  const std::vector<uint8_t> data = text_of(20 * kNabtsFramePayloadBytes);
  const auto frames = object_frames(data, "BIG.TXT", 0);
  // The header row of the object's sixth frame lost: the frame with it.
  const auto read = orc::nabts_framed_services(
      {datacast_of(0x0F0, frames, {5 * kNabtsFrameRows})});
  ASSERT_EQ(read.files.size(), 1u);
  const orc::NabtsCarriedFile& file = read.files[0];
  EXPECT_EQ(file.data.size(), data.size());
  EXPECT_EQ(file.bytes_lost(), kNabtsFramePayloadBytes);
  EXPECT_EQ(read.services[0].versions_whole, 0u);
  // The bytes either side of the hole are the file's.
  EXPECT_TRUE(std::equal(data.begin(), data.begin() + 100, file.data.begin()));
  EXPECT_EQ(file.data.back(), data.back());
}

TEST(NabtsFramedService, NamesAnObjectWhoseFirstFrameWasLostByItsNumber) {
  const std::vector<uint8_t> data = text_of(20 * kNabtsFramePayloadBytes);
  const auto frames = object_frames(data, "BIG.TXT", 0);
  const auto read =
      orc::nabts_framed_services({datacast_of(0x0F0, frames, {0})});
  ASSERT_EQ(read.files.size(), 1u);
  EXPECT_EQ(read.files[0].name, "object-0a05");
  EXPECT_TRUE(read.files[0].time.empty());
  EXPECT_EQ(read.files[0].data.size(), data.size());
  EXPECT_GT(read.files[0].bytes_lost(), 0u);
}

TEST(NabtsFramedService, TakesEachRowOfAnObjectFromACopyThatBroughtIt) {
  // The object sent twice, a row of its third frame lost the first time and
  // another row of it the second: whole from the two. A row of its fifth
  // frame lost both times is a hole of that row's bytes alone.
  const std::vector<uint8_t> data = text_of(9 * kNabtsFramePayloadBytes);
  std::vector<std::vector<uint8_t>> frames = object_frames(data, "TWO.TXT", 0);
  const size_t per_copy = frames.size();
  for (auto& f :
       object_frames(data, "TWO.TXT", static_cast<uint16_t>(per_copy))) {
    frames.push_back(std::move(f));
  }
  const size_t second = per_copy * kNabtsFrameRows;
  const auto read = orc::nabts_framed_services({datacast_of(
      0x0F0, frames,
      {2 * kNabtsFrameRows + 6, second + 2 * kNabtsFrameRows + 11,
       4 * kNabtsFrameRows + 9, second + 4 * kNabtsFrameRows + 9})});
  ASSERT_EQ(read.files.size(), 1u);
  EXPECT_EQ(read.files[0].bytes_lost(), kNabtsFrameRowBytes);
  EXPECT_EQ(read.services[0].versions, 1u);
  EXPECT_EQ(read.services[0].frames_in_part, 4u);
}

TEST(NabtsFramedService, KeepsEachVersionOfAnObjectSentAgainChanged) {
  // The object sent twice the same, then twice changed: two versions, each
  // whole, the first as first sent and the second as changed.
  const std::vector<uint8_t> first = text_of(4 * kNabtsFramePayloadBytes);
  const std::vector<uint8_t> second = [&first] {
    std::vector<uint8_t> changed = first;
    changed[700] = '#';
    return changed;
  }();
  std::vector<std::vector<uint8_t>> frames;
  uint16_t count = 0;
  for (const auto* data : {&first, &first, &second, &second}) {
    for (auto& f : object_frames(*data, "LIVE.TXT", count)) {
      frames.push_back(std::move(f));
      ++count;
    }
  }
  const auto read = orc::nabts_framed_services({datacast_of(0x0F0, frames)});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].objects, 1u);
  EXPECT_EQ(read.services[0].versions, 2u);
  EXPECT_EQ(read.services[0].versions_whole, 2u);
  ASSERT_EQ(read.files.size(), 2u);
  EXPECT_EQ(read.files[0].data, first);
  EXPECT_EQ(read.files[1].data, second);
  EXPECT_EQ(read.files[0].copies, 2u);
  EXPECT_EQ(read.files[1].name, "LIVE.TXT");
}

TEST(NabtsFramedService, UnpacksAFileSentInTheServicesContainer) {
  // The object is a file container: "WTF1", its address and a zlib stream of
  // "hello hello hello hello, framed world".
  const std::string address = "http://example.invalid/notes.txt";
  std::vector<uint8_t> container = {'W', 'T', 'F', '1'};
  container.resize(17, 0x01);
  container.push_back(static_cast<uint8_t>(address.size()));
  container.insert(container.end(), address.begin(), address.end());
  container.insert(container.end(), 14, 0x02);
  const std::vector<uint8_t> zlib = {
      0x78, 0xDA, 0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40,
      0x27, 0x75, 0x14, 0xD2, 0x8A, 0x12, 0x73, 0x53, 0x53, 0x14,
      0xCA, 0xF3, 0x8B, 0x72, 0x52, 0x00, 0x04, 0x5A, 0x0D, 0xB4};
  container.insert(container.end(), zlib.begin(), zlib.end());
  std::vector<std::vector<uint8_t>> frames;
  for (uint16_t copy = 0; copy < 16; ++copy) {
    for (auto& f : object_frames(container, "notes.wtf", copy)) {
      frames.push_back(std::move(f));
    }
  }
  const auto read = orc::nabts_framed_services({datacast_of(0x0F0, frames)});
  ASSERT_EQ(read.services.size(), 1u);
  EXPECT_EQ(read.services[0].containers, 1u);
  EXPECT_EQ(read.services[0].containers_unpacked, 1u);
  ASSERT_EQ(read.files.size(), 1u);
  EXPECT_EQ(read.files[0].name, "notes.txt");
  EXPECT_EQ(read.files[0].source, address);
  EXPECT_EQ(std::string(read.files[0].data.begin(), read.files[0].data.end()),
            "hello hello hello hello, framed world");
}

TEST(NabtsFramedService, JoinsAFileSentInNumberedParts) {
  // PART.000 to PART.002, each a whole object sent twice: joined into PART,
  // listed after them.
  std::vector<std::vector<uint8_t>> frames;
  std::vector<uint8_t> whole;
  uint16_t count = 0;
  for (uint16_t part = 0; part < 3; ++part) {
    std::vector<uint8_t> data = text_of(2 * kNabtsFramePayloadBytes);
    data.front() = static_cast<uint8_t>('A' + part);
    whole.insert(whole.end(), data.begin(), data.end());
    for (uint16_t copy = 0; copy < 2; ++copy) {
      for (auto& f :
           object_frames(data, "PART.00" + std::to_string(part), count,
                         static_cast<uint16_t>(kObject + part))) {
        frames.push_back(std::move(f));
        ++count;
      }
    }
  }
  const auto read = orc::nabts_framed_services({datacast_of(0x0F0, frames)});
  ASSERT_EQ(read.files.size(), 4u);
  EXPECT_EQ(read.files[3].name, "PART");
  EXPECT_EQ(read.files[3].joined_parts, 3u);
  EXPECT_EQ(read.files[3].data, whole);
}

TEST(NabtsFramedService, PassesOverACopyWithARowTakenAsReceived) {
  // The object sent twice; the first time a row of its third frame failed
  // its code and arrived wrong, the second time it arrived whole.
  const std::vector<uint8_t> data = text_of(9 * kNabtsFramePayloadBytes);
  std::vector<std::vector<uint8_t>> frames = object_frames(data, "TWO.TXT", 0);
  const size_t per_copy = frames.size();
  frames[2][5 * kNabtsFrameRowBytes + 4] ^= 0xFF;
  for (auto& f :
       object_frames(data, "TWO.TXT", static_cast<uint16_t>(per_copy))) {
    frames.push_back(std::move(f));
  }
  const auto read = orc::nabts_framed_services(
      {datacast_of(0x0F0, frames, {}, {2 * kNabtsFrameRows + 5})});
  ASSERT_EQ(read.files.size(), 1u);
  EXPECT_TRUE(read.files[0].data_present.empty());
  EXPECT_EQ(read.files[0].data, data);
  EXPECT_EQ(read.files[0].copies, 2u);
}

}  // namespace
}  // namespace orc_unit_test
