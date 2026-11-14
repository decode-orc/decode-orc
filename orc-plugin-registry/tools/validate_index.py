#!/usr/bin/env python3
#
# File:        validate_index.py
# Module:      orc-plugin-registry
# Purpose:     Validate a curated plugin index document
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Offline checks (default): schema conformance, SPDX license present, and a
# GitHub source_repo_url present. Schema 2 curates plugins without pinning
# versions, so pinned `artifacts` lists are rejected.
# With --online: additionally query each repository's latest GitHub release
# and verify it publishes at least one asset that follows the plugin artifact
# naming convention.
#
# Exit status is non-zero when any error is found; every error is printed.

import argparse
import json
import re
import sys

import yaml

KNOWN_SCHEMA_MAJOR = 2
# orc-plugin_<stage>_<platform>[-<arch>][_abi<N>].<ext>
# The <stage> token may itself contain underscores (e.g. "skeleton_passthrough"),
# matching the host's authoritative parser segment class in
# orc/core/plugin_artifact_name.cpp.
ASSET_NAME_RE = re.compile(
    r"^orc-plugin_[A-Za-z0-9._\-]+_(linux|macos|windows)"
    r"(-[A-Za-z0-9_]+)?(_abi[0-9]+)?\.(so|dylib|dll)$"
)
# A permissive SPDX identifier shape (not the full SPDX grammar).
SPDX_RE = re.compile(r"^[A-Za-z0-9.\-+]+$")

# The view Qt baseline: the oldest Qt minor any released orc-gui runs (the
# Flatpak's KDE runtime and the Windows build, both Qt 6.9). orc-gui refuses a
# view built for a newer minor than its own, so a release whose view declares
# a newer `qt` loses its window on those hosts. Keep in step with
# runtime-version in io.github.decode_orc.decode-orc.yml and with
# docs/technical/plugin-sdk.md ("View Qt baseline").
VIEW_QT_BASELINE = (6, 9)
VIEW_FILE_PREFIX = "orc-view_"


def view_warnings(pid, manifest):
    """Warnings about the view entries of one release manifest."""
    warnings = []
    for artifact in (manifest or {}).get("artifacts", []) or []:
        if not isinstance(artifact, dict) or artifact.get("kind") != "view":
            continue
        name = str(artifact.get("file", ""))
        platform = artifact.get("platform", "?")
        if not name.startswith(VIEW_FILE_PREFIX):
            warnings.append(
                "plugin '%s': view '%s' does not start with '%s'; hosts will "
                "not install it" % (pid, name, VIEW_FILE_PREFIX))
        qt = str(artifact.get("qt", ""))
        match = re.match(r"^(\d+)\.(\d+)$", qt)
        if not match:
            warnings.append(
                "plugin '%s': view '%s' declares no qt \"major.minor\""
                % (pid, name))
            continue
        version = (int(match.group(1)), int(match.group(2)))
        if version[0] != VIEW_QT_BASELINE[0] or version > VIEW_QT_BASELINE:
            warnings.append(
                "plugin '%s': %s view '%s' is built for Qt %s, newer than the "
                "view baseline Qt %d.%d; released orc-gui builds on Qt %d.%d "
                "will not load it"
                % (pid, platform, name, qt, VIEW_QT_BASELINE[0],
                   VIEW_QT_BASELINE[1], VIEW_QT_BASELINE[0],
                   VIEW_QT_BASELINE[1]))
    return warnings
GITHUB_REPO_RE = re.compile(
    r"^https?://(www\.)?github\.com/[^/]+/[^/?#]+/?$"
)


def validate_plugin(errors, index, plugin):
    where = "plugin[%d]" % index
    if not isinstance(plugin, dict):
        errors.append("%s: entry is not a mapping" % where)
        return

    plugin_id = plugin.get("id", "")
    if not plugin_id:
        errors.append("%s: missing 'id'" % where)
        plugin_id = "<unknown>"

    license_spdx = plugin.get("license_spdx", "")
    if not license_spdx:
        errors.append("plugin '%s': missing 'license_spdx'" % plugin_id)
    elif not SPDX_RE.match(str(license_spdx)):
        errors.append("plugin '%s': 'license_spdx' is not a valid SPDX id"
                      % plugin_id)

    repo = plugin.get("source_repo_url", "")
    if not repo:
        errors.append("plugin '%s': missing 'source_repo_url'" % plugin_id)
    elif not GITHUB_REPO_RE.match(str(repo)):
        errors.append(
            "plugin '%s': 'source_repo_url' must be a GitHub repository URL "
            "(https://github.com/<owner>/<repo>)" % plugin_id)

    if "artifacts" in plugin:
        errors.append(
            "plugin '%s': schema %d does not pin artifacts; remove the "
            "'artifacts' list — the host resolves the latest GitHub release "
            "at runtime" % (plugin_id, KNOWN_SCHEMA_MAJOR))


def validate_document(doc):
    errors = []
    if not isinstance(doc, dict):
        return ["index root is not a mapping"]

    schema = doc.get("registry_schema")
    if not isinstance(schema, int):
        errors.append("'registry_schema' must be an integer")
    elif schema < KNOWN_SCHEMA_MAJOR or schema > KNOWN_SCHEMA_MAJOR:
        errors.append("'registry_schema' %r is not a supported major version "
                      "(this validator understands %d)"
                      % (schema, KNOWN_SCHEMA_MAJOR))

    plugins = doc.get("plugins", [])
    if plugins is None:
        plugins = []
    if not isinstance(plugins, list):
        errors.append("'plugins' must be a list")
        return errors
    for i, plugin in enumerate(plugins):
        validate_plugin(errors, i, plugin)
    return errors


def latest_release_api_url(repo):
    match = re.match(
        r"^https?://(?:www\.)?github\.com/([^/]+)/([^/?#]+?)(?:\.git)?/?$",
        str(repo))
    if not match:
        return None
    return ("https://api.github.com/repos/%s/%s/releases/latest"
            % (match.group(1), match.group(2)))


def verify_online(doc, warnings):
    import urllib.request

    errors = []
    for plugin in doc.get("plugins", []) or []:
        pid = plugin.get("id", "<unknown>")
        repo = plugin.get("source_repo_url", "")
        api_url = latest_release_api_url(repo)
        if not api_url:
            continue  # offline checks already reported a bad URL
        request = urllib.request.Request(
            api_url, headers={"User-Agent": "decode-orc-index-validator"})
        try:
            with urllib.request.urlopen(request, timeout=60) as resp:
                release = json.load(resp)
        except Exception as exc:  # noqa: BLE001 - report any transport error
            errors.append(
                "plugin '%s': latest release unreachable at %s (%s)"
                % (pid, api_url, exc))
            continue
        assets = [a.get("name", "") for a in release.get("assets", []) or []]
        if not any(ASSET_NAME_RE.match(name) for name in assets):
            errors.append(
                "plugin '%s': latest release %s publishes no asset matching "
                "orc-plugin_<stage>_<platform>[_abi<N>].<ext> (assets: %s)"
                % (pid, release.get("tag_name", "<untagged>"),
                   ", ".join(assets) or "<none>"))
        # The release manifest's view entries, when it has any. Problems here
        # are warnings: the stage binary still installs.
        manifest_url = next(
            (a.get("browser_download_url") for a in
             release.get("assets", []) or []
             if a.get("name") == "orc-plugin-manifest.yaml"), None)
        if manifest_url:
            try:
                manifest_request = urllib.request.Request(
                    manifest_url,
                    headers={"User-Agent": "decode-orc-index-validator"})
                with urllib.request.urlopen(manifest_request,
                                            timeout=60) as resp:
                    manifest = yaml.safe_load(resp.read().decode("utf-8"))
                warnings += view_warnings(pid, manifest)
            except Exception as exc:  # noqa: BLE001 - report, do not fail
                warnings.append(
                    "plugin '%s': could not read the release manifest (%s)"
                    % (pid, exc))
    return errors


def main(argv):
    parser = argparse.ArgumentParser(description="Validate a plugin index")
    parser.add_argument("index", help="path to index.yaml")
    parser.add_argument("--online", action="store_true",
                        help="also verify each repository's latest release "
                             "publishes a conforming plugin asset")
    args = parser.parse_args(argv)

    try:
        with open(args.index, "r", encoding="utf-8") as handle:
            doc = yaml.safe_load(handle)
    except (OSError, yaml.YAMLError) as exc:
        print("error: failed to load %s: %s" % (args.index, exc),
              file=sys.stderr)
        return 2

    errors = validate_document(doc)
    warnings = []
    if not errors and args.online:
        errors += verify_online(doc, warnings)

    for message in warnings:
        print("warning: " + message, file=sys.stderr)

    if errors:
        for message in errors:
            print("error: " + message, file=sys.stderr)
        print("FAILED: %d problem(s) in %s" % (len(errors), args.index),
              file=sys.stderr)
        return 1

    print("OK: %s is valid" % args.index)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
