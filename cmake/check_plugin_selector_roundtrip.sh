#!/usr/bin/env bash
# File:        check_plugin_selector_roundtrip.sh
# Purpose:     Gate: every identifier the CLI prints is accepted verbatim by the
#              commands that take one — plugin selectors from `plugins list` and
#              stage names from `stages list` — and no selector field holds
#              placeholder text.
#
# Usage:       check_plugin_selector_roundtrip.sh <path-to-orc-cli>
#
# Registry-only: the fixture registry is built with `plugins add` under a
# redirected HOME/XDG_CONFIG_HOME, and no subcommand exercised here touches the
# network or the curated index.
# Kept bash-3.2 compatible.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

set -u

ORC_CLI="${1:-}"
if [[ -z "$ORC_CLI" || ! -x "$ORC_CLI" ]]; then
    echo "Usage: $0 <path-to-orc-cli>" >&2
    exit 1
fi

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "SKIP: $PYTHON not available; cannot read the --json listings" >&2
    exit 0
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home"
mkdir -p "$HOME"

# 'info' describes an installed plugin as well as an indexed one, so it may
# consult the curated index. Point that fetch at a closed port: every selector
# checked here is a registry one, and must resolve without the index. The
# proxies point at the same closed port so no fetch can leave the machine even
# if the index URL override is ever renamed.
export ORC_PLUGIN_INDEX_URL="http://127.0.0.1:9/index.yaml"
export http_proxy="http://127.0.0.1:9"
export https_proxy="http://127.0.0.1:9"
export all_proxy="http://127.0.0.1:9"
export no_proxy=""

case "$(uname -s)" in
    Darwin) PLUGIN_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*) PLUGIN_EXT=".dll" ;;
    *) PLUGIN_EXT=".so" ;;
esac

FAILURES=0

fail() {
    echo "FAIL: $*" >&2
    FAILURES=$((FAILURES + 1))
}

# --- Fixture registry --------------------------------------------------------
#
# One named entry and one with no plugin id at all. The id-less entry is the
# case a plain `<id>` interface cannot address.

NAMED_PLUGIN="$WORK_DIR/named${PLUGIN_EXT}"
UNNAMED_PLUGIN="$WORK_DIR/unnamed${PLUGIN_EXT}"
touch "$NAMED_PLUGIN" "$UNNAMED_PLUGIN"

# --yes stands in for the trust confirmation: adding a plugin grants trust, and
# there is no terminal to ask on here.
if ! "$ORC_CLI" plugins add "$NAMED_PLUGIN" --id com.example.named \
        --version 1.0.0 --yes >/dev/null; then
    fail "'plugins add' rejected the named fixture plugin"
fi
if ! "$ORC_CLI" plugins add "$UNNAMED_PLUGIN" --yes >/dev/null; then
    fail "'plugins add' rejected the id-less fixture plugin"
fi

# --- Plugin selectors --------------------------------------------------------
#
# The machine-readable listing is the contract's own statement of what a script
# should feed back, so the round trip starts from `--json`, not from the table.

LIST_JSON="$("$ORC_CLI" plugins list --json 2>/dev/null)"
SELECTORS="$(printf '%s' "$LIST_JSON" | "$PYTHON" -c '
import json, sys
for entry in json.load(sys.stdin)["entries"]:
    print(entry["selector"])
' 2>/dev/null)"

if [[ -z "$SELECTORS" ]]; then
    fail "'plugins list --json' carried no selectors"
fi

SELECTOR_COUNT=0
while IFS= read -r selector; do
    [[ -z "$selector" ]] && continue
    SELECTOR_COUNT=$((SELECTOR_COUNT + 1))

    # Placeholder text is banned from selector fields: a `<...>` stand-in is a
    # string no command accepts.
    case "$selector" in
        *"<"*|*">"*)
            fail "'plugins list' printed a placeholder as a selector: '$selector'"
            ;;
    esac

    # Every command that takes a selector must accept this one unchanged.
    for subcommand in enable disable trust untrust; do
        if ! "$ORC_CLI" plugins "$subcommand" "$selector" --yes \
                >/dev/null 2>&1; then
            fail "'plugins $subcommand' rejected printed selector '$selector'"
        fi
    done

    if ! "$ORC_CLI" plugins remove --dry-run "$selector" >/dev/null 2>&1; then
        fail "'plugins remove --dry-run' rejected printed selector '$selector'"
    fi

    # 'update' must accept the selector too. These local fixtures have no
    # source repository, so the update itself refuses — but with the generic
    # code, never with not-found: a printed selector must always resolve.
    "$ORC_CLI" plugins update "$selector" --yes >/dev/null 2>&1
    if [[ $? -eq 2 ]]; then
        fail "'plugins update' did not resolve printed selector '$selector'"
    fi

    # 'info' must describe an installed entry, and echo back a selector that is
    # itself usable as input.
    if ! INFO_OUTPUT="$("$ORC_CLI" plugins info "$selector" 2>/dev/null)"; then
        fail "'plugins info' rejected printed selector '$selector'"
    elif ! echo "$INFO_OUTPUT" | grep -q "^selector: *$selector\$"; then
        fail "'plugins info $selector' did not echo the selector back"
    fi
done <<EOF
$SELECTORS
EOF

if [[ "$SELECTOR_COUNT" -lt 2 ]]; then
    fail "expected at least the two fixture entries, saw $SELECTOR_COUNT"
fi

# A dry run must not have written anything.
LIST_AFTER="$("$ORC_CLI" plugins list --json 2>/dev/null \
    | "$PYTHON" -c 'import json,sys; print(len(json.load(sys.stdin)["entries"]))' \
    2>/dev/null)"
if [[ "$LIST_AFTER" != "$SELECTOR_COUNT" ]]; then
    fail "entry count changed during the round trip ($SELECTOR_COUNT -> $LIST_AFTER)"
fi

# --- Loaded core plugins -----------------------------------------------------
#
# Core plugins are discovered rather than registered, so they list under
# loaded_plugins, not entries — but 'stages list' prints their ids as the
# owning plugin, so 'plugins info' must accept those ids too (the loaded-
# plugin fallback path). Mutators rightly refuse them; only the describing
# commands are held to the round trip here.

LOADED_SELECTORS="$("$ORC_CLI" plugins list --core --json 2>/dev/null \
    | "$PYTHON" -c '
import json, sys
for plugin in json.load(sys.stdin)["loaded_plugins"]:
    print(plugin["selector"])
' 2>/dev/null)"

if [[ -z "$LOADED_SELECTORS" ]]; then
    fail "'plugins list --core --json' reported no loaded core plugins"
fi

while IFS= read -r selector; do
    [[ -z "$selector" ]] && continue
    case "$selector" in
        *"<"*|*">"*)
            fail "loaded_plugins carried a placeholder selector: '$selector'"
            ;;
    esac
    if ! "$ORC_CLI" plugins info "$selector" >/dev/null 2>&1; then
        fail "'plugins info' rejected loaded plugin selector '$selector'"
    fi
done <<EOF
$LOADED_SELECTORS
EOF

# --- Placeholder sweep over the human listing --------------------------------
#
# The table is input too: its selector lines must never hold a `<...>`
# stand-in (the old `<unnamed>` bug), on the core listing included.

if "$ORC_CLI" plugins list --core 2>/dev/null \
        | grep -E "^  (selector|id):" | grep -q "<"; then
    fail "'plugins list --core' printed placeholder text in a selector field"
fi

# --- Stage names -------------------------------------------------------------
#
# The same contract, one object further out: the name `stages list` prints is
# the token every stage-taking command accepts and a project's `stage:` key
# holds. A display name printed where a name belongs is caught here.

STAGE_NAMES="$("$ORC_CLI" stages list --core --json 2>/dev/null \
    | "$PYTHON" -c '
import json, sys
for stage in json.load(sys.stdin):
    print(stage["name"])
' 2>/dev/null)"

if [[ -z "$STAGE_NAMES" ]]; then
    fail "'stages list --core --json' carried no stage names"
fi

STAGE_COUNT=0
while IFS= read -r stage; do
    [[ -z "$stage" ]] && continue
    STAGE_COUNT=$((STAGE_COUNT + 1))

    # A stage id is one word; anything else — a display name above all — is
    # not what a project file or `nodes` command expects.
    case "$stage" in
        *[!A-Za-z0-9_]*)
            fail "'stages list' printed '$stage', which is not a stage id"
            continue
            ;;
    esac

    if ! "$ORC_CLI" stages info "$stage" >/dev/null 2>&1; then
        fail "'stages info' rejected printed stage name '$stage'"
    fi

    # A stage with no instructions is a documented non-zero exit, not a
    # rejected name; only "no such stage" is a round-trip failure.
    "$ORC_CLI" stages help "$stage" >/dev/null 2>&1
    if [[ $? -eq 2 ]]; then
        fail "'stages help' did not recognise printed stage name '$stage'"
    fi

done <<EOF
$STAGE_NAMES
EOF

if [[ "$STAGE_COUNT" -lt 1 ]]; then
    fail "expected at least one core stage, saw $STAGE_COUNT"
fi

# --- Owning plugin ids -------------------------------------------------------
#
# The owning plugin id 'stages list' prints is a plugin selector: the same
# string must be taken by 'plugins info' and by 'stages list --plugin'.

OWNING_IDS="$("$ORC_CLI" stages list --core --json 2>/dev/null \
    | "$PYTHON" -c '
import json, sys
seen = set()
for stage in json.load(sys.stdin):
    owner = stage.get("owning_plugin_id", "")
    if owner and owner not in seen:
        seen.add(owner)
        print(owner)
' 2>/dev/null)"

if [[ -z "$OWNING_IDS" ]]; then
    fail "'stages list --core --json' reported no owning plugin ids"
fi

while IFS= read -r owner; do
    [[ -z "$owner" ]] && continue
    if ! "$ORC_CLI" plugins info "$owner" >/dev/null 2>&1; then
        fail "'plugins info' rejected owning plugin id '$owner'"
    fi
    if ! "$ORC_CLI" stages list --core --plugin "$owner" >/dev/null 2>&1; then
        fail "'stages list --plugin' rejected owning plugin id '$owner'"
    fi
done <<EOF
$OWNING_IDS
EOF

# --- Selectors unique to the id-less entry -----------------------------------
#
# The id-less entry is reachable only through its path selector; removing it
# is the destructive end of the round trip, so it runs last.

if ! "$ORC_CLI" plugins remove "path:$UNNAMED_PLUGIN" >/dev/null; then
    fail "'plugins remove path:<path>' could not remove the id-less entry"
fi
if "$ORC_CLI" plugins list | grep -q "path:$UNNAMED_PLUGIN"; then
    fail "'plugins remove path:<path>' left the entry in the registry"
fi

# An unknown selector must fail rather than remove something else.
if "$ORC_CLI" plugins remove com.example.absent >/dev/null 2>&1; then
    fail "'plugins remove' accepted a selector that matches nothing"
fi

if [[ "$FAILURES" -eq 0 ]]; then
    echo "Selector round trip: passed"
    exit 0
fi

echo "Selector round trip: failed ($FAILURES violation(s))" >&2
exit 1
