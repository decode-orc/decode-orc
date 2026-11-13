/*
 * File:        read_only_file.cpp
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Positional reads from one open descriptor per input file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <orc/support/read_only_file.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace orc {

namespace {

// Size and modification time of |path|, or false when it cannot be read.
bool path_size_and_mtime(const std::string& path, uint64_t& size,
                         int64_t& mtime) {
  std::error_code ec;
  const auto s = std::filesystem::file_size(path, ec);
  if (ec) return false;
  const auto t = std::filesystem::last_write_time(path, ec);
  if (ec) return false;
  size = static_cast<uint64_t>(s);
  mtime = static_cast<int64_t>(t.time_since_epoch().count());
  return true;
}

// The descriptors every ReadOnlyFileSet in this plugin shares, by path. Weak:
// a file closes when no set holds it.
struct SharedFiles {
  std::mutex mutex;
  std::unordered_map<std::string, std::weak_ptr<const ReadOnlyFile>> files;
};

SharedFiles& shared_files() {
  static SharedFiles* files = new SharedFiles();  // never destroyed
  return *files;
}

// The shared descriptor for |path|: the live one if it still reads the file
// at |path|, otherwise a new one.
std::shared_ptr<const ReadOnlyFile> shared_file(const std::string& path,
                                                std::string& error) {
  SharedFiles& shared = shared_files();
  std::lock_guard<std::mutex> lock(shared.mutex);
  auto it = shared.files.find(path);
  if (it != shared.files.end()) {
    if (auto file = it->second.lock()) {
      if (file->still_current()) return file;
    }
  }
  auto file = ReadOnlyFile::open(path, error);
  if (!file) return nullptr;
  shared.files[path] = file;
  // Drop entries whose files have closed, so the map does not grow with
  // every path ever read.
  for (auto e = shared.files.begin(); e != shared.files.end();) {
    e = e->second.expired() ? shared.files.erase(e) : std::next(e);
  }
  return file;
}

}  // namespace

std::shared_ptr<const ReadOnlyFile> ReadOnlyFile::open(const std::string& path,
                                                       std::string& error) {
  std::shared_ptr<ReadOnlyFile> file(new ReadOnlyFile());
  file->path_ = path;
#if defined(_WIN32)
  HANDLE handle =
      CreateFileW(std::filesystem::path(path).wstring().c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "cannot open '" + path + "' (error " +
            std::to_string(GetLastError()) + ")";
    return nullptr;
  }
  file->handle_ = handle;
#else
  file->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file->fd_ < 0) {
    error = "cannot open '" + path + "': " + std::strerror(errno);
    return nullptr;
  }
  struct stat st{};
  if (::fstat(file->fd_, &st) == 0) {
    file->device_ = static_cast<uint64_t>(st.st_dev);
    file->inode_ = static_cast<uint64_t>(st.st_ino);
  }
#endif
  path_size_and_mtime(path, file->size_at_open_, file->mtime_at_open_);
  return file;
}

ReadOnlyFile::~ReadOnlyFile() {
#if defined(_WIN32)
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(static_cast<HANDLE>(handle_));
  }
#else
  if (fd_ >= 0) ::close(fd_);
#endif
}

int64_t ReadOnlyFile::read_at(uint64_t offset, void* data,
                              uint64_t length) const {
  auto* bytes = static_cast<char*>(data);
  uint64_t total = 0;
  while (total < length) {
#if defined(_WIN32)
    const DWORD part = static_cast<DWORD>(
        std::min<uint64_t>(length - total, uint64_t{1} << 30));
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), bytes, part, &got, &ov)) {
      if (GetLastError() == ERROR_HANDLE_EOF) break;
      return -1;
    }
    if (got == 0) break;  // end of file
#else
    const ssize_t got = ::pread(fd_, bytes, static_cast<size_t>(length - total),
                                static_cast<off_t>(offset));
    if (got < 0 && errno == EINTR) continue;
    if (got < 0) return -1;
    if (got == 0) break;  // end of file
#endif
    offset += static_cast<uint64_t>(got);
    bytes += got;
    total += static_cast<uint64_t>(got);
  }
  return static_cast<int64_t>(total);
}

bool ReadOnlyFile::still_current() const {
  uint64_t size = 0;
  int64_t mtime = 0;
  if (!path_size_and_mtime(path_, size, mtime)) return false;
  if (size != size_at_open_ || mtime != mtime_at_open_) return false;
#if !defined(_WIN32)
  struct stat st{};
  if (::stat(path_.c_str(), &st) != 0) return false;
  if (static_cast<uint64_t>(st.st_dev) != device_ ||
      static_cast<uint64_t>(st.st_ino) != inode_) {
    return false;
  }
#endif
  return true;
}

std::shared_ptr<const ReadOnlyFile> ReadOnlyFileSet::get(
    const std::string& path, std::string& error) const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = files_.find(path);
    if (it != files_.end()) return it->second;
  }
  // Another set may still hold a descriptor to a file since rewritten, so
  // shared_file() checks the one it finds.
  auto file = shared_file(path, error);
  if (!file) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  // Another thread may have opened it first; both hold the shared one.
  return files_.emplace(path, std::move(file)).first->second;
}

void ReadOnlyFileSet::refresh() const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = files_.begin(); it != files_.end();) {
    if (it->second->still_current()) {
      ++it;
      continue;
    }
    // Another set may hold the stale descriptor too; shared_file() replaces
    // the shared entry rather than reusing it.
    std::string error;
    auto file = shared_file(it->first, error);
    if (file) {
      it->second = std::move(file);
      ++it;
    } else {
      it = files_.erase(it);
    }
  }
}

}  // namespace orc
