/*
 * File:        observation_persistence_sqlite_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     SQLite sidecar round-trip, warm start, GC, version purge and
 *              corruption recovery (Phase 6, Tasks 6.1-6.3)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * Functional (not unit): exercises the real SQLite backend against temporary
 * files on disk, which unit tests may not touch (AGENTS.md §4.2).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "observation_store.h"
#include "sqlite_observation_persistence.h"

namespace orc {
namespace {

NodeFingerprint fp(const std::string& v) { return NodeFingerprint{v}; }

ObservationRecordKey key(const std::string& fingerprint, uint64_t field,
                         const std::string& id, const std::string& version) {
  return ObservationRecordKey{fp(fingerprint), FieldID(field), id, version};
}

ObservationRecord all_variants_record() {
  ObservationRecord r;
  r["ns"]["i32"] = static_cast<int32_t>(-42);
  r["ns"]["i64"] = static_cast<int64_t>(9000000000LL);
  r["ns"]["dbl"] = 3.14159265358979;
  r["ns"]["str"] = std::string("hello world");
  r["ns"]["flag"] = true;
  r["other"]["k"] = static_cast<int32_t>(7);
  return r;
}

// Unique temporary sidecar path per test; removed on teardown.
class SqliteSidecarTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("orc-obs-") + info->test_suite_name() + "-" +
            info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    db_path_ = (dir_ / "observations.sqlite").string();
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::string db_path_;
};

// ---------------------------------------------------------------------------
// Round-trip (all variants, empty records, replacement)
// ---------------------------------------------------------------------------

TEST_F(SqliteSidecarTest, RoundTrip_PreservesAllVariants) {
  const auto k = key("fpA", 4, "white_snr", "1.0.0");
  {
    SqliteObservationPersistence db(db_path_);
    db.save({PersistedObservation{k, all_variants_record()}});
  }
  // Reopen a fresh connection to prove durability across close/open.
  SqliteObservationPersistence db(db_path_);
  ObservationRecord loaded;
  bool found = false;
  db.load_matching({fp("fpA")},
                   [&](ObservationRecordKey lk, ObservationRecord rec) {
                     if (lk == k) {
                       loaded = std::move(rec);
                       found = true;
                     }
                   });
  ASSERT_TRUE(found);
  EXPECT_EQ(loaded, all_variants_record());
}

TEST_F(SqliteSidecarTest, LoadOne_ReadsBackSingleRecordByExactKey) {
  const auto k = key("fpA", 4, "white_snr", "1.0.0");
  SqliteObservationPersistence db(db_path_);
  db.save({PersistedObservation{k, all_variants_record()},
           PersistedObservation{key("fpA", 6, "white_snr", "1.0.0"),
                                ObservationRecord{}}});

  // Exact-key point read returns just that record.
  const auto loaded = db.load_one(k);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(*loaded, all_variants_record());

  // Present-but-empty stays distinguishable from never-persisted.
  const auto empty = db.load_one(key("fpA", 6, "white_snr", "1.0.0"));
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
  EXPECT_FALSE(db.load_one(key("fpA", 8, "white_snr", "1.0.0")).has_value());
}

TEST_F(SqliteSidecarTest, Exists_ProbesPresenceWithoutLoading) {
  const auto k = key("fpA", 4, "white_snr", "1.0.0");
  SqliteObservationPersistence db(db_path_);
  db.save({PersistedObservation{k, all_variants_record()},
           PersistedObservation{key("fpA", 6, "white_snr", "1.0.0"),
                                ObservationRecord{}}});

  EXPECT_TRUE(db.exists(k));
  // A present-but-empty record (sentinel row) still reports presence.
  EXPECT_TRUE(db.exists(key("fpA", 6, "white_snr", "1.0.0")));
  // Never-persisted keys — including near-miss variants of a stored key — do
  // not.
  EXPECT_FALSE(db.exists(key("fpA", 8, "white_snr", "1.0.0")));
  EXPECT_FALSE(db.exists(key("fpB", 4, "white_snr", "1.0.0")));
  EXPECT_FALSE(db.exists(key("fpA", 4, "black_psnr", "1.0.0")));
  EXPECT_FALSE(db.exists(key("fpA", 4, "white_snr", "2.0.0")));
}

TEST_F(SqliteSidecarTest, LoadStoredKeys_StreamsRecordIdentitiesInFieldOrder) {
  SqliteObservationPersistence db(db_path_);
  db.save({
      // Deliberately out of field order, and with a multi-value record whose
      // rows must collapse to a single key.
      PersistedObservation{key("fpA", 6, "black_psnr", "1.0.0"),
                           all_variants_record()},
      PersistedObservation{key("fpA", 2, "white_snr", "1.0.0"),
                           all_variants_record()},
      PersistedObservation{key("fpA", 2, "black_psnr", "1.0.0"),
                           ObservationRecord{}},
      PersistedObservation{key("fpA", 4, "white_snr", "2.0.0"),
                           all_variants_record()},
      // A different fingerprint must not appear in fpA's walk.
      PersistedObservation{key("fpB", 0, "white_snr", "1.0.0"),
                           all_variants_record()},
  });

  std::vector<std::string> seen;
  EXPECT_TRUE(db.load_stored_keys(
      fp("fpA"), [&](FieldID field, const std::string& observer_id,
                     const std::string& observer_version) {
        seen.push_back(std::to_string(field.value()) + "/" + observer_id + "/" +
                       observer_version);
        return true;
      }));

  // One entry per record (not per value row), ascending by field id, and the
  // present-but-empty record is reported like any other.
  EXPECT_EQ(seen, (std::vector<std::string>{
                      "2/black_psnr/1.0.0", "2/white_snr/1.0.0",
                      "4/white_snr/2.0.0", "6/black_psnr/1.0.0"}));

  // A sink that stops early abandons the walk; the call still reports that it
  // ran, so the caller does not fall back to per-key probes.
  std::size_t delivered = 0;
  EXPECT_TRUE(db.load_stored_keys(
      fp("fpA"), [&](FieldID, const std::string&, const std::string&) {
        ++delivered;
        return false;
      }));
  EXPECT_EQ(delivered, 1u);

  // An unknown fingerprint is a successful walk over nothing.
  delivered = 0;
  EXPECT_TRUE(db.load_stored_keys(
      fp("nope"), [&](FieldID, const std::string&, const std::string&) {
        ++delivered;
        return true;
      }));
  EXPECT_EQ(delivered, 0u);
}

TEST_F(SqliteSidecarTest, MergeFrom_AdoptsRowsTheTargetLacks) {
  const std::string cache_path = (dir_ / "quick-cache.sqlite").string();
  const auto shared = key("fpA", 0, "white_snr", "1.0.0");
  const auto only_in_cache = key("fpA", 2, "white_snr", "1.0.0");

  ObservationRecord cache_shared;
  cache_shared["ns"]["v"] = static_cast<int32_t>(999);  // must NOT win
  {
    SqliteObservationPersistence cache(cache_path);
    cache.save({PersistedObservation{shared, cache_shared},
                PersistedObservation{only_in_cache, all_variants_record()}});
  }

  SqliteObservationPersistence db(db_path_);
  db.save({PersistedObservation{shared, all_variants_record()}});

  // Merge adopts the missing record without overwriting the existing one.
  EXPECT_GT(db.merge_from(cache_path), 0u);
  const auto adopted = db.load_one(only_in_cache);
  ASSERT_TRUE(adopted.has_value());
  EXPECT_EQ(*adopted, all_variants_record());
  const auto kept = db.load_one(shared);
  ASSERT_TRUE(kept.has_value());
  EXPECT_EQ(*kept, all_variants_record());  // target's row untouched

  // A repeated merge has nothing new to contribute (count guard).
  EXPECT_EQ(db.merge_from(cache_path), 0u);
}

TEST_F(SqliteSidecarTest, Meta_RoundTripsAndSurvivesReopen) {
  {
    SqliteObservationPersistence db(db_path_);
    EXPECT_EQ(db.get_meta("observer_versions"), "");  // unset reads empty
    db.set_meta("observer_versions", "white_snr:1.0.0;");
    db.set_meta("observer_versions", "white_snr:2.0.0;");  // overwrite wins
  }
  SqliteObservationPersistence db(db_path_);
  EXPECT_EQ(db.get_meta("observer_versions"), "white_snr:2.0.0;");
  // The "app:" prefix keeps stamps clear of the reserved schema version key.
  EXPECT_EQ(db.get_meta("version"), "");
}

TEST_F(SqliteSidecarTest, MergeFrom_MissingSourceIsANoop) {
  SqliteObservationPersistence db(db_path_);
  db.save(
      {PersistedObservation{key("fpA", 0, "id", "v"), all_variants_record()}});
  EXPECT_EQ(db.merge_from((dir_ / "does-not-exist.sqlite").string()), 0u);
  EXPECT_EQ(db.merge_from(""), 0u);
}

TEST_F(SqliteSidecarTest, EmptyRecord_RoundTripsAsPresentButEmpty) {
  const auto k = key("fpE", 0, "silent", "1.0.0");
  SqliteObservationPersistence db(db_path_);
  db.save({PersistedObservation{k, ObservationRecord{}}});

  int matches = 0;
  bool empty = false;
  db.load_matching({fp("fpE")},
                   [&](ObservationRecordKey lk, ObservationRecord rec) {
                     ++matches;
                     empty = rec.empty();
                     EXPECT_EQ(lk, k);
                   });
  EXPECT_EQ(matches, 1);  // present...
  EXPECT_TRUE(empty);     // ...and empty
}

TEST_F(SqliteSidecarTest, Save_ReplacesPreviousValuesForKey) {
  const auto k = key("fpR", 2, "id", "1.0.0");
  SqliteObservationPersistence db(db_path_);

  ObservationRecord big;
  big["ns"]["a"] = static_cast<int32_t>(1);
  big["ns"]["b"] = static_cast<int32_t>(2);
  db.save({PersistedObservation{k, big}});

  ObservationRecord small;
  small["ns"]["a"] = static_cast<int32_t>(9);  // fewer values than before
  db.save({PersistedObservation{k, small}});

  ObservationRecord loaded;
  db.load_matching({fp("fpR")},
                   [&](ObservationRecordKey, ObservationRecord rec) {
                     loaded = std::move(rec);
                   });
  EXPECT_EQ(loaded, small);  // no stale "b" left behind
}

// ---------------------------------------------------------------------------
// Task 6.2 — warm start via the store, zero observer runs for unchanged content
// ---------------------------------------------------------------------------

TEST_F(SqliteSidecarTest, StoreRoundTrip_WarmStartRestoresRecords) {
  const std::unordered_set<NodeFingerprint> reachable{fp("n1"), fp("n2")};

  // Session 1: observe and close.
  {
    ObservationStore store;
    store.set_persistence(
        std::make_shared<SqliteObservationPersistence>(db_path_));
    store.put(key("n1", 0, "white_snr", "1.0.0"), all_variants_record());
    store.put(key("n1", 1, "white_snr", "1.0.0"), all_variants_record());
    store.put(key("n2", 0, "biphase", "1.0.0"), all_variants_record());
    store.flush();
  }

  // Session 2: reopen; warm start must restore every record with no writes.
  ObservationStore store;
  store.set_persistence(
      std::make_shared<SqliteObservationPersistence>(db_path_));
  store.warm_start(reachable);

  EXPECT_TRUE(store.has(key("n1", 0, "white_snr", "1.0.0")));
  EXPECT_TRUE(store.has(key("n1", 1, "white_snr", "1.0.0")));
  EXPECT_TRUE(store.has(key("n2", 0, "biphase", "1.0.0")));
  EXPECT_EQ(*store.get(key("n1", 0, "white_snr", "1.0.0")),
            all_variants_record());
}

TEST_F(SqliteSidecarTest, WarmStart_ChangedFingerprint_LoadsNothing) {
  {
    ObservationStore store;
    store.set_persistence(
        std::make_shared<SqliteObservationPersistence>(db_path_));
    store.put(key("old_fp", 0, "white_snr", "1.0.0"), all_variants_record());
    store.flush();
  }
  // Reopen after a source/pipeline change: the new fingerprint has no records,
  // so the affected node starts cold — recomputation happens under the new
  // provenance.
  ObservationStore store;
  store.set_persistence(
      std::make_shared<SqliteObservationPersistence>(db_path_));
  store.warm_start({fp("new_fp")});
  EXPECT_FALSE(store.has(key("new_fp", 0, "white_snr", "1.0.0")));

  // The old fingerprint's persisted record stays visible via read-through
  // (content-addressed keys keep that correct — e.g. an undo back to the old
  // parameters reuses it). It disappears only when the sidecar GC drops
  // fingerprints outside the retention set.
  EXPECT_TRUE(store.has(key("old_fp", 0, "white_snr", "1.0.0")));
  store.gc_persistence({fp("new_fp")});
  ObservationStore reader;
  reader.set_persistence(
      std::make_shared<SqliteObservationPersistence>(db_path_));
  EXPECT_FALSE(reader.has(key("old_fp", 0, "white_snr", "1.0.0")));
}

// ---------------------------------------------------------------------------
// Task 6.3 — GC, version purge, corruption recovery
// ---------------------------------------------------------------------------

TEST_F(SqliteSidecarTest, RetainOnly_RemovesOnlyUnreachable) {
  SqliteObservationPersistence db(db_path_);
  db.save(
      {PersistedObservation{key("keep", 0, "id", "v"), all_variants_record()},
       PersistedObservation{key("keep", 1, "id", "v"), all_variants_record()},
       PersistedObservation{key("gone", 0, "id", "v"), all_variants_record()}});
  ASSERT_EQ(db.record_count(), 3u);

  const std::size_t removed = db.retain_only({fp("keep")});
  EXPECT_EQ(removed, 1u);
  EXPECT_EQ(db.record_count(), 2u);

  int gone_matches = 0;
  db.load_matching({fp("gone")}, [&](ObservationRecordKey, ObservationRecord) {
    ++gone_matches;
  });
  EXPECT_EQ(gone_matches, 0);
}

TEST_F(SqliteSidecarTest, RetainOnly_CountsRecordsAcrossRemovedFingerprints) {
  SqliteObservationPersistence db(db_path_);
  db.save(
      {PersistedObservation{key("keep", 0, "id", "v"), all_variants_record()},
       PersistedObservation{key("gone_a", 0, "id", "v"), all_variants_record()},
       PersistedObservation{key("gone_a", 1, "id", "v"), all_variants_record()},
       PersistedObservation{key("gone_a", 1, "other", "v"),
                            all_variants_record()},
       PersistedObservation{key("gone_b", 0, "id", "v"),
                            all_variants_record()}});
  ASSERT_EQ(db.record_count(), 5u);

  EXPECT_EQ(db.retain_only({fp("keep")}), 4u);
  EXPECT_EQ(db.record_count(), 1u);
  EXPECT_EQ(db.retain_only({fp("keep")}), 0u);
}

TEST_F(SqliteSidecarTest, PurgeObserverVersion_LeavesOtherObserversIntact) {
  SqliteObservationPersistence db(db_path_);
  db.save({
      PersistedObservation{key("fp", 0, "white_snr", "1.0.0"),
                           all_variants_record()},  // stale
      PersistedObservation{key("fp", 1, "white_snr", "2.0.0"),
                           all_variants_record()},  // current
      PersistedObservation{key("fp", 0, "biphase", "1.0.0"),
                           all_variants_record()},  // other observer
  });

  const std::size_t removed = db.purge_observer_version("white_snr", "2.0.0");
  EXPECT_EQ(removed, 1u);
  EXPECT_EQ(db.record_count(), 2u);

  // The surviving records: current white_snr + biphase.
  EXPECT_TRUE(db.record_count() == 2u);
  int biphase = 0;
  int white_current = 0;
  int white_stale = 0;
  db.load_matching({fp("fp")}, [&](ObservationRecordKey lk, ObservationRecord) {
    if (lk.observer_id == "biphase") ++biphase;
    if (lk.observer_id == "white_snr" && lk.observer_version == "2.0.0")
      ++white_current;
    if (lk.observer_id == "white_snr" && lk.observer_version == "1.0.0")
      ++white_stale;
  });
  EXPECT_EQ(biphase, 1);
  EXPECT_EQ(white_current, 1);
  EXPECT_EQ(white_stale, 0);
}

TEST_F(SqliteSidecarTest, CorruptedDatabase_RecoversByRebuilding) {
  // Write garbage where a database is expected.
  {
    std::ofstream f(db_path_, std::ios::binary);
    f << "this is definitely not a sqlite database file, just noise!!!";
  }

  // Construction must not throw: it detects corruption and rebuilds.
  SqliteObservationPersistence db(db_path_);
  EXPECT_EQ(db.record_count(), 0u);

  // The rebuilt database is fully usable.
  const auto k = key("fp", 0, "id", "v");
  db.save({PersistedObservation{k, all_variants_record()}});
  ObservationRecord loaded;
  db.load_matching({fp("fp")},
                   [&](ObservationRecordKey, ObservationRecord rec) {
                     loaded = std::move(rec);
                   });
  EXPECT_EQ(loaded, all_variants_record());
}

// ---------------------------------------------------------------------------
// Read-connection pool
// ---------------------------------------------------------------------------

TEST_F(SqliteSidecarTest, ConcurrentReadsDuringWrites_ReturnCorrectRecords) {
  // Reads run on pooled connections while the writer commits on its own. Both
  // must stay correct: a probe sees either the last committed state or the
  // record it is looking for, never a torn one, and never a wrong record's
  // values (the pooled statements are reused, so a missed rebind would show up
  // here as another key's data).
  SqliteObservationPersistence db(db_path_);

  constexpr int kFields = 400;
  std::vector<PersistedObservation> seed;
  seed.reserve(kFields);
  for (int i = 0; i < kFields; ++i) {
    ObservationRecord record;
    record["ns"]["field"] = static_cast<int32_t>(i);
    seed.push_back(PersistedObservation{
        key("fpX", static_cast<uint64_t>(i), "obs", "1"), std::move(record)});
  }
  db.save(seed);

  std::atomic<bool> stop{false};
  std::atomic<int> mismatches{0};

  // Keep committing while the readers run, so every probe has a live writer to
  // contend with.
  std::thread writer([&] {
    for (int round = 0; round < 20 && !stop.load(); ++round) {
      std::vector<PersistedObservation> batch;
      for (int i = 0; i < kFields; ++i) {
        ObservationRecord record;
        record["ns"]["field"] = static_cast<int32_t>(i);
        batch.push_back(PersistedObservation{
            key("fpY", static_cast<uint64_t>(round * kFields + i), "obs", "1"),
            std::move(record)});
      }
      db.save(batch);
    }
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 8; ++t) {
    readers.emplace_back([&, t] {
      for (int pass = 0; pass < 40; ++pass) {
        for (int i = t; i < kFields; i += 8) {
          const auto k = key("fpX", static_cast<uint64_t>(i), "obs", "1");
          if (!db.exists(k)) {
            ++mismatches;
            continue;
          }
          const auto record = db.load_one(k);
          if (!record || record->at("ns").at("field") !=
                             ObservationValue{static_cast<int32_t>(i)}) {
            ++mismatches;
          }
          // A key that was never written must read as absent, whatever the
          // writer is doing.
          if (db.exists(key("fpZ", static_cast<uint64_t>(i), "obs", "1"))) {
            ++mismatches;
          }
        }
      }
    });
  }

  for (auto& reader : readers) reader.join();
  stop.store(true);
  writer.join();

  EXPECT_EQ(mismatches.load(), 0);
}

TEST_F(SqliteSidecarTest, PooledReadsDoNotPinTheWriteAheadLog) {
  // A pooled connection is returned to the pool after every read, so its
  // statements must be reset — a statement stopped mid-row (exists() hits its
  // LIMIT 1 and stops) holds a read snapshot open, and an idle connection
  // holding one stops SQLite ever checkpointing the WAL. The symptom is a -wal
  // file that grows for the life of the session instead of being recycled.
  SqliteObservationPersistence db(db_path_);

  const auto probe = key("fp", 0, "obs", "1");
  ObservationRecord record;
  record["ns"]["v"] = static_cast<int32_t>(1);
  db.save({PersistedObservation{probe, record}});

  // Take a hit on the pooled connection, then commit far more data than the
  // autocheckpoint threshold. If the probe pinned the log, none of it can be
  // checkpointed away.
  ASSERT_TRUE(db.exists(probe));

  constexpr int kRounds = 160;
  constexpr int kPerRound = 200;
  for (int round = 0; round < kRounds; ++round) {
    std::vector<PersistedObservation> batch;
    batch.reserve(kPerRound);
    for (int i = 0; i < kPerRound; ++i) {
      ObservationRecord payload;
      payload["ns"]["text"] = std::string(512, 'x');
      batch.push_back(PersistedObservation{
          key("fpBulk", static_cast<uint64_t>(round * kPerRound + i), "obs",
              "1"),
          std::move(payload)});
    }
    db.save(batch);
  }

  std::error_code ec;
  const auto wal_bytes = std::filesystem::file_size(db_path_ + "-wal", ec);
  ASSERT_FALSE(ec)
      << "no -wal file: the sidecar is expected to run in WAL mode";
  // A checkpointing WAL settles at SQLite's autocheckpoint threshold whatever
  // the volume written; a pinned one has to hold every byte of it. Well over
  // 16 MB of payload went in above, so the two outcomes are far apart.
  constexpr std::uintmax_t kPayloadBytes =
      static_cast<std::uintmax_t>(kRounds) * kPerRound * 512;
  EXPECT_LT(wal_bytes, kPayloadBytes / 2)
      << "the write-ahead log holds essentially everything written, so a "
         "pooled read connection is pinning a snapshot against checkpointing";
}

}  // namespace
}  // namespace orc
