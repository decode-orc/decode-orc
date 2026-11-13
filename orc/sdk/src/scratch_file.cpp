/*
 * File:        scratch_file.cpp
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Anonymous on-disk scratch files for stages whose working data
 *              grows with source length
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <orc/support/scratch_file.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace orc {

namespace {

std::string env_or_empty(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

std::string unique_name_part() {
  static std::atomic<uint64_t> counter{0};
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
  const auto pid = static_cast<uint64_t>(GetCurrentProcessId());
#else
  const auto pid = static_cast<uint64_t>(getpid());
#endif
  return std::to_string(pid) + "-" + std::to_string(counter.fetch_add(1)) +
         "-" + std::to_string(ticks);
}

}  // namespace

std::filesystem::path scratch_directory(const std::string& kind) {
  namespace fs = std::filesystem;
  const std::string override_dir = env_or_empty("ORC_CACHE_DIR");
  if (!override_dir.empty()) return fs::path(override_dir) / kind;
#if defined(_WIN32)
  const std::string local = env_or_empty("LOCALAPPDATA");
  if (!local.empty()) return fs::path(local) / "decode-orc" / "cache" / kind;
#elif defined(__APPLE__)
  const std::string home = env_or_empty("HOME");
  if (!home.empty()) {
    return fs::path(home) / "Library" / "Caches" / "decode-orc" / kind;
  }
#else
  const std::string xdg = env_or_empty("XDG_CACHE_HOME");
  if (!xdg.empty()) return fs::path(xdg) / "decode-orc" / kind;
  const std::string home = env_or_empty("HOME");
  if (!home.empty()) return fs::path(home) / ".cache" / "decode-orc" / kind;
#endif
  std::error_code ec;
  return fs::temp_directory_path(ec) / ("decode-orc-" + kind);
}

std::unique_ptr<ScratchFile> ScratchFile::create(
    const std::filesystem::path& directory, const std::string& prefix,
    const std::string& suffix, std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    error = "cannot create scratch directory '" + directory.string() +
            "': " + ec.message();
    return nullptr;
  }
  const std::filesystem::path path =
      directory / (prefix + unique_name_part() + suffix);

  std::unique_ptr<ScratchFile> file(new ScratchFile());
#if defined(_WIN32)
  // FILE_FLAG_DELETE_ON_CLOSE removes the file when the last handle closes,
  // including when the process dies.
  HANDLE handle = CreateFileW(
      path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "cannot create scratch file '" + path.string() + "' (error " +
            std::to_string(GetLastError()) + ")";
    return nullptr;
  }
  file->handle_ = handle;
#else
  file->fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (file->fd_ < 0) {
    error = "cannot create scratch file '" + path.string() +
            "': " + std::strerror(errno);
    return nullptr;
  }
  // Unlinked while open: the space is released when the descriptor closes,
  // however the process ends.
  ::unlink(path.c_str());
#endif
  return file;
}

ScratchFile::~ScratchFile() {
#if defined(_WIN32)
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(static_cast<HANDLE>(handle_));
  }
#else
  if (fd_ >= 0) ::close(fd_);
#endif
}

bool ScratchFile::write_at(uint64_t offset, const void* data,
                           uint64_t length) const {
  const auto* bytes = static_cast<const char*>(data);
  while (length > 0) {
#if defined(_WIN32)
    const DWORD part =
        static_cast<DWORD>(std::min<uint64_t>(length, uint64_t{1} << 30));
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), bytes, part, &written, &ov) ||
        written == 0) {
      return false;
    }
#else
    const ssize_t written = ::pwrite(fd_, bytes, static_cast<size_t>(length),
                                     static_cast<off_t>(offset));
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
#endif
    offset += static_cast<uint64_t>(written);
    bytes += written;
    length -= static_cast<uint64_t>(written);
  }
  return true;
}

bool ScratchFile::read_at(uint64_t offset, void* data, uint64_t length) const {
  auto* bytes = static_cast<char*>(data);
  while (length > 0) {
#if defined(_WIN32)
    const DWORD part =
        static_cast<DWORD>(std::min<uint64_t>(length, uint64_t{1} << 30));
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), bytes, part, &got, &ov) ||
        got == 0) {
      return false;
    }
#else
    const ssize_t got = ::pread(fd_, bytes, static_cast<size_t>(length),
                                static_cast<off_t>(offset));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
#endif
    offset += static_cast<uint64_t>(got);
    bytes += got;
    length -= static_cast<uint64_t>(got);
  }
  return true;
}

}  // namespace orc
