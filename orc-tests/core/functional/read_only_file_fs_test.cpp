/*
 * File:        read_only_file_fs_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     Positional reads through ReadOnlyFile and the shared
 *              descriptors of ReadOnlyFileSet
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * Functional (not unit): reads and rewrites real files (AGENTS.md §4.2).
 */

#include <gtest/gtest.h>
#include <orc/support/read_only_file.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace orc {
namespace tests {
namespace {

namespace fs = std::filesystem;

class ReadOnlyFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() /
           (std::string("orc-read-only-file-") + info->name());
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  // A file of |size| bytes where byte i is i % 251.
  std::string write_file(const std::string& name, size_t size,
                         uint8_t salt = 0) const {
    const fs::path path = dir_ / name;
    std::vector<char> bytes(size);
    for (size_t i = 0; i < size; ++i) {
      bytes[i] = static_cast<char>((i % 251) ^ salt);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path.string();
  }

  fs::path dir_;
};

TEST_F(ReadOnlyFileTest, ReadsAtOffsetsAndShortAtTheEnd) {
  const std::string path = write_file("a.bin", 10000);
  std::string error;
  const auto file = ReadOnlyFile::open(path, error);
  ASSERT_NE(file, nullptr) << error;

  std::vector<uint8_t> buf(100);
  ASSERT_EQ(file->read_at(5000, buf.data(), buf.size()), 100);
  for (size_t i = 0; i < buf.size(); ++i) {
    EXPECT_EQ(buf[i], (5000 + i) % 251);
  }
  EXPECT_EQ(file->read_at(9950, buf.data(), buf.size()), 50);
  EXPECT_EQ(buf[0], 9950 % 251);
  EXPECT_EQ(file->read_at(20000, buf.data(), buf.size()), 0);
}

TEST_F(ReadOnlyFileTest, MissingFileReportsAnError) {
  std::string error;
  EXPECT_EQ(ReadOnlyFile::open((dir_ / "absent.bin").string(), error), nullptr);
  EXPECT_FALSE(error.empty());

  ReadOnlyFileSet set;
  error.clear();
  EXPECT_EQ(set.get((dir_ / "absent.bin").string(), error), nullptr);
  EXPECT_FALSE(error.empty());
}

TEST_F(ReadOnlyFileTest, ConcurrentReadersSeeTheirOwnRanges) {
  constexpr size_t kSize = 1 << 20;
  const std::string path = write_file("c.bin", kSize);
  ReadOnlyFileSet set;
  std::string error;
  const auto file = set.get(path, error);
  ASSERT_NE(file, nullptr) << error;

  std::vector<std::thread> threads;
  std::vector<int> mismatches(8, 0);
  for (size_t t = 0; t < mismatches.size(); ++t) {
    threads.emplace_back([&, t] {
      std::vector<uint8_t> buf(4096);
      for (size_t n = 0; n < 200; ++n) {
        const uint64_t offset = ((t * 7919 + n * 104729) % (kSize - 4096));
        if (file->read_at(offset, buf.data(), buf.size()) != 4096) {
          ++mismatches[t];
          continue;
        }
        for (size_t i = 0; i < buf.size(); ++i) {
          if (buf[i] != (offset + i) % 251) {
            ++mismatches[t];
            break;
          }
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  for (size_t t = 0; t < mismatches.size(); ++t) {
    EXPECT_EQ(mismatches[t], 0) << "thread " << t;
  }
}

TEST_F(ReadOnlyFileTest, SetsShareOneDescriptorPerFile) {
  const std::string path = write_file("s.bin", 1000);
  ReadOnlyFileSet first;
  ReadOnlyFileSet second;
  std::string error;
  const auto a = first.get(path, error);
  const auto b = second.get(path, error);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a, b);
  EXPECT_EQ(first.get(path, error), a);
}

TEST_F(ReadOnlyFileTest, RefreshReopensARewrittenFile) {
  const std::string path = write_file("r.bin", 1000);
  ReadOnlyFileSet set;
  std::string error;
  const auto before = set.get(path, error);
  ASSERT_NE(before, nullptr);
  EXPECT_TRUE(before->still_current());

  set.refresh();
  EXPECT_EQ(set.get(path, error), before) << "unchanged file reopened";

  write_file("r.bin", 2000, 0x5A);
  EXPECT_FALSE(before->still_current());
  set.refresh();
  const auto after = set.get(path, error);
  ASSERT_NE(after, nullptr);
  EXPECT_NE(after, before);
  uint8_t byte = 0;
  ASSERT_EQ(after->read_at(1500, &byte, 1), 1);
  EXPECT_EQ(byte, (1500 % 251) ^ 0x5A);

  // A set that never refreshed is not handed the old descriptor when it
  // first opens the path either.
  ReadOnlyFileSet fresh;
  EXPECT_EQ(fresh.get(path, error), after);
}

#if !defined(_WIN32)
TEST_F(ReadOnlyFileTest, RefreshReopensAFileReplacedBySameSizeRename) {
  const std::string path = write_file("p.bin", 1000);
  ReadOnlyFileSet set;
  std::string error;
  const auto before = set.get(path, error);
  ASSERT_NE(before, nullptr);

  const std::string replacement = write_file("p.new", 1000, 0x33);
  fs::rename(replacement, path);
  EXPECT_FALSE(before->still_current());
  set.refresh();
  const auto after = set.get(path, error);
  ASSERT_NE(after, nullptr);
  uint8_t byte = 0;
  ASSERT_EQ(after->read_at(10, &byte, 1), 1);
  EXPECT_EQ(byte, 10 ^ 0x33);
}
#endif

TEST_F(ReadOnlyFileTest, RefreshDropsARemovedFile) {
  const std::string path = write_file("d.bin", 1000);
  ReadOnlyFileSet set;
  std::string error;
  ASSERT_NE(set.get(path, error), nullptr);
  fs::remove(path);
  set.refresh();
  error.clear();
  EXPECT_EQ(set.get(path, error), nullptr);
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace tests
}  // namespace orc
