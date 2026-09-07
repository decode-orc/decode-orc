/*
 * File:        preview_types_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for Phase 1 preview-refactor foundation types:
 *              - VideoDataType taxonomy enum coverage
 *              - ColorimetricMetadata defaults, round-trip, and equality
 *              - PreviewCoordinate construction, equality, and validity
 *              - PreviewNavigationExtent and PreviewGeometry validity
 *              - StagePreviewCapability schema and validity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/preview/orc_preview_types.h>
#include <orc/stage/preview/orc_vectorscope.h>
#include <orc/stage/preview/stage_preview_capability.h>

#include <climits>

#include "../../../orc/view-types/orc_preview_views.h"

namespace orc_unit_test {

// =============================================================================
// VideoDataType — taxonomy enum coverage
// =============================================================================

TEST(VideoDataTypeTest, AllSixValues_AreDistinct) {
  using T = orc::VideoDataType;
  EXPECT_NE(T::CompositeNTSC, T::CompositePAL);
  EXPECT_NE(T::YC_NTSC, T::YC_PAL);
  EXPECT_NE(T::ColourNTSC, T::ColourPAL);
  EXPECT_NE(T::CompositeNTSC, T::YC_NTSC);
  EXPECT_NE(T::CompositeNTSC, T::ColourNTSC);
  EXPECT_NE(T::YC_NTSC, T::ColourNTSC);
}

TEST(VideoDataTypeTest, SignalDomainTypes_AreDistinctFromColourDomainTypes) {
  // Signal-domain types (Composite*, YC_*) must be distinguishable
  // from colour-domain types (Colour*).
  using T = orc::VideoDataType;
  EXPECT_NE(T::CompositeNTSC, T::ColourNTSC);
  EXPECT_NE(T::CompositeNTSC, T::ColourPAL);
  EXPECT_NE(T::CompositePAL, T::ColourNTSC);
  EXPECT_NE(T::CompositePAL, T::ColourPAL);
  EXPECT_NE(T::YC_NTSC, T::ColourNTSC);
  EXPECT_NE(T::YC_NTSC, T::ColourPAL);
  EXPECT_NE(T::YC_PAL, T::ColourNTSC);
  EXPECT_NE(T::YC_PAL, T::ColourPAL);
}

TEST(VideoDataTypeTest, NtscVariants_AreDistinctFromPalVariants) {
  using T = orc::VideoDataType;
  EXPECT_NE(T::CompositeNTSC, T::CompositePAL);
  EXPECT_NE(T::YC_NTSC, T::YC_PAL);
  EXPECT_NE(T::ColourNTSC, T::ColourPAL);
}

// =============================================================================
// ColorimetricMatrixCoefficients — enum sanity
// =============================================================================

TEST(ColorimetricMatrixCoefficientsTest,
     Unspecified_IsDistinctFromAllConcreteValues) {
  using M = orc::ColorimetricMatrixCoefficients;
  EXPECT_NE(M::Unspecified, M::NTSC1953_FCC);
  EXPECT_NE(M::Unspecified, M::BT601_625);
  EXPECT_NE(M::Unspecified, M::BT601_525);
}

TEST(ColorimetricMatrixCoefficientsTest, NtscAndPalMatrices_AreDistinct) {
  using M = orc::ColorimetricMatrixCoefficients;
  EXPECT_NE(M::BT601_525, M::BT601_625);
}

// =============================================================================
// ColorimetricPrimaries — enum sanity
// =============================================================================

TEST(ColorimetricPrimariesTest, Unspecified_IsDistinctFromAllConcreteValues) {
  using P = orc::ColorimetricPrimaries;
  EXPECT_NE(P::Unspecified, P::NTSC1953);
  EXPECT_NE(P::Unspecified, P::SMPTE_C);
  EXPECT_NE(P::Unspecified, P::EBU_BT470_PAL);
  EXPECT_NE(P::Unspecified, P::BT709);
}

TEST(ColorimetricPrimariesTest, NtscAndPalPrimaries_AreDistinct) {
  EXPECT_NE(orc::ColorimetricPrimaries::SMPTE_C,
            orc::ColorimetricPrimaries::EBU_BT470_PAL);
}

// =============================================================================
// ColorimetricTransferCharacteristics — enum sanity
// =============================================================================

TEST(ColorimetricTransferCharacteristicsTest,
     Unspecified_IsDistinctFromAllConcreteValues) {
  using TC = orc::ColorimetricTransferCharacteristics;
  EXPECT_NE(TC::Unspecified, TC::Gamma22);
  EXPECT_NE(TC::Unspecified, TC::Gamma28);
  EXPECT_NE(TC::Unspecified, TC::BT709);
  EXPECT_NE(TC::Unspecified, TC::BT1886);
  EXPECT_NE(TC::Unspecified, TC::BT1886App1);
}

TEST(ColorimetricTransferCharacteristicsTest, NtscAndPalGammas_AreDistinct) {
  EXPECT_NE(orc::ColorimetricTransferCharacteristics::Gamma22,
            orc::ColorimetricTransferCharacteristics::Gamma28);
}

// =============================================================================
// ColorimetricMetadata — defaults
// =============================================================================

TEST(ColorimetricMetadataTest,
     DefaultConstructed_HasUnspecifiedMatrixCoefficients) {
  orc::ColorimetricMetadata meta{};
  EXPECT_EQ(meta.matrix_coefficients,
            orc::ColorimetricMatrixCoefficients::Unspecified);
}

TEST(ColorimetricMetadataTest, DefaultConstructed_HasUnspecifiedPrimaries) {
  orc::ColorimetricMetadata meta{};
  EXPECT_EQ(meta.primaries, orc::ColorimetricPrimaries::Unspecified);
}

TEST(ColorimetricMetadataTest,
     DefaultConstructed_HasUnspecifiedTransferCharacteristics) {
  orc::ColorimetricMetadata meta{};
  EXPECT_EQ(meta.transfer_characteristics,
            orc::ColorimetricTransferCharacteristics::Unspecified);
}

TEST(ColorimetricMetadataTest, DefaultNtsc_HasExpectedMatrixCoefficients) {
  auto meta = orc::ColorimetricMetadata::default_ntsc();
  EXPECT_EQ(meta.matrix_coefficients,
            orc::ColorimetricMatrixCoefficients::BT601_525);
}

TEST(ColorimetricMetadataTest, DefaultNtsc_HasExpectedPrimaries) {
  auto meta = orc::ColorimetricMetadata::default_ntsc();
  EXPECT_EQ(meta.primaries, orc::ColorimetricPrimaries::SMPTE_C);
}

TEST(ColorimetricMetadataTest, DefaultNtsc_HasExpectedTransferCharacteristics) {
  auto meta = orc::ColorimetricMetadata::default_ntsc();
  EXPECT_EQ(meta.transfer_characteristics,
            orc::ColorimetricTransferCharacteristics::Gamma22);
}

TEST(ColorimetricMetadataTest, DefaultPal_HasExpectedMatrixCoefficients) {
  auto meta = orc::ColorimetricMetadata::default_pal();
  EXPECT_EQ(meta.matrix_coefficients,
            orc::ColorimetricMatrixCoefficients::BT601_625);
}

TEST(ColorimetricMetadataTest, DefaultPal_HasExpectedPrimaries) {
  auto meta = orc::ColorimetricMetadata::default_pal();
  EXPECT_EQ(meta.primaries, orc::ColorimetricPrimaries::EBU_BT470_PAL);
}

TEST(ColorimetricMetadataTest, DefaultPal_HasExpectedTransferCharacteristics) {
  auto meta = orc::ColorimetricMetadata::default_pal();
  EXPECT_EQ(meta.transfer_characteristics,
            orc::ColorimetricTransferCharacteristics::Gamma28);
}

TEST(ColorimetricMetadataTest, DefaultPalM_UsesSystemMColorimetry) {
  // PAL-M is PAL chroma encoding on the 525-line System M raster, and BT.470
  // assigns colorimetry by system: System M's 2.2 gamma and SMPTE C primaries,
  // not the 2.8 gamma of the 625-line PAL systems.
  auto meta = orc::ColorimetricMetadata::default_pal_m();
  EXPECT_EQ(meta.matrix_coefficients,
            orc::ColorimetricMatrixCoefficients::BT601_525);
  EXPECT_EQ(meta.primaries, orc::ColorimetricPrimaries::SMPTE_C);
  EXPECT_EQ(meta.transfer_characteristics,
            orc::ColorimetricTransferCharacteristics::Gamma22);
}

TEST(ColorimetricMetadataTest, DefaultPalM_IsNotTheDefaultPalColorimetry) {
  EXPECT_NE(orc::ColorimetricMetadata::default_pal_m(),
            orc::ColorimetricMetadata::default_pal());
}

// =============================================================================
// ColorimetricMetadata — equality and round-trip
// =============================================================================

TEST(ColorimetricMetadataTest, DefaultNtscAndDefaultPal_AreNotEqual) {
  EXPECT_NE(orc::ColorimetricMetadata::default_ntsc(),
            orc::ColorimetricMetadata::default_pal());
}

TEST(ColorimetricMetadataTest, Equality_IsReflexive) {
  auto meta = orc::ColorimetricMetadata::default_ntsc();
  EXPECT_EQ(meta, meta);
}

TEST(ColorimetricMetadataTest, Copy_PreservesAllFields) {
  auto original = orc::ColorimetricMetadata::default_pal();
  auto copy = original;
  EXPECT_EQ(copy, original);
}

TEST(ColorimetricMetadataTest, ModifiedMatrix_IsNotEqualToOriginal) {
  auto original = orc::ColorimetricMetadata::default_ntsc();
  auto modified = original;
  modified.matrix_coefficients =
      orc::ColorimetricMatrixCoefficients::NTSC1953_FCC;
  EXPECT_NE(modified, original);
}

TEST(ColorimetricMetadataTest, ModifiedPrimaries_IsNotEqualToOriginal) {
  auto original = orc::ColorimetricMetadata::default_ntsc();
  auto modified = original;
  modified.primaries = orc::ColorimetricPrimaries::NTSC1953;
  EXPECT_NE(modified, original);
}

TEST(ColorimetricMetadataTest, ModifiedTransfer_IsNotEqualToOriginal) {
  auto original = orc::ColorimetricMetadata::default_ntsc();
  auto modified = original;
  modified.transfer_characteristics =
      orc::ColorimetricTransferCharacteristics::BT709;
  EXPECT_NE(modified, original);
}

TEST(ColorimetricMetadataTest, UnspecifiedInstance_IsNotEqualToDefaultNtsc) {
  orc::ColorimetricMetadata unspecified{};
  EXPECT_NE(unspecified, orc::ColorimetricMetadata::default_ntsc());
}

// =============================================================================
// PreviewCoordinate — construction and defaults
// =============================================================================

TEST(PreviewCoordinateTest, DefaultConstructed_HasZeroFieldIndex) {
  orc::PreviewCoordinate coord{};
  EXPECT_EQ(coord.field_index, 0u);
}

TEST(PreviewCoordinateTest, DefaultConstructed_HasZeroLineIndex) {
  orc::PreviewCoordinate coord{};
  EXPECT_EQ(coord.line_index, 0u);
}

TEST(PreviewCoordinateTest, DefaultConstructed_HasZeroSampleOffset) {
  orc::PreviewCoordinate coord{};
  EXPECT_EQ(coord.sample_offset, 0u);
}

TEST(PreviewCoordinateTest,
     DefaultConstructed_HasCompositeNtscDataTypeContext) {
  orc::PreviewCoordinate coord{};
  EXPECT_EQ(coord.data_type_context, orc::VideoDataType::CompositeNTSC);
}

TEST(PreviewCoordinateTest,
     DefaultConstructedVectorscope_DefaultsToActiveAreaOnly) {
  orc::PreviewCoordinate coord{};
  EXPECT_TRUE(coord.vectorscope_active_area_only);
}

// =============================================================================
// PreviewCoordinate — validity / bounds
// =============================================================================

TEST(PreviewCoordinateTest, DefaultConstructed_IsValid) {
  orc::PreviewCoordinate coord{};
  EXPECT_TRUE(coord.is_valid());
}

TEST(PreviewCoordinateTest, RepresentativeFieldIndex_IsValid) {
  orc::PreviewCoordinate coord{};
  coord.field_index = 1000u;
  EXPECT_TRUE(coord.is_valid());
}

TEST(PreviewCoordinateTest, MaxFieldIndex_IsNotValid) {
  orc::PreviewCoordinate coord{};
  coord.field_index = UINT64_MAX;
  EXPECT_FALSE(coord.is_valid());
}

TEST(PreviewCoordinateTest, MaxLineIndex_IsNotValid) {
  orc::PreviewCoordinate coord{};
  coord.line_index = UINT32_MAX;
  EXPECT_FALSE(coord.is_valid());
}

TEST(PreviewCoordinateTest, MaxSampleOffset_IsNotValid) {
  orc::PreviewCoordinate coord{};
  coord.sample_offset = UINT32_MAX;
  EXPECT_FALSE(coord.is_valid());
}

// =============================================================================
// PreviewCoordinate — equality
// =============================================================================

TEST(PreviewCoordinateTest, Equality_IsReflexive) {
  orc::PreviewCoordinate coord{42u, 10u, 200u, orc::VideoDataType::YC_PAL};
  EXPECT_EQ(coord, coord);
}

TEST(PreviewCoordinateTest, DifferentFieldIndex_IsNotEqual) {
  orc::PreviewCoordinate a{1u, 10u, 0u, orc::VideoDataType::CompositeNTSC};
  orc::PreviewCoordinate b{2u, 10u, 0u, orc::VideoDataType::CompositeNTSC};
  EXPECT_NE(a, b);
}

TEST(PreviewCoordinateTest, DifferentLineIndex_IsNotEqual) {
  orc::PreviewCoordinate a{0u, 10u, 0u, orc::VideoDataType::CompositePAL};
  orc::PreviewCoordinate b{0u, 20u, 0u, orc::VideoDataType::CompositePAL};
  EXPECT_NE(a, b);
}

TEST(PreviewCoordinateTest, DifferentSampleOffset_IsNotEqual) {
  orc::PreviewCoordinate a{0u, 0u, 100u, orc::VideoDataType::YC_NTSC};
  orc::PreviewCoordinate b{0u, 0u, 200u, orc::VideoDataType::YC_NTSC};
  EXPECT_NE(a, b);
}

TEST(PreviewCoordinateTest, DifferentDataTypeContext_IsNotEqual) {
  orc::PreviewCoordinate a{0u, 0u, 0u, orc::VideoDataType::CompositeNTSC};
  orc::PreviewCoordinate b{0u, 0u, 0u, orc::VideoDataType::CompositePAL};
  EXPECT_NE(a, b);
}

TEST(PreviewCoordinateTest, Copy_PreservesAllFields) {
  orc::PreviewCoordinate original{100u, 50u, 300u,
                                  orc::VideoDataType::ColourPAL};
  auto copy = original;
  EXPECT_EQ(copy, original);
}

TEST(PreviewCoordinateTest, DifferentVectorscopeAreaPreference_IsNotEqual) {
  orc::PreviewCoordinate active_only{0u, 0u, 0u, orc::VideoDataType::ColourNTSC,
                                     true};
  orc::PreviewCoordinate full_frame{0u, 0u, 0u, orc::VideoDataType::ColourNTSC,
                                    false};
  EXPECT_NE(active_only, full_frame);
}

// =============================================================================
// PreviewNavigationExtent — validity
// =============================================================================

TEST(PreviewNavigationExtentTest, DefaultConstructed_IsNotValid) {
  orc::PreviewNavigationExtent ext{};
  EXPECT_FALSE(ext.is_valid());
}

TEST(PreviewNavigationExtentTest, WithPositiveItemCountAndLabel_IsValid) {
  orc::PreviewNavigationExtent ext{100, 1, "field"};
  EXPECT_TRUE(ext.is_valid());
}

TEST(PreviewNavigationExtentTest, WithZeroItemCount_IsNotValid) {
  orc::PreviewNavigationExtent ext{0, 1, "field"};
  EXPECT_FALSE(ext.is_valid());
}

TEST(PreviewNavigationExtentTest, WithZeroGranularity_IsNotValid) {
  orc::PreviewNavigationExtent ext{100, 0, "field"};
  EXPECT_FALSE(ext.is_valid());
}

TEST(PreviewNavigationExtentTest, WithEmptyLabel_IsNotValid) {
  orc::PreviewNavigationExtent ext{100, 1, ""};
  EXPECT_FALSE(ext.is_valid());
}

TEST(PreviewNavigationExtentTest, FrameGranularity_IsValid) {
  // Frame-navigating stages expose every-other-field, so granularity == 2
  orc::PreviewNavigationExtent ext{50, 2, "frame"};
  EXPECT_TRUE(ext.is_valid());
}

// =============================================================================
// PreviewGeometry — validity
// =============================================================================

TEST(PreviewGeometryTest, DefaultConstructed_IsNotValid) {
  orc::PreviewGeometry geo{};
  EXPECT_FALSE(geo.is_valid());
}

TEST(PreviewGeometryTest, WithValidPalDimensions_IsValid) {
  orc::PreviewGeometry geo{910, 313, 4.0 / 3.0, 0.7};
  EXPECT_TRUE(geo.is_valid());
}

TEST(PreviewGeometryTest, WithValidNtscDimensions_IsValid) {
  orc::PreviewGeometry geo{760, 263, 4.0 / 3.0, 0.7};
  EXPECT_TRUE(geo.is_valid());
}

TEST(PreviewGeometryTest, WithZeroWidth_IsNotValid) {
  orc::PreviewGeometry geo{0, 313, 4.0 / 3.0, 0.7};
  EXPECT_FALSE(geo.is_valid());
}

TEST(PreviewGeometryTest, WithZeroHeight_IsNotValid) {
  orc::PreviewGeometry geo{910, 0, 4.0 / 3.0, 0.7};
  EXPECT_FALSE(geo.is_valid());
}

TEST(PreviewGeometryTest, WithZeroDisplayAspectRatio_IsNotValid) {
  orc::PreviewGeometry geo{910, 313, 0.0, 0.7};
  EXPECT_FALSE(geo.is_valid());
}

TEST(PreviewGeometryTest, WithZeroDarCorrectionFactor_IsNotValid) {
  orc::PreviewGeometry geo{910, 313, 4.0 / 3.0, 0.0};
  EXPECT_FALSE(geo.is_valid());
}

// =============================================================================
// StagePreviewCapability — schema and validity
// =============================================================================

TEST(StagePreviewCapabilityTest, DefaultConstructed_IsNotValid) {
  orc::StagePreviewCapability cap{};
  EXPECT_FALSE(cap.is_valid());
}

TEST(StagePreviewCapabilityTest, WithNoDataTypes_IsNotValid) {
  orc::StagePreviewCapability cap{};
  cap.navigation_extent = {100, 1, "field"};
  cap.geometry = {910, 313, 4.0 / 3.0, 0.7};
  // supported_data_types is still empty
  EXPECT_FALSE(cap.is_valid());
}

TEST(StagePreviewCapabilityTest, WithZeroItemCount_IsNotValid) {
  orc::StagePreviewCapability cap{};
  cap.supported_data_types = {orc::VideoDataType::CompositePAL};
  cap.navigation_extent = {0, 1, "field"};  // item_count == 0
  cap.geometry = {910, 313, 4.0 / 3.0, 0.7};
  EXPECT_FALSE(cap.is_valid());
}

TEST(StagePreviewCapabilityTest, WithZeroGeometryWidth_IsNotValid) {
  orc::StagePreviewCapability cap{};
  cap.supported_data_types = {orc::VideoDataType::CompositePAL};
  cap.navigation_extent = {100, 1, "field"};
  cap.geometry = {0, 313, 4.0 / 3.0, 0.7};  // active_width == 0
  EXPECT_FALSE(cap.is_valid());
}

TEST(StagePreviewCapabilityTest, MinimumValidCapability_IsValid) {
  orc::StagePreviewCapability cap{};
  cap.supported_data_types = {orc::VideoDataType::CompositePAL};
  cap.navigation_extent = {100, 1, "field"};
  cap.geometry = {910, 313, 4.0 / 3.0, 0.7};
  EXPECT_TRUE(cap.is_valid());
}

TEST(StagePreviewCapabilityTest, MultipleDataTypes_IsValid) {
  orc::StagePreviewCapability cap{};
  cap.supported_data_types = {orc::VideoDataType::YC_PAL,
                              orc::VideoDataType::ColourPAL};
  cap.navigation_extent = {50, 1, "frame"};
  cap.geometry = {928, 576, 4.0 / 3.0, 0.7};
  EXPECT_TRUE(cap.is_valid());
  EXPECT_EQ(cap.supported_data_types.size(), 2u);
}

// =============================================================================
// IStagePreviewCapability — interface mock smoke-test
// =============================================================================

namespace {
// Minimal mock to verify the interface can be implemented.
class MockPreviewCapabilityStage : public orc::IStagePreviewCapability {
 public:
  orc::StagePreviewCapability get_preview_capability() const override {
    orc::StagePreviewCapability cap{};
    cap.supported_data_types = {orc::VideoDataType::CompositePAL};
    cap.navigation_extent = {400, 1, "field"};
    cap.geometry = {910, 313, 4.0 / 3.0, 0.7};
    return cap;
  }
};
}  // anonymous namespace

TEST(IStagePreviewCapabilityTest,
     ConcreteImplementation_ReturnsValidCapability) {
  MockPreviewCapabilityStage stage{};
  auto cap = stage.get_preview_capability();
  EXPECT_TRUE(cap.is_valid());
}

TEST(IStagePreviewCapabilityTest,
     ConcreteImplementation_ReturnsExpectedDataType) {
  MockPreviewCapabilityStage stage{};
  auto cap = stage.get_preview_capability();
  ASSERT_EQ(cap.supported_data_types.size(), 1u);
  EXPECT_EQ(cap.supported_data_types.front(), orc::VideoDataType::CompositePAL);
}

TEST(IStagePreviewCapabilityTest,
     ConcreteImplementation_ReturnsExpectedItemCount) {
  MockPreviewCapabilityStage stage{};
  auto cap = stage.get_preview_capability();
  EXPECT_EQ(cap.navigation_extent.item_count, 400u);
}

// =============================================================================
// PreviewViewDataResult — is_valid() correctness
// =============================================================================

namespace {

orc::PreviewImage make_valid_preview_image() {
  orc::PreviewImage img{};
  img.width = 1;
  img.height = 1;
  img.rgb_data = {0, 0, 0};
  return img;
}

}  // anonymous namespace

TEST(PreviewViewDataResultTest, FailedResult_IsNotValid) {
  orc::PreviewViewDataResult result{};
  result.success = false;
  EXPECT_FALSE(result.is_valid());
}

TEST(PreviewViewDataResultTest, SuccessWithNoneKind_IsNotValid) {
  // A successful result with payload kind None has no useful payload.
  orc::PreviewViewDataResult result{};
  result.success = true;
  result.payload_kind = orc::PreviewViewPayloadKind::None;
  EXPECT_FALSE(result.is_valid());
}

TEST(PreviewViewDataResultTest, SuccessWithImageKindAndValidImage_IsValid) {
  orc::PreviewViewDataResult result{};
  result.success = true;
  result.payload_kind = orc::PreviewViewPayloadKind::Image;
  result.image = make_valid_preview_image();
  EXPECT_TRUE(result.is_valid());
}

TEST(PreviewViewDataResultTest, SuccessWithImageKindButEmptyImage_IsNotValid) {
  orc::PreviewViewDataResult result{};
  result.success = true;
  result.payload_kind = orc::PreviewViewPayloadKind::Image;
  // image left as std::nullopt
  EXPECT_FALSE(result.is_valid());
}

TEST(PreviewViewDataResultTest,
     SuccessWithImageKindButInvalidImage_IsNotValid) {
  orc::PreviewImage bad_image{};
  bad_image.width = 2;
  bad_image.height = 2;
  bad_image.rgb_data = {0, 0, 0};  // too small (needs 2*2*3 = 12 bytes)

  orc::PreviewViewDataResult result{};
  result.success = true;
  result.payload_kind = orc::PreviewViewPayloadKind::Image;
  result.image = bad_image;
  EXPECT_FALSE(result.is_valid());
}

TEST(PreviewViewDataResultTest, SuccessWithVectorscopeKindAndData_IsValid) {
  orc::VectorscopeData vs{};
  vs.width = 1;
  vs.height = 1;

  orc::PreviewViewDataResult result{};
  result.success = true;
  result.payload_kind = orc::PreviewViewPayloadKind::Vectorscope;
  result.vectorscope = vs;
  EXPECT_TRUE(result.is_valid());
}

TEST(PreviewViewDataResultTest,
     SuccessWithVectorscopeKindButNoData_IsNotValid) {
  orc::PreviewViewDataResult result{};
  result.success = true;
  result.payload_kind = orc::PreviewViewPayloadKind::Vectorscope;
  // vectorscope left as std::nullopt
  EXPECT_FALSE(result.is_valid());
}

}  // namespace orc_unit_test
