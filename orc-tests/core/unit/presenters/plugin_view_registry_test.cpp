/*
 * File:        plugin_view_registry_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     A plugin's stage and view binaries are one registry entry:
 *              recorded together, and removed together
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../../../../orc/core/include/stage_plugin_registry.h"
#include "../../../../orc/presenters/include/project_presenter.h"

namespace orc_unit_test {
namespace {

namespace fs = std::filesystem;

void set_env(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

// Points the registry at a scratch configuration folder for each test.
class PluginViewRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    home_ = fs::temp_directory_path() / "orc-plugin-view-registry-test";
    fs::remove_all(home_);
    fs::create_directories(home_);
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* appdata = std::getenv("APPDATA");
    saved_xdg_ = xdg ? xdg : "";
    saved_appdata_ = appdata ? appdata : "";
    set_env("XDG_CONFIG_HOME", home_.string());
    set_env("APPDATA", home_.string());
  }
  void TearDown() override {
    set_env("XDG_CONFIG_HOME", saved_xdg_);
    set_env("APPDATA", saved_appdata_);
    fs::remove_all(home_);
  }

  static orc::presenters::PluginRegistryEntryInfo entry_with_view() {
    orc::presenters::PluginRegistryEntryInfo info;
    info.plugin_id = "org.example.stage.demo";
    info.plugin_version = "1.0.0";
    info.path = "/plugins/orc-plugin_demo_linux_abi20.so";
    info.artifact_source = "local_path";
    info.trust_state = "trusted";
    info.view_path = "/plugins/orc-view_demo_linux_view1.so";
    info.view_release_asset_name = "orc-view_demo_linux_view1.so";
    info.view_sha256 = std::string(64, 'b');
    return info;
  }

  fs::path home_;
  std::string saved_xdg_;
  std::string saved_appdata_;
};

}  // namespace

TEST_F(PluginViewRegistryTest, AddingAPluginRecordsItsViewInTheSameEntry) {
  const auto added = orc::presenters::ProjectPresenter::addPluginRegistryEntry(
      entry_with_view());
  ASSERT_TRUE(added.success) << added.error_message;

  const auto registry = orc::StagePluginRegistry::load_default();
  ASSERT_EQ(registry.entries.size(), 1u);
  const auto& entry = registry.entries[0];
  EXPECT_EQ(entry.path, "/plugins/orc-plugin_demo_linux_abi20.so");
  EXPECT_TRUE(entry.has_view());
  EXPECT_EQ(entry.view_path, "/plugins/orc-view_demo_linux_view1.so");
  EXPECT_EQ(entry.view_sha256, std::string(64, 'b'));
}

TEST_F(PluginViewRegistryTest, RemovingAPluginRemovesItsViewToo) {
  ASSERT_TRUE(orc::presenters::ProjectPresenter::addPluginRegistryEntry(
                  entry_with_view())
                  .success);
  const auto removed =
      orc::presenters::ProjectPresenter::removePluginRegistryEntry(
          "org.example.stage.demo");
  ASSERT_TRUE(removed.success) << removed.error_message;

  const auto registry = orc::StagePluginRegistry::load_default();
  EXPECT_TRUE(registry.entries.empty());
  std::string text;
  {
    std::ifstream in(registry.registry_path);
    text.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
  }
  EXPECT_EQ(text.find("orc-view_demo"), std::string::npos) << text;
}

// Trust and Enabled are the entry's, so they switch both halves.
TEST_F(PluginViewRegistryTest, DisablingAPluginDisablesItsView) {
  ASSERT_TRUE(orc::presenters::ProjectPresenter::addPluginRegistryEntry(
                  entry_with_view())
                  .success);
  ASSERT_TRUE(orc::presenters::ProjectPresenter::setPluginRegistryEntryEnabled(
                  "org.example.stage.demo", false)
                  .success);
  const auto registry = orc::StagePluginRegistry::load_default();
  ASSERT_EQ(registry.entries.size(), 1u);
  EXPECT_FALSE(registry.entries[0].enabled);
  EXPECT_TRUE(registry.entries[0].has_view());
}

}  // namespace orc_unit_test
