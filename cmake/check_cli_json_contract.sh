#!/usr/bin/env bash
# File:        check_cli_json_contract.sh
# Purpose:     Gate: every --json command emits a document a script can parse,
#              key on and feed straight back to the commands that take an
#              identifier.
#
# Usage:       check_cli_json_contract.sh <path-to-orc-cli>
#
# Checks, in the words of the scripting contract:
#   - the output parses (python3 -m json.tool);
#   - no key is a display label — keys hold no spaces and no upper case, and
#     the enum-valued fields carry stable lower-case identifiers;
#   - every object describing an addressable thing carries a `selector`
#     (plugins) or `name` (stages) the corresponding command accepts unchanged,
#     and no selector holds placeholder text.
#
# Registry-only: the fixture registry is built under a redirected
# HOME/XDG_CONFIG_HOME, the curated index is seeded as an empty last-good cache
# and its fetch pointed at a closed port, so nothing here touches the network.
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
    echo "SKIP: $PYTHON not available; cannot check the JSON contract" >&2
    exit 0
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home"
export APPDATA="$WORK_DIR/home"
mkdir -p "$HOME/decode-orc"

export ORC_PLUGIN_INDEX_URL="http://127.0.0.1:9/index.yaml"
printf 'registry_schema: 2\nplugins: []\n' \
    > "$HOME/decode-orc/plugin-index-cache.yaml"

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

# --- Helper: keys and enum values are stable identifiers, not labels ---------

cat > "$WORK_DIR/check_keys.py" <<'PYTHON_EOF'
"""Report every key or enum value in a JSON document that reads as a label."""
import json
import re
import sys

STABLE = re.compile(r"^[a-z0-9_]+$")

# Fields whose value is an enumeration: they carry the presenter's stable id,
# never the display label the aligned table prints.
ENUM_FIELDS = ("load_state", "severity", "status", "kind", "type",
               "trust_state", "artifact_source")

problems = []


def walk(node, path):
    if isinstance(node, dict):
        for name, value in node.items():
            here = "%s.%s" % (path, name)
            if not STABLE.match(name):
                problems.append("%s: key is not a stable identifier" % here)
            if (name in ENUM_FIELDS and isinstance(value, str) and value
                    and not STABLE.match(value)):
                problems.append(
                    "%s: enum value %r is not a stable identifier"
                    % (here, value))
            if name == "selector" and isinstance(value, str):
                if not value:
                    problems.append("%s: selector is empty" % here)
                elif "<" in value or ">" in value:
                    problems.append(
                        "%s: selector holds placeholder text %r"
                        % (here, value))
            walk(value, here)
    elif isinstance(node, list):
        for index, item in enumerate(node):
            walk(item, "%s[%d]" % (path, index))


walk(json.load(sys.stdin), sys.argv[1])
for problem in problems:
    print(problem)
sys.exit(1 if problems else 0)
PYTHON_EOF

# --- Fixture registry --------------------------------------------------------
#
# One named entry and one with no plugin id: the id-less entry is the case a
# plain `<id>` interface cannot address, so its selector has to survive the
# round trip like any other.

NAMED_PLUGIN="$WORK_DIR/named${PLUGIN_EXT}"
UNNAMED_PLUGIN="$WORK_DIR/unnamed${PLUGIN_EXT}"
touch "$NAMED_PLUGIN" "$UNNAMED_PLUGIN"

if ! "$ORC_CLI" plugins add "$NAMED_PLUGIN" --id com.example.named \
        --version 1.10 --yes </dev/null >/dev/null 2>&1; then
    fail "'plugins add' rejected the named fixture plugin"
fi
if ! "$ORC_CLI" plugins add "$UNNAMED_PLUGIN" --yes </dev/null \
        >/dev/null 2>&1; then
    fail "'plugins add' rejected the id-less fixture plugin"
fi

# --- Every --json command parses, and keys nothing as a label ----------------

check_document() {
    # $1 = document root name for messages, rest = orc-cli arguments
    local label="$1"
    shift
    local output
    output="$("$ORC_CLI" "$@" --json </dev/null 2>/dev/null)"
    if [[ $? -ne 0 || -z "$output" ]]; then
        fail "'$label --json' produced no output"
        return
    fi
    if ! printf '%s' "$output" | "$PYTHON" -m json.tool >/dev/null 2>&1; then
        fail "'$label --json' is not valid JSON"
        return
    fi
    local problems
    problems="$(printf '%s' "$output" \
        | "$PYTHON" "$WORK_DIR/check_keys.py" "$label" 2>&1)"
    if [[ $? -ne 0 ]]; then
        fail "'$label --json' names things a script cannot key on:
$problems"
    fi
}

check_document "plugins list" plugins list
check_document "plugins list --core" plugins list --core
check_document "plugins search" plugins search
check_document "plugins info" plugins info com.example.named
check_document "plugins updates" plugins updates
check_document "plugins doctor" plugins doctor
check_document "stages list" stages list --core
check_document "stages info" stages info tbc_source

# --- Every printed selector is accepted unchanged ----------------------------

SELECTORS="$("$ORC_CLI" plugins list --json </dev/null 2>/dev/null \
    | "$PYTHON" -c 'import json,sys
for entry in json.load(sys.stdin)["entries"]:
    print(entry["selector"])')"

if [[ -z "$SELECTORS" ]]; then
    fail "'plugins list --json' carried no selectors"
fi

SELECTOR_COUNT=0
while IFS= read -r selector; do
    [[ -z "$selector" ]] && continue
    SELECTOR_COUNT=$((SELECTOR_COUNT + 1))

    if ! "$ORC_CLI" plugins info "$selector" </dev/null \
            >/dev/null 2>&1; then
        fail "'plugins info' rejected the selector from --json: '$selector'"
        continue
    fi

    ECHOED="$("$ORC_CLI" plugins info "$selector" --json </dev/null 2>/dev/null \
        | "$PYTHON" -c 'import json,sys; print(json.load(sys.stdin)["selector"])')"
    if [[ "$ECHOED" != "$selector" ]]; then
        fail "'plugins info --json' echoed '$ECHOED' for selector '$selector'"
    fi

    for subcommand in enable disable; do
        if ! "$ORC_CLI" plugins "$subcommand" "$selector" --yes </dev/null \
                >/dev/null 2>&1; then
            fail "'plugins $subcommand' rejected the selector from --json: '$selector'"
        fi
    done
    if ! "$ORC_CLI" plugins remove --dry-run "$selector" </dev/null \
            >/dev/null 2>&1; then
        fail "'plugins remove --dry-run' rejected the selector from --json: '$selector'"
    fi
done <<EOF
$SELECTORS
EOF

if [[ "$SELECTOR_COUNT" -lt 2 ]]; then
    fail "expected at least the two fixture entries, saw $SELECTOR_COUNT"
fi

# Loaded plugins are addressable too — 'plugins info' accepts a loaded core
# plugin's id — so their objects carry a selector that must round-trip.
LOADED_SELECTORS="$("$ORC_CLI" plugins list --core --json </dev/null 2>/dev/null \
    | "$PYTHON" -c 'import json,sys
for plugin in json.load(sys.stdin)["loaded_plugins"]:
    print(plugin["selector"])')"

while IFS= read -r selector; do
    [[ -z "$selector" ]] && continue
    if ! "$ORC_CLI" plugins info "$selector" </dev/null >/dev/null 2>&1; then
        fail "'plugins info' rejected the loaded-plugin selector '$selector'"
    fi
done <<EOF
$LOADED_SELECTORS
EOF

# --- Every printed stage name is accepted unchanged --------------------------

STAGE_NAMES="$("$ORC_CLI" stages list --core --json </dev/null 2>/dev/null \
    | "$PYTHON" -c 'import json,sys
for stage in json.load(sys.stdin):
    print(stage["name"])')"

if [[ -z "$STAGE_NAMES" ]]; then
    fail "'stages list --json' carried no stage names"
fi

STAGE_COUNT=0
while IFS= read -r stage; do
    [[ -z "$stage" ]] && continue
    STAGE_COUNT=$((STAGE_COUNT + 1))

    if ! "$ORC_CLI" stages info "$stage" --json </dev/null \
            > "$WORK_DIR/stage.json" 2>/dev/null; then
        fail "'stages info' rejected the name from --json: '$stage'"
        continue
    fi
    ECHOED="$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' \
        "$WORK_DIR/stage.json")"
    if [[ "$ECHOED" != "$stage" ]]; then
        fail "'stages info --json' echoed '$ECHOED' for stage '$stage'"
    fi

done <<EOF
$STAGE_NAMES
EOF

if [[ "$STAGE_COUNT" -lt 1 ]]; then
    fail "no core stage was checked"
fi

echo "Checked $SELECTOR_COUNT plugin selector(s) and $STAGE_COUNT stage(s)."

if [[ "$FAILURES" -eq 0 ]]; then
    echo "CLI JSON contract: passed"
    exit 0
fi

echo "CLI JSON contract: failed ($FAILURES violation(s))" >&2
exit 1
