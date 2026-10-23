/*
 * File:        project_args.cpp
 * Module:      orc-cli
 * Purpose:     Project and node arguments shared by the project subcommands
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "project_args.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <variant>

#include "node_editing.h"

namespace orc {
namespace cli {

bool load_project_or_report(const std::string& path,
                            orc::presenters::ProjectPresenter* presenter) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    std::cerr << "Error: Project file not found: " << path << "\n";
    return false;
  }
  try {
    if (presenter->loadProject(path)) {
      return true;
    }
    std::cerr << "Error: Failed to load project: " << path << "\n";
  } catch (const std::exception& e) {
    std::cerr << "Error: Failed to load project " << path << ": " << e.what()
              << "\n";
  }
  return false;
}

std::optional<orc::presenters::NodeInfo> find_node_or_report(
    const orc::presenters::ProjectPresenter& presenter,
    const std::string& address) {
  const auto nodes = presenter.getNodes();
  const auto resolution = orc::presenters::resolveNodeAddress(nodes, address);
  if (!resolution.found) {
    std::cerr << "Error: " << resolution.error << "\n";
    return std::nullopt;
  }
  return presenter.getNodeInfo(resolution.node_id);
}

void write_parameter_value_json(JsonWriter* json, const std::string& name,
                                const orc::ParameterValue& value) {
  if (const auto* v = std::get_if<bool>(&value)) {
    json->member_bool(name, *v);
  } else if (const auto* v = std::get_if<int32_t>(&value)) {
    json->member_int(name, *v);
  } else if (const auto* v = std::get_if<uint32_t>(&value)) {
    json->member_int(name, *v);
  } else if (std::holds_alternative<double>(value)) {
    json->member_number(name, orc::parameter_util::value_to_string(value));
  } else {
    json->member(name, std::get<std::string>(value));
  }
}

}  // namespace cli
}  // namespace orc
