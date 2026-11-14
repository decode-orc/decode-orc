/*
 * File:        plugin_details_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Unit tests for the shared plugin description both front ends use
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plugin_details.h"

#include <gtest/gtest.h>
#include <plugin_ux_strings.h>

#include <algorithm>
#include <string>
#include <vector>

namespace orc::presenters {
namespace {

PluginRegistryEntryInfo makeInstalledEntry() {
  PluginRegistryEntryInfo entry;
  entry.selector = "com.example.plugin";
  entry.plugin_id = "com.example.plugin";
  entry.plugin_version = "1.0.5";
  entry.path = "/plugins/example.so";
  entry.license_spdx = "MIT";
  entry.source_label = "https://example.invalid/example";
  entry.path_exists = true;
  entry.is_loaded = true;
  entry.required_host_abi = 12;
  entry.host_abi_version = 12;
  entry.load_state = PluginLoadState::WillLoad;
  entry.load_state_detail = plugin_ux::kLoadStateWillLoadDetail;
  return entry;
}

PluginIndexEntryInfo makeIndexEntry() {
  PluginIndexEntryInfo entry;
  entry.id = "com.example.plugin";
  entry.display_name = "Example Plugin";
  entry.description = "Does example things";
  entry.version = "1.0.6";
  entry.maintainer = "Example Maintainer";
  entry.license_spdx = "MIT";
  entry.source_repo_url = "https://example.invalid/example";
  entry.tags = {"video", "filter"};
  entry.has_compatible_build = true;
  return entry;
}

std::vector<std::string> labelsOf(
    const std::vector<PluginDetailField>& fields) {
  std::vector<std::string> labels;
  labels.reserve(fields.size());
  for (const auto& field : fields) {
    labels.push_back(field.label);
  }
  return labels;
}

std::string valueOf(const std::vector<PluginDetailField>& fields,
                    const std::string& label) {
  const auto it = std::find_if(
      fields.begin(), fields.end(),
      [&label](const PluginDetailField& f) { return f.label == label; });
  return it == fields.end() ? std::string() : it->value;
}

TEST(PluginDetailsTest, InstalledEntryFieldsAreInTheCanonicalOrder) {
  const auto entry = makeInstalledEntry();
  const auto fields = makePluginDetails(&entry, nullptr, nullptr);

  EXPECT_EQ(labelsOf(fields),
            (std::vector<std::string>{
                plugin_ux::kFieldSelector, plugin_ux::kFieldVersion,
                plugin_ux::kFieldLicense, plugin_ux::kFieldSource,
                plugin_ux::kFieldPath, plugin_ux::kFieldExists,
                plugin_ux::kFieldLoaded, plugin_ux::kFieldHostAbi,
                plugin_ux::kFieldStatus}));
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldSelector), "com.example.plugin");
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldHostAbi), "requires 12 (host 12)");
  EXPECT_NE(valueOf(fields, plugin_ux::kFieldStatus)
                .find(plugin_ux::kLoadStateWillLoadLabel),
            std::string::npos);
}

// A plugin without a view says nothing about one. With a view, the line says
// whether it loaded, and orc-cli, which never loads views, says why not.
TEST(PluginDetailsTest, ViewLineSaysWhetherTheViewLoaded) {
  auto entry = makeInstalledEntry();
  EXPECT_EQ(valueOf(makePluginDetails(&entry, nullptr, nullptr),
                    plugin_ux::kFieldView),
            "");

  entry.has_view = true;
  EXPECT_EQ(valueOf(makePluginDetails(&entry, nullptr, nullptr),
                    plugin_ux::kFieldView),
            plugin_ux::kViewGuiOnly);

  entry.views_attempted = true;
  EXPECT_EQ(valueOf(makePluginDetails(&entry, nullptr, nullptr),
                    plugin_ux::kFieldView),
            plugin_ux::kViewNotLoaded);

  entry.view_loaded = true;
  EXPECT_EQ(valueOf(makePluginDetails(&entry, nullptr, nullptr),
                    plugin_ux::kFieldView),
            plugin_ux::kViewLoaded);
}

// The selector leads and is never duplicated by an identical id; an id that
// differs from the selector is shown, so the pair is never ambiguous.
TEST(PluginDetailsTest, IdIsShownOnlyWhenItDiffersFromTheSelector) {
  auto entry = makeInstalledEntry();
  auto fields = makePluginDetails(&entry, nullptr, nullptr);
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldId), "");

  entry.selector = "path:/plugins/example.so";
  fields = makePluginDetails(&entry, nullptr, nullptr);
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldSelector),
            "path:/plugins/example.so");
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldId), "com.example.plugin");
}

TEST(PluginDetailsTest, EmptyFieldsAreOmittedRatherThanBlank) {
  PluginRegistryEntryInfo entry;
  entry.selector = "path:/plugins/unnamed.so";
  entry.path = "/plugins/unnamed.so";
  entry.load_state = PluginLoadState::NotTrusted;

  const auto fields = makePluginDetails(&entry, nullptr, nullptr);
  for (const auto& field : fields) {
    EXPECT_FALSE(field.value.empty()) << field.label;
  }
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldVersion), "");
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldHostAbi), "");
}

TEST(PluginDetailsTest, IndexEntryDescribesCompatibilityAndInstallState) {
  const auto indexed = makeIndexEntry();
  const auto fields = makePluginDetails(nullptr, &indexed, nullptr);

  EXPECT_EQ(labelsOf(fields),
            (std::vector<std::string>{
                plugin_ux::kFieldSelector, plugin_ux::kFieldName,
                plugin_ux::kFieldDescription, plugin_ux::kFieldLatest,
                plugin_ux::kFieldLicense, plugin_ux::kFieldMaintainer,
                plugin_ux::kFieldSource, plugin_ux::kFieldTags,
                plugin_ux::kFieldCompatible, plugin_ux::kFieldInstalled}));
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldTags), "video, filter");
  EXPECT_NE(valueOf(fields, plugin_ux::kFieldCompatible)
                .find(plugin_ux::kIndexCompatibleDeclared),
            std::string::npos);
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldInstalled), "no");
}

// An installed copy that is behind the latest release says so, in one wording
// both front ends render.
TEST(PluginDetailsTest, InstalledIndexEntryReportsTheVersionRelationship) {
  const auto installed = makeInstalledEntry();
  auto indexed = makeIndexEntry();
  indexed.already_installed = true;

  auto fields = makePluginDetails(&installed, &indexed, nullptr);
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldVersion), "1.0.5");
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldLatest), "1.0.6");
  EXPECT_NE(valueOf(fields, plugin_ux::kFieldInstalled).find("1.0.5"),
            std::string::npos);
  EXPECT_NE(valueOf(fields, plugin_ux::kFieldInstalled).find("1.0.6"),
            std::string::npos);

  indexed.version = "1.0.5";
  fields = makePluginDetails(&installed, &indexed, nullptr);
  EXPECT_NE(valueOf(fields, plugin_ux::kFieldInstalled).find("up to date"),
            std::string::npos);
}

TEST(PluginDetailsTest, UnreachableReleaseIsNotReportedAsALatestVersion) {
  auto indexed = makeIndexEntry();
  indexed.has_compatible_build = false;
  indexed.release_unreachable = true;
  indexed.compatibility_message = "the release could not be fetched";

  const auto fields = makePluginDetails(nullptr, &indexed, nullptr);
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldLatest), "");
  EXPECT_NE(valueOf(fields, plugin_ux::kFieldCompatible)
                .find(plugin_ux::kIndexLabelUnreachable),
            std::string::npos);
}

// A checked update is reported in the words the GUI's Update column uses.
TEST(PluginDetailsTest, UpdateStatusIsIncludedOnlyWhenACheckWasMade) {
  const auto entry = makeInstalledEntry();
  EXPECT_EQ(valueOf(makePluginDetails(&entry, nullptr, nullptr),
                    plugin_ux::kFieldUpdate),
            "");

  PluginUpdateStatusInfo status;
  status.plugin_id = entry.plugin_id;
  status.installed_version = "1.0.5";
  status.latest_version = "1.0.6";
  status.status = PluginUpdateStatus::UpdateAvailable;

  const auto fields = makePluginDetails(&entry, nullptr, &status);
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldUpdate),
            plugin_ux::updateAvailableLabel("1.0.6"));
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldLatest), "1.0.6");
}

TEST(PluginDetailsTest, UpdateStatusLabelsCoverEveryOutcome) {
  PluginUpdateStatusInfo status;
  status.status = PluginUpdateStatus::UpToDate;
  EXPECT_EQ(pluginUpdateStatusLabel(status), plugin_ux::kUpdateStatusUpToDate);

  status.status = PluginUpdateStatus::Unreachable;
  status.message = "host unreachable";
  EXPECT_NE(pluginUpdateStatusLabel(status).find("host unreachable"),
            std::string::npos);

  status.status = PluginUpdateStatus::Unknown;
  status.latest_version = "2.0.0";
  EXPECT_EQ(pluginUpdateStatusLabel(status),
            plugin_ux::updateLatestOnlyLabel("2.0.0"));

  status.status = PluginUpdateStatus::NotApplicable;
  EXPECT_EQ(pluginUpdateStatusLabel(status), plugin_ux::kUpdateStatusNone);
}

TEST(PluginDetailsTest, UpdateStatusIdsAreStableIdentifiers) {
  // Machine-readable output carries these instead of the labels above: they
  // hold no version number and no punctuation, so a script matches on the
  // outcome and reads the version from its own field.
  EXPECT_STREQ(pluginUpdateStatusId(PluginUpdateStatus::UpToDate),
               "up_to_date");
  EXPECT_STREQ(pluginUpdateStatusId(PluginUpdateStatus::UpdateAvailable),
               "update_available");
  EXPECT_STREQ(pluginUpdateStatusId(PluginUpdateStatus::Unreachable),
               "unreachable");
  EXPECT_STREQ(pluginUpdateStatusId(PluginUpdateStatus::Unknown), "unknown");
  EXPECT_STREQ(pluginUpdateStatusId(PluginUpdateStatus::NotApplicable),
               "not_applicable");
}

TEST(PluginDetailsTest, DiagnosticsRenderWithSeverityAndPath) {
  PluginDiagnosticInfo diagnostic;
  diagnostic.severity = PluginDiagnosticSeverity::Warning;
  diagnostic.message = "Plugin skipped";
  diagnostic.path = "/plugins/broken.so";

  EXPECT_EQ(formatPluginDiagnostic(diagnostic),
            "Warning: Plugin skipped [/plugins/broken.so]");

  diagnostic.path.clear();
  EXPECT_EQ(formatPluginDiagnostic(diagnostic), "Warning: Plugin skipped");

  EXPECT_STREQ(pluginDiagnosticSeverityLabel(PluginDiagnosticSeverity::Info),
               plugin_ux::kSeverityInfo);
  EXPECT_STREQ(pluginDiagnosticSeverityLabel(PluginDiagnosticSeverity::Error),
               plugin_ux::kSeverityError);
  EXPECT_STREQ(pluginDiagnosticSeverityId(PluginDiagnosticSeverity::Warning),
               "warning");
}

TEST(PluginDetailsTest, NothingToDescribeYieldsNoFields) {
  EXPECT_TRUE(makePluginDetails(nullptr, nullptr, nullptr).empty());
}

TEST(PluginDetailsTest, BundledPluginDescribesAsACoreEntry) {
  // A core plugin never reaches the registry, but stages name it as their
  // owning plugin, so its id has to describe like any other selector.
  LoadedPluginInfo loaded;
  loaded.plugin_id = "decode-orc.stage.tbc_source";
  loaded.plugin_version = "1.2.3";
  loaded.path = "/plugins/liborc-stage-plugin-tbc-source.so";
  loaded.license_spdx = "GPL-3.0-or-later";
  loaded.is_core_plugin = true;

  const auto entry = makeEntryForLoadedPlugin(loaded);
  EXPECT_EQ(entry.selector, "decode-orc.stage.tbc_source");
  EXPECT_EQ(entry.load_state, PluginLoadState::Core);
  EXPECT_EQ(entry.source_label, plugin_ux::kSourceCore);

  const auto fields = makePluginDetails(&entry, nullptr, nullptr);
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldSelector),
            "decode-orc.stage.tbc_source");
  EXPECT_EQ(valueOf(fields, plugin_ux::kFieldLoaded), "yes");
}

TEST(PluginDetailsTest, LoadedNonCorePluginReportsWhereItCameFrom) {
  // A plugin loaded from ORC_STAGE_PLUGIN_PATHS is not bundled, so it must not
  // claim to have shipped with Decode-Orc.
  LoadedPluginInfo loaded;
  loaded.plugin_id = "com.example.sideloaded";
  loaded.path = "/opt/plugins/example.so";
  loaded.is_core_plugin = false;

  const auto entry = makeEntryForLoadedPlugin(loaded);
  EXPECT_EQ(entry.source_label, "/opt/plugins/example.so");
  EXPECT_NE(entry.load_state, PluginLoadState::Core);
}

}  // namespace
}  // namespace orc::presenters
