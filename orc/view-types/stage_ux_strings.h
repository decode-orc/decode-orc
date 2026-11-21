/*
 * File:        stage_ux_strings.h
 * Module:      orc-view-types
 * Purpose:     Canonical user-facing strings for stage introspection (GUI +
 * CLI)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <string>

namespace orc {
namespace stage_ux {

// The words both front ends use when they describe a stage. A user who reads
// the GUI's parameter dialog and then scripts the same graph must meet the same
// vocabulary, so neither front end spells these out itself.
//
// Qt-free, like plugin_ux_strings.h, so orc/gui and orc/cli can both consume it
// (see AGENTS.md §8 module table).
//
// The stage *category* words (Source / Transform / Analysis / Sink) are not
// repeated here: they already have one home in the SDK's
// orc::stage_category_label(), which is what the GUI's Add Stage menu groups
// are built from.

// --- Stage fields -----------------------------------------------------------
//
// `name` is the internal stage token — the one thing `stages info` and
// `stages help` accept and a project's `stage:` key holds. The display name is
// a separate field and is never printed where a name belongs.

inline constexpr const char* kFieldName = "name";
inline constexpr const char* kFieldDisplayName = "display name";
inline constexpr const char* kFieldKind = "kind";
inline constexpr const char* kFieldPlugin = "plugin";
inline constexpr const char* kFieldCore = "core";
inline constexpr const char* kFieldDescription = "description";

// --- Parameter fields -------------------------------------------------------
//
// One label per piece of descriptor data the GUI's parameter dialog renders.

inline constexpr const char* kParamFieldDisplayName = "display name";
inline constexpr const char* kParamFieldDescription = "description";
inline constexpr const char* kParamFieldType = "type";
inline constexpr const char* kParamFieldRequired = "required";
inline constexpr const char* kParamFieldDefault = "default";
inline constexpr const char* kParamFieldMinimum = "minimum";
inline constexpr const char* kParamFieldMaximum = "maximum";
inline constexpr const char* kParamFieldAllowed = "allowed values";
inline constexpr const char* kParamFieldDependsOn = "depends on";
inline constexpr const char* kParamFieldFileExtension = "file extension";
inline constexpr const char* kParamFieldWritesFile = "writes the file";

/// Separator inside an `allowed_strings` entry that carries a display label
/// distinct from the stored value: "value\x1flabel". An entry without it is
/// both value and label. '\x1f' (unit separator) cannot appear in a parameter
/// value, so the split is unambiguous. Lives here so the GUI's combo boxes and
/// the CLI's `allowed values` field agree on what an entry means.
inline constexpr char kComboValueLabelSeparator = '\x1f';

// --- Messages ---------------------------------------------------------------

inline constexpr const char* kNoStages = "No stages to show.";
inline constexpr const char* kNoParameters = "This stage takes no parameters.";

/// A stage that ships no instructions.md is a documentation gap, not an empty
/// answer, so this is reported rather than printed as nothing.
inline constexpr const char* kNoInstructions =
    "No instructions are shipped with this stage.";

/// Frame and line numbers are stored 0-based and shown 1-based (see
/// frame_numbering.h). The human listing follows the GUI; --json does not,
/// because a script reads it back as a project file would.
inline constexpr const char* kIndexedSpecNote =
    "Frame and line numbers are shown 1-based, as the GUI shows them. --json "
    "reports the 0-based values a project file stores.";

/// Introduces the near matches offered for an unknown stage name.
inline constexpr const char* kNearMatchesIntro = "Did you mean:";

/**
 * @brief Explain that a stage name is not registered in this build.
 */
inline std::string unknownStageMessage(const std::string& stage_name) {
  return "no stage named '" + stage_name +
         "' is registered in this build of Decode-Orc";
}

}  // namespace stage_ux
}  // namespace orc
