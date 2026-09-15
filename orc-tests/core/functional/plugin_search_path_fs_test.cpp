/*
 * File:        plugin_search_path_fs_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     Executable-relative stage-plugin directory resolution
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * Functional (not unit): plugin_dir_for_executable resolves the executable
 * path through symlinks, so exercising it needs real directories and real
 * links on disk, which unit tests may not create (AGENTS.md §4.2).
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "stage_registry.h"

namespace orc {
namespace {

class PluginSearchPathFsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    root_ = std::filesystem::temp_directory_path() /
            (std::string("orc-plugin-search-") + info->name());
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
    // The temporary directory may itself sit behind a symlink (/tmp is a link
    // to /private/tmp on macOS), which the code under test resolves. Compare
    // against the resolved root so the expectations below are not the very
    // thing being tested.
    root_ = std::filesystem::canonical(root_);
  }

  void TearDown() override {
    std::error_code error_code;
    std::filesystem::remove_all(root_, error_code);
  }

  // Create an empty file at @p path, making its parent directories first.
  std::filesystem::path make_file(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    return path;
  }

  // The install layout plugin_dir_for_executable expects on this platform,
  // expressed relative to the directory holding the executable.
  std::filesystem::path expected_plugin_dir(
      const std::filesystem::path& executable_dir) {
#if defined(_WIN32)
    return (executable_dir / "orc-stage-plugins").lexically_normal();
#elif defined(__APPLE__)
    return (executable_dir / ".." / "PlugIns" / "orc-stage-plugins")
        .lexically_normal();
#else
    return (executable_dir / ".." / "lib" / "orc-stage-plugins")
        .lexically_normal();
#endif
  }

  std::filesystem::path root_;
};

TEST_F(PluginSearchPathFsTest, ReturnsEmptyPath_WhenExecutablePathIsEmpty) {
  EXPECT_TRUE(plugin_dir_for_executable({}).empty());
}

TEST_F(PluginSearchPathFsTest,
       ResolvesPluginDirBesideExecutable_WhenInvokedByRealPath) {
  const auto install_dir = root_ / "install";
  const auto executable = make_file(install_dir / "bin" / "orc-cli");
  const auto plugin_dir = expected_plugin_dir(executable.parent_path());
  std::filesystem::create_directories(plugin_dir);

  EXPECT_EQ(plugin_dir_for_executable(executable), plugin_dir);
}

// The regression behind issue #320. Only Linux hands the caller an
// already-resolved executable path (/proc/self/exe); on macOS and Windows the
// path can be the one the process was invoked through. Reached through a link
// outside the install tree - the bin/ links in the Nix package, or the
// /usr/local/bin/orc-cli link the DMG instructions suggest - an unresolved
// path derives a plugin directory that does not exist, and no stages load.
TEST_F(PluginSearchPathFsTest,
       ResolvesSamePluginDir_WhenInvokedThroughSymlinkOutsideInstallTree) {
  const auto install_dir = root_ / "install";
  const auto executable = make_file(install_dir / "bin" / "orc-cli");
  const auto plugin_dir = expected_plugin_dir(executable.parent_path());
  std::filesystem::create_directories(plugin_dir);

  const auto link_dir = root_ / "elsewhere";
  std::filesystem::create_directories(link_dir);
  const auto link = link_dir / "orc-cli";

  std::error_code error_code;
  std::filesystem::create_symlink(executable, link, error_code);
  if (error_code) {
    GTEST_SKIP() << "symlinks unavailable on this filesystem: "
                 << error_code.message();
  }

  EXPECT_EQ(plugin_dir_for_executable(link), plugin_dir);
}

// A path that cannot be canonicalised (nothing at the other end) must still
// yield the layout-derived candidate rather than an empty path: the caller
// checks the candidate for existence and falls back to the other search paths.
TEST_F(PluginSearchPathFsTest,
       FallsBackToUnresolvedPath_WhenExecutableDoesNotExist) {
  const auto executable = root_ / "missing" / "bin" / "orc-cli";

  EXPECT_EQ(plugin_dir_for_executable(executable),
            expected_plugin_dir(executable.parent_path()));
}

}  // namespace
}  // namespace orc
