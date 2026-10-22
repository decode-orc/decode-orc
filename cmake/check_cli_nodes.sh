#!/usr/bin/env bash
# File:        check_cli_nodes.sh
# Purpose:     Functional check of `orc-cli nodes list / get / set` against a
#              real project file: addressing by id and by name, typed and
#              range-checked values, the exit-code contract, saving in place
#              versus --output, and the renaming of duplicate stage names when
#              an older project is read.
#
# Usage:       check_cli_nodes.sh <path-to-orc-cli>
#
# Writes only into a temporary directory. No network and no capture files: the
# project's sources are left unconfigured, which every command here accepts.
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

# --- A template project -------------------------------------------------------

run 0 "export the template" \
    --source tbc_source --filters frame_map \
    --sink video_sink=output_path=out.mp4 \
    --video-format PAL --source-type composite --export-project p.orcprj
if [[ ! -f p.orcprj ]]; then
    fail "the template project was not written"
    echo "check_cli_nodes: failed ($FAILURES)" >&2
    exit 1
fi

# --- list ---------------------------------------------------------------------

run 0 "nodes list" nodes list p.orcprj
expect_out "nodes list" "Frame Map"
expect_out "nodes list" "field_mapping"

run 0 "nodes list --json" nodes list p.orcprj --json
if [[ "$(head -c 1 "$WORK_DIR/out")" != "[" ]]; then
    fail "nodes list --json: stdout is not a JSON array"
fi
expect_out "nodes list --json" '"label": "Video Sink"'
expect_out "nodes list --json" '"status": '

run 1 "nodes list on a missing project" nodes list missing.orcprj

# --- get ----------------------------------------------------------------------

run 0 "nodes get by name" nodes get p.orcprj "Video Sink"
expect_out "nodes get by name" "encoder_crf"
run 0 "nodes get by id" nodes get p.orcprj 3
expect_out "nodes get by id" "video_sink"
run 2 "nodes get with an unknown name" nodes get p.orcprj "No Such Stage"
expect_err "nodes get with an unknown name" "Video Sink"
run 2 "nodes get with an unknown id" nodes get p.orcprj 99

# --- set ----------------------------------------------------------------------

cp p.orcprj before.orcprj

run 1 "nodes set with a value of the wrong type" \
    nodes set p.orcprj "Video Sink" encoder_crf=abc
run 1 "nodes set with a value out of range" \
    nodes set p.orcprj "Video Sink" encoder_crf=99
run 1 "nodes set with a value that is not allowed" \
    nodes set p.orcprj "Video Sink" ffmpeg_format=not-a-format
run 2 "nodes set with an unknown key" \
    nodes set p.orcprj "Video Sink" no_such_key=1
run 2 "nodes set with a Y/C key on a composite project" \
    nodes set p.orcprj "TBC Source" y_path=a.tbcy c_path=a.tbcc
expect_err "nodes set with a Y/C key on a composite project" \
    "does not apply to a composite project"
if ! cmp -s p.orcprj before.orcprj; then
    fail "a refused 'nodes set' changed the project file"
fi

run 0 "nodes set --output" \
    nodes set p.orcprj "Video Sink" encoder_crf=20 --output q.orcprj
if ! cmp -s p.orcprj before.orcprj; then
    fail "'nodes set --output' changed the original project"
fi
run 0 "nodes get on the --output copy" nodes get q.orcprj 3 --json
expect_out "nodes get on the --output copy" '"value": 20'

run 0 "nodes set in place" nodes set p.orcprj 3 encoder_crf=22 hardware_encoder=none
run 0 "nodes get after set" nodes get p.orcprj "Video Sink" --json
expect_out "nodes get after set" '"value": 22'

# --- Duplicate names in an older project ----------------------------------------

cat > old.orcprj <<'EOF'
project:
  name: old
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 1
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: Frame Map
      x: 0
      y: 0
    - id: 2
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: Frame Map
      x: 0
      y: 0
  edges: []
EOF
cp old.orcprj old.before
run 0 "nodes list on duplicate names" nodes list old.orcprj
expect_out "nodes list on duplicate names" "Frame Map 1"
expect_out "nodes list on duplicate names" "Frame Map 2"
if ! cmp -s old.orcprj old.before; then
    fail "'nodes list' rewrote a project whose names it renamed"
fi
run 0 "nodes get by a renamed name" nodes get old.orcprj "Frame Map 2"

if [[ "$FAILURES" -eq 0 ]]; then
    echo "check_cli_nodes: passed"
    exit 0
fi
echo "check_cli_nodes: failed ($FAILURES)" >&2
exit 1
