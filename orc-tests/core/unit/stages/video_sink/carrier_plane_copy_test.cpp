/*
 * File:        carrier_plane_copy_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for flattening decoded planes into a scope carrier
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/sinks/common/carrier_plane_copy.h"

#include <gtest/gtest.h>

#include <vector>

namespace orc_unit_test {
namespace {

// A frame whose every sample is distinguishable, so a copy that transposed,
// skipped or repeated a line would not come out looking right by accident.
ComponentFrame makeNumberedFrame(int32_t nominal_width) {
  orc::SourceParameters params;
  params.system = orc::VideoSystem::NTSC;
  params.frame_width_nominal = nominal_width;

  ComponentFrame frame;
  frame.init(params, false);

  for (int32_t line = 0; line < frame.getHeight(); ++line) {
    double* y = frame.y(line);
    double* u = frame.u(line);
    double* v = frame.v(line);
    for (int32_t x = 0; x < frame.getWidth(); ++x) {
      y[x] = line * 1000.0 + x;
      u[x] = line * 1000.0 + x + 0.25;
      v[x] = line * 1000.0 + x + 0.5;
    }
  }
  return frame;
}

TEST(CarrierPlaneCopy, FlattensEverySampleTheLineLoopWouldHaveRead) {
  const ComponentFrame frame = makeNumberedFrame(8);
  const int32_t width = frame.getWidth();
  const int32_t height = frame.getHeight();

  std::vector<double> y_plane;
  std::vector<double> u_plane;
  std::vector<double> v_plane;
  orc::copy_component_planes_to_carrier(frame, width, height, y_plane, u_plane,
                                        v_plane);

  const std::size_t samples =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  ASSERT_EQ(y_plane.size(), samples);
  ASSERT_EQ(u_plane.size(), samples);
  ASSERT_EQ(v_plane.size(), samples);

  // The reference is the per-sample read the copy replaced.
  for (int32_t line = 0; line < height; ++line) {
    const double* y = frame.y(line);
    const double* u = frame.u(line);
    const double* v = frame.v(line);
    for (int32_t x = 0; x < width; ++x) {
      const std::size_t index =
          static_cast<std::size_t>(line) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x);
      EXPECT_DOUBLE_EQ(y_plane[index], y[x]) << "line " << line << " x " << x;
      EXPECT_DOUBLE_EQ(u_plane[index], u[x]) << "line " << line << " x " << x;
      EXPECT_DOUBLE_EQ(v_plane[index], v[x]) << "line " << line << " x " << x;
    }
  }
}

// The carrier is rebuilt for every displayed frame, so the buffers are handed
// back already sized. Whatever a previous frame left in them must be gone.
TEST(CarrierPlaneCopy, OverwritesWhateverThePreviousFrameLeftBehind) {
  const ComponentFrame frame = makeNumberedFrame(8);
  const int32_t width = frame.getWidth();
  const int32_t height = frame.getHeight();
  const std::size_t samples =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

  std::vector<double> y_plane(samples * 3, -1.0);
  std::vector<double> u_plane(samples * 3, -1.0);
  std::vector<double> v_plane(samples * 3, -1.0);

  orc::copy_component_planes_to_carrier(frame, width, height, y_plane, u_plane,
                                        v_plane);

  ASSERT_EQ(y_plane.size(), samples);
  for (const double sample : y_plane) {
    EXPECT_NE(sample, -1.0);
  }
}

TEST(CarrierPlaneCopy, ProducesNoPlanesForADegenerateFrame) {
  const ComponentFrame frame = makeNumberedFrame(8);

  std::vector<double> y_plane(4, 1.0);
  std::vector<double> u_plane(4, 1.0);
  std::vector<double> v_plane(4, 1.0);

  orc::copy_component_planes_to_carrier(frame, 0, frame.getHeight(), y_plane,
                                        u_plane, v_plane);
  EXPECT_TRUE(y_plane.empty());
  EXPECT_TRUE(u_plane.empty());
  EXPECT_TRUE(v_plane.empty());
}

}  // namespace
}  // namespace orc_unit_test
