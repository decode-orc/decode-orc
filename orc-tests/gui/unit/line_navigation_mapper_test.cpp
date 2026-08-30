/*
 * File:        line_navigation_mapper_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Unit tests for centralized line-scope navigation mapping helper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "line_navigation_mapper.h"

#include <gtest/gtest.h>

#include <vector>

namespace gui_unit_test {

// Models a weaved frame preview: even rows carry field 1 (index 2), odd rows
// carry field 2 (index 3), and both fields show field line image_y / 2. This
// is the row order map_preview_row_to_field() produces for a Frame_Field1_First
// weaved frame.
struct WeavedFrameModel {
  int image_height = 0;

  orc::FieldToImageMappingResult fieldToImage(uint64_t field_index,
                                              int field_line, int) const {
    if (field_index == 2) {
      return {true, field_line * 2};
    }
    if (field_index == 3) {
      return {true, field_line * 2 + 1};
    }
    return {false, 0};
  }

  orc::ImageToFieldMappingResult imageToField(int image_y, int) const {
    if (image_y < 0 || image_y >= image_height) {
      return {false, 0, 0};
    }
    const uint64_t field_index = (image_y % 2 == 0) ? 2 : 3;
    return {true, field_index, image_y / 2};
  }
};

orc::gui::LineNavigationTarget navigate(const WeavedFrameModel& model,
                                        int direction, uint64_t field_index,
                                        int line_number) {
  return orc::gui::computeLineNavigationTarget(
      {
          .direction = direction,
          .current_field = field_index,
          .current_line = line_number,
          .image_height = model.image_height,
      },
      [&model](uint64_t f, int l, int h) {
        return model.fieldToImage(f, l, h);
      },
      [&model](int y, int h) { return model.imageToField(y, h); });
}

TEST(LineNavigationMapperTest, ComputeTargetStepsOneRow_InWeavedFrame) {
  std::vector<int> observed_image_ys;
  int observed_next_height = -1;

  const auto result = orc::gui::computeLineNavigationTarget(
      {
          .direction = 1,
          .current_field = 2,
          .current_line = 120,
          .image_height = 525,
      },
      [](uint64_t field_index, int field_line, int image_height) {
        EXPECT_EQ(field_index, 2u);
        EXPECT_EQ(field_line, 120);
        EXPECT_EQ(image_height, 525);
        return orc::FieldToImageMappingResult{true, 240};
      },
      [&observed_image_ys, &observed_next_height](int image_y,
                                                  int image_height) {
        observed_image_ys.push_back(image_y);
        observed_next_height = image_height;
        return orc::ImageToFieldMappingResult{true, 3, 120};
      });

  // Row 241 is the next line of the picture - the other field's line, which
  // carries a different trace - so navigation lands on it rather than
  // skipping over it to the next line of the same field.
  EXPECT_TRUE(result.is_valid);
  EXPECT_EQ(result.field_index, 3u);
  EXPECT_EQ(result.line_number, 120);
  EXPECT_EQ(observed_image_ys, (std::vector<int>{241}));
  EXPECT_EQ(observed_next_height, 525);
}

TEST(LineNavigationMapperTest, ComputeTargetUpDown_IsReversibleInWeavedFrame) {
  const WeavedFrameModel model{.image_height = 526};

  // Down then up must return to the starting field and line, from both fields.
  for (const uint64_t start_field : {uint64_t{2}, uint64_t{3}}) {
    const int start_line = 120;

    const auto down = navigate(model, +1, start_field, start_line);
    ASSERT_TRUE(down.is_valid);
    const auto back_up =
        navigate(model, -1, down.field_index, down.line_number);
    ASSERT_TRUE(back_up.is_valid);
    EXPECT_EQ(back_up.field_index, start_field);
    EXPECT_EQ(back_up.line_number, start_line);

    const auto up = navigate(model, -1, start_field, start_line);
    ASSERT_TRUE(up.is_valid);
    const auto back_down = navigate(model, +1, up.field_index, up.line_number);
    ASSERT_TRUE(back_down.is_valid);
    EXPECT_EQ(back_down.field_index, start_field);
    EXPECT_EQ(back_down.line_number, start_line);
  }
}

TEST(LineNavigationMapperTest, ComputeTargetWalk_ReachesEveryPreviewRow) {
  // Every line the cross-hairs can select is one preview row, so walking the
  // buttons from the top must visit all of them - none may be reachable only
  // by clicking.
  const WeavedFrameModel model{.image_height = 20};

  std::vector<int> visited_rows;
  auto current = model.imageToField(0, model.image_height);
  ASSERT_TRUE(current.is_valid);
  visited_rows.push_back(0);

  for (int guard = 0; guard < 100; ++guard) {
    const auto next =
        navigate(model, +1, current.field_index, current.field_line);
    if (!next.is_valid) {
      break;
    }
    const auto row = model.fieldToImage(next.field_index, next.line_number, 0);
    ASSERT_TRUE(row.is_valid);
    visited_rows.push_back(row.image_y);
    current = {true, next.field_index, next.line_number};
  }

  std::vector<int> all_rows(model.image_height);
  for (int y = 0; y < model.image_height; ++y) {
    all_rows[y] = y;
  }
  EXPECT_EQ(visited_rows, all_rows);
}

TEST(LineNavigationMapperTest,
     ComputeTargetCrossesFieldBlockBoundary_InStackedLayout) {
  // Field-sequential/split layout: field 1 occupies rows 0..312, field 2
  // occupies rows 313..624. Stepping past the end of a block continues into
  // the next one, and stepping back returns to the row it came from.
  constexpr int kField1Lines = 313;
  constexpr int kImageHeight = 625;

  const auto field_to_image = [](uint64_t field_index, int field_line, int) {
    if (field_index == 2) {
      return orc::FieldToImageMappingResult{true, field_line};
    }
    if (field_index == 3) {
      return orc::FieldToImageMappingResult{true, field_line + kField1Lines};
    }
    return orc::FieldToImageMappingResult{false, 0};
  };
  const auto image_to_field = [](int image_y, int) {
    if (image_y < kField1Lines) {
      return orc::ImageToFieldMappingResult{true, 2, image_y};
    }
    return orc::ImageToFieldMappingResult{true, 3, image_y - kField1Lines};
  };

  const auto down = orc::gui::computeLineNavigationTarget(
      {
          .direction = 1,
          .current_field = 2,
          .current_line = kField1Lines - 1,
          .image_height = kImageHeight,
      },
      field_to_image, image_to_field);
  ASSERT_TRUE(down.is_valid);
  EXPECT_EQ(down.field_index, 3u);
  EXPECT_EQ(down.line_number, 0);

  const auto back_up = orc::gui::computeLineNavigationTarget(
      {
          .direction = -1,
          .current_field = down.field_index,
          .current_line = down.line_number,
          .image_height = kImageHeight,
      },
      field_to_image, image_to_field);
  ASSERT_TRUE(back_up.is_valid);
  EXPECT_EQ(back_up.field_index, 2u);
  EXPECT_EQ(back_up.line_number, kField1Lines - 1);
}

TEST(LineNavigationMapperTest,
     ComputeTargetInvalidWhenCurrentFieldToImageMapping_Fails) {
  bool map_image_called = false;

  const auto result = orc::gui::computeLineNavigationTarget(
      {
          .direction = 1,
          .current_field = 0,
          .current_line = 10,
          .image_height = 625,
      },
      [](uint64_t, int, int) {
        return orc::FieldToImageMappingResult{false, 0};
      },
      [&map_image_called](int, int) {
        map_image_called = true;
        return orc::ImageToFieldMappingResult{true, 0, 0};
      });

  EXPECT_FALSE(result.is_valid);
  EXPECT_FALSE(map_image_called);
}

TEST(LineNavigationMapperTest, Compute_TargetInvalidAtTopBoundary) {
  bool map_image_called = false;

  const auto result = orc::gui::computeLineNavigationTarget(
      {
          .direction = -1,
          .current_field = 1,
          .current_line = 0,
          .image_height = 525,
      },
      [](uint64_t, int, int) {
        return orc::FieldToImageMappingResult{true, 0};
      },
      [&map_image_called](int, int) {
        map_image_called = true;
        return orc::ImageToFieldMappingResult{true, 0, 0};
      });

  EXPECT_FALSE(result.is_valid);
  EXPECT_FALSE(map_image_called);
}

TEST(LineNavigationMapperTest,
     ComputeTargetInvalidWhenSteppedImageToFieldMapping_Fails) {
  const auto result = orc::gui::computeLineNavigationTarget(
      {
          .direction = 1,
          .current_field = 2,
          .current_line = 200,
          .image_height = 525,
      },
      [](uint64_t, int, int) {
        return orc::FieldToImageMappingResult{true, 400};
      },
      [](int, int) { return orc::ImageToFieldMappingResult{false, 0, 0}; });

  EXPECT_FALSE(result.is_valid);
}

TEST(LineNavigationMapperTest,
     ComputeTargetUpFromBottom_WorksWhenCurrentImageYIsOutOfRange) {
  int observed_next_image_y = -1;

  const auto result = orc::gui::computeLineNavigationTarget(
      {
          .direction = -1,
          .current_field = 1,
          .current_line = 262,
          .image_height = 525,
      },
      [](uint64_t, int, int) {
        // Reproduces asymmetry edge case at frame bottom.
        return orc::FieldToImageMappingResult{true, 525};
      },
      [&observed_next_image_y](int image_y, int) {
        observed_next_image_y = image_y;
        return orc::ImageToFieldMappingResult{true, 1, 261};
      });

  EXPECT_EQ(observed_next_image_y, 523);
  EXPECT_TRUE(result.is_valid);
  EXPECT_EQ(result.field_index, 1u);
  EXPECT_EQ(result.line_number, 261);
}

}  // namespace gui_unit_test