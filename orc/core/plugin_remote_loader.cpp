/*
 * File:        plugin_remote_loader.cpp
 * Module:      orc-core
 * Purpose:     Utilities for downloading and caching remote plugin binaries
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "include/plugin_remote_loader.h"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>

#include "include/plugin_artifact_name.h"
#include "include/plugin_release_manifest.h"
#include "include/sha256_hash.h"
#include "orc/abi/orc_plugin_abi.h"
#include "orc/view/orc_view_plugin_abi.h"

namespace orc {
namespace {

static size_t write_string_callback(void* contents, size_t size, size_t nmemb,
                                    std::string* output) {
  size_t real_size = size * nmemb;
  if (!output) {
    return 0;
  }
  output->append(static_cast<const char*>(contents), real_size);
  return real_size;
}

// Callback for libcurl to write downloaded data to file
static size_t write_callback(void* contents, size_t size, size_t nmemb,
                             std::ofstream* file) {
  size_t real_size = size * nmemb;
  if (!file || !file->is_open()) {
    return 0;
  }
  file->write(static_cast<char*>(contents),
              static_cast<std::streamsize>(real_size));
  return file->good() ? real_size : 0;
}

std::string env_or_empty(const char* name) {
  const char* value = std::getenv(name);
  if (!value) {
    return "";
  }
  return std::string(value);
}

std::filesystem::path resolve_default_config_dir() {
#if defined(_WIN32)
  std::string appdata = env_or_empty("APPDATA");
  if (!appdata.empty()) {
    return std::filesystem::path(appdata) / "decode-orc";
  }

  std::string userprofile = env_or_empty("USERPROFILE");
  if (!userprofile.empty()) {
    return std::filesystem::path(userprofile) / "AppData" / "Roaming" /
           "decode-orc";
  }
#else
  std::string xdg_config_home = env_or_empty("XDG_CONFIG_HOME");
  if (!xdg_config_home.empty()) {
    return std::filesystem::path(xdg_config_home) / "decode-orc";
  }

  std::string home = env_or_empty("HOME");
  if (!home.empty()) {
    return std::filesystem::path(home) / ".config" / "decode-orc";
  }
#endif

  return std::filesystem::current_path() / ".decode-orc";
}

std::string normalize_json_string(std::string value) {
  std::string::size_type pos = 0;
  while ((pos = value.find("\\/", pos)) != std::string::npos) {
    value.replace(pos, 2, "/");
    ++pos;
  }
  return value;
}

// Artifact naming helpers (extension, token, validation, selection) live in
// plugin_artifact_name.{h,cpp} — the single source of truth shared with
// stage_plugin_registry.cpp.

std::string infer_target_platform() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

bool fetch_text_url(const std::string& url, std::string* body,
                    std::string* error_message) {
  if (!body) {
    if (error_message) {
      *error_message = "Internal error: body output pointer is null";
    }
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    if (error_message) {
      *error_message = "Failed to initialize libcurl";
    }
    return false;
  }

  body->clear();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(
      curl, CURLOPT_USERAGENT,
      "decode-orc/1.0 (+https://github.com/decode-orc/decode-orc)");

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    if (error_message) {
      *error_message =
          std::string("Failed to fetch URL: ") + curl_easy_strerror(res);
    }
    curl_easy_cleanup(curl);
    return false;
  }

  long response_code =  // NOLINT(google-runtime-int): curl API requires long*
      0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  curl_easy_cleanup(curl);

  if (response_code < 200 || response_code >= 300) {
    if (error_message) {
      *error_message =
          "HTTP error " + std::to_string(response_code) + " while fetching URL";
    }
    return false;
  }

  return true;
}

std::string sanitize_cache_component(std::string value) {
  for (char& ch : value) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (!std::isalnum(uch) && ch != '.' && ch != '_' && ch != '-') {
      ch = '_';
    }
  }

  if (value.empty()) {
    return "unknown";
  }

  return value;
}

bool hex_digests_equal(const std::string& lhs, const std::string& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
        std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

// Move a checksum-mismatching artifact aside so it can never be dlopen'ed,
// while preserving it for inspection. Returns the quarantine path, or an
// empty string if the rename failed (in which case the file is removed).
std::string quarantine_artifact(const std::string& path) {
  const std::string quarantine_path = path + ".quarantined";
  std::error_code error_code;
  std::filesystem::remove(quarantine_path, error_code);
  error_code.clear();
  std::filesystem::rename(path, quarantine_path, error_code);
  if (error_code) {
    std::filesystem::remove(path, error_code);
    return "";
  }
  return quarantine_path;
}

// Compute the digest of a cached/downloaded file and compare it to the
// registry's expected digest.
PluginRemoteLoader::ChecksumStatus verify_file_checksum(
    const std::string& path, const std::string& expected_sha256_hex) {
  if (expected_sha256_hex.empty()) {
    return PluginRemoteLoader::ChecksumStatus::NotProvided;
  }
  const std::string actual = sha256_hex_of_file(path);
  if (!actual.empty() && hex_digests_equal(actual, expected_sha256_hex)) {
    return PluginRemoteLoader::ChecksumStatus::Match;
  }
  return PluginRemoteLoader::ChecksumStatus::Mismatch;
}

std::string extract_release_tag_from_asset_url(
    const std::string& github_release_url) {
  // Expected form: .../releases/download/<tag>/<asset>
  const std::regex download_regex(R"(/releases/download/([^/]+)/)");
  std::smatch match;
  if (!std::regex_search(github_release_url, match, download_regex)) {
    return "";
  }

  return normalize_json_string(match[1].str());
}

}  // namespace

PluginRemoteLoader::ResolveReleaseAssetResult
PluginRemoteLoader::resolve_release_asset_from_releases_url(
    const std::string& releases_url, const std::string& target_platform,
    std::vector<std::string>* warnings) {
  ResolveReleaseAssetResult result;

  const std::regex repo_regex(
      R"(^https?://github\.com/([^/]+)/([^/]+)(?:/.*)?$)");
  std::smatch repo_match;
  if (!std::regex_match(releases_url, repo_match, repo_regex)) {
    result.error_message =
        "Expected a GitHub releases URL like "
        "https://github.com/<owner>/<repo>/releases";
    return result;
  }

  const std::string owner = repo_match[1].str();
  std::string repo = repo_match[2].str();
  if (repo.size() > 4 && repo.substr(repo.size() - 4) == ".git") {
    repo = repo.substr(0, repo.size() - 4);
  }

  result.source_repo_url = "https://github.com/" + owner + "/" + repo;

  std::string api_url;
  std::string explicit_tag;

  const std::regex tag_regex(R"(/releases/tag/([^/?#]+))");
  std::smatch tag_match;
  if (std::regex_search(releases_url, tag_match, tag_regex)) {
    explicit_tag = tag_match[1].str();
    api_url = "https://api.github.com/repos/" + owner + "/" + repo +
              "/releases/tags/" + explicit_tag;
  } else {
    api_url = "https://api.github.com/repos/" + owner + "/" + repo +
              "/releases/latest";
  }

  std::string response_body;
  std::string fetch_error;
  if (!fetch_text_url(api_url, &response_body, &fetch_error)) {
    result.unreachable = true;
    result.error_message =
        "Failed to query GitHub release metadata: " + fetch_error;
    return result;
  }

  {
    const std::regex tag_name_regex(R"json("tag_name"\s*:\s*"([^"]+)")json");
    std::smatch tag_name_match;
    if (std::regex_search(response_body, tag_name_match, tag_name_regex)) {
      result.release_tag = normalize_json_string(tag_name_match[1].str());
    } else if (!explicit_tag.empty()) {
      result.release_tag = explicit_tag;
    }
  }

  std::vector<ReleaseAssetCandidate> candidates;

  const std::regex download_url_regex(
      R"json("browser_download_url"\s*:\s*"([^"]+)")json");
  for (std::sregex_iterator
           it(response_body.begin(), response_body.end(), download_url_regex),
       end;
       it != end; ++it) {
    const std::string raw_url = (*it)[1].str();
    const std::string url = normalize_json_string(raw_url);
    const auto slash = url.find_last_of('/');
    const std::string name =
        (slash == std::string::npos) ? url : url.substr(slash + 1);
    candidates.push_back({url, name});
  }

  if (candidates.empty()) {
    result.error_message =
        "No downloadable assets found in the selected GitHub release";
    return result;
  }

  const std::string platform =
      target_platform.empty() ? infer_target_platform() : target_platform;

  // The release manifest is mandatory: it is the single source of the
  // artifact list and its declared ABI/toolchain/digest, so there is exactly
  // one notion of "compatible" (the load-time gate remains the enforcement
  // point for what the binary actually is).
  const ReleaseAssetCandidate* manifest_asset = nullptr;
  for (const auto& candidate : candidates) {
    if (candidate.name == kPluginReleaseManifestAssetName) {
      manifest_asset = &candidate;
      break;
    }
  }
  if (manifest_asset == nullptr) {
    result.error_message =
        "The release does not include " +
        std::string(kPluginReleaseManifestAssetName) +
        "; a release manifest is required for a plugin to be installable";
    return result;
  }

  std::string manifest_body;
  std::string manifest_fetch_error;
  if (!fetch_text_url(manifest_asset->url, &manifest_body,
                      &manifest_fetch_error)) {
    result.unreachable = true;
    result.error_message =
        "Failed to fetch the release manifest: " + manifest_fetch_error;
    return result;
  }

  const auto parsed = parse_plugin_release_manifest_yaml(manifest_body);
  if (warnings) {
    warnings->insert(warnings->end(), parsed.warnings.begin(),
                     parsed.warnings.end());
  }
  if (!parsed.success) {
    result.error_message = parsed.error_message;
    return result;
  }

  const ManifestArtifactResolution manifest_resolution =
      resolve_manifest_artifact(parsed.manifest, platform,
                                kStagePluginHostAbiVersion,
                                ORC_SDK_TOOLCHAIN_TAG);
  if (!manifest_resolution.found) {
    result.error_message = manifest_resolution.error_message;
    return result;
  }

  const PluginReleaseManifestArtifact& artifact =
      parsed.manifest.artifacts[static_cast<size_t>(manifest_resolution.index)];

  if (!is_valid_release_asset_name(artifact.file)) {
    result.error_message =
        "The release manifest lists artifact '" + artifact.file +
        "' which does not follow the plugin naming convention "
        "(orc-plugin_<stage>_<platform>[_abi<N>].<so|dylib|dll>)";
    return result;
  }

  const ReleaseAssetCandidate* declared_asset = nullptr;
  for (const auto& candidate : candidates) {
    if (candidate.name == artifact.file) {
      declared_asset = &candidate;
      break;
    }
  }
  if (declared_asset == nullptr) {
    result.error_message = "The release manifest lists artifact '" +
                           artifact.file +
                           "' but the release publishes no such asset";
    return result;
  }

  result.abi_mismatch = manifest_resolution.abi_mismatch;
  result.toolchain_mismatch = manifest_resolution.toolchain_mismatch;
  result.asset_abi = artifact.abi;
  result.asset_toolchain = artifact.toolchain_tag;
  result.sha256 = artifact.sha256;
  if (warnings) {
    if (result.abi_mismatch) {
      warnings->push_back("The release manifest declares Orc ABI " +
                          std::to_string(artifact.abi) + " for '" +
                          artifact.file + "', but this host requires ABI " +
                          std::to_string(kStagePluginHostAbiVersion));
    }
    if (result.toolchain_mismatch) {
      warnings->push_back("The release manifest declares toolchain '" +
                          artifact.toolchain_tag + "' for '" + artifact.file +
                          "', but this host requires '" +
                          ORC_SDK_TOOLCHAIN_TAG "'");
    }
  }

  result.release_asset_url = declared_asset->url;
  result.release_asset_name = declared_asset->name;

  // The view binary travels with the stage binary but never stops it: a view
  // that is missing, misnamed or built for something else is left out and
  // the plugin installs without its window.
  const ManifestViewResolution view = resolve_manifest_view_artifact(
      parsed.manifest, platform, view::kViewPluginAbiVersion, 0, 0,
      ORC_SDK_TOOLCHAIN_TAG);
  if (view.found) {
    const PluginReleaseManifestArtifact& view_artifact =
        parsed.manifest.artifacts[static_cast<size_t>(view.index)];
    const ReleaseAssetCandidate* view_asset = nullptr;
    for (const auto& candidate : candidates) {
      if (candidate.name == view_artifact.file) {
        view_asset = &candidate;
        break;
      }
    }
    if (!is_valid_view_release_asset_name(view_artifact.file)) {
      result.view_left_out = "its file name '" + view_artifact.file +
                             "' is not of the form "
                             "'orc-view_<name>.<so|dylib|dll>'";
    } else if (view_asset == nullptr) {
      result.view_left_out =
          "the release publishes no asset '" + view_artifact.file + "'";
    } else if (!view.incompatibility.empty()) {
      result.view_left_out = view.incompatibility;
    } else {
      result.view_release_asset_url = view_asset->url;
      result.view_release_asset_name = view_asset->name;
      result.view_sha256 = view_artifact.sha256;
    }
    if (!result.view_left_out.empty() && warnings) {
      warnings->push_back(
          "The plugin installs without its view (the window its tools open) "
          "because " +
          result.view_left_out);
    }
  }

  result.success = true;
  return result;
}

std::string PluginRemoteLoader::resolve_plugin_cache_dir(std::string* error) {
  try {
    std::filesystem::path cache_dir =
        resolve_default_config_dir() / "plugin-cache";

    if (!std::filesystem::exists(cache_dir)) {
      std::filesystem::create_directories(cache_dir);
    }

    if (!std::filesystem::is_directory(cache_dir)) {
      if (error) {
        *error = "Plugin cache path exists but is not a directory: " +
                 cache_dir.string();
      }
      return "";
    }

    return cache_dir.string();
  } catch (const std::filesystem::filesystem_error& e) {
    if (error) {
      *error =
          std::string("Failed to access plugin cache directory: ") + e.what();
    }
    return "";
  }
}

std::string PluginRemoteLoader::get_cache_path(
    const std::string& asset_name, const std::string& target_platform,
    const std::string& release_tag) {
  std::string cache_dir_str = resolve_plugin_cache_dir();
  if (cache_dir_str.empty()) {
    return "";
  }

  std::filesystem::path cache_path =
      std::filesystem::path(cache_dir_str) / target_platform;
  if (!release_tag.empty()) {
    cache_path /= sanitize_cache_component(release_tag);
  }
  cache_path /= asset_name;
  return cache_path.string();
}

PluginRemoteLoader::ChecksumStatus PluginRemoteLoader::verify_checksum_hex(
    std::string_view data, const std::string& expected_sha256_hex) {
  if (expected_sha256_hex.empty()) {
    return ChecksumStatus::NotProvided;
  }
  return hex_digests_equal(sha256_hex(data), expected_sha256_hex)
             ? ChecksumStatus::Match
             : ChecksumStatus::Mismatch;
}

PluginRemoteLoader::DownloadResult PluginRemoteLoader::download_release_asset(
    const std::string& github_release_url, const std::string& asset_name,
    const std::string& target_platform, std::vector<std::string>* warnings,
    const std::string& expected_sha256) {
  DownloadResult result;

  // Validate asset name format (shared with stage_plugin_registry.cpp). The
  // same downloader fetches a plugin's view binary, so its name is accepted
  // too.
  if (!is_valid_release_asset_name(asset_name) &&
      !is_valid_view_release_asset_name(asset_name)) {
    result.error_message =
        "Invalid asset name format (expected "
        "orc-plugin_<stage>_<platform>[_abi<N>].<so|dylib|dll> or "
        "orc-view_<name>.<so|dylib|dll>): " +
        asset_name;
    if (warnings) {
      warnings->push_back(result.error_message);
    }
    return result;
  }

  const std::string release_tag =
      extract_release_tag_from_asset_url(github_release_url);

  // Check release-aware cache first. This prevents stale binaries when the same
  // asset name is reused across multiple release tags.
  std::string cache_path =
      get_cache_path(asset_name, target_platform, release_tag);
  if (!cache_path.empty() && std::filesystem::exists(cache_path)) {
    // Re-verify cache hits so a tampered or corrupted cached binary is
    // quarantined and replaced by a fresh download instead of being loaded.
    const auto cached_status =
        verify_file_checksum(cache_path, expected_sha256);
    if (cached_status == ChecksumStatus::Mismatch) {
      const std::string quarantine_path = quarantine_artifact(cache_path);
      const std::string warning =
          "Cached plugin binary failed sha256 verification and was "
          "quarantined" +
          (quarantine_path.empty() ? std::string(" (and removed)")
                                   : " to: " + quarantine_path) +
          "; re-downloading " + asset_name;
      spdlog::warn("{}", warning);
      if (warnings) {
        warnings->push_back(warning);
      }
    } else {
      spdlog::debug("Plugin binary found in cache: {}", cache_path);
      result.success = true;
      result.downloaded_path = cache_path;
      return result;
    }
  }

  // Ensure cache directory exists
  std::string cache_dir_str;
  {
    std::string error_msg;
    cache_dir_str = resolve_plugin_cache_dir(&error_msg);
    if (cache_dir_str.empty()) {
      result.error_message =
          "Failed to create plugin cache directory: " + error_msg;
      if (warnings) {
        warnings->push_back(result.error_message);
      }
      return result;
    }
  }

  // Create platform-specific subdirectory in cache
  try {
    std::filesystem::path platform_cache =
        std::filesystem::path(cache_dir_str) / target_platform;
    if (!release_tag.empty()) {
      platform_cache /= sanitize_cache_component(release_tag);
    }
    if (!std::filesystem::exists(platform_cache)) {
      std::filesystem::create_directories(platform_cache);
    }
    cache_path = (platform_cache / asset_name).string();
  } catch (const std::filesystem::filesystem_error& e) {
    result.error_message =
        std::string("Failed to create platform cache subdirectory: ") +
        e.what();
    if (warnings) {
      warnings->push_back(result.error_message);
    }
    return result;
  }

  // Initialize libcurl
  CURL* curl = curl_easy_init();
  if (!curl) {
    result.error_message = "Failed to initialize libcurl";
    if (warnings) {
      warnings->push_back(result.error_message);
    }
    return result;
  }

  // Open output file
  std::ofstream outfile(cache_path, std::ios::binary);
  if (!outfile.is_open()) {
    result.error_message =
        "Failed to open cache file for writing: " + cache_path;
    if (warnings) {
      warnings->push_back(result.error_message);
    }
    curl_easy_cleanup(curl);
    return result;
  }

  spdlog::debug("Downloading plugin binary from: {}", github_release_url);

  // Configure libcurl
  curl_easy_setopt(curl, CURLOPT_URL, github_release_url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outfile);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,
                   1L);  // Follow redirects (GitHub redirects)
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);    // 5 minute timeout
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);  // Fail on HTTP errors

  // Add User-Agent to be respectful
  curl_easy_setopt(
      curl, CURLOPT_USERAGENT,
      "decode-orc/1.0 (+https://github.com/decode-orc/decode-orc)");

  // Perform download
  CURLcode res = curl_easy_perform(curl);

  outfile.close();

  if (res != CURLE_OK) {
    std::string error = std::string("Failed to download plugin binary: ") +
                        curl_easy_strerror(res);
    result.error_message = error;
    if (warnings) {
      warnings->push_back(error);
    }

    // Clean up failed download
    try {
      std::filesystem::remove(cache_path);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }

    curl_easy_cleanup(curl);
    return result;
  }

  // Check response code
  long response_code =  // NOLINT(google-runtime-int): curl API requires long*
      0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

  curl_easy_cleanup(curl);

  if (response_code < 200 || response_code >= 300) {
    std::string error =
        std::string("HTTP error ") + std::to_string(response_code) +
        " downloading plugin binary from: " + github_release_url;
    result.error_message = error;
    if (warnings) {
      warnings->push_back(error);
    }

    // Clean up failed download
    try {
      std::filesystem::remove(cache_path);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }

    return result;
  }

  // Verify file was written
  if (!std::filesystem::exists(cache_path)) {
    std::string error =
        "Downloaded file not found at cache path: " + cache_path;
    result.error_message = error;
    if (warnings) {
      warnings->push_back(error);
    }
    return result;
  }

  uint64_t file_size = std::filesystem::file_size(cache_path);
  if (file_size == 0) {
    std::string error = "Downloaded file is empty (0 bytes): " + cache_path;
    result.error_message = error;
    if (warnings) {
      warnings->push_back(error);
    }

    // Clean up empty file
    try {
      std::filesystem::remove(cache_path);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }

    return result;
  }

  // Integrity gate: verify the download against the registry's expected
  // digest before it becomes a loadable cache entry.
  switch (verify_file_checksum(cache_path, expected_sha256)) {
    case ChecksumStatus::Match:
      break;
    case ChecksumStatus::Mismatch: {
      const std::string quarantine_path = quarantine_artifact(cache_path);
      result.error_message =
          "Downloaded plugin binary failed sha256 verification (expected " +
          expected_sha256 + "); the file was quarantined" +
          (quarantine_path.empty() ? std::string(" (and removed)")
                                   : " to: " + quarantine_path);
      if (warnings) {
        warnings->push_back(result.error_message);
      }
      return result;
    }
    case ChecksumStatus::NotProvided: {
      const std::string warning =
          "No sha256 recorded for downloaded plugin asset '" + asset_name +
          "'; artifact integrity was not verified";
      spdlog::warn("{}", warning);
      if (warnings) {
        warnings->push_back(warning);
      }
      break;
    }
  }

  spdlog::debug("Successfully downloaded plugin binary to cache: {} ({} bytes)",
                cache_path, file_size);

  result.success = true;
  result.downloaded_path = cache_path;
  return result;
}

}  // namespace orc
