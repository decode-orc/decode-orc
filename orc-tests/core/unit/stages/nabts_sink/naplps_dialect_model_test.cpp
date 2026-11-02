/*
 * File:        naplps_dialect_model_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS dialect model: telling text from PDI
 *              data
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_dialect_model.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace orc {
namespace {

// X3.110 §4.3.3 locking shifts, and the PDI opcodes the tests write.
constexpr uint8_t kSo = 0x0E;
constexpr uint8_t kSi = 0x0F;
constexpr uint8_t kSelectColour = 0x3E;
constexpr uint8_t kSetColour = 0x3C;
constexpr uint8_t kPolyFilled = 0x35;

void append(std::vector<uint8_t>& out, const std::string& text) {
  out.insert(out.end(), text.begin(), text.end());
}

/// A page of the shape a service sends: a palette in PDI data, then a line of
/// text, then a filled polygon of |vertices| three-byte displacements.
std::vector<uint8_t> page(size_t vertices) {
  std::vector<uint8_t> out{kSo};
  for (uint8_t entry = 0; entry < 8; ++entry) {
    out.insert(out.end(),
               {kSelectColour, static_cast<uint8_t>(0x40 + 4 * entry),
                kSetColour, 0x7F, static_cast<uint8_t>(0x40 + entry), 0x40});
  }
  out.push_back(kSi);
  append(out, "TODAY'S WEATHER: SUNNY AND WARM THROUGH THE WEEKEND");
  out.push_back(kSo);
  out.push_back(kPolyFilled);
  for (size_t v = 0; v < vertices; ++v) {
    out.insert(out.end(), {0x41, static_cast<uint8_t>(0x40 + (v % 7)), 0x42});
  }
  out.push_back(kSi);
  return out;
}

TEST(NaplpsDialectModel, ReadsTextAsTextAndPdiDataAsPdiData) {
  NaplpsDialectModel model;
  for (size_t i = 0; i < 4; ++i) {
    model.learn(page(10 + i), {});
  }
  ASSERT_EQ(model.records(), 4u);

  const std::string words = "THE WEEKEND WILL BE WARM";
  const auto* text = reinterpret_cast<const uint8_t*>(words.data());
  EXPECT_GT(model.text_score(text, words.size()),
            model.pdi_score(text, words.size()) + 1.0);

  const std::vector<uint8_t> palette{kSelectColour, 0x44, kSetColour,
                                     0x7F,          0x41, 0x40,
                                     kSelectColour, 0x48, kSetColour};
  EXPECT_GT(model.pdi_score(palette.data(), palette.size()),
            model.text_score(palette.data(), palette.size()) + 1.0);
}

// A byte pair across a lost byte never occurred: "X" and "Z" were never sent
// side by side, so the model knows no more of the pair than it would unlearned
// (add-half smoothing over 128 bytes).
TEST(NaplpsDialectModel, LearnsNothingFromWhatNeverArrived) {
  const uint8_t across[] = {'X', 'Z'};

  NaplpsDialectModel lost;
  lost.learn({kSi, 'X', 0x00, 'Z'}, {1, 1, 0, 1});
  EXPECT_DOUBLE_EQ(lost.text_score(across, 2), std::log(0.5 / 64.0));

  NaplpsDialectModel sent;
  sent.learn({kSi, 'X', 'Z'}, {});
  EXPECT_GT(sent.text_score(across, 2), lost.text_score(across, 2));
}

}  // namespace
}  // namespace orc
