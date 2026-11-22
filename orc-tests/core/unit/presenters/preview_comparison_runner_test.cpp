/*
 * File:        preview_comparison_runner_test.cpp
 * Module:      orc-core-unit-tests
 * Purpose:     PreviewComparisonRunner: option and index checks, size and
 *              count checks, the kept pair, derived views and WebP export
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_comparison_runner.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <webp/demux.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace orc_unit_test {

using orc::NodeID;
using orc::PreviewCompareCropMode;
using orc::PreviewCompareExportSettings;
using orc::PreviewCompareRequest;
using orc::PreviewCompareResult;
using orc::PreviewCompareViewMode;
using orc::PreviewCompareViewSettings;
using orc::PreviewImage;
using orc::PreviewOutputInfo;
using orc::PreviewOutputType;
using orc::PreviewRenderResult;
using orc::presenters::IPreviewImageSource;
using orc::presenters::PreviewComparisonRunner;
using ::testing::HasSubstr;

namespace {

const NodeID kNodeA(1);
const NodeID kNodeB(2);
const std::string kOption = "frame";

PreviewImage flat_image(uint32_t width, uint32_t height, uint8_t value) {
  PreviewImage image;
  image.width = width;
  image.height = height;
  image.rgb_data.assign(static_cast<size_t>(width) * height * 3, value);
  return image;
}

PreviewOutputInfo output(PreviewOutputType type, const std::string& option_id,
                         uint64_t count, const std::string& name = "Frame") {
  PreviewOutputInfo info{};
  info.type = type;
  info.display_name = name;
  info.count = count;
  info.is_available = true;
  info.dar_aspect_correction = 1.0;
  info.option_id = option_id;
  return info;
}

/// Serves fixed outputs and images per node and counts renders per node.
class FakeImageSource final : public IPreviewImageSource {
 public:
  std::map<NodeID, std::vector<PreviewOutputInfo>> node_outputs;
  std::map<NodeID, PreviewImage> node_images;
  std::map<NodeID, std::string> node_failures;
  std::map<NodeID, int> render_calls;

  PreviewRenderResult render(NodeID node, PreviewOutputType type,
                             uint64_t index,
                             const std::string& /*option_id*/) override {
    ++render_calls[node];
    PreviewRenderResult result{};
    result.node_id = node;
    result.output_type = type;
    result.output_index = index;
    const auto failure = node_failures.find(node);
    if (failure != node_failures.end()) {
      result.success = false;
      result.error_message = failure->second;
      return result;
    }
    result.image = node_images.at(node);
    result.success = true;
    return result;
  }

  std::vector<PreviewOutputInfo> outputs(NodeID node) override {
    const auto it = node_outputs.find(node);
    return it == node_outputs.end() ? std::vector<PreviewOutputInfo>{}
                                    : it->second;
  }
};

/// Both nodes offer one frame output, 10 frames, and serve 16 × 8 images
/// that differ at one sample.
FakeImageSource matching_source() {
  FakeImageSource source;
  source.node_outputs[kNodeA] = {
      output(PreviewOutputType::Frame_Field1_First, kOption, 10)};
  source.node_outputs[kNodeB] = {
      output(PreviewOutputType::Frame_Field1_First, kOption, 10)};
  source.node_images[kNodeA] = flat_image(16, 8, 100);
  PreviewImage b = flat_image(16, 8, 100);
  b.rgb_data[(3 * 16 + 5) * 3] = 110;
  source.node_images[kNodeB] = b;
  return source;
}

PreviewCompareRequest request(uint64_t index = 4) {
  PreviewCompareRequest req;
  req.node_a = kNodeA;
  req.node_b = kNodeB;
  req.output_type = PreviewOutputType::Frame_Field1_First;
  req.option_id = kOption;
  req.index = index;
  return req;
}

/// Frame count and the ANIMATION flag of encoded WebP bytes.
struct WebPInfo {
  bool parsed = false;
  bool animated = false;
  uint32_t frames = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

WebPInfo inspect(const std::vector<uint8_t>& bytes) {
  WebPInfo info;
  WebPData data{bytes.data(), bytes.size()};
  WebPDemuxer* demux = WebPDemux(&data);
  if (demux == nullptr) {
    return info;
  }
  info.parsed = true;
  info.animated =
      (WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS) & ANIMATION_FLAG) != 0;
  info.frames = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
  info.width = WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
  info.height = WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
  WebPDemuxDelete(demux);
  return info;
}

}  // namespace

// --- compare(): checks -----------------------------------------------------

TEST(PreviewComparisonRunnerTest, MatchingNodesGiveBothImages) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);

  const PreviewCompareResult result = runner.compare(request());

  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.image_a.rgb_data, source.node_images[kNodeA].rgb_data);
  EXPECT_EQ(result.image_b.rgb_data, source.node_images[kNodeB].rgb_data);
  EXPECT_EQ(result.count_a, 10u);
  EXPECT_EQ(result.count_b, 10u);
  EXPECT_TRUE(result.warnings.empty());
  EXPECT_EQ(result.request, request());
}

TEST(PreviewComparisonRunnerTest, MissingOptionOnBNamesWhatBOffers) {
  FakeImageSource source = matching_source();
  source.node_outputs[kNodeB] = {
      output(PreviewOutputType::Frame_Field1_First, "frame_sequential", 10,
             "Frame (sequential)"),
      output(PreviewOutputType::Frame_Field1, "field", 20, "Field")};
  PreviewComparisonRunner runner(source);

  const PreviewCompareResult result = runner.compare(request());

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("node B (" + kNodeB.to_string() + ")"));
  EXPECT_THAT(result.error, HasSubstr("option 'frame'"));
  EXPECT_THAT(
      result.error,
      HasSubstr("Frame (sequential) 'frame_sequential', Field 'field'"));
  EXPECT_EQ(source.render_calls[kNodeA], 0);
  EXPECT_EQ(source.render_calls[kNodeB], 0);
}

TEST(PreviewComparisonRunnerTest, MissingOutputOnANamesA) {
  FakeImageSource source = matching_source();
  source.node_outputs[kNodeA].clear();
  PreviewComparisonRunner runner(source);

  const PreviewCompareResult result = runner.compare(request());

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("node A ("));
  EXPECT_THAT(result.error, HasSubstr("it offers none"));
}

TEST(PreviewComparisonRunnerTest, UnavailableOutputDoesNotCount) {
  FakeImageSource source = matching_source();
  source.node_outputs[kNodeB][0].is_available = false;
  PreviewComparisonRunner runner(source);

  EXPECT_FALSE(runner.compare(request()).success);
}

TEST(PreviewComparisonRunnerTest, IndexBeyondBIsRefused) {
  FakeImageSource source = matching_source();
  source.node_outputs[kNodeB][0].count = 3;
  PreviewComparisonRunner runner(source);

  const PreviewCompareResult result = runner.compare(request(4));

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("frame 5 is beyond node B"));
  EXPECT_THAT(result.error, HasSubstr("which has 3 (frames 1 to 3)"));
  EXPECT_THAT(result.error, HasSubstr("so the two cannot be compared"));
  EXPECT_EQ(source.render_calls[kNodeB], 0);
}

TEST(PreviewComparisonRunnerTest, SizeMismatchIsRefusedWithBothSizes) {
  FakeImageSource source = matching_source();
  source.node_images[kNodeB] = flat_image(16, 9, 100);
  PreviewComparisonRunner runner(source);

  const PreviewCompareResult result = runner.compare(request());

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("16x8"));
  EXPECT_THAT(result.error, HasSubstr("16x9"));
}

TEST(PreviewComparisonRunnerTest, CountMismatchWarnsButCompares) {
  FakeImageSource source = matching_source();
  source.node_outputs[kNodeB][0].count = 7;
  PreviewComparisonRunner runner(source);

  const PreviewCompareResult result = runner.compare(request(4));

  ASSERT_TRUE(result.success) << result.error;
  ASSERT_EQ(result.warnings.size(), 1u);
  EXPECT_THAT(result.warnings[0], HasSubstr("has 10"));
  EXPECT_THAT(result.warnings[0], HasSubstr("has 7"));
  EXPECT_THAT(result.warnings[0], HasSubstr("different pictures"));
}

TEST(PreviewComparisonRunnerTest, RenderFailureNamesTheSideAndTheReason) {
  FakeImageSource source = matching_source();
  source.node_failures[kNodeB] = "decoder exploded";
  PreviewComparisonRunner runner(source);

  const PreviewCompareResult result = runner.compare(request());

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("rendering node B"));
  EXPECT_THAT(result.error, HasSubstr("decoder exploded"));
}

// --- compare(): the kept pair ---------------------------------------------

TEST(PreviewComparisonRunnerTest, TheSameRequestReusesTheKeptPair) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);

  const auto first = runner.compare(request());
  const auto second = runner.compare(request());

  ASSERT_TRUE(second.success);
  EXPECT_EQ(second.image_b.rgb_data, first.image_b.rgb_data);
  EXPECT_EQ(source.render_calls[kNodeA], 1);
  EXPECT_EQ(source.render_calls[kNodeB], 1);
}

TEST(PreviewComparisonRunnerTest, ViewAndExportChangesDoNotRenderAgain) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  const auto result = runner.compare(request());

  PreviewCompareViewSettings view;
  view.mode = PreviewCompareViewMode::Stretched;
  (void)PreviewComparisonRunner::derive(result, view);
  view.mode = PreviewCompareViewMode::Difference;
  (void)PreviewComparisonRunner::derive(result, view);
  PreviewCompareExportSettings exported;
  exported.scale = 2;
  (void)PreviewComparisonRunner::buildExport(runner.compare(request()),
                                             exported);

  EXPECT_EQ(source.render_calls[kNodeA], 1);
  EXPECT_EQ(source.render_calls[kNodeB], 1);
}

TEST(PreviewComparisonRunnerTest, AnotherIndexRendersAgain) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);

  (void)runner.compare(request(4));
  (void)runner.compare(request(5));

  EXPECT_EQ(source.render_calls[kNodeA], 2);
  EXPECT_EQ(source.render_calls[kNodeB], 2);
}

TEST(PreviewComparisonRunnerTest, InvalidateRendersAgain) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);

  (void)runner.compare(request());
  runner.invalidate();
  (void)runner.compare(request());

  EXPECT_EQ(source.render_calls[kNodeA], 2);
}

TEST(PreviewComparisonRunnerTest, AFailedRequestIsNotKept) {
  FakeImageSource source = matching_source();
  source.node_failures[kNodeB] = "not yet";
  PreviewComparisonRunner runner(source);

  EXPECT_FALSE(runner.compare(request()).success);
  source.node_failures.clear();
  EXPECT_TRUE(runner.compare(request()).success);
  EXPECT_EQ(source.render_calls[kNodeB], 2);
}

// --- derive() ---------------------------------------------------------------

TEST(PreviewComparisonRunnerTest, NormalViewIsTheRenderedPair) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  const auto result = runner.compare(request());

  const auto view =
      PreviewComparisonRunner::derive(result, PreviewCompareViewSettings{});

  ASSERT_TRUE(view.success) << view.error;
  EXPECT_EQ(view.image_a.rgb_data, result.image_a.rgb_data);
  EXPECT_EQ(view.image_b.rgb_data, result.image_b.rgb_data);
  EXPECT_FALSE(view.difference.is_valid());
}

TEST(PreviewComparisonRunnerTest, StretchedViewKeepsTheNativeSize) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  const auto result = runner.compare(request());
  PreviewCompareViewSettings settings;
  settings.mode = PreviewCompareViewMode::Stretched;
  settings.stretch_window = 3;
  settings.stretch_gain = 4.0;

  const auto view = PreviewComparisonRunner::derive(result, settings);

  ASSERT_TRUE(view.success) << view.error;
  EXPECT_EQ(view.image_a.width, 16u);
  EXPECT_EQ(view.image_a.height, 8u);
  EXPECT_EQ(view.image_b.width, 16u);
  EXPECT_EQ(view.image_b.height, 8u);
  // A is flat, so it is unchanged; B's +10 step is amplified about A's
  // median (100): 100 + 4 * 10.
  EXPECT_EQ(view.image_a.rgb_data, result.image_a.rgb_data);
  EXPECT_EQ(view.image_b.rgb_data[(3 * 16 + 5) * 3], 140);
}

TEST(PreviewComparisonRunnerTest, NormalAndStretchedViewIsSideBySide) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  const auto result = runner.compare(request());
  PreviewCompareViewSettings settings;
  settings.mode = PreviewCompareViewMode::NormalAndStretched;

  const auto view = PreviewComparisonRunner::derive(result, settings);

  ASSERT_TRUE(view.success) << view.error;
  // Two 16-sample halves and the 8-pixel gap.
  EXPECT_EQ(view.image_a.width, 40u);
  EXPECT_EQ(view.image_a.height, 8u);
  EXPECT_EQ(view.image_b.width, 40u);
}

TEST(PreviewComparisonRunnerTest, DifferenceViewIsOneImage) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  const auto result = runner.compare(request());
  PreviewCompareViewSettings settings;
  settings.mode = PreviewCompareViewMode::Difference;
  settings.difference_gain = 2.0;

  const auto view = PreviewComparisonRunner::derive(result, settings);

  ASSERT_TRUE(view.success) << view.error;
  EXPECT_FALSE(view.image_a.is_valid());
  EXPECT_FALSE(view.image_b.is_valid());
  ASSERT_EQ(view.difference.width, 16u);
  ASSERT_EQ(view.difference.height, 8u);
  EXPECT_EQ(view.difference.rgb_data[0], 128);
  EXPECT_EQ(view.difference.rgb_data[(3 * 16 + 5) * 3], 148);
}

TEST(PreviewComparisonRunnerTest, ABadStretchWindowIsAnError) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  PreviewCompareViewSettings settings;
  settings.mode = PreviewCompareViewMode::Stretched;
  settings.stretch_window = 4;

  const auto view =
      PreviewComparisonRunner::derive(runner.compare(request()), settings);

  EXPECT_FALSE(view.success);
  EXPECT_FALSE(view.error.empty());
}

TEST(PreviewComparisonRunnerTest, DerivingAFailedComparisonCarriesItsError) {
  PreviewCompareResult failed;
  failed.error = "node B has no Frame output";

  const auto view =
      PreviewComparisonRunner::derive(failed, PreviewCompareViewSettings{});

  EXPECT_FALSE(view.success);
  EXPECT_EQ(view.error, "node B has no Frame output");
}

// --- buildExport() / exportComparison() -------------------------------------

TEST(PreviewComparisonRunnerTest, ExportDecodesToTwoFrames) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  PreviewCompareExportSettings settings;
  settings.stretch = false;
  settings.scale = 2;

  const auto exported =
      PreviewComparisonRunner::buildExport(runner.compare(request()), settings);

  ASSERT_TRUE(exported.success) << exported.error;
  const WebPInfo info = inspect(exported.bytes);
  ASSERT_TRUE(info.parsed);
  EXPECT_TRUE(info.animated);
  EXPECT_EQ(info.frames, 2u);
  EXPECT_EQ(info.width, 32u);
  EXPECT_EQ(info.height, 16u);
  EXPECT_EQ(exported.frame_width, 32u);
  EXPECT_EQ(exported.frame_height, 16u);
  EXPECT_FALSE(exported.identical);
}

TEST(PreviewComparisonRunnerTest, ExportWithStretchHasTwoHalves) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);

  const auto exported = PreviewComparisonRunner::buildExport(
      runner.compare(request()), PreviewCompareExportSettings{});

  ASSERT_TRUE(exported.success) << exported.error;
  EXPECT_EQ(exported.frame_width, 16u + 8u + 16u);
  EXPECT_EQ(inspect(exported.bytes).frames, 2u);
}

TEST(PreviewComparisonRunnerTest, ExportReportsTheAutoCrop) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  PreviewCompareExportSettings settings;
  settings.crop_mode = PreviewCompareCropMode::Auto;
  settings.auto_crop_width = 4;
  settings.auto_crop_height = 4;
  settings.stretch = false;

  const auto exported =
      PreviewComparisonRunner::buildExport(runner.compare(request()), settings);

  ASSERT_TRUE(exported.success) << exported.error;
  EXPECT_EQ(exported.crop_width, 4u);
  EXPECT_EQ(exported.crop_height, 4u);
  // The window holds the one differing sample at (5, 3).
  EXPECT_LE(exported.crop_x, 5u);
  EXPECT_GT(exported.crop_x + exported.crop_width, 5u);
  EXPECT_LE(exported.crop_y, 3u);
  EXPECT_GT(exported.crop_y + exported.crop_height, 3u);
  EXPECT_EQ(exported.frame_width, 4u);
}

TEST(PreviewComparisonRunnerTest, ExportReportsAnExplicitCrop) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  PreviewCompareExportSettings settings;
  settings.crop_mode = PreviewCompareCropMode::Explicit;
  settings.crop_x = 2;
  settings.crop_y = 1;
  settings.crop_width = 6;
  settings.crop_height = 5;
  settings.stretch = false;

  const auto exported =
      PreviewComparisonRunner::buildExport(runner.compare(request()), settings);

  ASSERT_TRUE(exported.success) << exported.error;
  EXPECT_EQ(exported.crop_x, 2u);
  EXPECT_EQ(exported.crop_y, 1u);
  EXPECT_EQ(exported.crop_width, 6u);
  EXPECT_EQ(exported.crop_height, 5u);
  EXPECT_EQ(exported.frame_width, 6u);
  EXPECT_EQ(exported.frame_height, 5u);
}

TEST(PreviewComparisonRunnerTest, IdenticalImagesAreReported) {
  FakeImageSource source = matching_source();
  source.node_images[kNodeB] = source.node_images[kNodeA];
  PreviewComparisonRunner runner(source);

  const auto exported = PreviewComparisonRunner::buildExport(
      runner.compare(request()), PreviewCompareExportSettings{});

  ASSERT_TRUE(exported.success) << exported.error;
  EXPECT_TRUE(exported.identical);
  // The labels still differ, so the file still animates.
  EXPECT_EQ(inspect(exported.bytes).frames, 2u);
}

TEST(PreviewComparisonRunnerTest, ADifferenceOutsideTheCropCountsAsIdentical) {
  FakeImageSource source = matching_source();  // differs at (5, 3) only
  PreviewComparisonRunner runner(source);
  PreviewCompareExportSettings settings;
  settings.crop_mode = PreviewCompareCropMode::Explicit;
  settings.crop_x = 8;
  settings.crop_y = 0;
  settings.crop_width = 8;
  settings.crop_height = 8;

  const auto exported =
      PreviewComparisonRunner::buildExport(runner.compare(request()), settings);

  ASSERT_TRUE(exported.success) << exported.error;
  EXPECT_TRUE(exported.identical);
}

TEST(PreviewComparisonRunnerTest, ExportOutOfRangeSettingIsAnError) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  PreviewCompareExportSettings settings;
  settings.scale = 9;

  const auto exported =
      PreviewComparisonRunner::buildExport(runner.compare(request()), settings);

  EXPECT_FALSE(exported.success);
  EXPECT_FALSE(exported.error.empty());
  EXPECT_TRUE(exported.bytes.empty());
}

TEST(PreviewComparisonRunnerTest, ExportOfAFailedComparisonCarriesItsError) {
  FakeImageSource source = matching_source();
  source.node_outputs[kNodeB].clear();
  PreviewComparisonRunner runner(source);

  const auto exported = runner.exportComparison(
      request(), PreviewCompareExportSettings{}, "unused.webp");

  EXPECT_FALSE(exported.success);
  EXPECT_THAT(exported.error, HasSubstr("node B"));
}

TEST(PreviewComparisonRunnerTest, ExportComparisonWritesTheFile) {
  FakeImageSource source = matching_source();
  source.node_outputs[kNodeB][0].count = 9;  // a warning to carry through
  PreviewComparisonRunner runner(source);
  const std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) /
      "preview_comparison_runner_test.webp";

  const auto exported = runner.exportComparison(
      request(), PreviewCompareExportSettings{}, path.string());

  ASSERT_TRUE(exported.success) << exported.error;
  EXPECT_TRUE(exported.bytes.empty());
  EXPECT_EQ(exported.warnings.size(), 1u);
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  in.close();
  std::filesystem::remove(path);
  EXPECT_EQ(inspect(bytes).frames, 2u);
}

TEST(PreviewComparisonRunnerTest, AnUnwritablePathIsAnError) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  const std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) /
      "no-such-directory-for-orc-tests" / "out.webp";

  const auto exported = runner.exportComparison(
      request(), PreviewCompareExportSettings{}, path.string());

  EXPECT_FALSE(exported.success);
  EXPECT_THAT(exported.error, HasSubstr("out.webp"));
}

namespace {

/// Width and height from a PNG's IHDR chunk, or 0 × 0 when the bytes are
/// not a PNG.
std::pair<uint32_t, uint32_t> png_size(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  static const uint8_t kSignature[] = {0x89, 'P',  'N',  'G',
                                       0x0d, 0x0a, 0x1a, 0x0a};
  if (bytes.size() < 24 ||
      !std::equal(std::begin(kSignature), std::end(kSignature),
                  bytes.begin()) ||
      std::string(bytes.begin() + 12, bytes.begin() + 16) != "IHDR") {
    return {0, 0};
  }
  const auto be32 = [&](size_t at) {
    return (uint32_t{bytes[at]} << 24) | (uint32_t{bytes[at + 1]} << 16) |
           (uint32_t{bytes[at + 2]} << 8) | uint32_t{bytes[at + 3]};
  };
  return {be32(16), be32(20)};
}

}  // namespace

TEST(PreviewComparisonRunnerTest, TheDifferenceIsWrittenAtTheCropAndScale) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  const auto dir = std::filesystem::path(::testing::TempDir());
  const auto webp = dir / "preview_comparison_difference_test.webp";
  const auto png = dir / "preview_comparison_difference_test.png";
  PreviewCompareExportSettings settings;
  settings.crop_mode = PreviewCompareCropMode::Explicit;
  settings.crop_x = 2;
  settings.crop_y = 1;
  settings.crop_width = 6;
  settings.crop_height = 4;
  settings.scale = 3;
  settings.difference_path = png.string();

  const auto exported =
      runner.exportComparison(request(), settings, webp.string());

  ASSERT_TRUE(exported.success) << exported.error;
  EXPECT_EQ(png_size(png), std::make_pair(18u, 12u));
  std::filesystem::remove(webp);
  std::filesystem::remove(png);
}

TEST(PreviewComparisonRunnerTest, AnUnwritableDifferencePathFailsTheExport) {
  FakeImageSource source = matching_source();
  PreviewComparisonRunner runner(source);
  const auto dir = std::filesystem::path(::testing::TempDir());
  const auto webp = dir / "preview_comparison_bad_difference_test.webp";
  PreviewCompareExportSettings settings;
  settings.difference_path =
      (dir / "no-such-directory-for-orc-tests" / "diff.png").string();

  const auto exported =
      runner.exportComparison(request(), settings, webp.string());

  EXPECT_FALSE(exported.success);
  EXPECT_THAT(exported.error, HasSubstr("difference image"));
  EXPECT_THAT(exported.error, HasSubstr("diff.png"));
  std::filesystem::remove(webp);
}

TEST(PreviewComparisonRunnerTest, WritePngEnlargesAndRefusesBadInput) {
  const auto png = std::filesystem::path(::testing::TempDir()) /
                   "preview_comparison_write_png_test.png";

  EXPECT_EQ(
      PreviewComparisonRunner::writePng(flat_image(5, 3, 40), 2, png.string()),
      "");
  EXPECT_EQ(png_size(png), std::make_pair(10u, 6u));
  EXPECT_THAT(
      PreviewComparisonRunner::writePng(flat_image(5, 3, 40), 9, png.string()),
      HasSubstr("1 to 8"));
  EXPECT_EQ(PreviewComparisonRunner::writePng(PreviewImage{}, 1, png.string()),
            "the image is empty");
  std::filesystem::remove(png);
}

}  // namespace orc_unit_test
