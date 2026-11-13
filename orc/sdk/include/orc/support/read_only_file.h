/*
 * File:        read_only_file.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Positional reads from one open descriptor per input file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace orc {

/**
 * @brief An input file opened once and read at explicit offsets
 *
 * Source stages read a frame, a line or a sidecar block at a time from files
 * that can be hours long. Opening the file for every read costs a path lookup
 * and an open per read (a server round trip on a network share) and throws
 * away the kernel's read-ahead state. One descriptor read with positional
 * reads (`pread`; `ReadFile` with an offset on Windows) has neither cost and
 * never shares a file position, so concurrent reads need no lock.
 */
class ReadOnlyFile {
 public:
  /// Open |path| for reading. Returns nullptr and sets |error| on failure.
  static std::shared_ptr<const ReadOnlyFile> open(const std::string& path,
                                                  std::string& error);

  ~ReadOnlyFile();
  ReadOnlyFile(const ReadOnlyFile&) = delete;
  ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;

  const std::string& path() const { return path_; }

  /**
   * @brief Read up to |length| bytes at |offset|
   *
   * Returns the number of bytes read, which is less than |length| only at the
   * end of the file, or -1 on a read error.
   */
  int64_t read_at(uint64_t offset, void* data, uint64_t length) const;

  /**
   * @brief Whether |path| still names the file this descriptor reads
   *
   * False when the file has been replaced or rewritten since it was opened
   * (another identity, size or modification time), or removed.
   */
  bool still_current() const;

 private:
  ReadOnlyFile() = default;

  std::string path_;
  uint64_t size_at_open_ = 0;
  int64_t mtime_at_open_ = 0;  // file clock ticks
#if defined(_WIN32)
  void* handle_ = nullptr;  // HANDLE
#else
  int fd_ = -1;
  uint64_t device_ = 0;
  uint64_t inode_ = 0;
#endif
};

/**
 * @brief The open files of one reader, by path
 *
 * get() opens a path on first use and keeps it open. Every set in a plugin
 * shares one descriptor per file, so DAG copies and stage instances reading
 * the same capture do not multiply descriptors. Thread-safe.
 */
class ReadOnlyFileSet {
 public:
  /// The open file at |path|; nullptr with |error| set when it cannot open.
  std::shared_ptr<const ReadOnlyFile> get(const std::string& path,
                                          std::string& error) const;

  /**
   * @brief Reopen every file whose path now names a different file
   *
   * A file rewritten or replaced since it was opened is opened afresh; one
   * that is gone is dropped, for get() to report. Called where a stage
   * checks its inputs at execute, so a capture rewritten in place is read
   * afresh on the next run rather than through the old descriptor.
   */
  void refresh() const;

 private:
  mutable std::mutex mutex_;
  mutable std::unordered_map<std::string, std::shared_ptr<const ReadOnlyFile>>
      files_;
};

}  // namespace orc
