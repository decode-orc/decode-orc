/*
 * File:        observation_store_test.cpp
 * Module:      orc-core tests
 * Purpose:     Unit tests for the provenance-keyed ObservationStore (Task 2.1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "observation_store.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace orc {
namespace {

NodeFingerprint fp(const std::string& v) { return NodeFingerprint{v}; }

ObservationRecordKey key(const std::string& fingerprint, uint64_t field,
                         const std::string& id, const std::string& version) {
  return ObservationRecordKey{fp(fingerprint), FieldID(field), id, version};
}

// A record carrying one value of every ObservationValue variant.
ObservationRecord all_variants_record() {
  ObservationRecord record;
  record["ns"]["i32"] = static_cast<int32_t>(-42);
  record["ns"]["i64"] = static_cast<int64_t>(9000000000LL);
  record["ns"]["dbl"] = 3.14159;
  record["ns"]["str"] = std::string("hello");
  record["ns"]["flag"] = true;
  record["other_ns"]["k"] = static_cast<int32_t>(7);
  return record;
}

// ---------------------------------------------------------------------------
// Round-trip
// ---------------------------------------------------------------------------

TEST(ObservationStore_RoundTrip_PreservesAllVariants, Basic) {
  ObservationStore store;
  const auto k = key("aaa", 0, "white_snr", "1.0.0");

  EXPECT_FALSE(store.has(k));
  store.put(k, all_variants_record());

  ASSERT_TRUE(store.has(k));
  auto got = store.get(k);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, all_variants_record());
}

TEST(ObservationStore_LoadInto_PopulatesContextAtFieldId, Basic) {
  ObservationStore store;
  const uint64_t field = 11;
  const auto k = key("bbb", field, "biphase", "2.0.0");
  store.put(k, all_variants_record());

  ObservationContext ctx;
  ASSERT_TRUE(store.load_into(k, ctx));

  EXPECT_EQ(ctx.get(FieldID(field), "ns", "str"),
            ObservationValue(std::string("hello")));
  EXPECT_EQ(ctx.get(FieldID(field), "ns", "flag"), ObservationValue(true));
  EXPECT_EQ(ctx.get(FieldID(field), "other_ns", "k"),
            ObservationValue(static_cast<int32_t>(7)));
  // Nothing loaded for a different field.
  EXPECT_FALSE(ctx.has(FieldID(field + 1), "ns", "str"));
}

TEST(ObservationStore_LoadInto_MissLeavesContextUntouched, Basic) {
  ObservationStore store;
  ObservationContext ctx;
  EXPECT_FALSE(store.load_into(key("none", 0, "x", "1.0.0"), ctx));
  EXPECT_TRUE(ctx.get_namespaces(FieldID(0)).empty());
}

TEST(ObservationStore_EmptyRecord_IsDistinctFromAbsent, Basic) {
  ObservationStore store;
  const auto k = key("ccc", 0, "silent", "1.0.0");
  EXPECT_FALSE(store.has(k));
  store.put(k, ObservationRecord{});  // computed, produced nothing
  EXPECT_TRUE(store.has(k));
  auto got = store.get(k);
  ASSERT_TRUE(got.has_value());
  EXPECT_TRUE(got->empty());
}

TEST(ObservationStore_Put_OverwritesExistingKey, Basic) {
  ObservationStore store;
  const auto k = key("ddd", 0, "x", "1.0.0");
  ObservationRecord r1;
  r1["ns"]["v"] = static_cast<int32_t>(1);
  ObservationRecord r2;
  r2["ns"]["v"] = static_cast<int32_t>(2);

  store.put(k, r1);
  store.put(k, r2);
  EXPECT_EQ(store.size(), 1u);
  EXPECT_EQ((*store.get(k))["ns"]["v"],
            ObservationValue(static_cast<int32_t>(2)));
}

TEST(ObservationStore_DistinctKeyComponents_DoNotCollide, KeyIdentity) {
  ObservationStore store;
  store.put(key("fp1", 0, "id", "1.0.0"), all_variants_record());
  // Same everything except one component each.
  EXPECT_FALSE(store.has(key("fp2", 0, "id", "1.0.0")));   // fingerprint
  EXPECT_FALSE(store.has(key("fp1", 1, "id", "1.0.0")));   // field
  EXPECT_FALSE(store.has(key("fp1", 0, "id2", "1.0.0")));  // observer id
  EXPECT_FALSE(store.has(key("fp1", 0, "id", "2.0.0")));   // observer version
  EXPECT_TRUE(store.has(key("fp1", 0, "id", "1.0.0")));
}

// ---------------------------------------------------------------------------
// Eviction / retention
// ---------------------------------------------------------------------------

TEST(ObservationStore_Eviction_RespectsBudget, LruDropsOldest) {
  ObservationRecord record;  // ~305 bytes each; budget below fits two.
  record["ns"]["str"] = std::string(200, 'x');

  ObservationStore store(/*memory_budget_bytes=*/700);

  store.put(key("a", 0, "id", "v"), record);
  store.put(key("b", 0, "id", "v"), record);
  // Touch "a" so "b" becomes least-recently-used.
  EXPECT_TRUE(store.get(key("a", 0, "id", "v")).has_value());
  store.put(key("c", 0, "id", "v"), record);

  EXPECT_TRUE(store.has(key("a", 0, "id", "v")));   // recently used
  EXPECT_FALSE(store.has(key("b", 0, "id", "v")));  // evicted
  EXPECT_TRUE(store.has(key("c", 0, "id", "v")));   // just inserted
  EXPECT_LE(store.memory_usage_bytes(), store.memory_budget_bytes());
}

TEST(ObservationStore_RetainOnly_KeepsFingerprintSet, Retention) {
  ObservationStore store;
  store.put(key("keep1", 0, "id", "v"), all_variants_record());
  store.put(key("keep1", 1, "id", "v"), all_variants_record());
  store.put(key("drop", 0, "id", "v"), all_variants_record());
  store.put(key("keep2", 0, "id", "v"), all_variants_record());

  std::unordered_set<NodeFingerprint> keep{fp("keep1"), fp("keep2")};
  store.retain_only(keep, ObservationStore::kDefaultMemoryBudgetBytes);

  EXPECT_TRUE(store.has(key("keep1", 0, "id", "v")));
  EXPECT_TRUE(store.has(key("keep1", 1, "id", "v")));
  EXPECT_TRUE(store.has(key("keep2", 0, "id", "v")));
  EXPECT_FALSE(store.has(key("drop", 0, "id", "v")));
  EXPECT_EQ(store.size(), 3u);
}

TEST(ObservationStore_RetainOnly_EvictsToTightenedBudget, Retention) {
  ObservationRecord record;
  record["ns"]["str"] = std::string(200, 'x');
  ObservationStore store;
  store.put(key("k", 0, "id", "v"), record);
  store.put(key("k", 1, "id", "v"), record);
  store.put(key("k", 2, "id", "v"), record);

  // Keep the fingerprint but tighten the budget so only ~1 record survives.
  store.retain_only({fp("k")}, /*budget=*/400);
  EXPECT_LE(store.memory_usage_bytes(), 400u);
  EXPECT_GE(store.size(), 1u);
  EXPECT_LT(store.size(), 3u);
}

TEST(ObservationStore_Clear_RemovesEverything, Basic) {
  ObservationStore store;
  store.put(key("a", 0, "id", "v"), all_variants_record());
  store.clear();
  EXPECT_EQ(store.size(), 0u);
  EXPECT_EQ(store.memory_usage_bytes(), 0u);
  EXPECT_FALSE(store.has(key("a", 0, "id", "v")));
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST(ObservationStore_ConcurrentReadersWriters_StayConsistent, ThreadSafe) {
  ObservationStore store;  // ample budget: no eviction interferes.
  static constexpr int kWriters = 4;
  static constexpr int kPerWriter = 500;

  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&store, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        ObservationRecord rec;
        rec["ns"]["v"] = static_cast<int32_t>(w * 1000 + i);
        store.put(
            key("fp" + std::to_string(w), static_cast<uint64_t>(i), "id", "v"),
            rec);
      }
    });
  }
  // Concurrent readers probing keys that may or may not exist yet.
  std::atomic<int> hits{0};
  for (int r = 0; r < 2; ++r) {
    threads.emplace_back([&store, &hits] {
      for (int i = 0; i < kWriters * kPerWriter; ++i) {
        const int w = i % kWriters;
        auto got =
            store.get(key("fp" + std::to_string(w),
                          static_cast<uint64_t>(i / kWriters), "id", "v"));
        if (got.has_value()) {
          hits.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All writes are eventually present and readable.
  EXPECT_EQ(store.size(), static_cast<std::size_t>(kWriters * kPerWriter));
  for (int w = 0; w < kWriters; ++w) {
    for (int i = 0; i < kPerWriter; ++i) {
      auto got = store.get(
          key("fp" + std::to_string(w), static_cast<uint64_t>(i), "id", "v"));
      ASSERT_TRUE(got.has_value());
      EXPECT_EQ((*got)["ns"]["v"],
                ObservationValue(static_cast<int32_t>(w * 1000 + i)));
    }
  }
}

// ---------------------------------------------------------------------------
// get_or_compute
// ---------------------------------------------------------------------------

TEST(ObservationStore_GetOrCompute_ComputesOnceAndStores, Basic) {
  ObservationStore store;
  const auto k = key("aaa", 0, "stage:test", "1");
  int computed = 0;
  const auto compute = [&] {
    ++computed;
    return all_variants_record();
  };

  EXPECT_EQ(store.get_or_compute(k, compute), all_variants_record());
  EXPECT_EQ(store.get_or_compute(k, compute), all_variants_record());
  EXPECT_EQ(computed, 1);
  EXPECT_TRUE(store.has(k));
}

TEST(ObservationStore_GetOrCompute_ReturnsAStoredRecordUntouched, Basic) {
  ObservationStore store;
  const auto k = key("aaa", 0, "stage:test", "1");
  store.put(k, all_variants_record());

  bool computed = false;
  const auto got = store.get_or_compute(k, [&] {
    computed = true;
    return ObservationRecord{};
  });
  EXPECT_FALSE(computed);
  EXPECT_EQ(got, all_variants_record());
}

// A second caller arriving while the first computes gets the first's record
// rather than computing it again — whether it arrived in time to wait or only
// after the record was stored.
TEST(ObservationStore_GetOrCompute_ConcurrentCallerReusesTheResult, Basic) {
  ObservationStore store;
  const auto k = key("aaa", 0, "stage:test", "1");
  std::atomic<int> computed{0};
  const auto compute = [&] {
    ++computed;
    return all_variants_record();
  };

  ObservationRecord second;
  std::thread other;
  const auto first = store.get_or_compute(k, [&] {
    other = std::thread([&] { second = store.get_or_compute(k, compute); });
    return compute();
  });
  other.join();

  EXPECT_EQ(computed.load(), 1);
  EXPECT_EQ(first, all_variants_record());
  EXPECT_EQ(second, all_variants_record());
}

TEST(ObservationStore_GetOrCompute_FailureStoresNothing, Basic) {
  ObservationStore store;
  const auto k = key("aaa", 0, "stage:test", "1");

  EXPECT_THROW(
      store.get_or_compute(k,
                           []() -> ObservationRecord {
                             throw std::runtime_error("measurement failed");
                           }),
      std::runtime_error);
  EXPECT_FALSE(store.has(k));

  // The key is free again, so the next caller computes it.
  EXPECT_EQ(store.get_or_compute(k, [] { return all_variants_record(); }),
            all_variants_record());
}

}  // namespace
}  // namespace orc
