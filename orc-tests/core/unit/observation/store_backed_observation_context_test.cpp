/*
 * File:        store_backed_observation_context_test.cpp
 * Module:      orc-core tests
 * Purpose:     Unit tests for the store-backed read-through observation
 *              context used by triggered sinks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "store_backed_observation_context.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <string>
#include <vector>

namespace orc {
namespace {

constexpr FieldID::value_type kFieldsPerFrame = 2;

NodeFingerprint fp(const std::string& v) { return NodeFingerprint{v}; }

ObserverInfo stateless_observer(const std::string& id) {
  ObserverInfo info;
  info.id = id;
  info.version = "1.0.0";
  info.stateless = true;
  return info;
}

ObserverInfo stateful_observer(const std::string& id) {
  ObserverInfo info = stateless_observer(id);
  info.stateless = false;
  return info;
}

ObservationRecord record_with(const std::string& ns, const std::string& key,
                              int32_t value) {
  ObservationRecord record;
  record[ns][key] = value;
  return record;
}

ObservationRecordKey key_of(const NodeFingerprint& fingerprint, FieldID field,
                            const ObserverInfo& observer) {
  return ObservationRecordKey{fingerprint, field, observer.id,
                              observer.version};
}

class StoreBackedObservationContextTest : public ::testing::Test {
 protected:
  ObservationStore store_;
  ObservationContext inner_;
  NodeFingerprint fingerprint_ = fp("node-a");
  FrameIDRange range_{0, 3};  // frames 0-3 → fields 0-7

  // Store a record for both fields of @p frame under @p observer.
  void store_frame(const ObserverInfo& observer, FrameID frame, int32_t value) {
    const FieldID top(frame * kFieldsPerFrame);
    const FieldID bottom(frame * kFieldsPerFrame + 1);
    store_.put(key_of(fingerprint_, top, observer),
               record_with(observer.id, "value", value));
    store_.put(key_of(fingerprint_, bottom, observer),
               record_with(observer.id, "value", value + 1));
  }
};

TEST_F(StoreBackedObservationContextTest,
       ReadLoadsTheTouchedFieldAndOnlyThatField) {
  const auto observer = stateless_observer("white_snr");
  store_frame(observer, 0, 10);
  store_frame(observer, 1, 20);
  StoreBackedObservationContext context(inner_, store_, fingerprint_,
                                        {observer}, range_);

  EXPECT_TRUE(context.has(FieldID(0), "white_snr", "value"));
  const auto value = context.get(FieldID(0), "white_snr", "value");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(std::get<int32_t>(*value), 10);

  // Only the touched field materialised in the inner context.
  EXPECT_TRUE(inner_.has(FieldID(0), "white_snr", "value"));
  EXPECT_FALSE(inner_.has(FieldID(2), "white_snr", "value"));
}

TEST_F(StoreBackedObservationContextTest, MissingFieldReadsAsAbsent) {
  const auto observer = stateless_observer("white_snr");
  StoreBackedObservationContext context(inner_, store_, fingerprint_,
                                        {observer}, range_);

  EXPECT_FALSE(context.has(FieldID(4), "white_snr", "value"));
  EXPECT_FALSE(context.get(FieldID(4), "white_snr", "value").has_value());
}

TEST_F(StoreBackedObservationContextTest,
       ClearedFieldIsNotResurrectedByALaterRead) {
  const auto observer = stateless_observer("white_snr");
  store_frame(observer, 0, 10);
  StoreBackedObservationContext context(inner_, store_, fingerprint_,
                                        {observer}, range_);

  ASSERT_TRUE(context.has(FieldID(0), "white_snr", "value"));
  context.clear_field(FieldID(0));
  EXPECT_FALSE(context.has(FieldID(0), "white_snr", "value"));

  // A field cleared before ever being read stays gone too: the sink decided
  // it is done with the field, whatever the store still holds.
  context.clear_field(FieldID(1));
  EXPECT_FALSE(context.has(FieldID(1), "white_snr", "value"));
}

TEST_F(StoreBackedObservationContextTest, ClearWipesAndStopsAllLoading) {
  const auto observer = stateless_observer("white_snr");
  store_frame(observer, 0, 10);
  store_frame(observer, 1, 20);
  StoreBackedObservationContext context(inner_, store_, fingerprint_,
                                        {observer}, range_);

  ASSERT_TRUE(context.has(FieldID(0), "white_snr", "value"));
  context.clear();

  EXPECT_FALSE(context.has(FieldID(0), "white_snr", "value"));
  EXPECT_FALSE(context.has(FieldID(2), "white_snr", "value"));
}

TEST_F(StoreBackedObservationContextTest,
       SetMergesWithStoredRecordsOfTheSameField) {
  const auto observer = stateless_observer("white_snr");
  store_frame(observer, 0, 10);
  StoreBackedObservationContext context(inner_, store_, fingerprint_,
                                        {observer}, range_);

  // Writing to a field first must not hide its stored records: the load
  // happens before the write, exactly as the eager pre-load behaved.
  context.set(FieldID(0), "other", "computed", true);

  EXPECT_TRUE(context.has(FieldID(0), "white_snr", "value"));
  EXPECT_TRUE(context.has(FieldID(0), "other", "computed"));
}

TEST_F(StoreBackedObservationContextTest,
       StatefulObserverLoadsOnlyWhenEveryFrameIsCovered) {
  const auto observer = stateful_observer("colour_frame_phase");
  // Frames 0-2 covered, frame 3 missing → nothing may load.
  store_frame(observer, 0, 10);
  store_frame(observer, 1, 20);
  store_frame(observer, 2, 30);
  StoreBackedObservationContext partial(inner_, store_, fingerprint_,
                                        {observer}, range_);
  EXPECT_FALSE(partial.has(FieldID(0), "colour_frame_phase", "value"));

  // With the last frame stored, the stream is complete and loads per field.
  store_frame(observer, 3, 40);
  ObservationContext fresh_inner;
  StoreBackedObservationContext complete(fresh_inner, store_, fingerprint_,
                                         {observer}, range_);
  EXPECT_TRUE(complete.has(FieldID(0), "colour_frame_phase", "value"));
  EXPECT_TRUE(complete.has(FieldID(7), "colour_frame_phase", "value"));
}

TEST_F(StoreBackedObservationContextTest,
       FieldOutsideTheRangeDelegatesWithoutLoading) {
  const auto observer = stateless_observer("white_snr");
  const FieldID outside(99);
  store_.put(key_of(fingerprint_, outside, observer),
             record_with("white_snr", "value", 5));
  StoreBackedObservationContext context(inner_, store_, fingerprint_,
                                        {observer}, range_);

  EXPECT_FALSE(context.has(outside, "white_snr", "value"));
  inner_.set(outside, "manual", "flag", true);
  EXPECT_TRUE(context.has(outside, "manual", "flag"));
}

TEST_F(StoreBackedObservationContextTest,
       GetKeysAndNamespacesAndAllObservationsSeeStoredRecords) {
  const auto observer = stateless_observer("white_snr");
  store_frame(observer, 0, 10);
  StoreBackedObservationContext context(inner_, store_, fingerprint_,
                                        {observer}, range_);

  const auto namespaces = context.get_namespaces(FieldID(0));
  ASSERT_EQ(namespaces.size(), 1u);
  EXPECT_EQ(namespaces[0], "white_snr");
  const auto keys = context.get_keys(FieldID(0), "white_snr");
  ASSERT_EQ(keys.size(), 1u);
  EXPECT_EQ(keys[0], "value");
  const auto all = context.get_all_observations(FieldID(1));
  ASSERT_EQ(all.count("white_snr"), 1u);
  EXPECT_EQ(std::get<int32_t>(all.at("white_snr").at("value")), 11);
}

TEST_F(StoreBackedObservationContextTest,
       EmptyFingerprintNeverTouchesTheStore) {
  const auto observer = stateless_observer("white_snr");
  store_frame(observer, 0, 10);
  StoreBackedObservationContext context(inner_, store_, NodeFingerprint{},
                                        {observer}, range_);

  EXPECT_FALSE(context.has(FieldID(0), "white_snr", "value"));
}

}  // namespace
}  // namespace orc
