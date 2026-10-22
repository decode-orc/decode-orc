#!/usr/bin/env bash
# File:        check_cli_tools.sh
# Purpose:     Functional check of `orc-cli tools list / run` against real
#              project files: listing, the refusals and their exit codes, and
#              that a refused run leaves the project untouched.
#
# Usage:       check_cli_tools.sh <path-to-orc-cli>
#
# Writes only into a temporary directory. No network. The repository carries
# no capture short enough to analyse, so a real Disc Mapper run happens only
# when ORC_CLI_TOOLS_TBC names a composite PAL or NTSC .tbc file (with its
# .tbc.db or .tbc.json metadata beside it); without it that part is skipped.
# Kept bash-3.2 compatible.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors

set -u

ORC_CLI="${1:-}"
if [[ -z "$ORC_CLI" || ! -x "$ORC_CLI" ]]; then
    echo "Usage: $0 <path-to-orc-cli>" >&2
    exit 1
fi
# The checks run from inside the work directory
ORC_CLI="$(cd "$(dirname "$ORC_CLI")" && pwd)/$(basename "$ORC_CLI")"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home"
mkdir -p "$HOME"
cd "$WORK_DIR" || exit 1

FAILURES=0

fail() {
    echo "FAIL: $*" >&2
    FAILURES=$((FAILURES + 1))
}

# run <expected-exit> <description> <args...>: runs orc-cli with stdout in
# $WORK_DIR/out and stderr in $WORK_DIR/err, and checks the exit code.
run() {
    local expected="$1"
    local description="$2"
    shift 2
    "$ORC_CLI" "$@" > "$WORK_DIR/out" 2> "$WORK_DIR/err"
    local actual=$?
    if [[ "$actual" -ne "$expected" ]]; then
        fail "$description: expected exit $expected, got $actual"
        sed 's/^/    /' "$WORK_DIR/err" >&2
    fi
}

expect_out() {
    if ! grep -qF -- "$2" "$WORK_DIR/out"; then
        fail "$1: stdout does not contain '$2'"
    fi
}

expect_err() {
    if ! grep -qF -- "$2" "$WORK_DIR/err"; then
        fail "$1: stderr does not contain '$2'"
    fi
}

# --- Templates ----------------------------------------------------------------

run 0 "export the template" \
    --source tbc_source --filters frame_map \
    --sink video_sink=output_path=out.mp4 \
    --video-format PAL --source-type composite --export-project p.orcprj
run 0 "export the stream template" \
    --source "pal_tbc_stream_source=input_path=-" --filters frame_map \
    --sink video_sink=output_path=out.mp4 \
    --source-type composite --export-project stream.orcprj
if [[ ! -f p.orcprj || ! -f stream.orcprj ]]; then
    fail "the template projects were not written"
    echo "check_cli_tools: failed ($FAILURES)" >&2
    exit 1
fi
cp p.orcprj p.before

# --- list ---------------------------------------------------------------------

run 0 "tools list" tools list p.orcprj
expect_out "tools list" "field_mapping"
expect_out "tools list" "Disc Mapper"
expect_out "tools list" "--param include_lead_in_out"
expect_out "tools list" "ffmpeg_preset_config"

run 0 "tools list one node" tools list p.orcprj "Frame Map"
expect_out "tools list one node" "frame_map_range"
if grep -qF "ffmpeg_preset_config" "$WORK_DIR/out"; then
    fail "tools list one node: listed another node's tools"
fi

run 0 "tools list --json" tools list p.orcprj --json
if [[ "$(head -c 1 "$WORK_DIR/out")" != "[" ]]; then
    fail "tools list --json: stdout is not a JSON array"
fi
expect_out "tools list --json" '"id": "field_mapping"'
expect_out "tools list --json" '"can_apply": true'
expect_out "tools list --json" '"runnable": false'

run 2 "tools list an unknown node" tools list p.orcprj "No Such Stage"
expect_err "tools list an unknown node" "Frame Map"

# --- run: refusals ------------------------------------------------------------

run 2 "run a tool the node does not offer" \
    tools run p.orcprj source_alignment --node "Frame Map"
expect_err "run a tool the node does not offer" "field_mapping"

run 2 "run on an unknown node" \
    tools run p.orcprj field_mapping --node "No Such Stage"

run 1 "run without --node" tools run p.orcprj field_mapping

run 1 "run an interactive tool" \
    tools run p.orcprj ffmpeg_preset_config --node "Video Sink"
expect_err "run an interactive tool" "nodes set"

run 2 "run with an unknown parameter" \
    tools run p.orcprj field_mapping --node "Frame Map" --param bogus=1
expect_err "run with an unknown parameter" "tools list"

run 1 "run with a malformed parameter value" \
    tools run p.orcprj field_mapping --node "Frame Map" \
    --param include_lead_in_out=maybe

run 1 "--output without --apply" \
    tools run p.orcprj field_mapping --node "Frame Map" --output other.orcprj

run 1 "--csv on a tool that is not an analysis sink" \
    tools run p.orcprj field_mapping --node "Frame Map" --csv out.csv

run 5 "run with the source not configured" \
    tools run p.orcprj field_mapping --node "Frame Map" --apply
expect_err "run with the source not configured" "not configured"

run 5 "run behind a stream source" \
    tools run stream.orcprj field_mapping --node "Frame Map"
expect_err "run behind a stream source" "stream source"

if ! cmp -s p.orcprj p.before; then
    fail "a refused run changed the project"
fi
if [[ -e other.orcprj ]]; then
    fail "a refused run wrote --output"
fi

# --- run: a real capture (optional) -------------------------------------------

if [[ -n "${ORC_CLI_TOOLS_TBC:-}" ]]; then
    run 0 "point the template at the capture" \
        nodes set p.orcprj "TBC Source" input_path="$ORC_CLI_TOOLS_TBC"
    cp p.orcprj p.before

    run 0 "Disc Mapper dry run" \
        tools run p.orcprj field_mapping --node "Frame Map" --quiet
    expect_out "Disc Mapper dry run" "Would set:"
    if ! cmp -s p.orcprj p.before; then
        fail "a dry run changed the project"
    fi

    run 0 "Disc Mapper --apply --output" \
        tools run p.orcprj field_mapping --node "Frame Map" --quiet \
        --apply --output mapped.orcprj --json
    expect_out "Disc Mapper --apply --output" '"saved": true'
    expect_out "Disc Mapper --apply --output" '"ranges":'
    if ! grep -qF "ranges:" mapped.orcprj; then
        fail "Disc Mapper --apply: ranges not written to the saved project"
    fi
    if ! cmp -s p.orcprj p.before; then
        fail "--apply --output changed the original project"
    fi
else
    echo "check_cli_tools: ORC_CLI_TOOLS_TBC not set; skipping the capture run"
fi

if [[ "$FAILURES" -eq 0 ]]; then
    echo "check_cli_tools: passed"
    exit 0
fi
echo "check_cli_tools: failed ($FAILURES)" >&2
exit 1
