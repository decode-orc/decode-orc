/*
 * File:        plugin_remote_loader_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Unit tests for remote plugin binary download and caching
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../orc/core/include/plugin_remote_loader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace orc_unit_test {

// Note: Full end-to-end download testing requires libcurl and network access.
// These unit tests focus on validation and cache path logic.
// Integration tests with actual downloads to GitHub releases will be added to
// the test suite.

TEST(PluginRemoteLoaderTest, Get_CachePathFormsCorrectPlatformSubdirectory) {
  const auto path = orc::PluginRemoteLoader::get_cache_path(
      "orc-plugin_skeleton_passthrough_linux.so", "linux");
  EXPECT_TRUE(path.find("plugin-cache") != std::string::npos);
  EXPECT_TRUE(path.find("linux") != std::string::npos);
  EXPECT_TRUE(path.find("orc-plugin_skeleton_passthrough_linux.so") !=
              std::string::npos);
}

TEST(PluginRemoteLoaderTest, Get_CachePathFormsCorrectWindowsPath) {
  const auto path = orc::PluginRemoteLoader::get_cache_path(
      "orc-plugin_skeleton_passthrough_windows.dll", "windows");
  EXPECT_TRUE(path.find("plugin-cache") != std::string::npos);
  EXPECT_TRUE(path.find("windows") != std::string::npos);
  EXPECT_TRUE(path.find("orc-plugin_skeleton_passthrough_windows.dll") !=
              std::string::npos);
}

TEST(PluginRemoteLoaderTest, Download_RejectsInvalidAssetNames) {
  std::vector<std::string> warnings;

  // Missing stage and platform (no underscore separators)
  auto result = orc::PluginRemoteLoader::download_release_asset(
      "https://api.github.com/repos/example/repo/releases/download/v1.0.0/"
      "invalid.so",
      "invalid.so", "linux", &warnings);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.error_message.find("Invalid asset name format") !=
              std::string::npos);

  // Missing extension
  result = orc::PluginRemoteLoader::download_release_asset(
      "https://api.github.com/repos/example/repo/releases/download/v1.0.0/"
      "orc-plugin_stage_linux",
      "orc-plugin_stage_linux", "linux", &warnings);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.error_message.find("Invalid asset name format") !=
              std::string::npos);

  // Wrong extension
  result = orc::PluginRemoteLoader::download_release_asset(
      "https://api.github.com/repos/example/repo/releases/download/v1.0.0/"
      "orc-plugin_stage_linux.txt",
      "orc-plugin_stage_linux.txt", "linux", &warnings);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.error_message.find("Invalid asset name format") !=
              std::string::npos);

  // Valid format should pass validation (may fail on network, but not format
  // validation)
  result = orc::PluginRemoteLoader::download_release_asset(
      "https://invalid.example.test/releases/download/v1.0.0/"
      "orc-plugin_stage_linux.so",
      "orc-plugin_stage_linux.so", "linux", &warnings);
  // Format validation should pass (curl connection error is OK)
  EXPECT_TRUE(result.error_message.find("Invalid asset name format") ==
              std::string::npos);
}

// A plugin's view binary is fetched by the same downloader, so its release
// asset name must get past the name check.
TEST(PluginRemoteLoaderTest, Download_AcceptsViewAssetNames) {
  std::vector<std::string> warnings;
  const auto result = orc::PluginRemoteLoader::download_release_asset(
      "https://invalid.example.test/releases/download/v1.0.0/"
      "orc-view_teletext_linux_view1.so",
      "orc-view_teletext_linux_view1.so", "linux", &warnings);
  EXPECT_TRUE(result.error_message.find("Invalid asset name format") ==
              std::string::npos);
}

TEST(PluginRemoteLoaderTest, Download_RejectsViewAssetNamesWithDirectories) {
  std::vector<std::string> warnings;
  const auto result = orc::PluginRemoteLoader::download_release_asset(
      "https://invalid.example.test/releases/download/v1.0.0/x",
      "../orc-view_teletext_linux_view1.so", "linux", &warnings);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.error_message.find("Invalid asset name format") !=
              std::string::npos);
}

// Checksum verification decision logic operates on in-memory data; the
// file/download plumbing that feeds it is covered by functional tests.

TEST(PluginRemoteLoaderTest, Verify_ChecksumMatchesKnownDigest) {
  // sha256("abc")
  const auto status = orc::PluginRemoteLoader::verify_checksum_hex(
      "abc",
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(status, orc::PluginRemoteLoader::ChecksumStatus::Match);
}

TEST(PluginRemoteLoaderTest, Verify_ChecksumIsCaseInsensitive) {
  const auto status = orc::PluginRemoteLoader::verify_checksum_hex(
      "abc",
      "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
  EXPECT_EQ(status, orc::PluginRemoteLoader::ChecksumStatus::Match);
}

TEST(PluginRemoteLoaderTest, Verify_ChecksumReportsMismatch) {
  const auto status = orc::PluginRemoteLoader::verify_checksum_hex(
      "tampered payload",
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(status, orc::PluginRemoteLoader::ChecksumStatus::Mismatch);
}

TEST(PluginRemoteLoaderTest, Verify_ChecksumAbsentReportsNotProvided) {
  const auto status =
      orc::PluginRemoteLoader::verify_checksum_hex("anything", "");
  EXPECT_EQ(status, orc::PluginRemoteLoader::ChecksumStatus::NotProvided);
}

TEST(PluginRemoteLoaderTest, Verify_MalformedExpectedDigestNeverMatches) {
  // A registry typo (wrong length / non-hex) must fail closed.
  const auto status =
      orc::PluginRemoteLoader::verify_checksum_hex("abc", "not-a-digest");
  EXPECT_EQ(status, orc::PluginRemoteLoader::ChecksumStatus::Mismatch);
}

}  // namespace orc_unit_test
