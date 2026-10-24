/*
 * File:        nabts_salvage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the salvage of groups whose synchronizing packet
 *              was lost
 *
 * Covers: a loose run found in a listed record fills only what no copy of it
 * delivered; runs no record holds make up a page listed without its identity
 * once two of them agree, or one clean enough stands alone; nothing is
 * salvaged on a channel no record was named on; and the runs placed nowhere
 * are kept, and written as text as they arrived.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_salvage.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nabts_test_builders.h"

namespace orc_unit_test {
namespace {

using nabts::parity;

constexpr size_t kBlock = 28;

/// |count| odd-parity bytes of printable text that no six of repeat: a page's
/// bytes as far as finding where a run sits in them goes.
std::vector<uint8_t> page_bytes(uint32_t seed, size_t count) {
  std::vector<uint8_t> out;
  out.reserve(count);
  uint32_t state = seed;
  for (size_t i = 0; i < count; ++i) {
    state = state * 1103515245u + 12345u;
    out.push_back(parity(static_cast<uint8_t>(0x20 + ((state >> 16) % 0x5F))));
  }
  return out;
}

/// A listed record on |channel| holding |data|, named as transmitted, with the
/// bytes from |hole_from| to |hole_to| lost.
orc::NabtsCataloguedRecord listed(uint16_t channel,
                                  const std::vector<uint8_t>& data,
                                  size_t hole_from, size_t hole_to) {
  orc::NabtsCataloguedRecord record;
  record.channel = channel;
  record.address_text = "013";
  record.times_seen = 4;
  record.times_attested = 4;
  record.data = data;
  record.data_present.assign(data.size(), orc::kNabtsByteArrived);
  for (size_t i = hole_from; i < hole_to; ++i) {
    record.data[i] = 0;
    record.data_present[i] = orc::kNabtsByteLost;
  }
  return record;
}

/// A loose run on |channel| holding |bytes|, every one arrived.
orc::NabtsLooseRun loose(uint16_t channel, const std::vector<uint8_t>& bytes) {
  orc::NabtsLooseRun run;
  run.channel = channel;
  run.data = bytes;
  run.present.assign(bytes.size(), orc::kNabtsByteArrived);
  run.confidence.assign(bytes.size(), 200);
  run.block_bytes = kBlock;
  run.packets = static_cast<uint32_t>(bytes.size() / kBlock);
  run.first_continuity = 1;
  return run;
}

/// The stretch of |data| from |from|, |count| bytes long.
std::vector<uint8_t> stretch(const std::vector<uint8_t>& data, size_t from,
                             size_t count) {
  return std::vector<uint8_t>(
      data.begin() + static_cast<std::ptrdiff_t>(from),
      data.begin() + static_cast<std::ptrdiff_t>(from + count));
}

// A group whose start was lost kept the blocks its copies lost, where a
// recording loses one line in every field: the run fills the record's hole,
// and the bytes the vote settled stand as they were.
TEST(NabtsSalvage, FillsOnlyTheHolesOfTheRecordARunIsACopyOf) {
  const std::vector<uint8_t> truth = page_bytes(7, 6 * kBlock);
  std::vector<orc::NabtsCataloguedRecord> records{
      listed(0x000, truth, 2 * kBlock, 3 * kBlock)};
  // A byte the vote settled differently from what the run carries.
  records[0].data[kBlock + 3] = truth[kBlock + 3];
  std::vector<uint8_t> run_bytes = stretch(truth, kBlock, 3 * kBlock);
  run_bytes[3] = parity(static_cast<uint8_t>((truth[kBlock + 3] + 1) & 0x7F));

  orc::NabtsSalvage salvage;
  salvage.add(loose(0x000, run_bytes), 10);
  const orc::NabtsSalvageStats stats = salvage.apply(records);

  ASSERT_EQ(records.size(), 1u) << "no page of its own";
  EXPECT_EQ(stats.runs_matched, 1u);
  EXPECT_EQ(stats.bytes_filled, kBlock);
  EXPECT_EQ(stats.records_filled, 1u);
  for (size_t i = 2 * kBlock; i < 3 * kBlock; ++i) {
    EXPECT_EQ(records[0].data[i], truth[i]) << "byte " << i;
    EXPECT_NE(records[0].data_present[i], orc::kNabtsByteLost);
  }
  EXPECT_EQ(records[0].data[kBlock + 3], truth[kBlock + 3])
      << "a byte the vote settled is not the run's to change";
  EXPECT_EQ(records[0].bytes_salvaged, kBlock);
}

// Two runs of a page no record holds agree with each other, and are listed as
// a page of their own: its channel known, its identity not, its head lost.
TEST(NabtsSalvage, ListsAPageNoRecordHoldsWithoutItsIdentity) {
  std::vector<orc::NabtsCataloguedRecord> records{
      listed(0x000, page_bytes(7, 6 * kBlock), 6 * kBlock, 6 * kBlock)};
  std::vector<uint8_t> page = page_bytes(99, 5 * kBlock);
  page[5] = parity(0x0F);  // SI: the bytes before it were PDI
  orc::NabtsSalvage salvage;
  salvage.add(loose(0x000, stretch(page, 0, 4 * kBlock)), 10);
  salvage.add(loose(0x000, stretch(page, kBlock, 4 * kBlock)), 900);
  const orc::NabtsSalvageStats stats = salvage.apply(records);

  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(stats.pages_found, 1u);
  EXPECT_EQ(stats.runs_in_pages, 2u);
  const orc::NabtsCataloguedRecord& found = records[1];
  EXPECT_TRUE(found.identity_lost);
  EXPECT_EQ(found.channel, 0x000u);
  EXPECT_GE(found.address, orc::kNabtsIdentityLostAddressBase);
  EXPECT_EQ(found.address_text, "LOST1");
  EXPECT_EQ(found.times_seen, 2u);
  EXPECT_EQ(found.first_seen_frame, 10u);
  EXPECT_EQ(found.last_seen_frame, 900u);
  ASSERT_GT(found.data.size(), orc::kNabtsMaxDataBlockBytes);
  for (size_t i = 0; i < orc::kNabtsMaxDataBlockBytes; ++i) {
    EXPECT_EQ(found.data_present[i], orc::kNabtsByteLost)
        << "the head went with the synchronizing packet";
  }
  // The first shift the page holds is SI, so what comes before it was
  // picture description code: the page starts with the SO it followed,
  // supplied and marked as not received, and keeps every byte that arrived.
  EXPECT_EQ(found.data[orc::kNabtsMaxDataBlockBytes], 0x0E);
  EXPECT_EQ(found.data_present[orc::kNabtsMaxDataBlockBytes],
            orc::kNabtsByteLost);
  EXPECT_TRUE(std::equal(
      page.begin(), page.end(),
      found.data.begin() +
          static_cast<std::ptrdiff_t>(orc::kNabtsMaxDataBlockBytes + 1)));
}

// A page whose first shift is SO was in the primary set before it, which is
// where a page starts anyway: nothing is supplied.
TEST(NabtsSalvage, SuppliesNoShiftWhereThePageResumesInText) {
  std::vector<orc::NabtsCataloguedRecord> records{
      listed(0x000, page_bytes(7, 6 * kBlock), 6 * kBlock, 6 * kBlock)};
  std::vector<uint8_t> page = page_bytes(99, 5 * kBlock);
  page[5] = parity(0x0E);  // SO
  page[30] = parity(0x0F);
  orc::NabtsSalvage salvage;
  salvage.add(loose(0x000, stretch(page, 0, 4 * kBlock)), 10);
  salvage.add(loose(0x000, stretch(page, kBlock, 4 * kBlock)), 900);
  salvage.apply(records);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_TRUE(std::equal(
      page.begin(), page.end(),
      records[1].data.begin() +
          static_cast<std::ptrdiff_t>(orc::kNabtsMaxDataBlockBytes)));
}

// One run no record holds that fails its parity in places is as likely a
// misaddressed fragment as a page: it is not listed, and is kept as it
// arrived.
TEST(NabtsSalvage, ListsNoPageFromOneRunFailingItsParity) {
  std::vector<orc::NabtsCataloguedRecord> records{
      listed(0x000, page_bytes(7, 6 * kBlock), 6 * kBlock, 6 * kBlock)};
  std::vector<uint8_t> bytes = page_bytes(99, 9 * kBlock);
  for (size_t i = 0; i < bytes.size(); i += 5) {
    bytes[i] ^= 0x80;  // one byte in five fails its parity
  }
  orc::NabtsSalvage salvage;
  salvage.add(loose(0x000, bytes), 10);
  const orc::NabtsSalvageStats stats = salvage.apply(records);
  EXPECT_EQ(records.size(), 1u);
  EXPECT_EQ(stats.runs_unmatched, 1u);
  EXPECT_EQ(stats.pages_found, 0u);
  ASSERT_EQ(salvage.unplaced().size(), 1u);
  EXPECT_EQ(salvage.unplaced()[0].frame, 10u);
  EXPECT_EQ(salvage.unplaced()[0].run.data, bytes);
}

// A run of three packets that passes its parity throughout carries its own
// evidence as well as a long one does.
TEST(NabtsSalvage, ListsAShortCleanRunAsAPageOfItsOwn) {
  std::vector<orc::NabtsCataloguedRecord> records{
      listed(0x000, page_bytes(7, 6 * kBlock), 6 * kBlock, 6 * kBlock)};
  orc::NabtsSalvage salvage;
  salvage.add(loose(0x000, page_bytes(99, 3 * kBlock)), 10);
  const orc::NabtsSalvageStats stats = salvage.apply(records);
  EXPECT_EQ(stats.pages_found, 1u);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_TRUE(records[1].identity_lost);
  EXPECT_TRUE(salvage.unplaced().empty());
}

// A long run that passes its parity throughout carries its own evidence.
TEST(NabtsSalvage, ListsALongCleanRunAsAPageOfItsOwn) {
  std::vector<orc::NabtsCataloguedRecord> records{
      listed(0x000, page_bytes(7, 6 * kBlock), 6 * kBlock, 6 * kBlock)};
  orc::NabtsSalvage salvage;
  salvage.add(loose(0x000, page_bytes(99, 9 * kBlock)), 10);
  const orc::NabtsSalvageStats stats = salvage.apply(records);
  EXPECT_EQ(stats.pages_found, 1u);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_TRUE(records[1].identity_lost);
}

// A channel no record was named on has nothing to salvage into: runs there
// are left alone, not listed as a channel the service never used.
TEST(NabtsSalvage, LeavesAChannelNoRecordWasNamedOn) {
  std::vector<orc::NabtsCataloguedRecord> records{
      listed(0x000, page_bytes(7, 6 * kBlock), 6 * kBlock, 6 * kBlock)};
  orc::NabtsSalvage salvage;
  const std::vector<uint8_t> page = page_bytes(99, 9 * kBlock);
  salvage.add(loose(0x200, page), 10);
  salvage.add(loose(0x200, page), 20);
  const orc::NabtsSalvageStats stats = salvage.apply(records);
  EXPECT_EQ(records.size(), 1u);
  EXPECT_EQ(stats.pages_found, 0u);
  EXPECT_EQ(stats.runs_matched, 0u);
  EXPECT_EQ(salvage.unplaced().size(), 2u) << "kept as they arrived";
}

// The runs placed nowhere, as text: each run's channel, frame and packets,
// then its bytes as they arrived — characters, the shifts by name, '~' for a
// byte failing its parity and '_' for one of a packet that was lost.
TEST(NabtsSalvage, WritesTheRunsPlacedNowhereAsText) {
  orc::NabtsHeldRun held;
  held.frame = 4321;
  held.run = loose(0x2A0, {parity('H'), parity('I'), parity(0x0E), parity('A'),
                           static_cast<uint8_t>(parity('B') ^ 0x80), 0,
                           parity(0x0F), parity('!')});
  held.run.present[5] = orc::kNabtsByteLost;
  held.run.packets = 3;
  const std::string text = orc::nabts_unplaced_runs_text({held});
  EXPECT_NE(text.find("Channel 2A0, ended in frame 4321: 3 packets, 8 bytes "
                      "(1 lost)"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("  HI{SO}A~_{SI}!"), std::string::npos) << text;
  EXPECT_TRUE(orc::nabts_unplaced_runs_text({}).empty());
}

// A run that agrees with a record on the template two pages share, and on
// nothing after, is not taken for a copy of it.
TEST(NabtsSalvage, TakesNoRunForACopyOnASharedTemplateAlone) {
  const std::vector<uint8_t> template_bytes = page_bytes(5, 2 * kBlock);
  std::vector<uint8_t> listed_page = template_bytes;
  const std::vector<uint8_t> listed_story = page_bytes(7, 6 * kBlock);
  listed_page.insert(listed_page.end(), listed_story.begin(),
                     listed_story.end());
  std::vector<orc::NabtsCataloguedRecord> records{
      listed(0x000, listed_page, 4 * kBlock, 5 * kBlock)};
  std::vector<uint8_t> other_page = template_bytes;
  const std::vector<uint8_t> other_story = page_bytes(11, 6 * kBlock);
  other_page.insert(other_page.end(), other_story.begin(), other_story.end());

  orc::NabtsSalvage salvage;
  salvage.add(loose(0x000, other_page), 10);
  const orc::NabtsSalvageStats stats = salvage.apply(records);
  EXPECT_EQ(stats.runs_matched, 0u);
  EXPECT_EQ(stats.bytes_filled, 0u);
}

// The packets of a synchronizing packet whose header did not decode start with
// that header, which is no record data and is never compared or kept.
TEST(NabtsSalvage, SetsAsideTheHeaderARunStartsWith) {
  std::vector<uint8_t> truth = page_bytes(7, 6 * kBlock);
  // No record header can be read from the record's first byte, so only the
  // group header is set aside.
  truth[0] = parity(0x41);
  std::vector<orc::NabtsCataloguedRecord> records{
      listed(0x000, truth, kBlock, 2 * kBlock)};
  std::vector<uint8_t> run_bytes(orc::kNabtsGroupHeaderBytes, parity(0x41));
  const std::vector<uint8_t> body = stretch(truth, 0, 3 * kBlock);
  run_bytes.insert(run_bytes.end(), body.begin(), body.end());
  orc::NabtsLooseRun run = loose(0x000, run_bytes);
  run.starts_synchronizing = true;
  orc::NabtsSalvage salvage;
  salvage.add(run, 10);
  const orc::NabtsSalvageStats stats = salvage.apply(records);
  EXPECT_EQ(stats.runs_matched, 1u);
  EXPECT_EQ(stats.bytes_filled, kBlock);
}

}  // namespace
}  // namespace orc_unit_test
