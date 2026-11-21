/*
 * File:        plugin_remote_loader.h
 * Module:      orc-core
 * Purpose:     Utilities for downloading and caching remote plugin binaries
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/plugin_remote_loader.h. Use ProjectPresenter for plugin-aware stage access."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/plugin_remote_loader.h. Use ProjectPresenter for plugin-aware stage access."
#endif

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orc {

class PluginRemoteLoader {
 public:
  enum class ChecksumStatus {
    NotProvided,  // No expected digest supplied; integrity not verifiable
    Match,        // Computed SHA-256 equals the expected digest
    Mismatch,     // Computed SHA-256 differs from the expected digest
  };

  struct ResolveReleaseAssetResult {
    bool success = false;
    bool unreachable = false;   ///< The release metadata could not be fetched
                                ///< (as opposed to no matching asset existing).
    bool abi_mismatch = false;  ///< The manifest declares a different host
                                ///< ABI; the load-time gate would reject it.
    bool toolchain_mismatch = false;  ///< The manifest declares a toolchain
                                      ///< tag differing from the host's.
    uint32_t asset_abi = 0;           ///< The manifest-declared ABI.
    std::string asset_toolchain;  ///< Manifest-declared toolchain tag; empty
                                  ///< when undeclared.
    std::string sha256;  ///< Manifest digest for the selected asset; empty
                         ///< when undeclared.
    /// The release's view binary for the platform, when the manifest declares
    /// one that can load here. Empty otherwise.
    std::string view_release_asset_url;
    std::string view_release_asset_name;
    std::string view_sha256;
    /// Set when the manifest declares a view that was left out: why. The
    /// stage binary still resolves.
    std::string view_left_out;
    std::string source_repo_url;
    std::string release_tag;
    std::string release_asset_url;
    std::string release_asset_name;
    std::string error_message;
  };

  struct DownloadResult {
    bool success = false;
    std::string downloaded_path;
    std::string error_message;
  };

  /**
   * Download a plugin binary from a GitHub release asset URL.
   *
   * @param github_release_url Full GitHub API URL to the release asset
   *        Example:
   * https://api.github.com/repos/owner/repo/releases/download/v1.0.0/plugin.so
   * @param asset_name Expected filename (must match
   * orc-plugin_<stage>_<platform>[_abi<N>].<ext> pattern)
   * @param target_platform Platform identifier (linux, macos, windows)
   * @param warnings Optional warning accumulator for informational messages
   * @param expected_sha256 Optional SHA-256 digest (64 hex chars). When
   *        provided, both cache hits and fresh downloads are verified
   *        against it; a mismatching file is quarantined (renamed with a
   *        `.quarantined` suffix) and the download fails with an error.
   *        When absent, a warning notes the artifact cannot be verified.
   *
   * @return DownloadResult with success flag, local path (if success), and
   * error message (if failure)
   *
   * On success, the binary is cached in
   * ~/.config/decode-orc/plugin-cache/<platform>/ Subsequent calls with the
   * same URL/asset_name retrieve the cached copy.
   */
  static DownloadResult download_release_asset(
      const std::string& github_release_url, const std::string& asset_name,
      const std::string& target_platform,
      std::vector<std::string>* warnings = nullptr,
      const std::string& expected_sha256 = "");

  /**
   * Compare in-memory artifact bytes against an expected SHA-256 digest.
   *
   * @param data Raw artifact bytes
   * @param expected_sha256_hex Expected digest as hexadecimal text
   *        (case-insensitive); empty means "no checksum recorded"
   * @return NotProvided when expected_sha256_hex is empty, otherwise
   *         Match/Mismatch
   *
   * Thread safety: pure function, safe to call concurrently.
   */
  static ChecksumStatus verify_checksum_hex(
      std::string_view data, const std::string& expected_sha256_hex);

  /**
   * Resolve a GitHub releases URL to a concrete release asset for the target
   * platform.
   *
   * Accepted inputs include:
   * - https://github.com/<owner>/<repo>/releases
   * - https://github.com/<owner>/<repo>/releases/tag/<tag>
   */
  static ResolveReleaseAssetResult resolve_release_asset_from_releases_url(
      const std::string& releases_url, const std::string& target_platform,
      std::vector<std::string>* warnings = nullptr);

  /**
   * Resolve the cache directory for downloaded plugins.
   * Creates directory if it doesn't exist.
   *
   * @param error Optional error message accumulator
   * @return Cache directory path, or empty string on failure
   */
  static std::string resolve_plugin_cache_dir(std::string* error = nullptr);

  /**
   * Get the local cache path for a given asset without downloading.
   *
   * @param asset_name Expected filename
   * @param target_platform Platform identifier
   * @return Absolute path where asset would be cached
   */
  static std::string get_cache_path(const std::string& asset_name,
                                    const std::string& target_platform,
                                    const std::string& release_tag = "");
};

}  // namespace orc
