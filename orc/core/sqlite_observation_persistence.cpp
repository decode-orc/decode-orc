/*
 * File:        sqlite_observation_persistence.cpp
 * Module:      orc-core
 * Purpose:     SQLite-backed observation sidecar stored beside a project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "sqlite_observation_persistence.h"

#include <orc/support/logging.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

namespace orc {

namespace {

// Value-type tags stored in the value_type column. Persisted numerically, so
// their values are part of the on-disk format and must never be renumbered
// without a schema-version bump.
enum ValueType : int {
  kInt32 = 0,
  kInt64 = 1,
  kDouble = 2,
  kString = 3,
  kBool = 4,
  kEmptyRecord = 5,  // sentinel: observer ran, produced no values
};

// Serialise a variant value to its (type tag, text) on-disk representation.
// Doubles use %.17g so IEEE-754 doubles round-trip exactly.
std::pair<int, std::string> encode_value(const ObservationValue& value) {
  return std::visit(
      [](const auto& v) -> std::pair<int, std::string> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, int32_t>) {
          return {kInt32, std::to_string(v)};
        } else if constexpr (std::is_same_v<T, int64_t>) {
          return {kInt64, std::to_string(v)};
        } else if constexpr (std::is_same_v<T, double>) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%.17g", v);
          return {kDouble, std::string(buf)};
        } else if constexpr (std::is_same_v<T, std::string>) {
          return {kString, v};
        } else {  // bool
          return {kBool, v ? std::string("1") : std::string("0")};
        }
      },
      value);
}

// Reconstruct a variant value from its stored (type tag, text) representation.
ObservationValue decode_value(int value_type, const std::string& text) {
  switch (value_type) {
    case kInt32:
      return static_cast<int32_t>(std::stol(text));
    case kInt64:
      return static_cast<int64_t>(std::stoll(text));
    case kDouble:
      return std::stod(text);
    case kBool:
      return text == "1";
    case kString:
    default:
      return text;
  }
}

// RAII wrapper for a prepared statement, finalised on scope exit.
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      stmt_ = nullptr;
    }
  }
  ~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  explicit operator bool() const { return stmt_ != nullptr; }
  sqlite3_stmt* get() const { return stmt_; }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

// Execute a simple statement with no result rows. Returns true on success.
bool exec(sqlite3* db, const char* sql) {
  return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
  sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt* stmt, int index) {
  const auto* text = sqlite3_column_text(stmt, index);
  if (!text) return {};
  return reinterpret_cast<const char*>(text);
}

// SQL of the two statements every read connection keeps prepared.
constexpr const char* kExistsSql =
    "SELECT 1 FROM observation_record"
    " WHERE node_fingerprint = ? AND field_id = ?"
    " AND observer_id = ? AND observer_version = ? LIMIT 1";
constexpr const char* kLoadOneSql =
    "SELECT namespace, key, value_type, value"
    " FROM observation_record"
    " WHERE node_fingerprint = ? AND field_id = ?"
    " AND observer_id = ? AND observer_version = ?";

// Bind a whole record key into the four leading parameters shared by both
// point-query statements.
void bind_record_key(sqlite3_stmt* stmt, const ObservationRecordKey& key) {
  bind_text(stmt, 1, key.fingerprint.value);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(key.field_id.value()));
  bind_text(stmt, 3, key.observer_id);
  bind_text(stmt, 4, key.observer_version);
}

// Read a load_one result set (already stepped to its first row or not) into a
// record. Returns false when the statement yielded no rows at all.
bool collect_record(sqlite3_stmt* stmt, ObservationRecord& record) {
  bool found = false;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    found = true;
    const int value_type = sqlite3_column_int(stmt, 2);
    if (value_type == kEmptyRecord) continue;  // sentinel: present-but-empty
    record[column_text(stmt, 0)][column_text(stmt, 1)] =
        decode_value(value_type, column_text(stmt, 3));
  }
  return found;
}

// Upper bound on concurrently open read connections. The observation pool sizes
// itself from the core count and each worker holds at most one lease at a time,
// so this only has to cover the machine — a lease beyond it waits rather than
// failing.
std::size_t read_connection_cap() {
  const unsigned hw = std::thread::hardware_concurrency();
  return std::max<std::size_t>(4, hw == 0 ? 4 : hw + 2);
}

}  // namespace

SqliteObservationPersistence::ReadConnection::~ReadConnection() {
  if (exists_stmt) sqlite3_finalize(exists_stmt);
  if (load_one_stmt) sqlite3_finalize(load_one_stmt);
  if (db) sqlite3_close(db);
}

SqliteObservationPersistence::ReaderLease::ReaderLease(
    SqliteObservationPersistence* owner,
    std::unique_ptr<ReadConnection> connection)
    : owner_(owner), connection_(std::move(connection)) {}

SqliteObservationPersistence::ReaderLease::~ReaderLease() {
  if (connection_) owner_->release_reader(std::move(connection_));
}

SqliteObservationPersistence::SqliteObservationPersistence(std::string db_path)
    : db_path_(std::move(db_path)), max_readers_(read_connection_cap()) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!try_open_and_verify()) {
    // Corrupt file or schema mismatch: discard and rebuild rather than fail.
    ORC_LOG_WARN(
        "ObservationSidecar: '{}' is unusable (corrupt or wrong schema); "
        "rebuilding",
        db_path_);
    rebuild();
  }
}

SqliteObservationPersistence::~SqliteObservationPersistence() {
  // Destruction happens after the store has joined its write-behind thread and
  // dropped its handle, so no read can be in flight here.
  close_readers();
  std::lock_guard<std::mutex> lock(mutex_);
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

std::unique_ptr<SqliteObservationPersistence::ReadConnection>
SqliteObservationPersistence::open_read_connection() {
  auto connection = std::make_unique<ReadConnection>();
  // Opened read-write rather than read-only on purpose: a WAL reader needs
  // write access to the shared-memory index file, and the flag costs nothing
  // because nothing on this path issues a write.
  if (sqlite3_open_v2(db_path_.c_str(), &connection->db, SQLITE_OPEN_READWRITE,
                      nullptr) != SQLITE_OK) {
    return nullptr;
  }

  // Same page cache / mmap window as the writer (see configure_connection);
  // journal_mode and synchronous are properties of the database file, already
  // set by the writer, so they are not repeated here.
  exec(connection->db, "PRAGMA cache_size=-65536");
  exec(connection->db, "PRAGMA mmap_size=1073741824");
  exec(connection->db, "PRAGMA temp_store=MEMORY");
  sqlite3_busy_timeout(connection->db, 5000);

  if (sqlite3_prepare_v2(connection->db, kExistsSql, -1,
                         &connection->exists_stmt, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v2(connection->db, kLoadOneSql, -1,
                         &connection->load_one_stmt, nullptr) != SQLITE_OK) {
    return nullptr;
  }
  return connection;
}

SqliteObservationPersistence::ReaderLease
SqliteObservationPersistence::acquire_reader() {
  std::unique_lock<std::mutex> lock(read_mutex_);
  for (;;) {
    if (!idle_readers_.empty()) {
      auto connection = std::move(idle_readers_.back());
      idle_readers_.pop_back();
      return ReaderLease(this, std::move(connection));
    }
    if (readers_unavailable_) {
      return ReaderLease(this, nullptr);  // caller falls back to db_
    }
    if (open_readers_ < max_readers_) {
      ++open_readers_;
      lock.unlock();
      auto connection = open_read_connection();
      lock.lock();
      if (!connection) {
        // Opening failed (missing file, permissions, resource limit). Record
        // it so every later reader takes the writer-connection fallback
        // immediately instead of retrying a failing open per query.
        --open_readers_;
        readers_unavailable_ = true;
        ORC_LOG_WARN(
            "ObservationSidecar: cannot open a read connection to '{}'; "
            "reads fall back to the shared write connection",
            db_path_);
        return ReaderLease(this, nullptr);
      }
      return ReaderLease(this, std::move(connection));
    }
    // Pool is at its cap and every connection is leased: wait for one back.
    read_cv_.wait(lock);
  }
}

void SqliteObservationPersistence::release_reader(
    std::unique_ptr<ReadConnection> connection) noexcept {
  {
    std::lock_guard<std::mutex> lock(read_mutex_);
    // Growing the idle list allocates, and this runs from ~ReaderLease where
    // nothing may escape. push_back leaves its argument untouched when it
    // throws, so on failure the connection is closed here and its pool slot
    // given back; the next acquire_reader() simply opens a fresh one.
    try {
      idle_readers_.push_back(std::move(connection));
    } catch (...) {
      connection.reset();
      --open_readers_;
    }
  }
  read_cv_.notify_one();
}

void SqliteObservationPersistence::close_readers() {
  std::vector<std::unique_ptr<ReadConnection>> doomed;
  {
    std::lock_guard<std::mutex> lock(read_mutex_);
    doomed.swap(idle_readers_);
    open_readers_ -= doomed.size();
  }
  doomed.clear();  // connections closed outside the lock
}

void SqliteObservationPersistence::configure_connection() {
  // Performance configuration for this workload: a large (multi-GB),
  // regenerable cache written in frequent small batches by the write-behind
  // thread and read via indexed point/streaming queries.
  //
  //  - WAL: commits append to one log instead of creating and deleting a
  //    rollback-journal file per transaction — the write-behind path commits
  //    continuously during sweeps and triggers.
  //  - synchronous=NORMAL: with WAL this cannot corrupt the database on an
  //    application crash; an OS/power failure may lose the most recent
  //    transactions, which for a cache of recomputable observations merely
  //    means recomputing a few frames. FULL's per-commit fsyncs buy nothing
  //    here.
  //  - 64 MiB page cache + 1 GiB mmap window: point reads (read-through) and
  //    fingerprint streams (warm-up, merge) over a database far larger than
  //    SQLite's ~2 MiB default cache.
  //  - temp_store=MEMORY: GC's NOT-IN scratch structures stay off disk.
  //  - busy_timeout: a second connection (another app instance sharing the
  //    per-source quick cache) waits briefly instead of failing SQLITE_BUSY.
  exec(db_, "PRAGMA journal_mode=WAL");
  exec(db_, "PRAGMA synchronous=NORMAL");
  exec(db_, "PRAGMA cache_size=-65536");
  exec(db_, "PRAGMA mmap_size=1073741824");
  exec(db_, "PRAGMA temp_store=MEMORY");
  sqlite3_busy_timeout(db_, 5000);
}

bool SqliteObservationPersistence::try_open_and_verify() {
  if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    return false;
  }

  configure_connection();

  // A garbage file opens lazily; the first real statements surface corruption
  // (SQLITE_NOTADB / SQLITE_CORRUPT), and every failure path here funnels into
  // rebuild(). A full integrity_check would read the entire multi-GB B-tree on
  // every open — deliberately NOT done: this is a regenerable cache, and any
  // deeper corruption that slips past these probes surfaces as per-operation
  // errors that degrade to cache misses (recompute), never wrong data.
  if (!ensure_schema()) return false;

  // Cheap root-page probe of the main table (reads at most one page).
  {
    Statement probe(db_, "SELECT 1 FROM observation_record LIMIT 1");
    if (!probe) return false;
    const int rc = sqlite3_step(probe.get());
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return false;
  }

  // Reject an existing database written by a newer/older schema version.
  Statement stmt(db_, "SELECT value FROM schema_meta WHERE key = 'version'");
  if (!stmt) return false;
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return false;
  return column_text(stmt.get(), 0) == std::to_string(kSchemaVersion);
}

void SqliteObservationPersistence::rebuild() {
  // The file itself is about to be replaced, so any pooled read connection
  // still points at the discarded database. Only the constructor rebuilds, so
  // no reader can be leased out at this point.
  close_readers();
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  std::error_code ec;
  std::filesystem::remove(db_path_, ec);  // best effort; ignore failure

  if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
    const std::string msg =
        db_ ? sqlite3_errmsg(db_) : "unable to open observation sidecar";
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    throw std::runtime_error("ObservationSidecar: " + msg);
  }
  configure_connection();
  if (!ensure_schema()) {
    throw std::runtime_error(
        "ObservationSidecar: failed to initialise schema for '" + db_path_ +
        "'");
  }
}

bool SqliteObservationPersistence::ensure_schema() {
  if (!exec(db_,
            "CREATE TABLE IF NOT EXISTS observation_record("
            "  node_fingerprint TEXT NOT NULL,"
            "  field_id INTEGER NOT NULL,"
            "  observer_id TEXT NOT NULL,"
            "  observer_version TEXT NOT NULL,"
            "  namespace TEXT NOT NULL,"
            "  key TEXT NOT NULL,"
            "  value_type INTEGER NOT NULL,"
            "  value TEXT NOT NULL,"
            "  PRIMARY KEY (node_fingerprint, field_id, observer_id,"
            "               observer_version, namespace, key))")) {
    return false;
  }
  if (!exec(db_,
            "CREATE INDEX IF NOT EXISTS idx_obs_fingerprint"
            "  ON observation_record(node_fingerprint)")) {
    return false;
  }
  if (!exec(db_,
            "CREATE INDEX IF NOT EXISTS idx_obs_observer"
            "  ON observation_record(observer_id)")) {
    return false;
  }
  if (!exec(db_,
            "CREATE TABLE IF NOT EXISTS schema_meta("
            "  key TEXT PRIMARY KEY, value TEXT NOT NULL)")) {
    return false;
  }
  const std::string stamp_version =
      "INSERT OR IGNORE INTO schema_meta(key, value) VALUES ('version', '" +
      std::to_string(kSchemaVersion) + "')";
  return exec(db_, stamp_version.c_str());
}

void SqliteObservationPersistence::save(
    const std::vector<PersistedObservation>& records) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || records.empty()) return;

  Statement del(
      db_,
      "DELETE FROM observation_record WHERE node_fingerprint = ?"
      " AND field_id = ? AND observer_id = ? AND observer_version = ?");
  Statement ins(db_,
                "INSERT INTO observation_record(node_fingerprint, field_id,"
                " observer_id, observer_version, namespace, key, value_type,"
                " value) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
  if (!del || !ins) {
    ORC_LOG_ERROR("ObservationSidecar: failed to prepare save statements");
    return;
  }

  exec(db_, "BEGIN IMMEDIATE");
  for (const auto& item : records) {
    const auto& key = item.key;
    const auto field = static_cast<sqlite3_int64>(key.field_id.value());

    // Replace any existing rows for this key so a re-observation never leaves
    // stale values behind.
    sqlite3_reset(del.get());
    bind_text(del.get(), 1, key.fingerprint.value);
    sqlite3_bind_int64(del.get(), 2, field);
    bind_text(del.get(), 3, key.observer_id);
    bind_text(del.get(), 4, key.observer_version);
    sqlite3_step(del.get());

    auto insert_row = [&](const std::string& ns, const std::string& k,
                          int value_type, const std::string& value) {
      sqlite3_reset(ins.get());
      bind_text(ins.get(), 1, key.fingerprint.value);
      sqlite3_bind_int64(ins.get(), 2, field);
      bind_text(ins.get(), 3, key.observer_id);
      bind_text(ins.get(), 4, key.observer_version);
      bind_text(ins.get(), 5, ns);
      bind_text(ins.get(), 6, k);
      sqlite3_bind_int(ins.get(), 7, value_type);
      bind_text(ins.get(), 8, value);
      sqlite3_step(ins.get());
    };

    if (item.record.empty()) {
      // Present-but-empty record: store a single sentinel row.
      insert_row("", "", kEmptyRecord, "");
      continue;
    }
    for (const auto& [ns, keys] : item.record) {
      for (const auto& [k, v] : keys) {
        const auto [value_type, text] = encode_value(v);
        insert_row(ns, k, value_type, text);
      }
    }
  }
  exec(db_, "COMMIT");
}

void SqliteObservationPersistence::load_matching(
    const std::unordered_set<NodeFingerprint>& keep,
    const std::function<void(ObservationRecordKey, ObservationRecord)>& sink) {
  if (keep.empty()) return;

  // Warm-up streams a whole fingerprint (or the whole file) — long enough that
  // running it on the writer connection would stall every write behind it.
  auto reader = acquire_reader();
  std::unique_lock<std::mutex> writer_lock(mutex_, std::defer_lock);
  sqlite3* db = nullptr;
  if (reader) {
    db = reader->db;
  } else {
    writer_lock.lock();
    db = db_;
  }
  if (!db) return;

  // Small keep sets stream just the wanted fingerprints through the
  // idx_obs_fingerprint index — a single-fingerprint warm-up on a multi-GB
  // sidecar must not pay a full-table scan. Larger sets (whole-map warm
  // start) fall back to one ordered scan with the filter applied in the loop.
  std::string sql =
      "SELECT node_fingerprint, field_id, observer_id,"
      " observer_version, namespace, key, value_type, value"
      " FROM observation_record";
  constexpr std::size_t kIndexedKeepLimit = 8;
  const bool indexed = keep.size() <= kIndexedKeepLimit;
  if (indexed) {
    sql += " WHERE node_fingerprint IN (";
    for (std::size_t i = 0; i < keep.size(); ++i) {
      sql += i == 0 ? "?" : ",?";
    }
    sql += ")";
  }
  sql += " ORDER BY node_fingerprint, field_id, observer_id, observer_version";

  Statement stmt(db, sql.c_str());
  if (!stmt) return;
  if (indexed) {
    int slot = 1;
    for (const auto& fp : keep) {
      bind_text(stmt.get(), slot++, fp.value);
    }
  }

  // Rows arrive grouped by record key; accumulate a record and emit it when the
  // key changes.
  bool have_current = false;
  ObservationRecordKey current_key;
  ObservationRecord current_record;
  bool current_kept = false;

  auto flush_current = [&]() {
    if (have_current && current_kept) {
      sink(current_key, std::move(current_record));
    }
    current_record.clear();
    have_current = false;
  };

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    ObservationRecordKey row_key{
        NodeFingerprint{column_text(stmt.get(), 0)},
        FieldID(static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 1))),
        column_text(stmt.get(), 2), column_text(stmt.get(), 3)};

    if (!have_current || row_key != current_key) {
      flush_current();
      current_key = row_key;
      have_current = true;
      current_kept = keep.find(current_key.fingerprint) != keep.end();
    }
    if (!current_kept) continue;

    const std::string ns = column_text(stmt.get(), 4);
    const std::string k = column_text(stmt.get(), 5);
    const int value_type = sqlite3_column_int(stmt.get(), 6);
    if (value_type == kEmptyRecord) continue;  // sentinel: leave record empty
    current_record[ns][k] =
        decode_value(value_type, column_text(stmt.get(), 7));
  }
  flush_current();
}

std::optional<ObservationRecord> SqliteObservationPersistence::load_one(
    const ObservationRecordKey& key) {
  ObservationRecord record;

  // Pooled read connection with its statement already prepared: the hot path.
  if (auto reader = acquire_reader()) {
    sqlite3_stmt* stmt = reader->load_one_stmt;
    bind_record_key(stmt, key);
    const bool found = collect_record(stmt, record);
    // Reset before the connection goes back to the pool: a statement left
    // part-way through holds a read snapshot open, and an idle connection
    // holding one would keep the WAL from ever checkpointing.
    sqlite3_reset(stmt);
    if (!found) return std::nullopt;
    return record;
  }

  // Fallback: no read connection could be opened, so share the writer's.
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return std::nullopt;
  Statement stmt(db_, kLoadOneSql);
  if (!stmt) return std::nullopt;
  bind_record_key(stmt.get(), key);
  if (!collect_record(stmt.get(), record)) return std::nullopt;
  return record;
}

bool SqliteObservationPersistence::exists(const ObservationRecordKey& key) {
  // Presence probe on the primary-key prefix; LIMIT 1 stops at the first
  // value row so a large record costs no more than an empty one.
  if (auto reader = acquire_reader()) {
    sqlite3_stmt* stmt = reader->exists_stmt;
    bind_record_key(stmt, key);
    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    // LIMIT 1 means a hit stops mid-statement, which holds a read snapshot
    // open. Reset before returning the connection to the pool, or an idle
    // reader would pin the WAL against checkpointing for the rest of the
    // session.
    sqlite3_reset(stmt);
    return found;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;
  Statement stmt(db_, kExistsSql);
  if (!stmt) return false;
  bind_record_key(stmt.get(), key);
  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

bool SqliteObservationPersistence::load_stored_keys(
    const NodeFingerprint& fingerprint,
    const std::function<bool(FieldID, const std::string&, const std::string&)>&
        sink) {
  // A whole-node key walk is long-running; on a pooled connection it no longer
  // shuts the write-behind thread out for its whole duration.
  auto reader = acquire_reader();
  std::unique_lock<std::mutex> writer_lock(mutex_, std::defer_lock);
  sqlite3* db = nullptr;
  if (reader) {
    db = reader->db;
  } else {
    writer_lock.lock();
    db = db_;
  }
  if (!db) return false;

  // field_id/observer_id/observer_version are a prefix of the primary key, so
  // this is served entirely from the primary-key index: it walks index pages
  // in order and never touches a table row, where the bulky value text lives.
  // That is what makes a whole-node coverage answer cost one sequential scan
  // instead of millions of point queries. DISTINCT collapses a record's
  // per-value rows; the ORDER BY is the index's own order (so it is free) and
  // is what lets the caller turn row arrival into progress.
  Statement stmt(db,
                 "SELECT DISTINCT field_id, observer_id, observer_version"
                 " FROM observation_record WHERE node_fingerprint = ?"
                 " ORDER BY field_id, observer_id, observer_version");
  if (!stmt) return false;
  bind_text(stmt.get(), 1, fingerprint.value);

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const FieldID field(
        static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 0)));
    if (!sink(field, column_text(stmt.get(), 1), column_text(stmt.get(), 2))) {
      break;  // caller cancelled; the walk itself still counts as performed
    }
  }
  return true;
}

std::string SqliteObservationPersistence::get_meta(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return {};
  Statement stmt(db_, "SELECT value FROM schema_meta WHERE key = ?");
  if (!stmt) return {};
  bind_text(stmt.get(), 1, "app:" + key);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return {};
  return column_text(stmt.get(), 0);
}

void SqliteObservationPersistence::set_meta(const std::string& key,
                                            const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return;
  Statement stmt(db_,
                 "INSERT OR REPLACE INTO schema_meta(key, value)"
                 " VALUES (?, ?)");
  if (!stmt) return;
  bind_text(stmt.get(), 1, "app:" + key);
  bind_text(stmt.get(), 2, value);
  sqlite3_step(stmt.get());
}

std::size_t SqliteObservationPersistence::merge_from(
    const std::string& source_db_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || source_db_path.empty()) return 0;

  std::error_code ec;
  if (!std::filesystem::exists(source_db_path, ec) || ec) return 0;

  Statement attach(db_, "ATTACH DATABASE ? AS merge_src");
  if (!attach) return 0;
  bind_text(attach.get(), 1, source_db_path);
  if (sqlite3_step(attach.get()) != SQLITE_DONE) {
    ORC_LOG_WARN("ObservationSidecar: cannot attach '{}' for merge",
                 source_db_path);
    return 0;
  }

  std::size_t inserted = 0;
  {
    // Count guard: a source with no more rows than we already hold cannot
    // contribute anything new through INSERT OR IGNORE on the full PK — the
    // common case after a completed merge, kept cheap.
    sqlite3_int64 own_rows = 0;
    sqlite3_int64 src_rows = 0;
    Statement own_count(db_, "SELECT COUNT(*) FROM observation_record");
    Statement src_count(db_,
                        "SELECT COUNT(*) FROM merge_src.observation_record");
    const bool counts_ok =
        own_count && sqlite3_step(own_count.get()) == SQLITE_ROW &&
        (own_rows = sqlite3_column_int64(own_count.get(), 0), true) &&
        src_count && sqlite3_step(src_count.get()) == SQLITE_ROW &&
        (src_rows = sqlite3_column_int64(src_count.get(), 0), true);

    if (counts_ok && src_rows > 0 && src_rows > own_rows) {
      exec(db_, "BEGIN IMMEDIATE");
      // Adopt whole records only: a record (one observer's output for one
      // field) is atomic, so rows are copied only when the target holds NO
      // rows for that record key. Row-level INSERT OR IGNORE would blend rows
      // from two different observations of the same record.
      if (exec(db_,
               "INSERT INTO observation_record"
               " SELECT s.* FROM merge_src.observation_record AS s"
               " WHERE NOT EXISTS ("
               "   SELECT 1 FROM observation_record AS t"
               "   WHERE t.node_fingerprint = s.node_fingerprint"
               "     AND t.field_id = s.field_id"
               "     AND t.observer_id = s.observer_id"
               "     AND t.observer_version = s.observer_version)")) {
        inserted = static_cast<std::size_t>(sqlite3_changes(db_));
      }
      exec(db_, "COMMIT");
    }
  }

  exec(db_, "DETACH DATABASE merge_src");
  return inserted;
}

std::size_t SqliteObservationPersistence::retain_only(
    const std::unordered_set<NodeFingerprint>& keep) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;

  // Collect the distinct fingerprints present, then delete those not kept.
  std::vector<std::string> to_delete;
  {
    Statement stmt(db_,
                   "SELECT DISTINCT node_fingerprint FROM observation_record");
    if (!stmt) return 0;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      std::string fp = column_text(stmt.get(), 0);
      if (keep.find(NodeFingerprint{fp}) == keep.end()) {
        to_delete.push_back(std::move(fp));
      }
    }
  }
  if (to_delete.empty()) return 0;

  const std::size_t before = distinct_record_count_locked();
  Statement del(db_,
                "DELETE FROM observation_record WHERE node_fingerprint = ?");
  if (!del) return 0;
  exec(db_, "BEGIN IMMEDIATE");
  for (const auto& fp : to_delete) {
    sqlite3_reset(del.get());
    bind_text(del.get(), 1, fp);
    sqlite3_step(del.get());
  }
  exec(db_, "COMMIT");
  return before - distinct_record_count_locked();
}

std::size_t SqliteObservationPersistence::purge_observer_version(
    const std::string& observer_id, const std::string& current_version) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;

  Statement del(db_,
                "DELETE FROM observation_record WHERE observer_id = ?"
                " AND observer_version <> ?");
  if (!del) return 0;
  const std::size_t before = distinct_record_count_locked();
  bind_text(del.get(), 1, observer_id);
  bind_text(del.get(), 2, current_version);
  if (sqlite3_step(del.get()) != SQLITE_DONE) return 0;
  return before - distinct_record_count_locked();
}

std::size_t SqliteObservationPersistence::distinct_record_count_locked() {
  if (!db_) return 0;
  Statement stmt(db_,
                 "SELECT COUNT(*) FROM (SELECT DISTINCT node_fingerprint,"
                 " field_id, observer_id, observer_version"
                 " FROM observation_record)");
  if (!stmt) return 0;
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return 0;
  return static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 0));
}

std::size_t SqliteObservationPersistence::record_count() {
  std::lock_guard<std::mutex> lock(mutex_);
  return distinct_record_count_locked();
}

}  // namespace orc
