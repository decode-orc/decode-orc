/*
 * File:        video_parameter_observation_presenter_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Unit tests for frame-keyed observation lookup in the video
 *              parameter observation presenter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "video_parameter_observation_presenter.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <cstdint>
#include <optional>

namespace orc::presenters {
namespace {

// FrameQualityObserver stores one result per frame, keyed by the frame's
// first field (frame 3 -> field id 6).
orc::ObservationContext makeFrameQualityContext() {
  orc::ObservationContext ctx;
  ctx.set(orc::FieldID(6), "disc_quality", "quality_score", 0.75);
  ctx.set(orc::FieldID(6), "disc_quality", "dropout_count", int32_t{4});
  return ctx;
}

TEST(VideoParameterObservationPresenter,
     FrameQuality_IsReported_ForFirstFieldOfFrame) {
  const auto ctx = makeFrameQualityContext();

  const auto view = VideoParameterObservationPresenter::extractObservations(
      orc::FieldID(6), &ctx, std::nullopt);

  ASSERT_TRUE(view.quality_score.has_value());
  EXPECT_DOUBLE_EQ(*view.quality_score, 0.75);
  ASSERT_TRUE(view.dropout_count.has_value());
  EXPECT_EQ(*view.dropout_count, 4);
}

TEST(VideoParameterObservationPresenter,
     FrameQuality_IsReported_ForSecondFieldOfFrame) {
  const auto ctx = makeFrameQualityContext();

  const auto view = VideoParameterObservationPresenter::extractObservations(
      orc::FieldID(7), &ctx, std::nullopt);

  ASSERT_TRUE(view.quality_score.has_value());
  EXPECT_DOUBLE_EQ(*view.quality_score, 0.75);
  ASSERT_TRUE(view.dropout_count.has_value());
  EXPECT_EQ(*view.dropout_count, 4);
}

TEST(VideoParameterObservationPresenter,
     FrameQuality_IsAbsent_ForNeighbouringFrame) {
  const auto ctx = makeFrameQualityContext();

  const auto view = VideoParameterObservationPresenter::extractObservations(
      orc::FieldID(8), &ctx, std::nullopt);

  EXPECT_FALSE(view.quality_score.has_value());
  EXPECT_FALSE(view.dropout_count.has_value());
}

}  // namespace
}  // namespace orc::presenters
