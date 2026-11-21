/*
 * File:        scratch_file.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Anonymous on-disk scratch files for stages whose working data
 *              grows with source length
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace orc {

/**
 * @brief The per-user directory for scratch files of one |kind|
 *
 * `ORC_CACHE_DIR/<kind>` when that variable is set; otherwise the platform's
 * per-user cache directory: `%LOCALAPPDATA%\decode-orc\cache\<kind>` on
 * Windows, `~/Library/Caches/decode-orc/<kind>` on macOS, and
 * `$XDG_CACHE_HOME/decode-orc/<kind>` or `~/.cache/decode-orc/<kind>`
 * elsewhere. The temp directory is the last resort only, because it may be
 * RAM-backed. The directory is not created here.
 */
std::filesystem::path scratch_directory(const std::string& kind);

/**
 * @brief A file that exists only while this object does
 *
 * Created in a given directory under a unique name and removed from the
 * directory at once (POSIX: unlinked while open; Windows: delete-on-close), so
 * its space is released when the object is destroyed or the process ends,
 * however it ends.
 *
 * Reads and writes are positioned and never share a file position, so
 * concurrent calls on distinct ranges need no lock.
 */
class ScratchFile {
 public:
  /**
   * @brief Create a scratch file in |directory|, creating the directory
   *
   * The file is named |prefix| + a process-unique part + |suffix|. Returns
   * nullptr and sets |error| on failure.
   */
  static std::unique_ptr<ScratchFile> create(
      const std::filesystem::path& directory, const std::string& prefix,
      const std::string& suffix, std::string& error);

  ~ScratchFile();
  ScratchFile(const ScratchFile&) = delete;
  ScratchFile& operator=(const ScratchFile&) = delete;

  /// Write |length| bytes at |offset|; false on any failure (disk full).
  bool write_at(uint64_t offset, const void* data, uint64_t length) const;

  /// Read exactly |length| bytes at |offset|; false if they are not all there.
  bool read_at(uint64_t offset, void* data, uint64_t length) const;

 private:
  ScratchFile() = default;

#if defined(_WIN32)
  void* handle_ = nullptr;  // HANDLE; INVALID_HANDLE_VALUE when unset
#else
  int fd_ = -1;
#endif
};

}  // namespace orc
