/*
 * File:        plugin_ux_strings.h
 * Module:      orc-view-types
 * Purpose:     Canonical user-facing strings for plugin management (GUI + CLI)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace orc {
namespace plugin_ux {

// Single source of the words both front ends use for plugin management. The
// GUI and the CLI must never spell these out themselves: a user who reads the
// dialog and then scripts the same action must meet the same vocabulary.
//
// This header is deliberately Qt-free so orc/gui and orc/cli can both consume
// it (see AGENTS.md §8 module table).

// --- Trust ------------------------------------------------------------------

/// Title of the trust confirmation.
inline constexpr const char* kTrustDialogTitle = "Plugin Trust";

/// The one warning shown before any action that lets a plugin binary run.
inline constexpr const char* kTrustWarning =
    "Warning! Plugins execute code locally on your computer - Are you sure you "
    "trust the source and author of this plugin?";

/// Question appended to the warning where the answer is typed rather than
/// clicked (the GUI asks the same thing with its Yes / No buttons).
inline constexpr const char* kTrustPrompt = "Trust this plugin? [y/N]: ";

/// Shown when the user answers anything other than yes.
inline constexpr const char* kTrustDeclined =
    "Trust was not granted; nothing was changed.";

/// Shown when there is no terminal to ask on. Names the flag that replaces the
/// answer so a script author is told the fix rather than left hanging.
inline constexpr const char* kTrustNotInteractive =
    "trust must be confirmed before a plugin binary may run, and stdin is not "
    "a terminal; pass --yes to confirm without prompting";

// --- Post-change note -------------------------------------------------------

/// Prefix used when the note is printed as an aside rather than as a label.
inline constexpr const char* kNotePrefix = "Note: ";

/// Registry edits are reconciled at startup, never mid-session.
inline constexpr const char* kRegistryChangeNote =
    "Registry changes take effect on the next application launch.";

/// Asked when a dialog that changed the registry is closed. Decode-Orc cannot
/// relaunch itself, so the prompt promises only the half it can deliver: the
/// quit. Starting the application again is the user's step.
inline constexpr const char* kQuitToApplyPrompt =
    "Plugin changes take effect the next time Decode-Orc starts.\n\n"
    "Quit now? Decode-Orc cannot restart itself, so you will need to start it "
    "again yourself.";

// --- Load state -------------------------------------------------------------
//
// "Enabled" means "will load at the next launch". Every other label names the
// single reason the entry will not load.

inline constexpr const char* kLoadStateWillLoadLabel = "Enabled";
inline constexpr const char* kLoadStateWillLoadDetail =
    "will load at the next application launch";

inline constexpr const char* kLoadStateDisabledLabel = "Disabled";
inline constexpr const char* kLoadStateDisabledDetail =
    "will not load until it is enabled";

inline constexpr const char* kLoadStateNotTrustedLabel = "Not trusted yet";
inline constexpr const char* kLoadStateNotTrustedDetail =
    "will not load until you confirm that it may run";

inline constexpr const char* kLoadStateAbiMismatchLabel = "Needs a rebuild";

inline constexpr const char* kLoadStateFileMissingLabel = "Binary missing";

inline constexpr const char* kLoadStateCoreLabel = "Core plugin";
inline constexpr const char* kLoadStateCoreDetail =
    "ships with Decode-Orc and always loads";

/// Separator between a load-state label and its detail clause.
inline constexpr const char* kLabelDetailSeparator = " — ";

/// Marker appended to a version when the binary cannot load on this host.
inline constexpr const char* kNeedsRebuildMarker = " ⚠";

/**
 * @brief Explain an ABI mismatch in the canonical wording.
 *
 * @param required_abi ABI the plugin declares it was built against.
 * @param host_abi ABI this build of the host provides.
 */
inline std::string abiMismatchDetail(uint32_t required_abi, uint32_t host_abi) {
  return "requires Orc ABI " + std::to_string(required_abi) +
         " but this host is ABI " + std::to_string(host_abi) +
         "; needs a rebuild for Orc ABI " + std::to_string(host_abi);
}

/**
 * @brief Explain that a locally registered plugin binary is not on disk.
 */
inline std::string fileMissingDetail(const std::string& path) {
  return "the plugin binary is not present at '" + path + "'";
}

/**
 * @brief Present a canonical lower-case clause as a standalone sentence.
 *
 * The clauses above are worded to read inside a line of output; a front end
 * that shows one on its own capitalises and terminates it through here rather
 * than keeping a second copy of the wording.
 */
inline std::string asSentence(const std::string& clause) {
  if (clause.empty()) {
    return clause;
  }
  std::string sentence = clause;
  sentence[0] =
      static_cast<char>(std::toupper(static_cast<unsigned char>(sentence[0])));
  if (sentence.back() != '.' && sentence.back() != '!' &&
      sentence.back() != '?') {
    sentence += '.';
  }
  return sentence;
}

// --- Source of an entry -----------------------------------------------------

/// Shown in the Source column / field for plugins bundled with the host.
inline constexpr const char* kSourceCore = "Core";

// --- Field labels -----------------------------------------------------------
//
// One spelling per field: a user who reads `plugins info` and then opens the
// Plugin Manager's details pane meets the same words in the same order.

inline constexpr const char* kFieldSelector = "selector";
inline constexpr const char* kFieldId = "id";
inline constexpr const char* kFieldName = "name";
inline constexpr const char* kFieldDescription = "description";
inline constexpr const char* kFieldVersion = "version";
inline constexpr const char* kFieldLatest = "latest";
inline constexpr const char* kFieldLicense = "license";
inline constexpr const char* kFieldMaintainer = "maintainer";
inline constexpr const char* kFieldSource = "source";
inline constexpr const char* kFieldTags = "tags";
inline constexpr const char* kFieldPath = "path";
inline constexpr const char* kFieldEnabled = "enabled";
inline constexpr const char* kFieldCore = "core";
inline constexpr const char* kFieldExists = "exists";
inline constexpr const char* kFieldLoaded = "loaded";
/// The plugin's view binary (the windows its tools open), when it has one.
inline constexpr const char* kFieldView = "view";
inline constexpr const char* kViewLoaded = "loaded";
inline constexpr const char* kViewNotLoaded =
    "not loaded; see Diagnostics for why";
inline constexpr const char* kViewGuiOnly = "installed; only orc-gui loads it";
inline constexpr const char* kFieldHostAbi = "host ABI";
inline constexpr const char* kFieldCompatible = "compatible";
inline constexpr const char* kFieldInstalled = "installed";
inline constexpr const char* kFieldStatus = "status";
inline constexpr const char* kFieldUpdate = "update";

/// Shown where a details view has nothing selected to describe.
inline constexpr const char* kDetailsNoSelection =
    "Select a plugin to see its details.";

// --- Diagnostics ------------------------------------------------------------

inline constexpr const char* kDiagnosticsTitle = "Diagnostics";
inline constexpr const char* kDiagnosticsNone =
    "No plugin diagnostics were reported.";
inline constexpr const char* kSearchPathsTitle = "Plugin search paths";

inline constexpr const char* kSeverityInfo = "Info";
inline constexpr const char* kSeverityWarning = "Warning";
inline constexpr const char* kSeverityError = "Error";

// --- Curated index (available plugins) --------------------------------------

inline constexpr const char* kIndexOfflineCached =
    "offline — showing the last cached index";
inline constexpr const char* kIndexOfflineNoCache =
    "offline — no cached index available";
inline constexpr const char* kIndexUnavailable =
    "The plugin index is unavailable.";
inline constexpr const char* kIndexLoadFailed =
    "the plugin index could not be loaded";

inline constexpr const char* kIndexCompatible = "compatible with this host";
inline constexpr const char* kIndexNoCompatibleBuild =
    "no compatible build for this host";

/// Why a compatible build is claimed: the release says so itself.
inline constexpr const char* kIndexCompatibleDeclared =
    "declared by the release manifest";

/// Short annotations appended to an index entry in a list.
inline constexpr const char* kIndexLabelInstalled = "installed";
inline constexpr const char* kIndexLabelIncompatible = "incompatible";
inline constexpr const char* kIndexLabelUnreachable = "unreachable";

/**
 * @brief Value of the `compatible` field for one index entry.
 *
 * @param has_compatible_build The latest release declares a build for this
 *                             host.
 * @param release_unreachable The release information could not be fetched.
 * @param detail Presenter-supplied compatibility message; may be empty.
 */
inline std::string indexCompatibilityValue(bool has_compatible_build,
                                           bool release_unreachable,
                                           const std::string& detail) {
  const std::string suffix =
      detail.empty() ? std::string() : kLabelDetailSeparator + detail;
  if (has_compatible_build) {
    return std::string("yes") + kLabelDetailSeparator +
           kIndexCompatibleDeclared;
  }
  if (release_unreachable) {
    return std::string(kIndexLabelUnreachable) + suffix;
  }
  return "no" + (suffix.empty() ? std::string(kLabelDetailSeparator) +
                                      kIndexNoCompatibleBuild
                                : suffix);
}

/**
 * @brief Value of the `installed` field for one index entry.
 *
 * Says not just whether a copy is registered but how it relates to the latest
 * published release, so an outdated install reads the same in the CLI and in
 * the GUI's browse dialog.
 *
 * @param installed A copy is present in the local registry.
 * @param installed_version Version recorded for that copy; may be empty.
 * @param latest_version Latest published release; may be empty when unknown.
 */
inline std::string installedValue(bool installed,
                                  const std::string& installed_version,
                                  const std::string& latest_version) {
  if (!installed) {
    return "no";
  }
  if (installed_version.empty()) {
    return "yes";
  }
  std::string value = std::string("yes") + kLabelDetailSeparator +
                      installed_version + " installed";
  if (latest_version.empty()) {
    return value;
  }
  return value + (installed_version == latest_version
                      ? " (up to date)"
                      : " (update available: " + latest_version + ")");
}

/**
 * @brief One line describing how the index listing was sourced.
 *
 * Takes plain values rather than a presenter type so both front ends can call
 * it; the GUI shows it as the browse dialog's status banner and the CLI prints
 * it above the listing, so the two read identically.
 *
 * @param available An index (fresh or cached) was loaded.
 * @param offline The network fetch failed.
 * @param from_cache The listing came from the last-good cache.
 * @param entry_count Number of entries in the listing.
 * @param error_message Presenter-supplied failure detail; may be empty.
 */
inline std::string indexStatusMessage(bool available, bool offline,
                                      bool from_cache, size_t entry_count,
                                      const std::string& error_message) {
  if (!available) {
    return error_message.empty() ? std::string(kIndexUnavailable)
                                 : error_message;
  }
  const std::string count = std::to_string(entry_count);
  if (offline) {
    return count + " plugins (" +
           (from_cache ? kIndexOfflineCached : kIndexOfflineNoCache) + ").";
  }
  return count + " plugins available.";
}

// --- Update status ----------------------------------------------------------
//
// The GUI Update column and the CLI `update:` field report the same outcome in
// the same words.

inline constexpr const char* kUpdateStatusNone = "—";
inline constexpr const char* kUpdateStatusChecking = "Checking...";
inline constexpr const char* kUpdateStatusUpToDate = "Up to date";
inline constexpr const char* kUpdateStatusUnreachable = "Unreachable";

/// A newer release is published upstream.
inline std::string updateAvailableLabel(const std::string& latest_version) {
  return "Update available (" + latest_version + ")";
}

/// The latest release is known but the installed version is not, so the two
/// cannot be compared.
inline std::string updateLatestOnlyLabel(const std::string& latest_version) {
  return "Latest: " + latest_version;
}

}  // namespace plugin_ux
}  // namespace orc
