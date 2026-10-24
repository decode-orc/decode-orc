/*
 * File:        nabts_caption_stream_test.cpp
 * Module:      orc-core-tests
 * Purpose:     The caption service kept as it arrived (CEA-516 §7.3.10)
 *
 * Covers: a caption is a run of arrivals of one version at one address, voted
 * only within itself and dated by its first copy; the catalogue hands caption
 * arrivals to the stream and keeps everything else; which arrivals count as
 * the caption service when a header had to be corrected; the stream's bound;
 * and the cues read off it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_caption_stream.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nabts_record_catalogue.h"
#include "nabts_test_builders.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc_unit_test {
namespace {

/// §7.1.5: channel A00, address 000 is the start of captioning.
constexpr uint16_t kCaptionChannel = 0xA00;

/// |text| as record data: seven-bit characters with the odd parity §3.3 gives
/// every data byte of a type-zero group.
std::vector<uint8_t> text_data(const std::string& text) {
  std::vector<uint8_t> out;
  out.reserve(text.size());
  for (const char letter : text) {
    out.push_back(nabts::parity(static_cast<uint8_t>(letter)));
  }
  return out;
}

/**
 * @brief One arrival of a caption record, as the record assembler delivers it
 *
 * Non-cyclic (§5.2.2.3), with the Caption and Update Flags §7.3.10.1 has every
 * caption record carry, and its identity arrived as transmitted.
 */
orc::NabtsMessage caption(uint8_t version, const std::string& text,
                          bool intact = true) {
  orc::NabtsMessage out;
  out.channel = kCaptionChannel;
  out.address.value = 0;
  out.address.long_form = false;
  out.type = orc::kNabtsRecordTypeNoncyclicPresentation;
  out.version = version;
  out.classification.present = true;
  out.classification.caption = true;
  out.classification.update = true;
  out.classification.version_present = true;
  out.classification.version = version;
  out.data = text_data(text);
  out.records = 1;
  out.links_expected = 1;
  out.complete = true;
  out.intact = intact;
  out.aligned = true;
  out.identity_attested = true;
  return out;
}

/// |message| with the byte at |position| replaced by one failing its parity —
/// a copy damaged there, as a recording damages one.
orc::NabtsMessage damaged_at(orc::NabtsMessage message, size_t position) {
  message.intact = false;
  message.data[position] = static_cast<uint8_t>(message.data[position] ^ 0x01);
  return message;
}

/// The captions with their pages presented, as the viewer and the export both
/// read them.
std::vector<orc::NabtsCataloguedRecord> presented(
    std::vector<orc::NabtsCataloguedRecord> captions) {
  orc::nabts_interpret_records(captions, orc::kNaplpsGridReference);
  return captions;
}

// §7.3.10.1 gives every new caption the next version, and Y16 has sixteen, so a
// service reuses versions constantly. Twenty captions cycling four versions are
// twenty captions, each with its own words — not four records voted together.
TEST(NabtsCaptionStream, TwentyCaptionsCyclingFourVersionsAreTwentyCues) {
  orc::NabtsRecordCatalogue catalogue;
  uint64_t frame = 0;
  for (int index = 0; index < 20; ++index) {
    const uint8_t version = static_cast<uint8_t>(index % 4);
    const std::string text = "CAPTION " + std::to_string(index);
    // Each caption repeated to aid error recovery, as §7.1.3 has it.
    catalogue.merge(caption(version, text), frame);
    catalogue.merge(caption(version, text), frame + 2);
    frame += 30;
  }

  EXPECT_TRUE(catalogue.records().empty())
      << "the caption service entered the version-keyed catalogue";
  const auto captions = presented(catalogue.captions());
  ASSERT_EQ(captions.size(), 20u);

  const auto cues = orc::nabts_caption_cues(captions);
  ASSERT_EQ(cues.size(), 20u);
  for (size_t index = 0; index < cues.size(); ++index) {
    EXPECT_EQ(cues[index].text, "CAPTION " + std::to_string(index));
    // Dated by the caption's first copy, which is when a receiver would first
    // have shown it.
    EXPECT_EQ(cues[index].start_frame, index * 30u);
    EXPECT_EQ(captions[index].times_seen, 2u);
  }
}

// A caption damaged in one run must not vote with a later caption that happens
// to carry the same version: the two share a number and nothing else.
TEST(NabtsCaptionStream, ACopyOfOneCaptionNeverVotesInAnother) {
  orc::NabtsCaptionStream stream;
  const std::string first = "THE FIRST CAPTION READS SO";
  const std::string third = "A THIRD ONE UNDER VERSION 1";

  // Version 1, three damaged copies, each wrong somewhere else.
  stream.add(damaged_at(caption(1, first), 0), 10);
  stream.add(damaged_at(caption(1, first), 5), 12);
  stream.add(damaged_at(caption(1, first), 9), 14);
  // Version 2, then version 1 again for a different caption: two damaged
  // copies, outnumbered by the first caption's three were they pooled.
  stream.add(caption(2, "SECOND"), 40);
  stream.add(damaged_at(caption(1, third), 2), 70);
  stream.add(damaged_at(caption(1, third), 7), 72);

  const auto captions = stream.captions();
  ASSERT_EQ(captions.size(), 3u);
  EXPECT_EQ(captions[0].data, text_data(first))
      << "the first caption was not voted back from its own copies";
  EXPECT_EQ(captions[0].copies_voted, 3u);
  EXPECT_EQ(captions[2].data, text_data(third))
      << "the third caption's vote took in copies of another caption";
  EXPECT_EQ(captions[2].copies_voted, 2u);
  EXPECT_EQ(captions[2].first_seen_frame, 70u);
  EXPECT_EQ(captions[2].version, 1u);
}

// One copy of a caption filed under a version misread in its Hamming 8/4 byte
// splits the run, but it reads the same words, so the cue carries on.
TEST(NabtsCaptionStream, ACopyUnderAMisreadVersionDoesNotSplitTheCue) {
  orc::NabtsCaptionStream stream;
  stream.add(caption(5, "STILL THE SAME"), 100);
  auto misread = caption(7, "STILL THE SAME");
  misread.identity_attested = false;
  stream.add(misread, 102);
  stream.add(caption(5, "STILL THE SAME"), 104);
  stream.add(caption(6, "NEXT"), 200);

  const auto captions = presented(stream.captions());
  ASSERT_EQ(captions.size(), 4u);
  const auto cues = orc::nabts_caption_cues(captions);
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0].text, "STILL THE SAME");
  EXPECT_EQ(cues[0].start_frame, 100u);
  EXPECT_EQ(cues[0].end_frame, 200u);
  EXPECT_EQ(cues[1].text, "NEXT");
}

// Two caption services interleave — §7.3.10.1 allows one per language — and a
// caption at one address does not end the caption at the other.
TEST(NabtsCaptionStream, CaptionsAtTwoAddressesAreRunsOfTheirOwn) {
  orc::NabtsCaptionStream stream;
  auto other = [](uint8_t version, const std::string& text) {
    auto message = caption(version, text);
    message.address.value = 0x00100;  // short address 001
    return message;
  };
  stream.add(caption(1, "ENGLISH ONE"), 10);
  stream.add(other(1, "FRANCAIS UN"), 11);
  stream.add(caption(1, "ENGLISH ONE"), 12);
  stream.add(other(2, "FRANCAIS DEUX"), 40);
  stream.add(caption(2, "ENGLISH TWO"), 41);

  const auto captions = stream.captions();
  ASSERT_EQ(captions.size(), 4u);
  EXPECT_EQ(captions[0].data, text_data("ENGLISH ONE"));
  EXPECT_EQ(captions[0].times_seen, 2u);
  EXPECT_EQ(captions[1].data, text_data("FRANCAIS UN"));
  EXPECT_EQ(captions[2].data, text_data("FRANCAIS DEUX"));
  EXPECT_EQ(captions[3].data, text_data("ENGLISH TWO"));
}

// A cyclic page whose header had to be corrected and reads the Caption Flag is
// a page with a misread bit, not a caption: it stays in the catalogue, where
// the flag is voted from its appearances.
TEST(NabtsCaptionStream, APageWithACorrectedCaptionFlagStaysACatalogueRecord) {
  orc::NabtsRecordCatalogue catalogue;
  auto page = caption(0, "A PAGE");
  page.channel = 0x000;
  page.address.value = 0x02000;
  page.type = orc::kNabtsRecordTypeCyclicPresentation;
  page.identity_attested = false;
  catalogue.merge(page, 0);

  EXPECT_TRUE(catalogue.captions().empty());
  EXPECT_EQ(catalogue.records().size(), 1u);
}

// A caption whose flag byte was corrected clear still comes from the address
// the caption service is using, as a non-cyclic record: it is kept with the
// service rather than listed as a record of its own.
TEST(NabtsCaptionStream, ACaptionWhoseFlagWasCorrectedClearStaysACaption) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(caption(3, "FLAGGED"), 0);
  auto unflagged = caption(3, "FLAGGED");
  unflagged.classification.caption = false;
  unflagged.identity_attested = false;
  catalogue.merge(unflagged, 2);

  EXPECT_TRUE(catalogue.records().empty());
  const auto captions = catalogue.captions();
  ASSERT_EQ(captions.size(), 1u);
  EXPECT_EQ(captions[0].times_seen, 2u);
}

// A caption whose record type was corrected into the cyclic type still carries
// the Caption Flag and comes from the address the caption service is using: it
// is kept with the service, not left in the catalogue as a record of its own.
// An LP tape can carry five such arrivals among 656.
TEST(NabtsCaptionStream, ACaptionWhoseTypeWasCorrectedStaysACaption) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(caption(8, "ON AIR"), 0);
  auto misread = caption(8, "ON AIR");
  misread.type = orc::kNabtsRecordTypeCyclicPresentation;
  misread.identity_attested = false;
  catalogue.merge(misread, 2);

  EXPECT_TRUE(catalogue.records().empty());
  const auto captions = catalogue.captions();
  ASSERT_EQ(captions.size(), 1u);
  EXPECT_EQ(captions[0].times_seen, 2u);
}

// An undamaged caption arriving at an address nothing else uses, with no flag,
// is not the caption service.
TEST(NabtsCaptionStream, AnUnflaggedAttestedRecordIsNotACaption) {
  orc::NabtsCaptionStream stream;
  auto record = caption(0, "CONTROL");
  record.classification.caption = false;
  EXPECT_FALSE(stream.accepts(record));
  EXPECT_TRUE(stream.accepts(caption(0, "CAPTION")));
}

// The stream is bounded: past the bound a caption is counted, once however
// many copies of it arrive, and not kept.
TEST(NabtsCaptionStream, CaptionsPastTheBoundAreCountedOnceAndNotKept) {
  orc::NabtsCaptionStream stream(3);
  for (int index = 0; index < 5; ++index) {
    const uint8_t version = static_cast<uint8_t>(index);
    stream.add(caption(version, "C" + std::to_string(index)),
               static_cast<uint64_t>(index * 10));
    stream.add(caption(version, "C" + std::to_string(index)),
               static_cast<uint64_t>(index * 10 + 1));
  }
  EXPECT_EQ(stream.size(), 3u);
  EXPECT_EQ(stream.dropped(), 2u);
  const auto captions = stream.captions();
  ASSERT_EQ(captions.size(), 3u);
  EXPECT_EQ(captions[0].data, text_data("C0"));
  EXPECT_EQ(captions[2].data, text_data("C2"));
}

// A caption keeps what §7.1.5 reserves its address for, and its identity.
TEST(NabtsCaptionStream, ACaptionCarriesItsIdentityAndReservedPurpose) {
  orc::NabtsCaptionStream stream;
  auto message = caption(9, "HELLO");
  message.reserved_purpose = "Start of captioning";
  stream.add(message, 7);

  const auto captions = stream.captions();
  ASSERT_EQ(captions.size(), 1u);
  EXPECT_EQ(captions[0].channel, kCaptionChannel);
  EXPECT_EQ(captions[0].channel_text, "A00/000");
  EXPECT_EQ(captions[0].version, 9u);
  EXPECT_EQ(captions[0].reserved_purpose, "Start of captioning");
  EXPECT_TRUE(captions[0].caption);
  EXPECT_TRUE(captions[0].update);
  EXPECT_EQ(captions[0].record_type,
            orc::kNabtsRecordTypeNoncyclicPresentation);
  EXPECT_EQ(captions[0].first_seen_frame, 7u);
  EXPECT_EQ(captions[0].times_intact, 1u);
  EXPECT_EQ(captions[0].times_attested, 1u);
}

}  // namespace
}  // namespace orc_unit_test
