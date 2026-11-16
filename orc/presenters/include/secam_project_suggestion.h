/*
 * File:        secam_project_suggestion.h
 * Module:      orc-presenters
 * Purpose:     Detects a PAL project that treats SECAM, and words the offer
 *              to switch it to the SECAM project format
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "i_project_presenter.h"

namespace orc::presenters {

/**
 * @brief A PAL project that treats its signal as SECAM.
 *
 * Before SECAM was a project format, a SECAM capture was handled in a PAL
 * project by choosing Video Sink's `secam` decoder, or vbi_source's SECAM
 * capture format. Such a project still loads
 * and runs as it is. This says that it looks like SECAM, and whether it can be
 * switched to the SECAM format: only when every stage in it supports SECAM.
 * Nothing is converted unless the user agrees.
 */
struct SecamProjectSuggestion {
  /// The nodes that treat the signal as SECAM, as "node N (name)": Video
  /// Sinks using the `secam` decoder, and VBI Sources on the SECAM format.
  std::vector<std::string> secam_nodes;
  /// The nodes whose stage does not support SECAM, as "node N (name)".
  /// The project cannot be switched while any remain.
  std::vector<std::string> blocking_nodes;

  bool canSwitch() const { return blocking_nodes.empty(); }

  /// What was found, and what blocks a switch when canSwitch() is false.
  std::string description() const;
  /// The text the GUI shows: description(), and when canSwitch() a question
  /// asking whether to switch.
  std::string message() const;
  /// The text the CLI logs: description(), and when canSwitch() how to switch.
  std::string cliMessage() const;
};

/**
 * @brief The suggestion for the project @p presenter holds, if it needs one.
 *
 * Returns nullopt unless the project format is PAL and at least one Video Sink
 * uses the `secam` decoder, either as its `decoder_type` or, when that is
 * `multiple`, in a `decoder_schedule` entry, or a vbi_source uses the SECAM
 * capture format.
 */
std::optional<SecamProjectSuggestion> findSecamProjectSuggestion(
    IProjectPresenter& presenter);

}  // namespace orc::presenters
