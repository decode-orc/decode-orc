/*
 * File:        view_plugin_loader_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     The view binary gate (view ABI, Qt, toolchain), the
 *              contract-id table, and which files and registry entries the
 *              view loader opens
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/core/include/view_plugin_loader.h"

#include <gtest/gtest.h>
#include <orc/abi/orc_plugin_abi.h>

#include <filesystem>
#include <string>

#include "../../../../orc/core/include/plugin_artifact_name.h"

namespace orc_unit_test {
namespace {

namespace fs = std::filesystem;

std::string fixture(const std::string& name) {
#if defined(_WIN32)
  const char* ext = ".dll";
#elif defined(__APPLE__)
  const char* ext = ".dylib";
#else
  const char* ext = ".so";
#endif
  return (fs::path(ORC_TEST_FIXTURE_VIEW_BUILD_DIR) /
          ("orc-view_test_" + name + ext))
      .string();
}

orc::ViewHostVersion host() {
  orc::ViewHostVersion version;
  version.qt_major = 6;
  version.qt_minor = 5;
  version.toolchain_tag = ORC_SDK_TOOLCHAIN_TAG;
  return version;
}

orc::view::ViewPluginDescriptor descriptor(uint32_t qt_major,
                                           uint32_t qt_minor) {
  return {"test.view", "1.0.0",  orc::view::kViewPluginAbiVersion,
          qt_major,    qt_minor, ORC_SDK_TOOLCHAIN_TAG};
}

bool has_diagnostic(const std::string& path, const std::string& needle) {
  for (const auto& d : orc::ViewPluginRegistry::instance().diagnostics()) {
    if (d.path == path && d.message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

class ViewPluginLoaderTest : public ::testing::Test {
 protected:
  void SetUp() override { orc::ViewPluginRegistry::instance().reset(); }
  void TearDown() override { orc::ViewPluginRegistry::instance().reset(); }
};

}  // namespace

// --- The gate ---------------------------------------------------------------

TEST(ViewDescriptorGateTest, AcceptsAMatchingView) {
  EXPECT_EQ(orc::check_view_descriptor(descriptor(6, 5), host()), "");
}

// Qt keeps binary compatibility within a major version, so a view built
// against an older minor runs on the host's newer Qt.
TEST(ViewDescriptorGateTest, AcceptsAViewBuiltForAnOlderQtMinor) {
  EXPECT_EQ(orc::check_view_descriptor(descriptor(6, 2), host()), "");
}

TEST(ViewDescriptorGateTest, RefusesAViewBuiltForANewerQtMinor) {
  const std::string why = orc::check_view_descriptor(descriptor(6, 8), host());
  EXPECT_NE(why.find("Qt 6.8"), std::string::npos) << why;
  EXPECT_NE(why.find("Qt 6.5"), std::string::npos) << why;
}

TEST(ViewDescriptorGateTest, RefusesAnotherQtMajor) {
  EXPECT_NE(orc::check_view_descriptor(descriptor(5, 15), host()), "");
  EXPECT_NE(orc::check_view_descriptor(descriptor(7, 0), host()), "");
}

TEST(ViewDescriptorGateTest, RefusesAnotherViewAbi) {
  auto d = descriptor(6, 5);
  d.view_abi_version = orc::view::kViewPluginAbiVersion + 1;
  const std::string why = orc::check_view_descriptor(d, host());
  EXPECT_NE(why.find("view ABI"), std::string::npos) << why;
}

TEST(ViewDescriptorGateTest, RefusesAnotherToolchain) {
  auto d = descriptor(6, 5);
  d.toolchain_tag = "not-this-compiler/not-this-stdlib";
  const std::string why = orc::check_view_descriptor(d, host());
  EXPECT_NE(why.find("toolchain"), std::string::npos) << why;
}

TEST(ViewDescriptorGateTest, RefusesAViewWithNoPluginId) {
  auto d = descriptor(6, 5);
  d.plugin_id = "";
  EXPECT_NE(orc::check_view_descriptor(d, host()), "");
  d.plugin_id = nullptr;
  EXPECT_NE(orc::check_view_descriptor(d, host()), "");
}

// --- The file name ----------------------------------------------------------

TEST(ViewBinaryFileNameTest, RecognisesViewBinariesByTheirPrefix) {
  EXPECT_TRUE(
      orc::is_view_binary_file_name("orc-view_teletext_linux_view1.so"));
  EXPECT_TRUE(orc::is_view_binary_file_name("liborc-view_teletext.so"));
  EXPECT_TRUE(orc::is_view_binary_file_name("/opt/plugins/orc-view_demo.dll"));
  EXPECT_FALSE(orc::is_view_binary_file_name("orc-plugin_teletext_linux.so"));
  EXPECT_FALSE(
      orc::is_view_binary_file_name("/opt/orc-view_x/orc-plugin_a.so"));
  EXPECT_FALSE(orc::is_view_binary_file_name("orc-view_"));
}

// --- Loading ----------------------------------------------------------------

TEST_F(ViewPluginLoaderTest, LoadsACompatibleViewAndRegistersItsContracts) {
  auto& registry = orc::ViewPluginRegistry::instance();
  ASSERT_TRUE(registry.load_binary(fixture("compatible"), host()));

  EXPECT_NE(registry.find_factory("test.view.alpha.v1"), nullptr);
  EXPECT_NE(registry.find_factory("test.view.beta.v1"), nullptr);
  EXPECT_EQ(registry.find_factory("test.view.unknown.v1"), nullptr);
  ASSERT_EQ(registry.loaded().size(), 1u);
  EXPECT_EQ(registry.loaded()[0].plugin_id, "test.view.one");
  EXPECT_EQ(registry.loaded()[0].contract_ids.size(), 2u);
}

TEST_F(ViewPluginLoaderTest, RefusesASecondViewForTheSamePlugin) {
  auto& registry = orc::ViewPluginRegistry::instance();
  ASSERT_TRUE(registry.load_binary(fixture("compatible"), host()));
  EXPECT_FALSE(registry.load_binary(fixture("same_id"), host()));

  EXPECT_EQ(registry.find_factory("test.view.gamma.v1"), nullptr);
  EXPECT_TRUE(has_diagnostic(fixture("same_id"), "already loaded"));
}

// The first view to claim a contract keeps it; the clashing binary's other
// views still register.
TEST_F(ViewPluginLoaderTest, FirstViewForAContractKeepsIt) {
  auto& registry = orc::ViewPluginRegistry::instance();
  ASSERT_TRUE(registry.load_binary(fixture("compatible"), host()));
  const auto alpha = registry.find_factory("test.view.alpha.v1");
  ASSERT_TRUE(registry.load_binary(fixture("clashing"), host()));

  EXPECT_EQ(registry.find_factory("test.view.alpha.v1"), alpha);
  EXPECT_NE(registry.find_factory("test.view.epsilon.v1"), nullptr);
  EXPECT_TRUE(has_diagnostic(fixture("clashing"), "test.view.alpha.v1"));
}

TEST_F(ViewPluginLoaderTest, RefusesAViewForAnotherViewAbi) {
  auto& registry = orc::ViewPluginRegistry::instance();
  EXPECT_FALSE(registry.load_binary(fixture("bad_abi"), host()));
  EXPECT_EQ(registry.find_factory("test.view.zeta.v1"), nullptr);
  EXPECT_TRUE(has_diagnostic(fixture("bad_abi"), "view ABI"));
}

TEST_F(ViewPluginLoaderTest, RefusesAViewBuiltForANewerQt) {
  auto& registry = orc::ViewPluginRegistry::instance();
  EXPECT_FALSE(registry.load_binary(fixture("newer_qt"), host()));
  EXPECT_EQ(registry.find_factory("test.view.theta.v1"), nullptr);
  EXPECT_TRUE(has_diagnostic(fixture("newer_qt"), "Qt 6.99"));
}

TEST_F(ViewPluginLoaderTest, ReportsAFileThatIsNotAViewBinary) {
  auto& registry = orc::ViewPluginRegistry::instance();
  const std::string missing = fixture("does_not_exist");
  EXPECT_FALSE(registry.load_binary(missing, host()));
  EXPECT_TRUE(has_diagnostic(missing, "Failed to open"));
}

// A folder is searched for view binaries by name: a library that is not
// named orc-view_* is never opened, even when it is in fact a view.
TEST_F(ViewPluginLoaderTest, FolderScanOpensOnlyFilesNamedAsViews) {
  const fs::path folder =
      fs::temp_directory_path() / "orc-view-loader-test-folder";
  fs::remove_all(folder);
  fs::create_directories(folder);
  const fs::path source = fixture("compatible");
  fs::copy_file(source, folder / source.filename());
  // The clashing fixture under a stage binary's name: if the scan opened it,
  // test.view.epsilon.v1 would register.
  fs::copy_file(fixture("clashing"),
                folder / ("orc-plugin_not_a_view" +
                          fs::path(fixture("clashing")).extension().string()));

  auto& registry = orc::ViewPluginRegistry::instance();
  registry.load(host(), {}, {folder.string()});

  EXPECT_NE(registry.find_factory("test.view.alpha.v1"), nullptr);
  EXPECT_EQ(registry.find_factory("test.view.epsilon.v1"), nullptr);
  EXPECT_EQ(registry.loaded().size(), 1u);
  fs::remove_all(folder);
}

// Trust and Enabled cover the whole plugin: a view loads only when its
// registry entry would let the stage binary load.
TEST_F(ViewPluginLoaderTest, LoadsTheViewOfATrustedEnabledEntryOnly) {
  orc::StagePluginRegistryEntry untrusted;
  untrusted.plugin_id = "test.view.one";
  untrusted.view_path = fixture("compatible");
  untrusted.trust_state = "untrusted";

  orc::StagePluginRegistryEntry disabled = untrusted;
  disabled.trust_state = "trusted";
  disabled.enabled = false;

  auto& registry = orc::ViewPluginRegistry::instance();
  registry.load(host(), {untrusted, disabled}, {});
  EXPECT_TRUE(registry.loaded().empty());

  orc::StagePluginRegistryEntry trusted = disabled;
  trusted.enabled = true;
  registry.load(host(), {trusted}, {});
  ASSERT_EQ(registry.loaded().size(), 1u);
  EXPECT_EQ(registry.loaded()[0].path, fixture("compatible"));
}

}  // namespace orc_unit_test
