/*
 * File:        vbi_source_validation_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for raw VBI capture configuration validation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_validation.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "vbi_source_format.h"

namespace orc {
namespace {

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
  return format;
}

// Does any reported violation name the given parameter or phrase?
bool mentions(const std::vector<std::string>& errors,
              const std::string& needle) {
  for (const std::string& error : errors) {
    if (error.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string joined(const std::vector<std::string>& errors) {
  std::string text;
  for (const std::string& error : errors) {
    text += "\n  " + error;
  }
  return text;
}

TEST(VBISourceValidation, ReferenceCaptureConfigurationIsAccepted) {
  const VBISourceFormat format = bt8x8_pal_format();
  constexpr uint64_t kReferenceRawBytes = 24117706752ull;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, kReferenceRawBytes);

  EXPECT_TRUE(errors.empty()) << joined(errors);
}

// A capture stops when it stops, and nothing that writes one of these files
// rounds it off first. Whatever follows the last whole frame is short of a
// frame and is simply not emitted, so no trailing remainder is refused.
TEST(VBISourceValidation, AnyTrailingRemainderIsAccepted) {
  const VBISourceFormat format = bt8x8_pal_format();
  const uint64_t frame_bytes = format.bytes_per_frame();

  for (const uint64_t remainder :
       {uint64_t{0}, uint64_t{7}, format.bytes_per_field(),
        format.bytes_per_record(), frame_bytes - 1}) {
    const std::vector<std::string> errors =
        validate_vbi_source_config(format, frame_bytes * 10 + remainder);
    EXPECT_TRUE(errors.empty()) << remainder << ": " << joined(errors);
  }
}

// The circulating VBI-only .tbc crops end on an odd field about as often as
// not, which is the ordinary case the tolerance was first written for.
TEST(VBISourceValidation, ATrailingWholeFieldIsAccepted) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (WST)", format, error))
      << error;

  const uint64_t frame_bytes = format.bytes_per_frame();
  const uint64_t field_bytes = format.bytes_per_field();

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, frame_bytes * 100 + field_bytes);

  EXPECT_TRUE(errors.empty()) << joined(errors);
}

// A card dump killed at the keyboard stops part-way through a record, which is
// no more a misconfiguration than stopping on a field boundary is. What a
// remainder can no longer do is witness a wrong geometry — the run-in fit is
// what catches that, and it does so naming the cause.
TEST(VBISourceValidation, ACaptureCutMidRecordIsAccepted) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset("cx23885 card dump, 8-bit (NABTS)",
                                       format, error))
      << error;

  // The reference capture: 18 093 whole frames and 34 048 bytes that are
  // neither a whole field nor a whole record.
  const std::vector<std::string> errors =
      validate_vbi_source_config(format, 625328128ull);

  EXPECT_TRUE(errors.empty()) << joined(errors);
  EXPECT_EQ(625328128ull / format.bytes_per_frame(), 18093ull);
  EXPECT_EQ(625328128ull % format.bytes_per_frame(), 34048ull);
}

// A stream too short to hold a frame at all says so, rather than reporting a
// factorisation remainder the user cannot act on.
TEST(VBISourceValidation, ACaptureShorterThanOneFrameIsRejected) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (WST)", format, error))
      << error;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, format.bytes_per_field());

  EXPECT_TRUE(mentions(errors, "one complete frame")) << joined(errors);
}

TEST(VBISourceValidation, EmptyCaptureIsRejected) {
  const VBISourceFormat format = bt8x8_pal_format();

  const std::vector<std::string> errors = validate_vbi_source_config(format, 0);

  EXPECT_TRUE(mentions(errors, "empty")) << joined(errors);
}

TEST(VBISourceValidation, UnknownStreamLengthSkipsTheFactorisationCheck) {
  const VBISourceFormat format = bt8x8_pal_format();

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(errors.empty()) << joined(errors);
}

// The excess records would have no frame line to map to, so this is an error
// rather than a truncation.
TEST(VBISourceValidation, FieldRangeLongerThanTheStandardLineListIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_lines = 20;
  format.field_range = VBIFieldRange{0, 17};  // 18 records; WST allows 16

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "container.field_range")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "16")) << joined(errors);
}

TEST(VBISourceValidation, FieldRangeBeyondTheStoredRecordsIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_range = VBIFieldRange{4, 19};  // field only stores 16 records

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "container.field_lines")) << joined(errors);
}

TEST(VBISourceValidation, InvertedFieldRangeIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_range = VBIFieldRange{10, 4};

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "inverted")) << joined(errors);
}

TEST(VBISourceValidation, ValidSamplesBeyondTheRecordStrideIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.valid_samples = 2100;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "container.valid_samples")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "container.line_length")) << joined(errors);
}

// The trailer overlaps the final record's padding rather than extending the
// frame, so it can never be larger than that padding.
TEST(VBISourceValidation, FrameTrailerLargerThanTheRecordPaddingIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.frame_trailer_bytes = 8;  // only four padding bytes exist

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "container.frame_trailer_bytes"))
      << joined(errors);
}

TEST(VBISourceValidation, FrameCounterTrailerMustBeWideEnoughToHoldIt) {
  VBISourceFormat format = bt8x8_pal_format();
  format.frame_trailer_bytes = 2;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "4-byte frame sequence number"))
      << joined(errors);
}

// Sample 0 of a TBC-derived record is 0H by construction, so the rule is
// enforced now even though those formats are not yet readable.
TEST(VBISourceValidation, TBCDerivedSourceMustNotCalibrateItsCaptureOffset) {
  VBISourceFormat format = bt8x8_pal_format();
  format.family = VBISourceFamily::kTBCDerived;
  format.capture_offset_is_auto = true;
  format.capture_offset_samples = 244.0;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "calibration.capture_offset")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "'auto'")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "0H")) << joined(errors);
}

// A crop may start a sample or two either side of 0H, and the user is the only
// one who can say so, so a configured figure is allowed even though a fitted
// one is not.
TEST(VBISourceValidation, TBCDerivedSourceMayBeGivenAManualOffset) {
  VBISourceFormat format = bt8x8_pal_format();
  format.family = VBISourceFamily::kTBCDerived;
  format.capture_offset_is_auto = false;
  format.capture_offset_samples = -1.5;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_FALSE(mentions(errors, "calibration.capture_offset"))
      << joined(errors);
}

TEST(VBISourceValidation, TBCDerivedSourceWithAZeroOffsetIsAccepted) {
  VBISourceFormat format = bt8x8_pal_format();
  format.family = VBISourceFamily::kTBCDerived;
  format.capture_offset_is_auto = false;
  format.capture_offset_samples = 0.0;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_FALSE(mentions(errors, "calibration.capture_offset"))
      << joined(errors);
}

TEST(VBISourceValidation, CardCaptureMayCalibrateItsCaptureOffset) {
  VBISourceFormat format = bt8x8_pal_format();
  ASSERT_EQ(format.family, VBISourceFamily::kCardCapture);
  ASSERT_TRUE(format.capture_offset_is_auto);

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_FALSE(mentions(errors, "calibration.capture_offset"))
      << joined(errors);
}

TEST(VBISourceValidation, UnsupportedSystemPairingIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.tt_system = VBITeletextSystem::kNABTS;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "teletext.system")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "NABTS")) << joined(errors);
}

// No preset expands to an empty descriptor, but validation is what would catch
// a preset entry that forgot a field, so it must name every one of them rather
// than stopping at the first.
TEST(VBISourceValidation, AnEmptyContainerNamesEveryMissingField) {
  const VBISourceFormat format;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "container.sample_rate")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "container.line_length")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "container.field_lines")) << joined(errors);
}

TEST(VBISourceValidation, TransportBitsPerSampleMustMatchTheConfiguredFormat) {
  const VBISourceFormat format = bt8x8_pal_format();

  VBITransportHints hints;
  hints.bits_per_sample = 16;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt, hints);

  EXPECT_TRUE(mentions(errors, "container.sample_format")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "u8")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "16 bits")) << joined(errors);
}

VBISourceFormat tbc_crop_nabts_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (NABTS)", format, error))
      << error;
  return format;
}

// A crop kept as a video stores a record to a picture row: a 910-sample record
// is a 910-wide picture, however many rows the picture has.
TEST(VBISourceValidation, PicturesAsWideAsTheRecordsAreAccepted) {
  const VBISourceFormat format = tbc_crop_nabts_format();

  VBITransportHints hints;
  hints.bits_per_sample = 16;
  hints.record_samples = 910;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, format.bytes_per_frame() * 4u, hints);

  EXPECT_TRUE(errors.empty()) << joined(errors);
}

// A video of 720-sample rows is not a 910-sample crop, however its bytes
// happen to divide: read as one, every record would straddle two rows.
TEST(VBISourceValidation, PicturesOfAnotherWidthThanTheRecordsAreRejected) {
  const VBISourceFormat format = tbc_crop_nabts_format();

  VBITransportHints hints;
  hints.bits_per_sample = 16;
  hints.record_samples = 720;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, format.bytes_per_frame() * 4u, hints);

  EXPECT_TRUE(mentions(errors, "container.line_length")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "720 samples wide")) << joined(errors);
}

// A flat stream has no rows, so it declares no record length and nothing about
// the format's is checked against one.
TEST(VBISourceValidation, AFlatStreamDeclaresNoRecordLength) {
  const VBISourceFormat format = tbc_crop_nabts_format();

  VBITransportHints hints;
  hints.bits_per_sample = 16;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, format.bytes_per_frame() * 4u, hints);

  EXPECT_TRUE(errors.empty()) << joined(errors);
}

}  // namespace
}  // namespace orc
