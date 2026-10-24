/*
 * File:        naplps_reconstruct_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for NAPLPS record reconstruction: filling what a
 *              recording lost, and reading what is left around it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_reconstruct.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace orc {
namespace {

// X3.110 §4.3.3 locking shifts, and the PDI opcodes the tests write.
constexpr uint8_t kSo = 0x0E;
constexpr uint8_t kSi = 0x0F;
constexpr uint8_t kPolyFilled = 0x35;
constexpr uint8_t kSetColour = 0x3C;
constexpr uint8_t kSelectColour = 0x3E;

/// A record as the recovery leaves it: the bytes, with [start, end) lost.
struct Recovered {
  std::vector<uint8_t> data;
  std::vector<uint8_t> present;
};
Recovered losing(std::vector<uint8_t> data, size_t start, size_t end) {
  std::vector<uint8_t> present(data.size(), 1);
  for (size_t i = start; i < end; ++i) {
    data[i] = 0;
    present[i] = 0;
  }
  return {std::move(data), std::move(present)};
}

void append(std::vector<uint8_t>& out, const std::string& text) {
  out.insert(out.end(), text.begin(), text.end());
}

/// A palette in PDI data, a line of text and a filled polygon of |vertices|
/// three-byte displacements: the shape of the pages a service sends.
std::vector<uint8_t> page(size_t vertices, uint8_t tint = 0) {
  std::vector<uint8_t> out{kSo};
  for (uint8_t entry = 0; entry < 8; ++entry) {
    out.insert(
        out.end(),
        {kSelectColour, static_cast<uint8_t>(0x40 + 4 * entry), kSetColour,
         0x7F, static_cast<uint8_t>(0x40 + entry + tint), 0x40});
  }
  out.push_back(kSi);
  append(out, "TODAY'S WEATHER: SUNNY AND WARM THROUGH THE WEEKEND");
  out.push_back(kSo);
  out.push_back(kPolyFilled);
  for (size_t v = 0; v < vertices; ++v) {
    out.insert(out.end(), {0x41, static_cast<uint8_t>(0x40 + (v % 7)), 0x42});
  }
  out.push_back(kSi);
  return out;
}

NaplpsDialectModel model_of(std::initializer_list<size_t> vertex_counts) {
  NaplpsDialectModel model;
  for (size_t vertices : vertex_counts) {
    model.learn(page(vertices), {});
  }
  return model;
}

size_t count(const std::vector<NaplpsByteOrigin>& origin,
             NaplpsByteOrigin kind) {
  return static_cast<size_t>(std::count(origin.begin(), origin.end(), kind));
}

TEST(NaplpsReconstruct, LeavesARecordThatLostNothingAsItIs) {
  const std::vector<uint8_t> data = page(10);
  const NaplpsReconstruction out =
      naplps_reconstruct_record(data, {}, nullptr, 0, nullptr);
  EXPECT_EQ(out.data, data);
  EXPECT_FALSE(out.summary.changed());
  EXPECT_EQ(count(out.origin, NaplpsByteOrigin::kRecorded), data.size());
}

TEST(NaplpsReconstruct, FillsFromTheLibraryAndSaysWhereFrom) {
  const std::vector<uint8_t> sent = page(40);
  NabtsRecordLibrary library;
  library.add({"reference", "000-051-v0", sent, {}});
  library.build();

  // The palette and the text both survive; the polygon loses 27 bytes.
  const size_t start = sent.size() - 80;
  const Recovered recovered = losing(sent, start, start + 27);
  const NaplpsReconstruction out = naplps_reconstruct_record(
      recovered.data, recovered.present, &library, SIZE_MAX, nullptr);

  EXPECT_EQ(out.data, sent);
  EXPECT_EQ(count(out.origin, NaplpsByteOrigin::kFilled), 27u);
  EXPECT_EQ(out.summary.holes, 1u);
  EXPECT_EQ(out.summary.holes_filled, 1u);
  EXPECT_EQ(out.summary.bytes_filled, 27u);
  EXPECT_EQ(out.summary.sources, std::vector<std::string>{"reference"});
}

// What arrived around a loss is read as it arrived — as a receiver reads on
// past a lost packet's filler (X3.110 §6.1.6.1) — unless the bytes after it
// call for a shift: a vertex list that lost part of a vertex reads on.
TEST(NaplpsReconstruct, LeavesWhatArrivedAroundALossAsItArrived) {
  const NaplpsDialectModel model = model_of({60, 70, 80});
  const std::vector<uint8_t> sent = page(40);  // A 120-byte vertex list.
  const size_t list = sent.size() - 1 - 120;
  // Lost: 13 bytes from part way into the fifth vertex.
  const Recovered recovered = losing(sent, list + 14, list + 27);
  const NaplpsReconstruction out = naplps_reconstruct_record(
      recovered.data, recovered.present, nullptr, 0, &model);

  EXPECT_EQ(out.data, recovered.data);
  EXPECT_EQ(count(out.origin, NaplpsByteOrigin::kLost), 13u);
  EXPECT_EQ(count(out.origin, NaplpsByteOrigin::kRecorded), sent.size() - 13);
  EXPECT_FALSE(out.summary.changed());
}

// The loss took the SI before a line of text: read on as PDI data, the text
// would be opcodes and operands, which is what the model can tell.
TEST(NaplpsReconstruct, PutsBackAShiftTheLossTook) {
  const NaplpsDialectModel model = model_of({10, 12, 14});
  std::vector<uint8_t> sent{kSo,  kSelectColour, 0x44, kSetColour,
                            0x7F, 0x41,          0x40, kSi};
  append(sent, "SUNNY AND WARM THROUGH THE WEEKEND");
  const Recovered recovered = losing(sent, 6, 10);  // "0x40 SI SU"
  const NaplpsReconstruction out = naplps_reconstruct_record(
      recovered.data, recovered.present, nullptr, 0, &model);

  EXPECT_EQ(out.origin[9], NaplpsByteOrigin::kInferred);
  EXPECT_EQ(out.data[9] & 0x7F, kSi);
  EXPECT_EQ(out.summary.bytes_inferred, 1u);
}

TEST(NaplpsReconstruct, PutsNoShiftWhereTheBytesReadTheWayTheyWould) {
  const NaplpsDialectModel model = model_of({10, 12, 14});
  std::vector<uint8_t> sent{kSi};
  append(sent, "SUNNY AND WARM THROUGH THE WEEKEND AND INTO MONDAY");
  const Recovered recovered = losing(sent, 10, 14);
  const NaplpsReconstruction out = naplps_reconstruct_record(
      recovered.data, recovered.present, nullptr, 0, &model);
  EXPECT_EQ(out.summary.bytes_inferred, 0u);
  EXPECT_FALSE(out.summary.changed());
}

TEST(NaplpsReconstruct, OnlyFillsWhenReadingHolesIsOff) {
  const NaplpsDialectModel model = model_of({10});
  const std::vector<uint8_t> sent = page(40);
  const size_t list = sent.size() - 1 - 120;
  const Recovered recovered = losing(sent, list + 14, list + 27);
  NaplpsReconstructionOptions options;
  options.read_holes = false;
  const NaplpsReconstruction out = naplps_reconstruct_record(
      recovered.data, recovered.present, nullptr, 0, &model, options);
  EXPECT_EQ(out.data, recovered.data);
  EXPECT_EQ(count(out.origin, NaplpsByteOrigin::kLost), 13u);
}

// ---------------------------------------------------------------------------
// Bytes received wrong
// ---------------------------------------------------------------------------

/// |data| with b8 set where that gives each byte the odd parity CEA-516 §3.3
/// gives every data byte of a teletext record: a record as transmitted.
std::vector<uint8_t> with_parity(std::vector<uint8_t> data) {
  for (uint8_t& byte : data) {
    int ones = 0;
    for (int bit = 0; bit < 7; ++bit) ones += (byte >> bit) & 1;
    byte = static_cast<uint8_t>((byte & 0x7F) | (ones % 2 == 0 ? 0x80 : 0));
  }
  return data;
}

/// A library holding |records|, each from another recording.
NabtsRecordLibrary library_of(
    std::initializer_list<std::vector<uint8_t>> records) {
  NabtsRecordLibrary library;
  for (const std::vector<uint8_t>& record : records) {
    library.add(
        NabtsLibraryRecord{"another recording", "000-001-v0", record, {}});
  }
  library.build();
  return library;
}

// A byte that fails its parity is known to be wrong. Where another recording
// has the same bytes all round it, what it has in its place is what was sent.
TEST(NaplpsReconstruct, CorrectsAByteReceivedFailingParity) {
  const std::vector<uint8_t> sent = with_parity(page(12));
  const NabtsRecordLibrary library = library_of({sent});
  std::vector<uint8_t> received = sent;
  constexpr size_t kWrong = 70;  // inside the line of text
  received[kWrong] ^= 0x01;      // one bit: parity fails

  const NaplpsReconstruction out = naplps_reconstruct_record(
      received, {}, &library, library.records().size(), nullptr);
  EXPECT_EQ(out.data, sent);
  EXPECT_EQ(out.origin[kWrong], NaplpsByteOrigin::kCorrected);
  EXPECT_EQ(out.summary.bytes_corrupt, 1u);
  EXPECT_EQ(out.summary.bytes_corrected, 1u);
  EXPECT_TRUE(out.summary.changed());
  EXPECT_EQ(out.summary.sources, std::vector<std::string>{"another recording"});
}

// Nothing is guessed: with no record agreeing round it, a byte received wrong
// stays as received, and is counted.
TEST(NaplpsReconstruct, LeavesAWrongByteNoRecordVouchesFor) {
  const std::vector<uint8_t> sent = with_parity(page(12));
  std::vector<uint8_t> other = with_parity(page(12, /*tint=*/3));
  std::fill(other.begin() + 50, other.begin() + 110, with_parity({'#'})[0]);
  const NabtsRecordLibrary library = library_of({other});
  std::vector<uint8_t> received = sent;
  received[70] ^= 0x01;

  const NaplpsReconstruction out = naplps_reconstruct_record(
      received, {}, &library, library.records().size(), nullptr);
  EXPECT_EQ(out.data, received);
  EXPECT_EQ(out.summary.bytes_corrupt, 1u);
  EXPECT_EQ(out.summary.bytes_corrected, 0u);
  EXPECT_FALSE(out.summary.changed());
}

// A byte that passed its parity is what arrived, and a record that differs
// from another there is a record that differs: only a byte proved wrong is
// ever replaced.
TEST(NaplpsReconstruct, NeverReplacesAByteThatPassedItsParity) {
  const std::vector<uint8_t> sent = with_parity(page(12));
  const NabtsRecordLibrary library = library_of({sent});
  std::vector<uint8_t> received = sent;
  received[70] ^= 0x03;  // two bits: parity passes

  const NaplpsReconstruction out = naplps_reconstruct_record(
      received, {}, &library, library.records().size(), nullptr);
  EXPECT_EQ(out.data, received);
  EXPECT_EQ(out.summary.bytes_corrupt, 0u);
}

// The correction is a layer of its own, and off leaves the bytes as received.
TEST(NaplpsReconstruct, CorrectsNothingWhenCorrectionIsOff) {
  const std::vector<uint8_t> sent = with_parity(page(12));
  const NabtsRecordLibrary library = library_of({sent});
  std::vector<uint8_t> received = sent;
  received[70] ^= 0x01;

  NaplpsReconstructionOptions options;
  options.correct = false;
  const NaplpsReconstruction out = naplps_reconstruct_record(
      received, {}, &library, library.records().size(), nullptr, options);
  EXPECT_EQ(out.data, received);
  EXPECT_EQ(out.summary.bytes_corrected, 0u);
}

}  // namespace
}  // namespace orc
