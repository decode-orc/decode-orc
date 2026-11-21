/*
 * File:        plugin_details.cpp
 * Module:      orc-presenters
 * Purpose:     One ordered description of a plugin, shared by the GUI and CLI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plugin_details.h"

#include <plugin_ux_strings.h>

#include "plugin_load_state.h"
#include "plugin_selector.h"

namespace orc::presenters {

namespace {

/// Append a field, dropping it when there is nothing to report.
void add(std::vector<PluginDetailField>* fields, const char* label,
         std::string value) {
  if (value.empty()) {
    return;
  }
  fields->push_back(PluginDetailField{label, std::move(value)});
}

std::string joinTags(const std::vector<std::string>& tags) {
  std::string joined;
  for (const auto& tag : tags) {
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += tag;
  }
  return joined;
}

}  // namespace

std::vector<PluginDetailField> makePluginDetails(
    const PluginRegistryEntryInfo* installed,
    const PluginIndexEntryInfo* indexed, const PluginUpdateStatusInfo* update) {
  std::vector<PluginDetailField> fields;
  if (installed == nullptr && indexed == nullptr) {
    return fields;
  }

  // The selector leads: the description is itself usable as input to the
  // commands that take a plugin.
  const std::string selector =
      installed != nullptr ? installed->selector : indexed->id;
  add(&fields, plugin_ux::kFieldSelector, selector);

  const std::string plugin_id =
      installed != nullptr && !installed->plugin_id.empty()
          ? installed->plugin_id
          : (indexed != nullptr ? indexed->id : std::string());
  if (plugin_id != selector) {
    add(&fields, plugin_ux::kFieldId, plugin_id);
  }

  if (indexed != nullptr) {
    add(&fields, plugin_ux::kFieldName, indexed->display_name);
    add(&fields, plugin_ux::kFieldDescription, indexed->description);
  }

  const std::string installed_version =
      installed != nullptr ? installed->plugin_version : std::string();
  add(&fields, plugin_ux::kFieldVersion, installed_version);

  // The latest published release: from the index when browsing, else from an
  // update check when one has been made.
  std::string latest_version;
  if (indexed != nullptr && !indexed->release_unreachable) {
    latest_version = indexed->version;
  } else if (update != nullptr) {
    latest_version = update->latest_version;
  }
  add(&fields, plugin_ux::kFieldLatest, latest_version);

  if (update != nullptr) {
    add(&fields, plugin_ux::kFieldUpdate, pluginUpdateStatusLabel(*update));
  }

  add(&fields, plugin_ux::kFieldLicense,
      installed != nullptr && !installed->license_spdx.empty()
          ? installed->license_spdx
          : (indexed != nullptr ? indexed->license_spdx : std::string()));

  if (indexed != nullptr) {
    add(&fields, plugin_ux::kFieldMaintainer, indexed->maintainer);
  }

  add(&fields, plugin_ux::kFieldSource,
      installed != nullptr && !installed->source_label.empty()
          ? installed->source_label
          : (indexed != nullptr ? indexed->source_repo_url : std::string()));

  if (indexed != nullptr) {
    add(&fields, plugin_ux::kFieldTags, joinTags(indexed->tags));
  }

  if (installed != nullptr) {
    add(&fields, plugin_ux::kFieldPath, installed->path);
    add(&fields, plugin_ux::kFieldExists,
        installed->path_exists ? "yes" : "no");
    add(&fields, plugin_ux::kFieldLoaded, installed->is_loaded ? "yes" : "no");
    if (installed->has_view) {
      add(&fields, plugin_ux::kFieldView,
          installed->view_loaded       ? plugin_ux::kViewLoaded
          : installed->views_attempted ? plugin_ux::kViewNotLoaded
                                       : plugin_ux::kViewGuiOnly);
    }
    if (installed->required_host_abi != 0) {
      add(&fields, plugin_ux::kFieldHostAbi,
          "requires " + std::to_string(installed->required_host_abi) +
              " (host " + std::to_string(installed->host_abi_version) + ")");
    }
  }

  if (indexed != nullptr) {
    add(&fields, plugin_ux::kFieldCompatible,
        plugin_ux::indexCompatibilityValue(indexed->has_compatible_build,
                                           indexed->release_unreachable,
                                           indexed->compatibility_message));
    // An index entry is described to someone deciding whether to install it,
    // so it says how any installed copy relates to the latest release.
    add(&fields, plugin_ux::kFieldInstalled,
        plugin_ux::installedValue(
            installed != nullptr || indexed->already_installed,
            installed_version, indexed->version));
  }

  if (installed != nullptr) {
    add(&fields, plugin_ux::kFieldStatus,
        pluginLoadStateSummary(installed->load_state,
                               installed->load_state_detail));
  }

  return fields;
}

PluginRegistryEntryInfo makeEntryForLoadedPlugin(
    const LoadedPluginInfo& loaded) {
  PluginRegistryEntryInfo entry;
  // The selector rule registry entries follow: the id when there is one, else
  // the path — never empty, so the row stays addressable either way.
  entry.selector =
      makePluginSelector(loaded.plugin_id, loaded.path, std::string());
  entry.plugin_id = loaded.plugin_id;
  entry.plugin_version = loaded.plugin_version;
  entry.path = loaded.path;
  entry.license_spdx = loaded.license_spdx;
  entry.is_core_plugin = loaded.is_core_plugin;
  entry.is_loaded = true;
  // It is loaded, so its binary is there; nothing about it is pending a
  // decision, which is exactly what the Core state says.
  entry.path_exists = true;
  entry.enabled = true;
  entry.trust_state = kPluginTrustStateTrusted;
  // A plugin can also be loaded from ORC_STAGE_PLUGIN_PATHS without being
  // bundled, and that came from a path, not from Decode-Orc.
  entry.source_label =
      loaded.is_core_plugin ? std::string(plugin_ux::kSourceCore) : loaded.path;
  entry.load_state = computePluginLoadState(entry, &entry.load_state_detail);
  return entry;
}

std::string pluginUpdateStatusLabel(const PluginUpdateStatusInfo& status) {
  switch (status.status) {
    case PluginUpdateStatus::UpToDate:
      return plugin_ux::kUpdateStatusUpToDate;
    case PluginUpdateStatus::UpdateAvailable:
      return plugin_ux::updateAvailableLabel(status.latest_version);
    case PluginUpdateStatus::Unreachable:
      return std::string(plugin_ux::kUpdateStatusUnreachable) +
             (status.message.empty()
                  ? std::string()
                  : plugin_ux::kLabelDetailSeparator + status.message);
    case PluginUpdateStatus::Unknown:
      return plugin_ux::updateLatestOnlyLabel(status.latest_version);
    case PluginUpdateStatus::NotApplicable:
      return plugin_ux::kUpdateStatusNone;
  }
  return plugin_ux::kUpdateStatusNone;
}

const char* pluginUpdateStatusId(PluginUpdateStatus status) {
  switch (status) {
    case PluginUpdateStatus::UpToDate:
      return "up_to_date";
    case PluginUpdateStatus::UpdateAvailable:
      return "update_available";
    case PluginUpdateStatus::Unreachable:
      return "unreachable";
    case PluginUpdateStatus::Unknown:
      return "unknown";
    case PluginUpdateStatus::NotApplicable:
      return "not_applicable";
  }
  return "not_applicable";
}

const char* pluginDiagnosticSeverityLabel(PluginDiagnosticSeverity severity) {
  switch (severity) {
    case PluginDiagnosticSeverity::Info:
      return plugin_ux::kSeverityInfo;
    case PluginDiagnosticSeverity::Warning:
      return plugin_ux::kSeverityWarning;
    case PluginDiagnosticSeverity::Error:
      return plugin_ux::kSeverityError;
  }
  return plugin_ux::kSeverityInfo;
}

const char* pluginDiagnosticSeverityId(PluginDiagnosticSeverity severity) {
  switch (severity) {
    case PluginDiagnosticSeverity::Info:
      return "info";
    case PluginDiagnosticSeverity::Warning:
      return "warning";
    case PluginDiagnosticSeverity::Error:
      return "error";
  }
  return "info";
}

std::string formatPluginDiagnostic(const PluginDiagnosticInfo& diagnostic) {
  std::string line = pluginDiagnosticSeverityLabel(diagnostic.severity);
  line += ": ";
  line += diagnostic.message;
  if (!diagnostic.path.empty()) {
    line += " [" + diagnostic.path + "]";
  }
  return line;
}

}  // namespace orc::presenters
