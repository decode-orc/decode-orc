/*
 * File:        vbi_source_format_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the raw VBI container descriptor and presets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_format.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <optional>

#include "vbi_source_validation.h"

namespace orc {
namespace {

// The 525-line bt8x8 and cx2388x entries, spelled once each: their names are
// long enough that writing them out at every use puts the closing bracket past
// the column limit and splits the literal in half.
constexpr const char* kBt8x8NTSCWSTPreset =
    "bt8x8 card dump, 8-bit (WST, NTSC source)";
constexpr const char* kBt8x8NTSCNABTSPreset =
    "bt8x8 card dump, 8-bit (NABTS, NTSC source)";
constexpr const char* kCx2388xNTSCWSTPreset =
    "cx2388x card dump, 8-bit (WST, NTSC source)";
constexpr const char* kCx2388xNTSCNABTSPreset =
    "cx2388x card dump, 8-bit (NABTS, NTSC source)";

// The bt8x8 PAL preset must expand to the values in the design's known-format
// table, field by field. These are the only numbers the reader ever sees, so
// a wrong entry here is a silently wrong decode everywhere downstream.
TEST(VBISourceFormat, Bt8x8PALPresetExpandsToTheDocumentedContainer) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
  EXPECT_TRUE(error.empty());

  EXPECT_DOUBLE_EQ(format.sample_rate_hz, 35468950.0);
  EXPECT_EQ(format.line_length, 2048u);
  EXPECT_EQ(format.valid_samples, 2044u);
  EXPECT_EQ(format.sample_format, VBISampleFormat::kU8);
  EXPECT_EQ(format.field_lines, 16u);
  EXPECT_EQ(format.field_range.start, 0u);
  EXPECT_EQ(format.field_range.end, 15u);
  EXPECT_EQ(format.frame_trailer_bytes, 4u);
  EXPECT_TRUE(format.frame_trailer_is_counter);
  EXPECT_EQ(format.first_field, 1u);
  EXPECT_EQ(format.tv_system, VBITVSystem::kPAL);
  EXPECT_EQ(format.tt_system, VBITeletextSystem::kWST);
  EXPECT_EQ(format.family, VBISourceFamily::kCardCapture);
}

// The bt8x8 capture offset is explicitly unreliable hardware folklore, so the
// preset carries it as a calibration starting point rather than a value to be
// used as configured.
TEST(VBISourceFormat, Bt8x8PALPresetCalibratesItsCaptureOffset) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;

  EXPECT_TRUE(format.capture_offset_is_auto);
  EXPECT_DOUBLE_EQ(format.capture_offset_samples, 244.0);
}

// What the calibration should expect of a source is a property of the source
// format, not a global constant: a cleanly time-base corrected capture locks
// to a fraction of a sample, where material played off tape into a capture
// card legitimately scatters by several (design §5.3.4, §5.3.6).
TEST(VBISourceFormat, Bt8x8PALPresetCarriesCleanSourceCalibrationThresholds) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;

  EXPECT_DOUBLE_EQ(format.calibration.tight_spread_samples, 0.5);

  // A bt8x8 card is how tape and off-air material is captured, so the preset
  // has to tolerate the line-to-line scatter such a source really shows while
  // still classifying it as jitter rather than as clean.
  EXPECT_DOUBLE_EQ(format.calibration.maximum_spread_samples, 8.0);
  EXPECT_DOUBLE_EQ(format.calibration.maximum_drift_samples, 8.0);

  // The search has to reach the whole span the vhs-teletext windows in
  // design §5.3.3 imply for this card, without straying into the data.
  EXPECT_DOUBLE_EQ(format.calibration.search_tolerance_samples, 48.0);
  EXPECT_GT(format.calibration.acceptance_correlation, 0.0);
  EXPECT_LT(format.calibration.acceptance_correlation, 1.0);
  EXPECT_GT(format.calibration.minimum_acceptance_fraction, 0.0);
}

// The SECAM entry is the same container: the driver's SECAM television norm
// shares the PAL one's sampling clock, its vbipack and its vbistart, so a byte
// of one dump is a byte of the other. A preset that quietly differed anywhere
// else would be describing a container nobody has measured.
TEST(VBISourceFormat, Bt8x8SECAMPresetIsTheSameContainerAsPAL) {
  VBISourceFormat pal;
  VBISourceFormat secam;
  std::string error;

  ASSERT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", pal, error))
      << error;
  ASSERT_TRUE(expand_vbi_source_preset(
      "bt8x8 card dump, 8-bit (WST, SECAM source)", secam, error))
      << error;

  EXPECT_DOUBLE_EQ(secam.sample_rate_hz, pal.sample_rate_hz);
  EXPECT_EQ(secam.line_length, pal.line_length);
  EXPECT_EQ(secam.valid_samples, pal.valid_samples);
  EXPECT_EQ(secam.sample_format, pal.sample_format);
  EXPECT_EQ(secam.field_lines, pal.field_lines);
  EXPECT_EQ(secam.field_range.start, pal.field_range.start);
  EXPECT_EQ(secam.field_range.end, pal.field_range.end);
  EXPECT_EQ(secam.frame_trailer_bytes, pal.frame_trailer_bytes);
  EXPECT_EQ(secam.frame_trailer_is_counter, pal.frame_trailer_is_counter);
  EXPECT_EQ(secam.first_field, pal.first_field);
  EXPECT_EQ(secam.capture_offset_is_auto, pal.capture_offset_is_auto);
  EXPECT_DOUBLE_EQ(secam.capture_offset_samples, pal.capture_offset_samples);

  // Post-decode SECAM is a 625-line signal carrying the same World System
  // Teletext, so it is placed on PAL frames by the same card-capture path.
  EXPECT_EQ(secam.tv_system, VBITVSystem::kPAL);
  EXPECT_EQ(secam.tt_system, VBITeletextSystem::kWST);
  EXPECT_EQ(secam.family, VBISourceFamily::kCardCapture);
}

// The one thing that does differ. A SECAM transmission with vertical colour
// identification spends field lines 8-15 — records 1 to 8 of every stored
// field — on the identification signal, and the reference tape leaves only
// records 12-14 carrying teletext. The share of records the run-in can be
// found on is therefore a fraction of a PAL capture's, and it is the only
// calibration threshold that can be relaxed without weakening the checks that
// actually catch a wrong container: the spread, the drift and the absolute
// floor of accepted records are untouched.
TEST(VBISourceFormat, Bt8x8SECAMPresetExpectsFewerLinesToCarryTeletext) {
  VBISourceFormat pal;
  VBISourceFormat secam;
  std::string error;

  ASSERT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", pal, error))
      << error;
  ASSERT_TRUE(expand_vbi_source_preset(
      "bt8x8 card dump, 8-bit (WST, SECAM source)", secam, error))
      << error;

  EXPECT_LT(secam.calibration.minimum_acceptance_fraction,
            pal.calibration.minimum_acceptance_fraction);
  EXPECT_GT(secam.calibration.minimum_acceptance_fraction, 0.0);

  // Three teletext lines of the sixteen records a field stores is 18,75% of
  // them before a single line is lost to tape, which is what the PAL figure
  // rejects and this one has to admit.
  EXPECT_LT(secam.calibration.minimum_acceptance_fraction, 3.0 / 16.0);

  EXPECT_DOUBLE_EQ(secam.calibration.acceptance_correlation,
                   pal.calibration.acceptance_correlation);
  EXPECT_DOUBLE_EQ(secam.calibration.search_tolerance_samples,
                   pal.calibration.search_tolerance_samples);
  EXPECT_DOUBLE_EQ(secam.calibration.tight_spread_samples,
                   pal.calibration.tight_spread_samples);
  EXPECT_DOUBLE_EQ(secam.calibration.maximum_spread_samples,
                   pal.calibration.maximum_spread_samples);
  EXPECT_DOUBLE_EQ(secam.calibration.maximum_drift_samples,
                   pal.calibration.maximum_drift_samples);
}

// The 525-line bt8x8 entry is the driver's NTSC television norm, field for
// field: bttv_tvnorms[] gives it Fsc = 28 636 363, vbipack = 144 (so 1024 +
// 144 x 4 = 1600 real samples of the 2048-byte record) and vbistart =
// {10, 273}.  Every one of these was confirmed against the reference capture,
// and getting any of them wrong is a silently wrong decode rather than a
// refusal.
TEST(VBISourceFormat, Bt8x8NTSCPresetExpandsToTheDriverContainer) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(expand_vbi_source_preset(kBt8x8NTSCWSTPreset, format, error))
      << error;
  EXPECT_TRUE(error.empty());

  EXPECT_DOUBLE_EQ(format.sample_rate_hz, 28636363.0);
  EXPECT_EQ(format.line_length, 2048u);
  EXPECT_EQ(format.valid_samples, 1600u);
  EXPECT_EQ(format.sample_format, VBISampleFormat::kU8);
  EXPECT_EQ(format.field_lines, 16u);

  // vbistart[0] is broadcast frame line 10, which is the head of the 525-line
  // teletext list, so records 0-11 are the whole of it and records 12-15 are
  // the start of the picture.
  EXPECT_EQ(format.field_range.start, 0u);
  EXPECT_EQ(format.field_range.end, 11u);
  EXPECT_EQ(format.field_range.count(),
            standard_teletext_lines_per_field(VBITVSystem::kNTSC,
                                              VBITeletextSystem::kWST));

  // The frame counter survives the change of norm: there is simply far more
  // padding for it to sit in.
  EXPECT_EQ(format.frame_trailer_bytes, 4u);
  EXPECT_TRUE(format.frame_trailer_is_counter);
  EXPECT_EQ(format.record_padding_bytes(), 448u);

  EXPECT_EQ(format.first_field, 1u);
  EXPECT_EQ(format.tv_system, VBITVSystem::kNTSC);
  EXPECT_EQ(format.tt_system, VBITeletextSystem::kWST);
  EXPECT_EQ(format.family, VBISourceFamily::kCardCapture);
}

// The same card and the same driver, so the record stride, the field stride
// and the frame trailer carry over from the 625-line entry — and nothing else
// does.  A preset that shared the sampling rate, the valid sample count or the
// field range with PAL would be describing a container that does not exist.
TEST(VBISourceFormat, Bt8x8NTSCPresetSharesOnlyTheCardsGeometryWithPAL) {
  VBISourceFormat pal;
  VBISourceFormat ntsc;
  std::string error;

  ASSERT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", pal, error))
      << error;
  ASSERT_TRUE(expand_vbi_source_preset(kBt8x8NTSCWSTPreset, ntsc, error))
      << error;

  EXPECT_EQ(ntsc.line_length, pal.line_length);
  EXPECT_EQ(ntsc.field_lines, pal.field_lines);
  EXPECT_EQ(ntsc.sample_format, pal.sample_format);
  EXPECT_EQ(ntsc.frame_trailer_bytes, pal.frame_trailer_bytes);
  EXPECT_EQ(ntsc.frame_trailer_is_counter, pal.frame_trailer_is_counter);
  EXPECT_EQ(ntsc.first_field, pal.first_field);
  EXPECT_EQ(ntsc.family, pal.family);

  EXPECT_NE(ntsc.sample_rate_hz, pal.sample_rate_hz);
  EXPECT_NE(ntsc.valid_samples, pal.valid_samples);
  EXPECT_NE(ntsc.field_range.end, pal.field_range.end);
  EXPECT_NE(ntsc.tv_system, pal.tv_system);

  // A bt8x8 NTSC frame is the same 65 536 bytes as a PAL one, which is why
  // the frame counter still lands on the last four of them.
  EXPECT_EQ(ntsc.bytes_per_record(), 2048u);
  EXPECT_EQ(ntsc.bytes_per_field(), 32768u);
  EXPECT_EQ(ntsc.bytes_per_frame(), 65536u);
}

// The driver's VBI_OFFSET is one constant for every television norm and its
// own source calls it experimental, so on this norm as on the 625-line one it
// is a starting hint for the fit rather than a value to be used as configured.
// The reference capture settles 5,1 samples away from it.
TEST(VBISourceFormat, Bt8x8NTSCPresetCalibratesItsCaptureOffset) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset(kBt8x8NTSCWSTPreset, format, error))
      << error;

  EXPECT_TRUE(format.capture_offset_is_auto);
  EXPECT_DOUBLE_EQ(format.capture_offset_samples, 244.0);
}

// The two thresholds that cannot be inherited from the 625-line entry, and the
// reasons they are the only two.  A US broadcaster carried its magazines on a
// handful of the twelve lines where a 625-line one filled most of the list, so
// the share of records that lock is far lower; and the spread limit is a
// tolerance in time, so at a slower sampling clock it is a smaller count of
// samples, not the same one.
TEST(VBISourceFormat, Bt8x8NTSCPresetKeepsTheCardsToleranceInTimeNotInSamples) {
  VBISourceFormat pal;
  VBISourceFormat ntsc;
  std::string error;
  ASSERT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", pal, error))
      << error;
  ASSERT_TRUE(expand_vbi_source_preset(kBt8x8NTSCWSTPreset, ntsc, error))
      << error;

  const double pal_spread_ns =
      pal.calibration.maximum_spread_samples / pal.sample_rate_hz * 1e9;
  const double ntsc_spread_ns =
      ntsc.calibration.maximum_spread_samples / ntsc.sample_rate_hz * 1e9;
  EXPECT_NEAR(ntsc_spread_ns, pal_spread_ns, 10.0);
  EXPECT_LT(ntsc.calibration.maximum_spread_samples,
            pal.calibration.maximum_spread_samples);
  EXPECT_DOUBLE_EQ(ntsc.calibration.maximum_drift_samples,
                   ntsc.calibration.maximum_spread_samples);

  // The reference capture scatters by 3,3 samples, so the limit has to sit
  // above that and nowhere near far enough above it to admit a wrong rate.
  EXPECT_GT(ntsc.calibration.maximum_spread_samples, 3.3);

  // Five records of the first stored field and three of the second carry the
  // service on the reference capture: a third of the twenty-four sampled per
  // frame, which the 625-line quarter would only just admit.
  EXPECT_LT(ntsc.calibration.minimum_acceptance_fraction,
            pal.calibration.minimum_acceptance_fraction);
  EXPECT_LT(ntsc.calibration.minimum_acceptance_fraction, 3.0 / 24.0);
  EXPECT_GT(ntsc.calibration.minimum_acceptance_fraction, 0.0);

  // Everything else is the 625-line entry's.
  EXPECT_DOUBLE_EQ(ntsc.calibration.acceptance_correlation,
                   pal.calibration.acceptance_correlation);
  EXPECT_DOUBLE_EQ(ntsc.calibration.tight_spread_samples,
                   pal.calibration.tight_spread_samples);
  EXPECT_DOUBLE_EQ(ntsc.calibration.search_tolerance_samples,
                   pal.calibration.search_tolerance_samples);
}

// The card knows nothing about what the lines carry, so the two 525-line
// service entries may differ in the service and in nothing else.
TEST(VBISourceFormat, Bt8x8NTSCServiceVariantsShareOneContainer) {
  VBISourceFormat wst;
  VBISourceFormat nabts;
  std::string error;

  ASSERT_TRUE(expand_vbi_source_preset(kBt8x8NTSCWSTPreset, wst, error))
      << error;
  ASSERT_TRUE(expand_vbi_source_preset(kBt8x8NTSCNABTSPreset, nabts, error))
      << error;

  EXPECT_DOUBLE_EQ(nabts.sample_rate_hz, wst.sample_rate_hz);
  EXPECT_EQ(nabts.line_length, wst.line_length);
  EXPECT_EQ(nabts.valid_samples, wst.valid_samples);
  EXPECT_EQ(nabts.sample_format, wst.sample_format);
  EXPECT_EQ(nabts.field_lines, wst.field_lines);
  EXPECT_EQ(nabts.field_range.start, wst.field_range.start);
  EXPECT_EQ(nabts.field_range.end, wst.field_range.end);
  EXPECT_EQ(nabts.frame_trailer_bytes, wst.frame_trailer_bytes);
  EXPECT_EQ(nabts.frame_trailer_is_counter, wst.frame_trailer_is_counter);
  EXPECT_EQ(nabts.first_field, wst.first_field);
  EXPECT_DOUBLE_EQ(nabts.capture_offset_samples, wst.capture_offset_samples);
  EXPECT_EQ(nabts.capture_offset_is_auto, wst.capture_offset_is_auto);
  EXPECT_EQ(nabts.family, wst.family);

  EXPECT_EQ(wst.tt_system, VBITeletextSystem::kWST);
  EXPECT_EQ(nabts.tt_system, VBITeletextSystem::kNABTS);
}

// The reference 525-line sample is 910 819 328 raw bytes, which must
// factorise into exactly 13 898 whole bt8x8 NTSC frames.
TEST(VBISourceFormat, Bt8x8NTSCFrameSizeFactorisesTheReferenceCaptureLength) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset(kBt8x8NTSCWSTPreset, format, error))
      << error;

  constexpr uint64_t kReferenceRawBytes = 910819328ull;
  EXPECT_EQ(kReferenceRawBytes % format.bytes_per_frame(), 0u);
  EXPECT_EQ(kReferenceRawBytes / format.bytes_per_frame(), 13898u);
}

// A bt8x8 PAL frame is 65 536 bytes: sixteen 2048-byte records in each of two
// fields, with the frame counter inside the final record's padding rather
// than appended to the frame.
TEST(VBISourceFormat, Bt8x8PALFrameGeometryDerivesTheDocumentedByteCounts) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;

  EXPECT_EQ(format.bytes_per_sample(), 1u);
  EXPECT_EQ(format.bytes_per_record(), 2048u);
  EXPECT_EQ(format.bytes_per_field(), 32768u);
  EXPECT_EQ(format.bytes_per_frame(), 65536u);
  EXPECT_EQ(format.record_padding_bytes(), 4u);
  EXPECT_GE(format.record_padding_bytes(), format.frame_trailer_bytes);
}

// The reference sample is 24 117 706 752 raw bytes, which must factorise into
// exactly 368 007 whole bt8x8 PAL frames.
TEST(VBISourceFormat, Bt8x8PALFrameSizeFactorisesTheReferenceCaptureLength) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;

  constexpr uint64_t kReferenceRawBytes = 24117706752ull;
  EXPECT_EQ(kReferenceRawBytes % format.bytes_per_frame(), 0u);
  EXPECT_EQ(kReferenceRawBytes / format.bytes_per_frame(), 368007u);
}

TEST(VBISourceFormat, UnknownPresetIsRejectedAndListsTheKnownNames) {
  VBISourceFormat format;
  std::string error;

  EXPECT_FALSE(expand_vbi_source_preset("bt8x8-secam", format, error));
  EXPECT_NE(error.find("bt8x8-secam"), std::string::npos);
  EXPECT_NE(error.find("bt8x8 card dump, 8-bit (WST)"), std::string::npos);
}

// Just enough presets to cover the captures that exist, and every one of them
// a complete configuration: there is no "custom" entry to leave half filled in.
TEST(VBISourceFormat, PresetNamesAreTheWholeChoiceAndAllExpand) {
  const std::vector<std::string> names = vbi_source_preset_names();

  EXPECT_EQ(
      names,
      (std::vector<std::string>{
          "bt8x8 card dump, 8-bit (WST)",
          "bt8x8 card dump, 8-bit (WST, SECAM source)",
          "bt8x8 card dump, 8-bit (WST, NTSC source)",
          "bt8x8 card dump, 8-bit (NABTS, NTSC source)", kCx2388xNTSCWSTPreset,
          kCx2388xNTSCNABTSPreset, "cx23885 card dump, 8-bit (WST)",
          "cx23885 card dump, 8-bit (NABTS)", ".tbc VBI crop, 16-bit (WST)",
          ".tbc VBI crop, 16-bit (NABTS)"}));

  for (const std::string& name : names) {
    VBISourceFormat format;
    std::string error;
    ASSERT_TRUE(expand_vbi_source_preset(name, format, error)) << name;
    // Complete means complete: nothing is left at an unusable default.
    EXPECT_GT(format.sample_rate_hz, 0.0) << name;
    EXPECT_GT(format.line_length, 0u) << name;
    EXPECT_GT(format.valid_samples, 0u) << name;
    EXPECT_GT(format.field_lines, 0u) << name;
    EXPECT_GT(format.field_range.count(), 0u) << name;
    EXPECT_TRUE(format.first_field == 1u || format.first_field == 2u) << name;
    EXPECT_TRUE(validate_vbi_source_config(format, std::nullopt).empty())
        << name;
  }
}

// A capture's television system fixes the geometry of the frames it can be
// placed on, so a project is only offered the presets of its own system.
TEST(VBISourceFormat, PresetNamesAreFilteredByTelevisionSystem) {
  const std::vector<std::string> pal =
      vbi_source_preset_names(VBITVSystem::kPAL);
  const std::vector<std::string> ntsc =
      vbi_source_preset_names(VBITVSystem::kNTSC);

  // A PAL project is offered the one card container in its two source
  // flavours; an NTSC project is offered its three card containers and its .tbc
  // crop, each in the two services its captures might carry.
  EXPECT_EQ(pal, (std::vector<std::string>{
                     "bt8x8 card dump, 8-bit (WST)",
                     "bt8x8 card dump, 8-bit (WST, SECAM source)"}));
  EXPECT_EQ(
      ntsc,
      (std::vector<std::string>{
          "bt8x8 card dump, 8-bit (WST, NTSC source)",
          "bt8x8 card dump, 8-bit (NABTS, NTSC source)", kCx2388xNTSCWSTPreset,
          kCx2388xNTSCNABTSPreset, "cx23885 card dump, 8-bit (WST)",
          "cx23885 card dump, 8-bit (NABTS)", ".tbc VBI crop, 16-bit (WST)",
          ".tbc VBI crop, 16-bit (NABTS)"}));

  // Between them the two lists are the whole table: no preset is unreachable.
  EXPECT_EQ(pal.size() + ntsc.size(), vbi_source_preset_names().size());
}

// The cx23885 container is the Linux driver's cx23885_vbi_fmt(), field for
// field: 27 MHz, VBI_LINE_LENGTH samples with no padding, and
// VBI_NTSC_LINE_COUNT records from V4L2_VBI_ITU_525_F1_START + 9. Getting any
// of these wrong is a silently wrong decode rather than a refusal.
TEST(VBISourceFormat, Cx23885NTSCPresetExpandsToTheDriverContainer) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(expand_vbi_source_preset("cx23885 card dump, 8-bit (NABTS)",
                                       format, error))
      << error;
  EXPECT_TRUE(error.empty());

  EXPECT_DOUBLE_EQ(format.sample_rate_hz, 27000000.0);
  EXPECT_EQ(format.line_length, 1440u);
  EXPECT_EQ(format.sample_format, VBISampleFormat::kU8);
  EXPECT_EQ(format.field_lines, 12u);

  // The window is the digital active line, so every stored sample is real:
  // no padding, and therefore nowhere for a frame counter to live.
  EXPECT_EQ(format.valid_samples, format.line_length);
  EXPECT_EQ(format.record_padding_bytes(), 0u);
  EXPECT_EQ(format.frame_trailer_bytes, 0u);
  EXPECT_FALSE(format.frame_trailer_is_counter);

  // Twelve records from field line 10 is the whole 525-line teletext list, so
  // unlike the .tbc crop there are no records to skip at either end.
  EXPECT_EQ(format.field_range.start, 0u);
  EXPECT_EQ(format.field_range.end, 11u);
  EXPECT_EQ(format.field_range.count(),
            standard_teletext_lines_per_field(VBITVSystem::kNTSC,
                                              VBITeletextSystem::kNABTS));

  // start[0] is the field 1 line list and the driver stores it first.
  EXPECT_EQ(format.first_field, 1u);

  EXPECT_EQ(format.tv_system, VBITVSystem::kNTSC);
  EXPECT_EQ(format.tt_system, VBITeletextSystem::kNABTS);
  EXPECT_EQ(format.family, VBISourceFamily::kCardCapture);
}

// 1440 samples of one byte, twelve records to a field, two fields to a frame.
TEST(VBISourceFormat, Cx23885NTSCFrameGeometryDerivesTheCaptureByteCounts) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset("cx23885 card dump, 8-bit (NABTS)",
                                       format, error))
      << error;

  EXPECT_EQ(format.bytes_per_sample(), 1u);
  EXPECT_EQ(format.bytes_per_record(), 1440u);
  EXPECT_EQ(format.bytes_per_field(), 17280u);
  EXPECT_EQ(format.bytes_per_frame(), 34560u);
}

// The two services share the container completely — the card knows nothing
// about what the lines carry — so the entries may differ in the service and in
// nothing else.
TEST(VBISourceFormat, Cx23885NTSCServiceVariantsShareOneContainer) {
  VBISourceFormat wst;
  VBISourceFormat nabts;
  std::string error;

  ASSERT_TRUE(
      expand_vbi_source_preset("cx23885 card dump, 8-bit (WST)", wst, error))
      << error;
  ASSERT_TRUE(expand_vbi_source_preset("cx23885 card dump, 8-bit (NABTS)",
                                       nabts, error))
      << error;

  EXPECT_DOUBLE_EQ(nabts.sample_rate_hz, wst.sample_rate_hz);
  EXPECT_EQ(nabts.line_length, wst.line_length);
  EXPECT_EQ(nabts.valid_samples, wst.valid_samples);
  EXPECT_EQ(nabts.sample_format, wst.sample_format);
  EXPECT_EQ(nabts.field_lines, wst.field_lines);
  EXPECT_EQ(nabts.field_range.start, wst.field_range.start);
  EXPECT_EQ(nabts.field_range.end, wst.field_range.end);
  EXPECT_EQ(nabts.first_field, wst.first_field);
  EXPECT_DOUBLE_EQ(nabts.capture_offset_samples, wst.capture_offset_samples);
  EXPECT_EQ(nabts.capture_offset_is_auto, wst.capture_offset_is_auto);
  EXPECT_EQ(nabts.family, wst.family);

  EXPECT_EQ(wst.tt_system, VBITeletextSystem::kWST);
  EXPECT_EQ(nabts.tt_system, VBITeletextSystem::kNABTS);
}

// The driver reports f->fmt.vbi.offset = 0 without measuring anything, so the
// configured figure is the window SMPTE 125M says the card's timing generator
// produces — 122 samples at 13,5 MHz, which is 244 at 27 MHz — and it is a
// starting point for calibration rather than a value to be used as configured.
TEST(VBISourceFormat, Cx23885NTSCPresetCalibratesItsCaptureOffset) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset("cx23885 card dump, 8-bit (NABTS)",
                                       format, error))
      << error;

  EXPECT_TRUE(format.capture_offset_is_auto);
  EXPECT_DOUBLE_EQ(format.capture_offset_samples, 244.0);

  // The reference capture puts sample 0 at 9,31 us against the standard's
  // 9,04, and the service anchor is a further 22 samples out at this rate, so
  // the search has to reach a good deal further than the figure itself
  // suggests.
  EXPECT_GE(format.calibration.search_tolerance_samples, 30.0);
}

// A US network carried its magazine on two or three of the twelve lines, so the
// share of records that lock is a fraction of what a 625-line capture shows.
// Only that one threshold is relaxed: the spread and drift checks, which are
// what actually catch a wrong container, keep the bt8x8 preset's tolerance
// expressed at this preset's sampling rate.
TEST(VBISourceFormat, Cx23885NTSCPresetExpectsFewLinesToCarryTeletext) {
  VBISourceFormat cx;
  VBISourceFormat bt;
  std::string error;
  ASSERT_TRUE(
      expand_vbi_source_preset("cx23885 card dump, 8-bit (NABTS)", cx, error))
      << error;
  ASSERT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", bt, error))
      << error;

  EXPECT_GT(cx.calibration.minimum_acceptance_fraction, 0.0);
  EXPECT_LT(cx.calibration.minimum_acceptance_fraction,
            bt.calibration.minimum_acceptance_fraction);

  // Both captures of the reference broadcast carry the service on two records
  // of the twelve, so the bar has to sit below that and not far below it.
  EXPECT_LT(cx.calibration.minimum_acceptance_fraction, 2.0 / 12.0);
  EXPECT_GT(cx.calibration.minimum_acceptance_fraction, 1.0 / 12.0);

  // 8 samples at the bt8x8's 35,47 MHz is 226 ns; the same tolerance in time
  // is what this preset allows at 27 MHz, rather than the same count.
  const double bt_spread_ns =
      bt.calibration.maximum_spread_samples / bt.sample_rate_hz * 1e9;
  const double cx_spread_ns =
      cx.calibration.maximum_spread_samples / cx.sample_rate_hz * 1e9;
  EXPECT_NEAR(cx_spread_ns, bt_spread_ns, 10.0);
  EXPECT_DOUBLE_EQ(cx.calibration.maximum_drift_samples,
                   cx.calibration.maximum_spread_samples);
  EXPECT_DOUBLE_EQ(cx.calibration.acceptance_correlation,
                   bt.calibration.acceptance_correlation);
}

// The 525-line cx2388x container is the cx88 driver's cx8800_vbi_fmt(), field
// for field: 8 x fsc NTSC, a 2048-byte record of which 1600 samples are real,
// and VBI_LINE_NTSC_COUNT records from field line 10.  Every one of these was
// confirmed against the reference capture.
TEST(VBISourceFormat, Cx2388xNTSCPresetExpandsToTheDriverContainer) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(expand_vbi_source_preset(kCx2388xNTSCNABTSPreset, format, error))
      << error;
  EXPECT_TRUE(error.empty());

  EXPECT_DOUBLE_EQ(format.sample_rate_hz, 28636363.0);
  EXPECT_EQ(format.line_length, 2048u);
  EXPECT_EQ(format.valid_samples, 1600u);
  EXPECT_EQ(format.sample_format, VBISampleFormat::kU8);
  EXPECT_EQ(format.field_lines, 12u);

  // Twelve records from field line 10 is the whole 525-line teletext list.
  EXPECT_EQ(format.field_range.start, 0u);
  EXPECT_EQ(format.field_range.end, 11u);
  EXPECT_EQ(format.field_range.count(),
            standard_teletext_lines_per_field(VBITVSystem::kNTSC,
                                              VBITeletextSystem::kNABTS));

  // The padding holds a constant, not a sequence number.
  EXPECT_EQ(format.record_padding_bytes(), 448u);
  EXPECT_EQ(format.frame_trailer_bytes, 0u);
  EXPECT_FALSE(format.frame_trailer_is_counter);

  EXPECT_EQ(format.first_field, 1u);
  EXPECT_EQ(format.tv_system, VBITVSystem::kNTSC);
  EXPECT_EQ(format.tt_system, VBITeletextSystem::kNABTS);
  EXPECT_EQ(format.family, VBISourceFamily::kCardCapture);
}

// The same clock, stride and valid sample count as the bt8x8 on this norm, and
// so the same calibration; what differs is the field — twelve records rather
// than sixteen — and the frame counter the bt8x8 has and this card does not.
TEST(VBISourceFormat, Cx2388xNTSCPresetDiffersFromBt8x8NTSCOnlyInItsField) {
  VBISourceFormat cx;
  VBISourceFormat bt;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset(kCx2388xNTSCWSTPreset, cx, error))
      << error;
  ASSERT_TRUE(expand_vbi_source_preset(kBt8x8NTSCWSTPreset, bt, error))
      << error;

  EXPECT_DOUBLE_EQ(cx.sample_rate_hz, bt.sample_rate_hz);
  EXPECT_EQ(cx.line_length, bt.line_length);
  EXPECT_EQ(cx.valid_samples, bt.valid_samples);
  EXPECT_EQ(cx.sample_format, bt.sample_format);
  EXPECT_EQ(cx.field_range.start, bt.field_range.start);
  EXPECT_EQ(cx.field_range.end, bt.field_range.end);
  EXPECT_DOUBLE_EQ(cx.capture_offset_samples, bt.capture_offset_samples);
  EXPECT_TRUE(cx.capture_offset_is_auto);

  EXPECT_DOUBLE_EQ(cx.calibration.search_tolerance_samples,
                   bt.calibration.search_tolerance_samples);
  EXPECT_DOUBLE_EQ(cx.calibration.maximum_spread_samples,
                   bt.calibration.maximum_spread_samples);
  EXPECT_DOUBLE_EQ(cx.calibration.maximum_drift_samples,
                   bt.calibration.maximum_drift_samples);
  EXPECT_DOUBLE_EQ(cx.calibration.minimum_acceptance_fraction,
                   bt.calibration.minimum_acceptance_fraction);

  // The reference capture scatters by 2,6 samples.
  EXPECT_GT(cx.calibration.maximum_spread_samples, 2.6);

  EXPECT_NE(cx.field_lines, bt.field_lines);
  EXPECT_NE(cx.frame_trailer_is_counter, bt.frame_trailer_is_counter);
  EXPECT_EQ(cx.bytes_per_frame(), 49152u);
}

// The card knows nothing about what the lines carry, so the two service entries
// may differ in the service and in nothing else.
TEST(VBISourceFormat, Cx2388xNTSCServiceVariantsShareOneContainer) {
  VBISourceFormat wst;
  VBISourceFormat nabts;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset(kCx2388xNTSCWSTPreset, wst, error))
      << error;
  ASSERT_TRUE(expand_vbi_source_preset(kCx2388xNTSCNABTSPreset, nabts, error))
      << error;

  EXPECT_DOUBLE_EQ(nabts.sample_rate_hz, wst.sample_rate_hz);
  EXPECT_EQ(nabts.line_length, wst.line_length);
  EXPECT_EQ(nabts.valid_samples, wst.valid_samples);
  EXPECT_EQ(nabts.field_lines, wst.field_lines);
  EXPECT_EQ(nabts.field_range.start, wst.field_range.start);
  EXPECT_EQ(nabts.field_range.end, wst.field_range.end);
  EXPECT_EQ(nabts.first_field, wst.first_field);
  EXPECT_DOUBLE_EQ(nabts.capture_offset_samples, wst.capture_offset_samples);
  EXPECT_EQ(nabts.family, wst.family);

  EXPECT_EQ(wst.tt_system, VBITeletextSystem::kWST);
  EXPECT_EQ(nabts.tt_system, VBITeletextSystem::kNABTS);
}

// The reference sample (issue #332) is 316 735 488 raw bytes, which must
// factorise into exactly 6444 whole cx2388x NTSC frames.
TEST(VBISourceFormat, Cx2388xNTSCFrameSizeFactorisesTheReferenceCaptureLength) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset(kCx2388xNTSCNABTSPreset, format, error))
      << error;

  constexpr uint64_t kReferenceRawBytes = 316735488ull;
  EXPECT_EQ(kReferenceRawBytes % format.bytes_per_frame(), 0u);
  EXPECT_EQ(kReferenceRawBytes / format.bytes_per_frame(), 6444u);
}

// The circulating NTSC VBI-only crops: sixteen whole 4 x fsc .tbc lines per
// field with no padding, of which records 1 to 12 are broadcast lines 10-21.
TEST(VBISourceFormat, TBCVBINTSCPresetExpandsToTheMeasuredContainer) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (WST)", format, error))
      << error;

  EXPECT_DOUBLE_EQ(format.sample_rate_hz, kNtscSampleRate);
  EXPECT_EQ(format.line_length, 910u);
  EXPECT_EQ(format.valid_samples, 910u);
  EXPECT_EQ(format.sample_format, VBISampleFormat::kU16LE);
  EXPECT_EQ(format.field_lines, 16u);
  EXPECT_EQ(format.field_range.start, 1u);
  EXPECT_EQ(format.field_range.end, 12u);
  EXPECT_EQ(format.field_range.count(), 12u);
  EXPECT_EQ(format.frame_trailer_bytes, 0u);
  EXPECT_FALSE(format.frame_trailer_is_counter);
  EXPECT_EQ(format.record_padding_bytes(), 0u);
  EXPECT_EQ(format.bytes_per_field(), 29120u);
  EXPECT_EQ(format.bytes_per_frame(), 58240u);
  EXPECT_EQ(format.tv_system, VBITVSystem::kNTSC);
  EXPECT_EQ(format.tt_system, VBITeletextSystem::kWST);
  EXPECT_EQ(format.family, VBISourceFamily::kTBCDerived);
}

// A time-base corrected capture already has sample 0 of every record at 0H, so
// its offset is known exactly and must never be fitted (design §5.3.3).
TEST(VBISourceFormat, TBCVBINTSCPresetNeverCalibratesItsCaptureOffset) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (WST)", format, error))
      << error;

  EXPECT_FALSE(format.capture_offset_is_auto);
  EXPECT_DOUBLE_EQ(format.capture_offset_samples, 0.0);
}

// The standard's teletext line lists (design §5.1) bound how many records a
// source may usefully carry per field.
TEST(VBISourceFormat, StandardTeletextLineCountsMatchTheSystemDefinitions) {
  EXPECT_EQ(standard_teletext_lines_per_field(VBITVSystem::kPAL,
                                              VBITeletextSystem::kWST),
            16u);

  // Both 525-line services sit on the same twelve lines per field; they differ
  // in framing code and packet length, not in vertical placement.
  EXPECT_EQ(standard_teletext_lines_per_field(VBITVSystem::kNTSC,
                                              VBITeletextSystem::kWST),
            12u);
  EXPECT_EQ(standard_teletext_lines_per_field(VBITVSystem::kNTSC,
                                              VBITeletextSystem::kNABTS),
            12u);
  EXPECT_EQ(standard_teletext_lines_per_field(VBITVSystem::kPALM,
                                              VBITeletextSystem::kNABTS),
            12u);
}

// A pairing with no defined line list reports zero rather than guessing one.
TEST(VBISourceFormat, MismatchedSystemPairingHasNoDefinedLineList) {
  EXPECT_EQ(standard_teletext_lines_per_field(VBITVSystem::kPAL,
                                              VBITeletextSystem::kNABTS),
            0u);
}

// The spelling still appears in error messages, so it still has to be right.
TEST(VBISourceFormat, SampleFormatsHaveTheirDocumentedSpellings) {
  EXPECT_EQ(to_string(VBISampleFormat::kU8), "u8");
  EXPECT_EQ(to_string(VBISampleFormat::kU16LE), "u16le");
  EXPECT_EQ(to_string(VBISampleFormat::kS16LE), "s16le");
}

TEST(VBISourceFormat, SixteenBitContainersMeasureTwoBytesPerSample) {
  VBISourceFormat format;
  format.line_length = 1135;
  format.valid_samples = 1135;
  format.field_lines = 16;
  format.sample_format = VBISampleFormat::kU16LE;

  EXPECT_EQ(format.bytes_per_sample(), 2u);
  EXPECT_EQ(format.bytes_per_record(), 2270u);
  EXPECT_EQ(format.bytes_per_frame(), 72640u);
  EXPECT_EQ(format.record_padding_bytes(), 0u);
}

}  // namespace
}  // namespace orc
