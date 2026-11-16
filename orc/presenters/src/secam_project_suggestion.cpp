/*
 * File:        secam_project_suggestion.cpp
 * Module:      orc-presenters
 * Purpose:     Detects a PAL project that decodes SECAM, and words the offer
 *              to switch it to the SECAM project format
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../include/secam_project_suggestion.h"

#include <cctype>
#include <set>
#include <string>
#include <variant>

namespace orc::presenters {

namespace {

constexpr const char kVideoSinkStage[] = "video_sink";
constexpr const char kSecamDecoder[] = "secam";
constexpr const char kVbiSourceStage[] = "vbi_source";
// vbi_source's capture format for a SECAM source; its lines go on SECAM frames
// in a SECAM project and on PAL frames in a PAL one.
constexpr const char kVbiSecamFormat[] =
    "bt8x8 card dump, 8-bit (WST, SECAM source)";

std::string string_parameter(
    const std::map<std::string, orc::ParameterValue>& params,
    const std::string& key) {
  const auto it = params.find(key);
  if (it == params.end()) return {};
  if (const auto* text = std::get_if<std::string>(&it->second)) {
    return *text;
  }
  return {};
}

std::string lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

// True when a decoder_schedule names the secam decoder in any entry: as the
// default ("secam"), for held pictures ("held:secam") or for a frame range
// ("0-99:secam"). Entries are separated by commas or white space.
bool schedule_uses_secam(const std::string& schedule) {
  std::string token;
  auto token_is_secam = [&token]() { return lower(token) == kSecamDecoder; };
  for (const char c : schedule) {
    if (c == ',' || c == ';' || c == ':' ||
        std::isspace(static_cast<unsigned char>(c))) {
      if (token_is_secam()) return true;
      token.clear();
    } else {
      token += c;
    }
  }
  return token_is_secam();
}

std::string trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  const auto last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1);
}

std::string describe_node(const NodeInfo& node,
                          const std::string& display_name) {
  // The loader gives an unlabelled node its id as a label, which says nothing
  // the "node N" does not; the stage's display name says more.
  const std::string id = node.node_id.to_string();
  const std::string label = trim(node.label);
  const std::string& name =
      (label.empty() || label == id) ? display_name : label;
  std::string text = "node " + id;
  if (!name.empty()) {
    text += " (" + name + ")";
  }
  return text;
}

std::string join(const std::vector<std::string>& items) {
  std::string text;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      text += (i + 1 == items.size()) ? " and " : ", ";
    }
    text += items[i];
  }
  return text;
}

}  // namespace

std::string SecamProjectSuggestion::description() const {
  const bool one_decoder = secam_nodes.size() == 1;
  std::string text = "This is a PAL project, but " + join(secam_nodes) +
                     (one_decoder ? " treats" : " treat") +
                     " it as SECAM. SECAM is now a project format of its own, "
                     "with SECAM-specific stages and checks; the project "
                     "still runs as it is.";
  if (!canSwitch()) {
    const bool one_blocker = blocking_nodes.size() == 1;
    text += "\n\nIt cannot be switched to SECAM yet: " + join(blocking_nodes) +
            (one_blocker ? " uses a stage that does" : " use stages that do") +
            " not support SECAM. Replace " + (one_blocker ? "it" : "them") +
            " to switch the project.";
  }
  return text;
}

std::string SecamProjectSuggestion::message() const {
  if (!canSwitch()) {
    return description();
  }
  return description() +
         "\n\nSwitch this project to SECAM? Only the project format changes; "
         "save the project to keep it.";
}

std::string SecamProjectSuggestion::cliMessage() const {
  if (!canSwitch()) {
    return description();
  }
  return description() +
         " To switch it, open it in orc-gui and accept the offer, or set "
         "video_format: SECAM in the project file.";
}

std::optional<SecamProjectSuggestion> findSecamProjectSuggestion(
    IProjectPresenter& presenter) {
  if (presenter.getVideoFormat() != VideoFormat::PAL) {
    return std::nullopt;
  }

  // Display names for every stage, and the set a SECAM project offers: the
  // same filter the add-stage menu and `orc-cli stages list --format` apply.
  std::map<std::string, std::string> display_names;
  for (const auto& stage : presenter.listAllStages()) {
    display_names[stage.name] = stage.display_name;
  }
  std::set<std::string> secam_stages;
  for (const auto& stage :
       presenter.listAvailableStagesForFormat(VideoFormat::SECAM)) {
    secam_stages.insert(stage.name);
  }

  SecamProjectSuggestion suggestion;
  for (const auto& node : presenter.getNodes()) {
    const std::string& display_name = display_names[node.stage_name];
    if (node.stage_name == kVideoSinkStage) {
      const auto params = presenter.getNodeParameters(node.node_id);
      const std::string decoder =
          lower(string_parameter(params, "decoder_type"));
      // A schedule is read only when decoder_type hands the choice to it.
      if (decoder == kSecamDecoder ||
          (decoder == "multiple" &&
           schedule_uses_secam(string_parameter(params, "decoder_schedule")))) {
        suggestion.secam_nodes.push_back(describe_node(node, display_name));
      }
    } else if (node.stage_name == kVbiSourceStage) {
      const auto params = presenter.getNodeParameters(node.node_id);
      if (string_parameter(params, "format") == kVbiSecamFormat) {
        suggestion.secam_nodes.push_back(describe_node(node, display_name));
      }
    }
    if (secam_stages.count(node.stage_name) == 0) {
      suggestion.blocking_nodes.push_back(describe_node(node, display_name));
    }
  }

  if (suggestion.secam_nodes.empty()) {
    return std::nullopt;
  }
  return suggestion;
}

}  // namespace orc::presenters
