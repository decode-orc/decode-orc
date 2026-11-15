/*
 * File:        project_type_choices.h
 * Module:      orc-gui
 * Purpose:     The project types the New Project dialog offers
 *              (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>

#include <array>
#include <optional>
#include <string_view>

namespace orc::gui {

// One entry in the New Project dialog's "Project type" list: a video system
// and a source format, under the label the list shows.
struct ProjectTypeChoice {
  std::string_view label;
  orc::VideoSystem system;
  orc::SourceType source;
};

// Every video system a project can have, each as composite and as Y/C, in the
// order the dialog lists them. Unknown is not a project type.
inline constexpr std::array<ProjectTypeChoice, 8> kProjectTypeChoices{{
    {"NTSC Composite", orc::VideoSystem::NTSC, orc::SourceType::Composite},
    {"NTSC YC", orc::VideoSystem::NTSC, orc::SourceType::YC},
    {"PAL Composite", orc::VideoSystem::PAL, orc::SourceType::Composite},
    {"PAL YC", orc::VideoSystem::PAL, orc::SourceType::YC},
    {"PAL-M Composite", orc::VideoSystem::PAL_M, orc::SourceType::Composite},
    {"PAL-M YC", orc::VideoSystem::PAL_M, orc::SourceType::YC},
    {"SECAM Composite", orc::VideoSystem::SECAM, orc::SourceType::Composite},
    {"SECAM YC", orc::VideoSystem::SECAM, orc::SourceType::YC},
}};

// The choice a label names, or nullopt for a label the list does not have.
inline std::optional<ProjectTypeChoice> projectTypeChoiceForLabel(
    std::string_view label) {
  for (const auto& choice : kProjectTypeChoices) {
    if (choice.label == label) {
      return choice;
    }
  }
  return std::nullopt;
}

}  // namespace orc::gui
