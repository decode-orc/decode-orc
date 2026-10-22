#!/usr/bin/env bash
# File:        check_cli_nodes.sh
# Purpose:     Functional check of `orc-cli nodes list / get / set / reset`
#              against real project files: addressing by id and by name,
#              typed and range-checked values, the exit-code contract, saving
#              in place versus --output, resets to defaults and to what a
#              Video Parameters stage's input reports, and the renaming of
#              duplicate stage names when an older project is read.
#
# Usage:       check_cli_nodes.sh <path-to-orc-cli>
#
# Writes only into a temporary directory. No network and no capture files:
# most sources are left unconfigured, which those commands accept, and the
# Video Parameters reset reads one blank frame the script writes itself.
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

# --- reset --------------------------------------------------------------------

run 0 "nodes reset an ordinary stage" \
    nodes reset p.orcprj "Video Sink" --output reset.orcprj
expect_out "nodes reset an ordinary stage" "to their defaults"
run 0 "nodes get after reset" nodes get reset.orcprj "Video Sink" --json
if grep -qF '"value": 22' "$WORK_DIR/out"; then
    fail "nodes reset: encoder_crf kept its set value"
fi
run 2 "nodes reset an unknown stage" nodes reset p.orcprj "No Such Stage"

# One blank PAL frame (709379 samples of 10-bit 4fsc, two bytes each) with its
# encoding named, so the source needs no .meta sidecar. The first Video
# Parameters overrides the black level; the second must reset to that value,
# which is what its input reports, rather than to the format's standard 256.
head -c 1418758 /dev/zero > blank.cvbs
run 0 "export the Video Parameters project" \
    --source "PAL_CVBS_Source=input_path=blank.cvbs:sample_encoding=CVBS_U10_4FSC" \
    --filters "video_params=blackLevel=300,video_params" \
    --sink video_sink=output_path=out.mp4 \
    --video-format PAL --source-type composite --export-project vp.orcprj
run 0 "nodes set the second Video Parameters" \
    nodes set vp.orcprj "Video Parameters 2" blackLevel=500
run 0 "nodes reset Video Parameters" nodes reset vp.orcprj "Video Parameters 2"
expect_out "nodes reset Video Parameters" "the values its input reports"
run 0 "nodes get after the Video Parameters reset" \
    nodes get vp.orcprj "Video Parameters 2" --json
expect_out "nodes get after the Video Parameters reset" '"value": 300'

# With no capture to read there is nothing to reset to, and nothing is saved.
run 0 "nodes set an unreadable capture" \
    nodes set vp.orcprj "CVBS Source" input_path=missing.cvbs
cp vp.orcprj vp.before
run 5 "nodes reset Video Parameters with an unreadable input" \
    nodes reset vp.orcprj "Video Parameters 2"
expect_err "nodes reset Video Parameters with an unreadable input" \
    "could not be read"
if ! cmp -s vp.orcprj vp.before; then
    fail "a refused 'nodes reset' changed the project file"
fi

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
