/*
 * File:        nabts_catalogue_view_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS page to display-list conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_catalogue_view.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include "nabts_record_catalogue.h"
#include "naplps_render_grid.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {
namespace {

/// A snapshot holding one primitive, which is enough to check what the list
/// carries alongside its operations.
NabtsPageSnapshot pageWithOneLine(double pel) {
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.colour_map);

  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kLine;
  primitive.points = {NabtsPoint{0.1, 0.1}, NabtsPoint{0.9, 0.6}};
  primitive.origin = primitive.points.front();
  primitive.logical_pel = NabtsSize{pel, pel};
  snapshot.primitives.push_back(primitive);
  return snapshot;
}

// The list states the receiver it was resolved against, because X3.110 sizes
// stroke width, texture dots and incremental rasters in that receiver's pixels
// (§5.3.2.2.6) and a renderer given no resolution can only guess at them.
TEST(NabtsCatalogueViewTest, TheDisplayListCarriesTheGridItWasResolvedAgainst) {
  struct Case {
    NaplpsRenderMode mode;
    int width;
    int height;
  };
  const std::vector<Case> cases = {{NaplpsRenderMode::kReference, 256, 200},
                                   {NaplpsRenderMode::kTwice, 512, 400},
                                   {NaplpsRenderMode::kThrice, 768, 600},
                                   {NaplpsRenderMode::kTwiceVector, 512, 400}};

  for (const Case& item : cases) {
    const CatalogueDisplayList list =
        nabts_page_display_list(pageWithOneLine(0.0), item.mode);
    EXPECT_EQ(list.nominal_width, item.width)
        << "mode " << naplps_render_mode_name(item.mode);
    EXPECT_EQ(list.nominal_height, item.height)
        << "mode " << naplps_render_mode_name(item.mode);
  }
}

// Table D1 item 10 makes unit y 0 to 0.78125 the visible extent, while §4.2.2
// puts that extent in a 4:3 display area. The two are different numbers and the
// list carries both, which is what lets a renderer draw a receiver's
// rectangular pixel as a rectangle instead of squaring the picture up.
TEST(NabtsCatalogueViewTest, TheVisibleExtentAndTheDisplayShapeAreBothStated) {
  const CatalogueDisplayList list =
      nabts_page_display_list(pageWithOneLine(0.0));
  EXPECT_NEAR(list.aspect_height, 0.78125, 1e-12);
  EXPECT_NEAR(list.display_aspect_height, 0.75, 1e-12);
  EXPECT_NE(list.aspect_height, list.display_aspect_height);
}

// The vector mode hands a renderer the geometry the record described, so the
// page's operations are the record's shapes and the pel stays in unit space:
// turning it into pixels there would bake one receiver into a list whose whole
// point is that it has not chosen one.
TEST(NabtsCatalogueViewTest, TheVectorModeEmitsTheRecordsOwnGeometry) {
  const NabtsPageSnapshot snapshot = pageWithOneLine(0.02);
  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);

  ASSERT_EQ(list.ops.size(), 1u);
  EXPECT_EQ(list.ops[0].kind, CatalogueDrawKind::kLine);
  ASSERT_EQ(list.ops[0].points.size(), 2u);
  EXPECT_NEAR(list.ops[0].points[0].x, 0.1, 1e-12);
  EXPECT_NEAR(list.ops[0].pen_size.dx, 0.02, 1e-12);
}

// A pixel mode deposits the page into the receiver's frame buffer and emits
// what is on it, so the operations are runs of pixels rather than the shapes
// that put them there. They are rectangles because every renderer of a display
// list already draws those — pixel output needs no new operation kind.
TEST(NabtsCatalogueViewTest, APixelModeEmitsRunsOfPixels) {
  const NabtsPageSnapshot snapshot = pageWithOneLine(0.02);
  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kReference);

  ASSERT_FALSE(list.ops.empty());
  for (const CatalogueDrawOp& op : list.ops) {
    EXPECT_EQ(op.kind, CatalogueDrawKind::kRectangle);
    EXPECT_TRUE(op.filled);
    // A pixel run has no pen: its extent is its own, not a stroke's.
    EXPECT_EQ(op.pen_size.dx, 0.0);
    EXPECT_EQ(op.pen_size.dy, 0.0);
    // Every run sits inside the unit screen it was rastered from.
    EXPECT_GE(op.origin.x, 0.0);
    EXPECT_GE(op.origin.y, 0.0);
    EXPECT_LE(op.origin.x + op.size.dx, 1.0 + 1e-9);
    EXPECT_LE(op.origin.y + op.size.dy, kNabtsDisplayAreaHeight + 1e-9);
  }
}

// A finer receiver resolves the same page into more, smaller pixels. That is
// the whole observable difference between the pixel modes, and it is what
// X3.110 §5.3.2.2.6's "at least one and possibly many display pixels" comes
// down to once a receiver is named.
TEST(NabtsCatalogueViewTest, AFinerReceiverEmitsSmallerPixels) {
  const NabtsPageSnapshot snapshot = pageWithOneLine(0.02);
  const CatalogueDisplayList coarse =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kReference);
  const CatalogueDisplayList fine =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwice);

  ASSERT_FALSE(coarse.ops.empty());
  ASSERT_FALSE(fine.ops.empty());
  // One row of the finer grid is half the height of one of the coarser.
  EXPECT_LT(fine.ops[0].size.dy, coarse.ops[0].size.dy);
  EXPECT_NEAR(fine.ops[0].size.dy, coarse.ops[0].size.dy / 2.0, 1e-9);
}

// The default is the receiver Table D1 requires, so a caller that names no mode
// gets the resolution a decoder of the period displayed.
TEST(NabtsCatalogueViewTest, TheDefaultResolutionIsTheReferenceReceiver) {
  const CatalogueDisplayList list =
      nabts_page_display_list(pageWithOneLine(0.0));
  EXPECT_EQ(list.nominal_width, 256);
  EXPECT_EQ(list.nominal_height, 200);
}

// ---------------------------------------------------------------------------
// Interpretation at catalogue-build time
// ---------------------------------------------------------------------------

/// A dataset holding one presentation record whose data draws |text| through a
/// DRCS definition, so the record exercises the resolution-dependent path.
NabtsAnalysisDataset datasetWithDrcsRecord() {
  NabtsAnalysisDataset data;
  NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address_text = "000";
  record.record_type = 1;  // §5.2.2.3 non-cyclic presentation
  record.times_seen = 1;
  record.times_intact = 1;
  record.complete = true;
  // ESC 4/3 (DEF DRCS), the character to define, then SO to invoke the PDI set
  // and a filled rectangle over the buffer, then ESC 4/5 (END).
  record.data = {0x1B, 0x43, 0x20, 0x0E, 0x6B, 0x7F, 0x1B, 0x45};
  data.records.push_back(record);
  return data;
}

// The receiver is chosen when the page is browsed, not when the recording is
// read, so changing it takes effect without reading the recording again. X3.110
// §6.2.3 sizes a DRCS storage buffer from the receiver's resolution, which is
// the one thing that used to be fixed by the recovery pass.
TEST(NabtsCatalogueViewTest, TheResolutionAppliesWithoutReRunningRecovery) {
  const NabtsAnalysisDataset data = datasetWithDrcsRecord();

  std::vector<NabtsCataloguedRecord> coarse = data.records;
  nabts_interpret_records(coarse, kNaplpsGridReference);
  std::vector<NabtsCataloguedRecord> fine = data.records;
  nabts_interpret_records(fine, kNaplpsGridTwice);

  ASSERT_EQ(coarse.size(), 1u);
  ASSERT_EQ(fine.size(), 1u);
  ASSERT_FALSE(coarse[0].page.drcs.empty())
      << "the fixture defined no downloadable character";
  ASSERT_FALSE(fine[0].page.drcs.empty());

  // The same definition, stored at each receiver's own resolution.
  EXPECT_GT(fine[0].page.drcs[0].width, coarse[0].page.drcs[0].width);
  EXPECT_GT(fine[0].page.drcs[0].height, coarse[0].page.drcs[0].height);
}

// Recovery catalogues the record data and stops there: a page is what a
// receiver makes of that data, so building it belongs with the receiver.
TEST(NabtsCatalogueViewTest, ADatasetCarriesRecordDataRatherThanAPage) {
  const NabtsAnalysisDataset data = datasetWithDrcsRecord();
  EXPECT_FALSE(data.records[0].data.empty());
  EXPECT_TRUE(data.records[0].page.primitives.empty());

  // The catalogue is where it becomes something to look at.
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  EXPECT_FALSE(catalogue.items.empty());
}

// What a page says is not what it looks like: the resolution decides how it is
// drawn, and the text a reader searches, the captions and the item table have
// to come out the same at every one of them.
TEST(NabtsCatalogueViewTest, TheReadingOfAPageIsTheSameAtEveryResolution) {
  const NabtsAnalysisDataset data = datasetWithDrcsRecord();

  const CatalogueDataset reference =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  for (const NaplpsRenderMode mode :
       {NaplpsRenderMode::kThrice, NaplpsRenderMode::kTwice,
        NaplpsRenderMode::kTwiceVector}) {
    const CatalogueDataset other = build_nabts_catalogue(data, mode);
    ASSERT_EQ(other.items.size(), reference.items.size())
        << "mode " << naplps_render_mode_name(mode);
    for (size_t i = 0; i < other.items.size(); ++i) {
      EXPECT_EQ(other.items[i].id, reference.items[i].id);
      EXPECT_EQ(other.items[i].values, reference.items[i].values);
      EXPECT_EQ(other.items[i].badges, reference.items[i].badges);
    }
    // The text a reader searches and the caption track are read off the page,
    // not off how it is drawn, so they cannot move with the resolution either.
    ASSERT_EQ(other.payloads.size(), reference.payloads.size());
    for (size_t i = 0; i < other.payloads.size(); ++i) {
      EXPECT_EQ(other.payloads[i].companion_text,
                reference.payloads[i].companion_text)
          << "the text a reader searches changed with the resolution";
      EXPECT_EQ(other.payloads[i].table.rows, reference.payloads[i].table.rows);
    }
  }
}

// The four receivers are offered to the reader through the schema, so the host
// can put them on a dropdown without knowing what a receiver is.
TEST(NabtsCatalogueViewTest, TheSchemaOffersTheReceiversAsViewOptions) {
  const CatalogueDataset catalogue =
      build_nabts_catalogue(datasetWithDrcsRecord(), NaplpsRenderMode::kThrice);

  EXPECT_FALSE(catalogue.schema.view_label.empty())
      << "an unlabelled set of options is not offered at all";
  ASSERT_EQ(catalogue.schema.view_options.size(), 4u);

  // Every option names a mode back, so what the host round-trips selects the
  // receiver the reader picked rather than falling back to another.
  for (const auto& option : catalogue.schema.view_options) {
    EXPECT_FALSE(option.label.empty());
    EXPECT_FALSE(option.tooltip.empty());
    EXPECT_EQ(naplps_render_mode_name(naplps_render_mode_from_name(
                  option.id, NaplpsRenderMode::kTwiceVector)),
              option.id)
        << "option '" << option.id << "' names no receiver";
  }

  // And the catalogue says which of them it was built under.
  EXPECT_EQ(catalogue.schema.view_option,
            naplps_render_mode_name(NaplpsRenderMode::kThrice));
}

// ---------------------------------------------------------------------------
// Syntax repair, offered to the reader
// ---------------------------------------------------------------------------

/// A presentation record drawing one line, with |damage| exclusive-ored into
/// the byte at |offset| — which is what a single-bit error looks like on the
/// wire, the payload changing while the odd parity of CEA-516 §3.3 does not.
NabtsAnalysisDataset datasetWithLineRecord(size_t offset = 0,
                                           uint8_t damage = 0) {
  NabtsAnalysisDataset data;
  NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address_text = "000";
  record.record_type = 1;  // §5.2.2.3 non-cyclic presentation
  record.times_seen = 1;
  record.times_intact = 1;
  record.complete = true;
  // SO invokes the PDI set, LINE ABS (2/8) takes one three-byte coordinate
  // word — the default multi-value length of §5.3.2.2.3.
  const std::vector<uint8_t> plain = {0x0E, 0x28, 0x49, 0x42, 0x40};
  for (const uint8_t byte : plain) {
    record.data.push_back(teletext_odd_parity_encode(byte));
  }
  if (damage != 0 && offset < record.data.size()) {
    record.data[offset] = static_cast<uint8_t>(record.data[offset] ^ damage);
  }
  data.records.push_back(record);
  return data;
}

// The browser offers the choice, because whether a damaged page is presented as
// recovered or as transmitted changes nothing the recovery found and everything
// about what is on screen.
TEST(NabtsCatalogueViewTest, TheCatalogueOffersTheSyntaxRepairToggle) {
  const CatalogueDataset catalogue = build_nabts_catalogue(
      datasetWithLineRecord(), NaplpsRenderMode::kReference,
      /*repair=*/true);

  ASSERT_EQ(catalogue.schema.toggles.size(), 1u);
  EXPECT_EQ(catalogue.schema.toggles.front().id, kNabtsRepairToggleId);
  EXPECT_TRUE(catalogue.schema.toggles.front().active);
  EXPECT_FALSE(catalogue.schema.toggles.front().label.empty());

  const CatalogueDataset off = build_nabts_catalogue(
      datasetWithLineRecord(), NaplpsRenderMode::kReference, /*repair=*/false);
  ASSERT_EQ(off.schema.toggles.size(), 1u);
  EXPECT_FALSE(off.schema.toggles.front().active);
}

// With the toggle off, a record is interpreted exactly as it always was.
TEST(NabtsCatalogueViewTest, RepairOffInterpretsTheRecordAsTransmitted) {
  const NabtsAnalysisDataset data = datasetWithDrcsRecord();

  std::vector<NabtsCataloguedRecord> baseline = data.records;
  nabts_interpret_records(baseline, kNaplpsGridReference);
  std::vector<NabtsCataloguedRecord> unrepaired = data.records;
  nabts_interpret_records(unrepaired, kNaplpsGridReference, /*repair=*/false);

  ASSERT_EQ(baseline.size(), unrepaired.size());
  EXPECT_EQ(baseline.front().page.primitives.size(),
            unrepaired.front().page.primitives.size());
  EXPECT_EQ(unrepaired.front().page.diagnostics.repaired_bytes, 0u);
}

// §5.3.1 puts numeric data in columns 4 to 7. Knock b7 out of an operand and
// the byte becomes a transparent control, which §5.3.1 lets stand inside the
// PDI without ending it — so the line is still drawn, to a point assembled from
// what is left and zero-extended. The damage does not hide the page; it draws a
// different one. The grammar admits one correction, and with the toggle on the
// page comes back.
TEST(NabtsCatalogueViewTest, RepairOnDrawsThePageTheSenderDescribed) {
  const NabtsAnalysisDataset damaged = datasetWithLineRecord(3, 0x40);

  std::vector<NabtsCataloguedRecord> pristine = datasetWithLineRecord().records;
  nabts_interpret_records(pristine, kNaplpsGridReference, /*repair=*/false);
  std::vector<NabtsCataloguedRecord> as_sent = damaged.records;
  nabts_interpret_records(as_sent, kNaplpsGridReference, /*repair=*/false);
  std::vector<NabtsCataloguedRecord> repaired = damaged.records;
  nabts_interpret_records(repaired, kNaplpsGridReference, /*repair=*/true);

  ASSERT_EQ(pristine.front().page.primitives.size(), 1u);
  ASSERT_EQ(as_sent.front().page.primitives.size(), 1u);
  ASSERT_EQ(repaired.front().page.primitives.size(), 1u);

  const NabtsPoint wanted =
      pristine.front().page.primitives.front().points.back();
  const NabtsPoint drawn =
      as_sent.front().page.primitives.front().points.back();
  const NabtsPoint mended =
      repaired.front().page.primitives.front().points.back();

  // The lost byte carried low-order bits of both components, so what survives
  // still puts the line's end on the right column and the wrong row.
  EXPECT_TRUE(drawn.x != wanted.x || drawn.y != wanted.y)
      << "the damage drew the line somewhere else";
  EXPECT_DOUBLE_EQ(mended.x, wanted.x);
  EXPECT_DOUBLE_EQ(mended.y, wanted.y);

  // What was recovered is left exactly as it was recovered: the repair changes
  // how the record reads, not what the run found or what it would export.
  EXPECT_EQ(repaired.front().data, damaged.records.front().data);
}

// A page drawn partly from guesses says so, so a reader can weigh what they are
// looking at.
TEST(NabtsCatalogueViewTest, ARepairedPageCarriesWhatWasDoneToIt) {
  std::vector<NabtsCataloguedRecord> records =
      datasetWithLineRecord(3, 0x40).records;
  nabts_interpret_records(records, kNaplpsGridReference, /*repair=*/true);

  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records.front().page.diagnostics.repaired_bytes, 1u);
  // And the interpreter's own diagnostics are untouched by the stamping.
  EXPECT_GT(records.front().page.diagnostics.bytes_read, 0u);
}

// The totals a run reports are the same readings a reader will be shown.
TEST(NabtsCatalogueViewTest, TheRunTotalsCountWhatTheRepairDid) {
  const NabtsLintTotals clean =
      nabts_lint_records(datasetWithLineRecord().records);
  EXPECT_EQ(clean.records_linted, 1u);
  EXPECT_EQ(clean.records_faulted, 0u);
  EXPECT_EQ(clean.bytes_repaired, 0u);
  // Nothing to say about a recording whose pages all arrived clean.
  EXPECT_NE(clean.summary().find("none faulted"), std::string::npos);

  const NabtsLintTotals damaged =
      nabts_lint_records(datasetWithLineRecord(3, 0x40).records);
  EXPECT_EQ(damaged.records_linted, 1u);
  EXPECT_EQ(damaged.records_faulted, 1u);
  EXPECT_EQ(damaged.bytes_repaired, 1u);
  EXPECT_LT(damaged.errors_after, damaged.errors_before);
  EXPECT_NE(damaged.summary().find("corrected"), std::string::npos);
}

// An application record carries function descriptors rather than presentation
// code, so there is nothing here for a NAPLPS linter to say about it.
TEST(NabtsCatalogueViewTest, TheRunTotalsIgnoreNonPresentationRecords) {
  NabtsAnalysisDataset data = datasetWithLineRecord();
  data.records.front().record_type = 2;  // §5.2.2.4 application

  const NabtsLintTotals totals = nabts_lint_records(data.records);
  EXPECT_EQ(totals.records_linted, 0u);
  EXPECT_TRUE(totals.summary().empty());
}

/// The catalogue's notice about the repair, or empty where it said nothing.
std::string repair_notice(const CatalogueDataset& catalogue) {
  for (const std::string& notice : catalogue.summary.notices) {
    if (notice.find("Syntax repair") != std::string::npos) {
      return notice;
    }
  }
  return {};
}

// A reader cannot judge a repaired page without being told it was repaired, and
// cannot tell "the grammar found nothing" from "the pass never ran" unless the
// catalogue says which. So the notice is written either way.
TEST(NabtsCatalogueViewTest, TheCatalogueSaysWhatTheRepairDid) {
  const CatalogueDataset repaired = build_nabts_catalogue(
      datasetWithLineRecord(3, 0x40), NaplpsRenderMode::kReference,
      /*repair=*/true);
  // Two clauses, for a reader: how widely the records were altered, and how
  // many of the pages that reached. "Corrected" is a claim about the outcome
  // that nothing here can support, so it is not made. The byte counts behind
  // it are the log's business.
  const std::string did = repair_notice(repaired);
  EXPECT_NE(did.find("1 of 1 pages altered"), std::string::npos) << did;
  EXPECT_NE(did.find("drawing differently"), std::string::npos) << did;

  // Off, and the notice says the pages are the recording's own.
  const CatalogueDataset as_received = build_nabts_catalogue(
      datasetWithLineRecord(3, 0x40), NaplpsRenderMode::kReference,
      /*repair=*/false);
  EXPECT_NE(repair_notice(as_received).find("off"), std::string::npos)
      << repair_notice(as_received);

  // On, over a recording that needed nothing: the pass ran and said so.
  const CatalogueDataset clean = build_nabts_catalogue(
      datasetWithLineRecord(), NaplpsRenderMode::kReference, /*repair=*/true);
  EXPECT_NE(repair_notice(clean).find("no page needed altering"),
            std::string::npos)
      << repair_notice(clean);
}

// And the page a reader is looking at says what was done to *it*, on the
// condition line beside it — the notice is about the catalogue, and a reader
// stepping through pages needs to know which one in front of them was touched.
TEST(NabtsCatalogueViewTest, ARepairedPageSaysSoOnItsOwnConditionLine) {
  const CatalogueDataset repaired = build_nabts_catalogue(
      datasetWithLineRecord(3, 0x40), NaplpsRenderMode::kReference,
      /*repair=*/true);
  ASSERT_FALSE(repaired.payloads.empty());
  EXPECT_NE(repaired.payloads.front().condition.find("1 byte corrected"),
            std::string::npos)
      << repaired.payloads.front().condition;

  const CatalogueDataset as_received = build_nabts_catalogue(
      datasetWithLineRecord(3, 0x40), NaplpsRenderMode::kReference,
      /*repair=*/false);
  EXPECT_EQ(as_received.payloads.front().condition.find("corrected"),
            std::string::npos)
      << as_received.payloads.front().condition;
}

// And in the list itself, so a reader stepping through pages can see at a
// glance which of them the grammar had a hand in. The condition line says what
// was done; the mark says where to look for it.
TEST(NabtsCatalogueViewTest, ARepairedPageIsMarkedInTheList) {
  const CatalogueDataset repaired = build_nabts_catalogue(
      datasetWithLineRecord(3, 0x40), NaplpsRenderMode::kReference,
      /*repair=*/true);
  ASSERT_FALSE(repaired.items.empty());
  const std::vector<std::string>& marks = repaired.items.front().badges;
  EXPECT_NE(std::find(marks.begin(), marks.end(), "*"), marks.end());
  // The identity itself is untouched: the mark is a badge beside it, so the
  // find box and every id the host round-trips still say what they said.
  EXPECT_EQ(repaired.items.front().values.front(),
            repaired.items.front().find_key);
  EXPECT_NE(repaired.items.front().tooltip.find("Syntax repair altered"),
            std::string::npos)
      << repaired.items.front().tooltip;

  // Nothing is marked with the repair turned off, and nothing is marked on a
  // page that needed none of it.
  const CatalogueDataset as_received = build_nabts_catalogue(
      datasetWithLineRecord(3, 0x40), NaplpsRenderMode::kReference,
      /*repair=*/false);
  const std::vector<std::string>& none = as_received.items.front().badges;
  EXPECT_EQ(std::find(none.begin(), none.end(), "*"), none.end());

  const CatalogueDataset clean = build_nabts_catalogue(
      datasetWithLineRecord(), NaplpsRenderMode::kReference, /*repair=*/true);
  const std::vector<std::string>& clean_marks = clean.items.front().badges;
  EXPECT_EQ(std::find(clean_marks.begin(), clean_marks.end(), "*"),
            clean_marks.end());
}

// ---------------------------------------------------------------------------
// Versions of one address
// ---------------------------------------------------------------------------

/// Two versions of presentation record 000/012 and one version of 000/013.
NabtsAnalysisDataset datasetWithTwoVersions() {
  NabtsAnalysisDataset data;
  for (const auto& [address, version, first, last] :
       std::vector<std::tuple<uint64_t, uint8_t, uint64_t, uint64_t>>{
           {0x012, 0, 10, 40}, {0x012, 2, 50, 90}, {0x013, 0, 10, 90}}) {
    NabtsCataloguedRecord record = datasetWithLineRecord().records.front();
    record.address = address;
    record.version = version;
    char text[16];
    std::snprintf(text, sizeof(text), "%03X", static_cast<unsigned>(address));
    record.address_text = text;
    record.channel_text = std::string("000/") + text;
    record.first_seen_frame = first;
    record.last_seen_frame = last;
    data.records.push_back(record);
  }
  return data;
}

// CEA-516 §5.2.1 makes the version part of a record's identity, so each
// version is a row of its own — and each of those rows says it is one of
// several, so a revised page does not read as two unrelated records.
TEST(NabtsCatalogueViewTest, TheVersionsOfOneAddressAreMarkedInTheList) {
  const CatalogueDataset catalogue = build_nabts_catalogue(
      datasetWithTwoVersions(), NaplpsRenderMode::kReference);

  int marked = 0;
  for (const CatalogueItem& item : catalogue.items) {
    const bool revised = item.id.find("/012") != std::string::npos;
    const bool has_mark = std::find(item.badges.begin(), item.badges.end(),
                                    "2 versions") != item.badges.end();
    EXPECT_EQ(has_mark, revised) << item.id;
    if (revised) {
      ++marked;
      EXPECT_NE(item.tooltip.find("Address 000/012 was transmitted in 2 "
                                  "versions during the recording: v0 (frames "
                                  "11-41) and v2 (frames 51-91)"),
                std::string::npos)
          << item.tooltip;
    }
  }
  EXPECT_EQ(marked, 2);
}

// A record kept although its address never arrived undamaged says so, and
// only that one: the rest were named as transmitted.
TEST(NabtsCatalogueViewTest, ARecordKeptWithoutAnAttestedAddressIsMarked) {
  NabtsAnalysisDataset data = datasetWithTwoVersions();
  for (NabtsCataloguedRecord& record : data.records) {
    record.times_attested = record.address == 0x013 ? 0 : record.times_seen;
  }
  data.summary.identity_reconciliation.identities_kept = 1;

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  for (const CatalogueItem& item : catalogue.items) {
    const bool kept = item.id.find("/013") != std::string::npos;
    const bool marked = std::find(item.badges.begin(), item.badges.end(),
                                  "unconfirmed") != item.badges.end();
    EXPECT_EQ(marked, kept) << item.id;
    EXPECT_EQ(item.tooltip.find("never arrived undamaged") != std::string::npos,
              kept)
        << item.id;
  }
}

}  // namespace
}  // namespace orc
