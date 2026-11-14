/*
 * File:        tool_frame_renderer_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     PreviewToolFrameRenderer: the frames a plugin view's
 *              render_frame() gets, with the dropouts the node reports
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tool_frame_renderer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "mocks/mock_render_presenter.h"

namespace gui_unit_test {
namespace {

using ::testing::_;
using ::testing::Eq;
using ::testing::NiceMock;
using ::testing::Return;

orc::PreviewOutputInfo sequentialOutput(uint64_t count, bool yc = false) {
  orc::PreviewOutputInfo info{};
  info.type = orc::PreviewOutputType::Frame_Field1_First;
  info.option_id = "sequential_clamped";
  info.count = count;
  info.is_available = true;
  info.dar_aspect_correction = 0.75;
  info.has_separate_channels = yc;
  return info;
}

orc::PreviewRenderResult frameWithDropout() {
  orc::PreviewRenderResult result;
  result.success = true;
  result.image.width = 4;
  result.image.height = 2;
  result.image.rgb_data.assign(4 * 2 * 3, 50);
  orc::DropoutRegion region;
  region.line = 1;
  region.start_sample = 2;
  region.end_sample = 3;
  result.image.dropout_regions = {region};
  return result;
}

TEST(ToolFrameRendererTest, RendersTheSequentialFrameWithItsDropouts) {
  auto presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  ON_CALL(*presenter, getAvailableOutputs(_))
      .WillByDefault(
          Return(std::vector<orc::PreviewOutputInfo>{sequentialOutput(10)}));
  EXPECT_CALL(*presenter, renderPreview(_, _, 3, Eq("sequential_clamped"), _,
                                        orc::PreviewPixelDelivery::Rgb))
      .WillOnce(Return(frameWithDropout()));
  PreviewToolFrameRenderer renderer(presenter);

  const auto image = renderer.render(orc::NodeID(1), 3);

  ASSERT_TRUE(image.ok) << image.error;
  EXPECT_EQ(image.frame_index, 3u);
  EXPECT_EQ(image.frame_count, 10u);
  EXPECT_EQ(image.width, 4u);
  EXPECT_EQ(image.height, 2u);
  EXPECT_EQ(image.rgb.size(), 24u);
  EXPECT_DOUBLE_EQ(image.dar_aspect_correction, 0.75);
  ASSERT_EQ(image.dropouts.size(), 1u);
  EXPECT_EQ(image.dropouts[0].line, 1u);
  EXPECT_EQ(image.dropouts[0].start_sample, 2u);
  EXPECT_EQ(image.dropouts[0].end_sample, 3u);
}

TEST(ToolFrameRendererTest, ShowsTheLumaOfAYcSource) {
  auto presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  ON_CALL(*presenter, getAvailableOutputs(_))
      .WillByDefault(Return(
          std::vector<orc::PreviewOutputInfo>{sequentialOutput(10, true)}));
  EXPECT_CALL(*presenter,
              renderPreview(_, _, 0, Eq("sequential_clamped_yc"), _, _))
      .WillOnce(Return(frameWithDropout()));
  PreviewToolFrameRenderer renderer(presenter);

  EXPECT_TRUE(renderer.render(orc::NodeID(1), 0).ok);
}

TEST(ToolFrameRendererTest, SaysHowManyFramesThereAreWhenItCannotRender) {
  auto presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  ON_CALL(*presenter, getAvailableOutputs(_))
      .WillByDefault(
          Return(std::vector<orc::PreviewOutputInfo>{sequentialOutput(10)}));
  EXPECT_CALL(*presenter, renderPreview).Times(0);
  PreviewToolFrameRenderer renderer(presenter);

  const auto image = renderer.render(orc::NodeID(1), 10);

  EXPECT_FALSE(image.ok);
  EXPECT_EQ(image.frame_count, 10u);
  EXPECT_FALSE(image.error.empty());
}

TEST(ToolFrameRendererTest, ANodeWithoutFramesIsAnError) {
  auto presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  ON_CALL(*presenter, getAvailableOutputs(_))
      .WillByDefault(Return(std::vector<orc::PreviewOutputInfo>{}));
  PreviewToolFrameRenderer renderer(presenter);

  const auto image = renderer.render(orc::NodeID(1), 0);

  EXPECT_FALSE(image.ok);
  EXPECT_EQ(image.frame_count, 0u);
  EXPECT_EQ(image.error, "The node has no frames to render");
}

}  // namespace
}  // namespace gui_unit_test
