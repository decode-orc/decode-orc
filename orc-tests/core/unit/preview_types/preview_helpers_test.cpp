/*
 * File:        preview_helpers_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for standard preview rendering line indexing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace orc_unit_test {

namespace {

// PAL geometry: 625 lines of 1135 samples nominal, with lines 312 and 624
// carrying two extra samples each (EBU Tech. 3280-E §1.3.1). Every field-2
// line therefore starts 2 samples past a naive line * 1135 stride, which is
// exactly the layout property the preview renderer must respect.
constexpr size_t kPalWidth = 1135;
constexpr size_t kPalHeight = 625;

// Column of the bright marker sample written into every line.
constexpr uint32_t kMarkerX = 3;

// In-memory PAL YC representation with the correct non-uniform flat layout.
// Each luma/chroma line is black except for a white marker at kMarkerX, so a
// rendered row shows exactly where the renderer believes the line starts.
class FakePalYcRepresentation : public orc::VideoFrameRepresentation {
 public:
  FakePalYcRepresentation() {
    const size_t total = orc::frame_line_sample_offset(orc::VideoSystem::PAL,
                                                       kPalWidth, kPalHeight);
    luma_.assign(total, static_cast<sample_type>(orc::kPalBlack));
    chroma_.assign(total, static_cast<sample_type>(orc::kPalBlack));
    for (size_t line = 0; line < kPalHeight; ++line) {
      const size_t offset =
          orc::frame_line_sample_offset(orc::VideoSystem::PAL, kPalWidth, line);
      luma_[offset + kMarkerX] = static_cast<sample_type>(orc::kPalWhite);
      chroma_[offset + kMarkerX] = static_cast<sample_type>(orc::kPalWhite);
    }
  }

  orc::FrameIDRange frame_range() const override { return {0, 0}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(orc::FrameID id) const override { return id == 0; }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (id != 0) return std::nullopt;
    orc::FrameDescriptor desc;
    desc.frame_id = 0;
    desc.system = orc::VideoSystem::PAL;
    desc.height = kPalHeight;
    desc.samples_total = luma_.size();
    desc.samples_per_line_nominal = kPalWidth;
    return desc;
  }

  const sample_type* get_frame(orc::FrameID id) const override {
    return id == 0 ? luma_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return id == 0 ? luma_ : std::vector<sample_type>{};
  }

  bool has_separate_channels() const override { return true; }
  const sample_type* get_frame_luma(orc::FrameID id) const override {
    return id == 0 ? luma_.data() : nullptr;
  }
  const sample_type* get_frame_chroma(orc::FrameID id) const override {
    return id == 0 ? chroma_.data() : nullptr;
  }
  const sample_type* get_line_luma(orc::FrameID id,
                                   size_t line) const override {
    return line_ptr(luma_, id, line);
  }
  const sample_type* get_line_chroma(orc::FrameID id,
                                     size_t line) const override {
    return line_ptr(chroma_, id, line);
  }

  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters params;
    params.system = orc::VideoSystem::PAL;
    params.frame_width_nominal = static_cast<int32_t>(kPalWidth);
    params.frame_height = static_cast<int32_t>(kPalHeight);
    params.sync_tip_level = orc::kPalSyncTip;
    params.black_level = orc::kPalBlack;
    params.white_level = orc::kPalWhite;
    params.peak_level = orc::kPalPeak;
    return params;
  }

  void set_dropout_hints(std::vector<orc::DropoutRun> hints) {
    dropout_hints_ = std::move(hints);
  }
  std::vector<orc::DropoutRun> get_dropout_hints(
      orc::FrameID id) const override {
    return id == 0 ? dropout_hints_ : std::vector<orc::DropoutRun>{};
  }

 private:
  const sample_type* line_ptr(const std::vector<sample_type>& plane,
                              orc::FrameID id, size_t line) const {
    if (id != 0 || line >= kPalHeight) return nullptr;
    return plane.data() + orc::frame_line_sample_offset(orc::VideoSystem::PAL,
                                                        kPalWidth, line);
  }

  std::vector<sample_type> luma_;
  std::vector<sample_type> chroma_;
  std::vector<orc::DropoutRun> dropout_hints_;
};

uint8_t gray_at(const orc::PreviewImage& image, uint32_t row, uint32_t x) {
  return image.rgb_data[(static_cast<size_t>(row) * image.width + x) * 3];
}

// Every rendered row must show the marker at kMarkerX and darkness two
// samples later. A renderer that strides field 2 with a fixed line * 1135
// offset reads those lines 2 samples early, moving the marker to kMarkerX + 2.
void expect_marker_aligned_on_all_rows(const orc::PreviewImage& image) {
  ASSERT_TRUE(image.is_valid());
  ASSERT_EQ(image.width, static_cast<uint32_t>(kPalWidth));
  ASSERT_EQ(image.height, static_cast<uint32_t>(kPalHeight));
  for (uint32_t row = 0; row < image.height; ++row) {
    ASSERT_GT(gray_at(image, row, kMarkerX), 128) << "row " << row;
    ASSERT_LT(gray_at(image, row, kMarkerX + 2), 128) << "row " << row;
  }
}

// Composite PAL representation, black except for a single white marker on one
// frame-flat line, used to verify active-area cropping selects the right rows
// and columns.  Buffer is in sequential field order (field 1 lines then field
// 2 lines); marked_buf_line indexes that buffer directly.
class FakePalMarkedLineRepresentation : public orc::VideoFrameRepresentation {
 public:
  FakePalMarkedLineRepresentation(size_t marked_buf_line, uint32_t marker_col) {
    const size_t total = orc::frame_line_sample_offset(orc::VideoSystem::PAL,
                                                       kPalWidth, kPalHeight);
    // White background so a blanking-level boundary line is distinguishable
    // (for PAL kPalBlack == kPalBlanking, so a black background could not tell
    // the outline apart from the picture).
    frame_.assign(total, static_cast<sample_type>(orc::kPalWhite));
    const size_t off = orc::frame_line_sample_offset(
        orc::VideoSystem::PAL, kPalWidth, marked_buf_line);
    frame_[off + marker_col] = static_cast<sample_type>(orc::kPalWhite);
  }

  orc::FrameIDRange frame_range() const override { return {0, 0}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(orc::FrameID id) const override { return id == 0; }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (id != 0) return std::nullopt;
    orc::FrameDescriptor desc;
    desc.frame_id = 0;
    desc.system = orc::VideoSystem::PAL;
    desc.height = kPalHeight;
    desc.samples_total = frame_.size();
    desc.samples_per_line_nominal = kPalWidth;
    return desc;
  }

  const sample_type* get_frame(orc::FrameID id) const override {
    return id == 0 ? frame_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return id == 0 ? frame_ : std::vector<sample_type>{};
  }

  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters params;
    params.system = orc::VideoSystem::PAL;
    params.frame_width_nominal = static_cast<int32_t>(kPalWidth);
    params.frame_height = static_cast<int32_t>(kPalHeight);
    params.sync_tip_level = orc::kPalSyncTip;
    params.blanking_level = orc::kPalBlanking;
    params.black_level = orc::kPalBlack;
    params.white_level = orc::kPalWhite;
    params.peak_level = orc::kPalPeak;
    params.active_video_start = active_video_start_;
    params.active_video_end = active_video_end_;
    params.first_active_frame_line = first_active_frame_line_;
    params.last_active_frame_line = last_active_frame_line_;
    return params;
  }

  int32_t active_video_start_ = 0;
  int32_t active_video_end_ = 0;
  int32_t first_active_frame_line_ = 0;
  int32_t last_active_frame_line_ = 0;

 private:
  std::vector<sample_type> frame_;
};

}  // namespace

// Masking must keep the full frame at full size while dimming everything
// outside the active rectangle [first,last) × [active_start,active_end).  The
// white background means active pixels stay bright and masked pixels are
// dimmed.  In the interlaced (weaved) layout the active-area line parameters
// are display rows directly, so there is a single active band.
TEST(PreviewHelpersTest, MaskInactiveArea_Interlaced_DimsOutsideActiveRect) {
  auto rep = std::make_shared<FakePalMarkedLineRepresentation>(
      /*marked_buf_line=*/75, /*marker_col=*/60);
  rep->active_video_start_ = 10;
  rep->active_video_end_ = 110;  // active columns [10,110)
  rep->first_active_frame_line_ = 100;
  rep->last_active_frame_line_ = 300;  // active weaved rows [100,300)

  const auto image = orc::PreviewHelpers::render_standard_preview(
      rep, "interlaced_raw", 0, orc::PreviewNavigationHint::Random,
      /*mask_inactive_area=*/true);

  ASSERT_TRUE(image.is_valid());
  // Full frame preserved (no crop, no resize).
  EXPECT_EQ(image.width, static_cast<uint32_t>(kPalWidth));
  EXPECT_EQ(image.height, static_cast<uint32_t>(kPalHeight));

  // A pixel well inside the active rectangle stays bright.
  EXPECT_GT(gray_at(image, 150, 50), 128);

  // Just outside each edge is dimmed relative to just inside it.
  EXPECT_LT(gray_at(image, 99, 50), gray_at(image, 100, 50));   // above top
  EXPECT_LT(gray_at(image, 300, 50), gray_at(image, 299, 50));  // below bottom
  EXPECT_LT(gray_at(image, 150, 9), gray_at(image, 150, 10));   // left of start
  EXPECT_LT(gray_at(image, 150, 110),
            gray_at(image, 150, 109));  // right of end
}

// In the sequential layout the display stacks the field-1 buffer block above
// the field-2 block, so the single weaved active-line range [first,last) maps
// to one active band per field — twice the horizontal borders.  Field 1 is on
// even weaved rows, so weaved line w corresponds to field-1 buffer row w/2 and
// field-2 buffer row kPalField1Lines + (w-1)/2.
TEST(PreviewHelpersTest, MaskInactiveArea_Sequential_HasOneBandPerField) {
  auto rep = std::make_shared<FakePalMarkedLineRepresentation>(
      /*marked_buf_line=*/75, /*marker_col=*/60);
  rep->active_video_start_ = 10;
  rep->active_video_end_ = 110;  // active columns [10,110)
  rep->first_active_frame_line_ = 100;
  rep->last_active_frame_line_ = 300;  // active weaved rows [100,300)

  const auto image = orc::PreviewHelpers::render_standard_preview(
      rep, "sequential_raw", 0, orc::PreviewNavigationHint::Random,
      /*mask_inactive_area=*/true);

  ASSERT_TRUE(image.is_valid());
  EXPECT_EQ(image.width, static_cast<uint32_t>(kPalWidth));
  EXPECT_EQ(image.height, static_cast<uint32_t>(kPalHeight));

  // Field-1 band: weaved [100,300) → field-1 buffer rows [50,150).
  EXPECT_GT(gray_at(image, 100, 50), 128);                    // inside band 1
  EXPECT_LT(gray_at(image, 49, 50), gray_at(image, 50, 50));  // top border 1
  EXPECT_LT(gray_at(image, 150, 50),
            gray_at(image, 149, 50));  // bottom bord. 1

  // Field-2 band: weaved [100,300) → field-2 buffer rows
  // kPalField1Lines + [50,150) = [363,463).
  const int f2 = orc::kPalField1Lines;           // 313
  EXPECT_GT(gray_at(image, f2 + 100, 50), 128);  // inside band 2
  EXPECT_LT(gray_at(image, f2 + 49, 50),
            gray_at(image, f2 + 50, 50));  // top border 2
  EXPECT_LT(gray_at(image, f2 + 150, 50),
            gray_at(image, f2 + 149, 50));  // bottom border 2

  // The gap between the two bands (field-1 rows above 150, still before the
  // field-2 block) is dimmed.
  EXPECT_LT(gray_at(image, 200, 50), gray_at(image, 100, 50));

  // Column masking still applies within an active row.
  EXPECT_LT(gray_at(image, 100, 9), gray_at(image, 100, 10));
  EXPECT_LT(gray_at(image, 100, 110), gray_at(image, 100, 109));
}

// Without the flag the full frame is rendered plain, with no mask.
TEST(PreviewHelpersTest, NoMask_RendersPlainFullFrame) {
  auto rep = std::make_shared<FakePalMarkedLineRepresentation>(75, 60);
  rep->active_video_start_ = 10;
  rep->active_video_end_ = 110;
  rep->first_active_frame_line_ = 100;
  rep->last_active_frame_line_ = 300;

  const auto image =
      orc::PreviewHelpers::render_standard_preview(rep, "sequential_raw", 0);

  ASSERT_TRUE(image.is_valid());
  EXPECT_EQ(image.width, static_cast<uint32_t>(kPalWidth));
  EXPECT_EQ(image.height, static_cast<uint32_t>(kPalHeight));
  // No mask: a pixel outside the active area matches one inside (both white).
  EXPECT_EQ(gray_at(image, 99, 50), gray_at(image, 100, 50));
}

TEST(PreviewHelpersTest, SequentialLumaPreview_UsesPalLineOffsets) {
  auto representation = std::make_shared<FakePalYcRepresentation>();
  const auto image = orc::PreviewHelpers::render_standard_preview(
      representation, "sequential_clamped_y", 0);
  expect_marker_aligned_on_all_rows(image);
}

TEST(PreviewHelpersTest, SequentialChromaPreview_UsesPalLineOffsets) {
  auto representation = std::make_shared<FakePalYcRepresentation>();
  const auto image = orc::PreviewHelpers::render_standard_preview(
      representation, "sequential_clamped_c", 0);
  expect_marker_aligned_on_all_rows(image);
}

TEST(PreviewHelpersTest, InterlacedLumaPreview_UsesPalLineOffsets) {
  auto representation = std::make_shared<FakePalYcRepresentation>();
  const auto image = orc::PreviewHelpers::render_standard_preview(
      representation, "interlaced_clamped_y", 0);
  expect_marker_aligned_on_all_rows(image);
}

TEST(PreviewHelpersTest, CompositePathPreview_UsesPalLineOffsets) {
  auto representation = std::make_shared<FakePalYcRepresentation>();
  const auto image = orc::PreviewHelpers::render_standard_preview(
      representation, "sequential_clamped", 0);
  expect_marker_aligned_on_all_rows(image);
}

// Regression: sequential dropout regions must land on the exact frame-flat
// line for BOTH fields. The display-row recomposition used field1_lines =
// height/2 (312 for PAL) while the field split used the true field-1 line
// count (313), shifting every field-2 region up one line — which drew the
// overlay on the wrong line and made dropout_map removals created from those
// regions miss their runs entirely.
TEST(PreviewHelpersTest,
     SequentialDropoutRegions_UseFrameFlatLinesForBothFields) {
  auto representation = std::make_shared<FakePalYcRepresentation>();

  const size_t field1_line = 100;  // PAL field 1 (< 313)
  const size_t field2_line = 400;  // PAL field 2 (>= 313)
  std::vector<orc::DropoutRun> hints;
  for (size_t line : {field1_line, field2_line}) {
    orc::DropoutRun run;
    run.frame_id = 0;
    run.sample_start =
        orc::frame_line_sample_offset(orc::VideoSystem::PAL, kPalWidth, line) +
        200;
    run.sample_count = 100;
    run.severity = 100;
    hints.push_back(run);
  }
  representation->set_dropout_hints(std::move(hints));

  const auto image = orc::PreviewHelpers::render_standard_preview(
      representation, "sequential_clamped", 0);
  ASSERT_TRUE(image.is_valid());
  ASSERT_EQ(image.dropout_regions.size(), 2u);

  EXPECT_EQ(image.dropout_regions[0].line, field1_line);
  EXPECT_EQ(image.dropout_regions[0].start_sample, 200u);
  EXPECT_EQ(image.dropout_regions[0].end_sample, 300u);

  EXPECT_EQ(image.dropout_regions[1].line, field2_line);
  EXPECT_EQ(image.dropout_regions[1].start_sample, 200u);
  EXPECT_EQ(image.dropout_regions[1].end_sample, 300u);
}

// Regression (issue #209): a malformed/out-of-range dropout in older ld-decode
// metadata could underflow a field-line to -1, producing a ~2^64 sample offset
// that spun the PAL bracket search in frame_flat_offset_to_line_sample forever
// — hanging the (single) render worker so the preview stuck on "Rendering..."
// and no stage could trigger. The search must now always terminate and honour
// its sample_in_line < line_len invariant.
TEST(FrameLineUtilTest, FlatOffsetToLineSample_TerminatesOnPathologicalOffset) {
  // Offset far beyond any real frame, as produced by an underflowed field line.
  const uint64_t pathological = static_cast<uint64_t>(-1) - 5;
  auto [line, sample_in_line] = orc::frame_flat_offset_to_line_sample(
      orc::VideoSystem::PAL, kPalWidth, pathological);

  // Must return (no hang) and keep the invariant the caller's loop relies on to
  // make progress: the sample offset never reaches the line length.
  const size_t line_len =
      orc::frame_line_sample_count(orc::VideoSystem::PAL, kPalWidth, line);
  EXPECT_LT(sample_in_line, line_len);
}

// Regression (issue #209): rendering a preview whose dropout hints contain an
// out-of-range offset must complete rather than hang the render worker.
TEST(PreviewHelpersTest, RenderStandardPreview_TerminatesOnPathologicalHint) {
  auto representation = std::make_shared<FakePalYcRepresentation>();

  std::vector<orc::DropoutRun> hints;
  orc::DropoutRun run;
  run.frame_id = 0;
  run.sample_start = static_cast<uint64_t>(-1) - 50;  // absurd offset
  run.sample_count = 100;
  run.severity = 100;
  hints.push_back(run);
  representation->set_dropout_hints(std::move(hints));

  // A hang here would time the test out; reaching the assertion proves the
  // dropout loop's zero-progress guard terminates the render.
  const auto image = orc::PreviewHelpers::render_standard_preview(
      representation, "sequential_clamped", 0);
  EXPECT_TRUE(image.is_valid());
}

// ---------------------------------------------------------------------------
// Greyscale level scaling
//
// The mapping from a 10-bit sample to an 8-bit grey is tabulated rather than
// divided per pixel. The table has to be an optimisation and nothing else, so
// these tests compare the rendered bytes against the mapping computed directly
// - including for samples outside the 10-bit domain, which the table does not
// cover and which a malformed source can still produce.
// ---------------------------------------------------------------------------

namespace {

// The mapping as written in the specification of the two preview options,
// computed here independently of the renderer.
uint8_t expectedGrey(int32_t sample, bool clamped) {
  const int32_t low = clamped ? orc::kPalBlack : orc::kPalSyncTip;
  const int32_t high = clamped ? orc::kPalWhite : orc::kPalPeak;
  const int32_t range = high - low;
  if (range <= 0) return 0;
  const int32_t scaled = ((sample - low) * 255) / range;
  return static_cast<uint8_t>(std::max(0, std::min(255, scaled)));
}

// A PAL frame whose first row walks a chosen set of sample values, so one
// render covers the whole mapping rather than the two levels a flat frame has.
class RampPalRepresentation : public orc::VideoFrameRepresentation {
 public:
  explicit RampPalRepresentation(std::vector<int32_t> ramp) {
    const size_t total = orc::frame_line_sample_offset(orc::VideoSystem::PAL,
                                                       kPalWidth, kPalHeight);
    samples_.assign(total, static_cast<sample_type>(orc::kPalBlack));
    for (size_t x = 0; x < ramp.size() && x < kPalWidth; ++x) {
      samples_[x] = static_cast<sample_type>(ramp[x]);
    }
    ramp_ = std::move(ramp);
  }

  const std::vector<int32_t>& ramp() const { return ramp_; }

  orc::FrameIDRange frame_range() const override { return {0, 0}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(orc::FrameID id) const override { return id == 0; }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (id != 0) return std::nullopt;
    orc::FrameDescriptor desc;
    desc.frame_id = 0;
    desc.system = orc::VideoSystem::PAL;
    desc.height = kPalHeight;
    desc.samples_total = samples_.size();
    desc.samples_per_line_nominal = kPalWidth;
    return desc;
  }

  const sample_type* get_frame(orc::FrameID id) const override {
    return id == 0 ? samples_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return id == 0 ? samples_ : std::vector<sample_type>{};
  }
  const sample_type* get_line(orc::FrameID id, size_t line) const override {
    if (id != 0 || line >= kPalHeight) return nullptr;
    return samples_.data() + orc::frame_line_sample_offset(
                                 orc::VideoSystem::PAL, kPalWidth, line);
  }

  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters params;
    params.system = orc::VideoSystem::PAL;
    params.frame_width_nominal = static_cast<int32_t>(kPalWidth);
    params.frame_height = static_cast<int32_t>(kPalHeight);
    params.sync_tip_level = orc::kPalSyncTip;
    params.black_level = orc::kPalBlack;
    params.white_level = orc::kPalWhite;
    params.peak_level = orc::kPalPeak;
    return params;
  }

  std::vector<orc::DropoutRun> get_dropout_hints(
      orc::FrameID /*id*/) const override {
    return {};
  }

 private:
  std::vector<sample_type> samples_;
  std::vector<int32_t> ramp_;
};

std::vector<int32_t> domainRamp() {
  std::vector<int32_t> ramp;
  for (int32_t sample = 0; sample < 1024; ++sample) {
    ramp.push_back(sample);
  }
  return ramp;
}

void expectRampRendersAsMapped(const std::string& option_id, bool clamped) {
  auto rep = std::make_shared<RampPalRepresentation>(domainRamp());
  const auto image = orc::PreviewHelpers::render_standard_preview(
      rep, option_id, 0, orc::PreviewNavigationHint::Random,
      /*mask_inactive_area=*/false);

  ASSERT_TRUE(image.is_valid());
  ASSERT_EQ(image.width, static_cast<uint32_t>(kPalWidth));

  for (size_t x = 0; x < rep->ramp().size(); ++x) {
    const uint8_t expected = expectedGrey(rep->ramp()[x], clamped);
    const size_t offset = x * 3;
    EXPECT_EQ(image.rgb_data[offset + 0], expected) << "at sample " << x;
    EXPECT_EQ(image.rgb_data[offset + 1], expected) << "at sample " << x;
    EXPECT_EQ(image.rgb_data[offset + 2], expected) << "at sample " << x;
  }
}

}  // namespace

TEST(PreviewHelpersGreyscale, ClampedOptionMapsEverySampleInTheDomain) {
  expectRampRendersAsMapped("sequential_clamped", /*clamped=*/true);
}

TEST(PreviewHelpersGreyscale, RawOptionMapsEverySampleInTheDomain) {
  expectRampRendersAsMapped("sequential_raw", /*clamped=*/false);
}

// A source that hands over samples the 10-bit domain cannot hold still has to
// be rendered by the mapping, not by whatever the table's ends happen to say.
TEST(PreviewHelpersGreyscale, MapsSamplesOutsideTheTenBitDomain) {
  const std::vector<int32_t> ramp = {-32768, -5000, -1, 0, 1023, 1024, 9000};
  auto rep = std::make_shared<RampPalRepresentation>(ramp);

  const auto image = orc::PreviewHelpers::render_standard_preview(
      rep, "sequential_clamped", 0, orc::PreviewNavigationHint::Random,
      /*mask_inactive_area=*/false);

  ASSERT_TRUE(image.is_valid());
  for (size_t x = 0; x < ramp.size(); ++x) {
    // The representation stores samples as int16_t, so compare against what
    // actually reaches the renderer.
    const auto stored = static_cast<int32_t>(
        static_cast<orc::VideoFrameRepresentation::sample_type>(ramp[x]));
    EXPECT_EQ(image.rgb_data[x * 3], expectedGrey(stored, /*clamped=*/true))
        << "at sample " << x;
  }
}

// The table is kept between renders, keyed on the levels it was built for. Two
// sources with different levels must not see each other's table.
TEST(PreviewHelpersGreyscale, RebuildsTheMappingWhenTheOptionChangesLevels) {
  auto rep = std::make_shared<RampPalRepresentation>(domainRamp());

  const auto clamped = orc::PreviewHelpers::render_standard_preview(
      rep, "sequential_clamped", 0, orc::PreviewNavigationHint::Random, false);
  const auto raw = orc::PreviewHelpers::render_standard_preview(
      rep, "sequential_raw", 0, orc::PreviewNavigationHint::Random, false);
  const auto clamped_again = orc::PreviewHelpers::render_standard_preview(
      rep, "sequential_clamped", 0, orc::PreviewNavigationHint::Random, false);

  ASSERT_TRUE(clamped.is_valid());
  ASSERT_TRUE(raw.is_valid());
  ASSERT_NE(clamped.rgb_data, raw.rgb_data);
  EXPECT_EQ(clamped.rgb_data, clamped_again.rgb_data);
}

}  // namespace orc_unit_test
