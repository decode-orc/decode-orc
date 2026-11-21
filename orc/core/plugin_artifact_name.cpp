/*
 * File:        plugin_artifact_name.cpp
 * Module:      orc-core
 * Purpose:     Single source of truth for stage-plugin release-artifact naming:
 *              validation and ABI-tag parsing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "include/plugin_artifact_name.h"

#include <filesystem>
#include <regex>
#include <string_view>

namespace orc {
namespace {

constexpr const char* kAssetPrefix = "orc-plugin_";

// A single `<stage>_<platform>` segment (stage and platform each allow the same
// character class; the split between them is not needed for validation).
constexpr const char* kSegmentChars = "[A-Za-z0-9._-]+";

}  // namespace

bool is_view_binary_file_name(const std::string& name) {
  const std::string file = std::filesystem::path(name).filename().string();
  constexpr std::string_view kView = "orc-view_";
  constexpr std::string_view kLibView = "liborc-view_";
  const std::string_view view(file);
  return (view.size() > kView.size() &&
          view.substr(0, kView.size()) == kView) ||
         (view.size() > kLibView.size() &&
          view.substr(0, kLibView.size()) == kLibView);
}

bool is_valid_view_release_asset_name(const std::string& name) {
  static const std::regex view_regex(
      R"(^orc-view_[A-Za-z0-9._-]+\.(so|dylib|dll)$)");
  return std::regex_match(name, view_regex) &&
         name.find("..") == std::string::npos;
}

ParsedReleaseAssetName parse_release_asset_name(const std::string& name) {
  ParsedReleaseAssetName parsed;

  // Overall structure: prefix + at least two `_`-separated segments + optional
  // `_abi<N>` + extension. The trailing extension is matched first so the
  // optional ABI tag can be extracted from the stem without greedy ambiguity.
  static const std::regex ext_regex(R"(\.(so|dylib|dll)$)");
  std::smatch ext_match;
  if (!std::regex_search(name, ext_match, ext_regex)) {
    return parsed;
  }
  const std::string ext = ext_match[1].str();
  std::string stem = name.substr(0, name.size() - (ext.size() + 1));

  bool has_abi = false;
  uint32_t abi = 0;
  static const std::regex abi_regex(R"(_abi([0-9]+)$)");
  std::smatch abi_match;
  if (std::regex_search(stem, abi_match, abi_regex)) {
    has_abi = true;
    abi = static_cast<uint32_t>(std::stoul(abi_match[1].str()));
    stem = stem.substr(0, stem.size() - abi_match[0].length());
  }

  // The remaining stem must be orc-plugin_<stage>_<platform> with two non-empty
  // segments.
  static const std::regex stem_regex(std::string("^") + kAssetPrefix +
                                     kSegmentChars + "_" + kSegmentChars + "$");
  if (!std::regex_match(stem, stem_regex)) {
    return parsed;
  }

  parsed.valid = true;
  parsed.ext = ext;
  parsed.has_abi = has_abi;
  parsed.abi = abi;
  return parsed;
}

bool is_valid_release_asset_name(const std::string& name) {
  return parse_release_asset_name(name).valid;
}

std::string platform_artifact_extension(const std::string& target_platform) {
  if (target_platform == "windows") {
    return ".dll";
  }
  if (target_platform == "macos") {
    return ".dylib";
  }
  return ".so";
}

std::string platform_artifact_token(const std::string& target_platform) {
  if (target_platform == "windows") {
    return "_windows";
  }
  if (target_platform == "macos") {
    return "_macos";
  }
  return "_linux";
}

}  // namespace orc
