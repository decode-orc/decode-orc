#!/usr/bin/env bash
# File:        check_cli_preview.sh
# Purpose:     Functional check of `orc-cli preview outputs / render / compare`
#              against a real project: the outputs listing, a PNG render, an
#              A/B comparison written as a two-frame animated WebP (with its
#              difference PNG), the identical-pair note, and the refusals and
#              their exit codes.
#
# Usage:       check_cli_preview.sh <path-to-orc-cli> <template-project-directory>
#
# The template is orc-tests/fixtures/cli-projects/cvbs_video_params_video.orcprj:
# CVBS Source -> Video Parameters (black 300) -> Video Parameters 2 -> Video
# Sink. The script writes the one-frame capture it reads, a flat mid-level
# signal, so the source and the black-300 Video Parameters stage show
# different greys. Writes only into a temporary directory. No network.
# Kept bash-3.2 compatible.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

set -u

ORC_CLI="${1:-}"
FIXTURES="${2:-}"
if [[ -z "$ORC_CLI" || ! -x "$ORC_CLI" || ! -d "$FIXTURES" ]]; then
    echo "Usage: $0 <path-to-orc-cli> <template-project-directory>" >&2
    exit 1
fi
ORC_CLI="$(cd "$(dirname "$ORC_CLI")" && pwd)/$(basename "$ORC_CLI")"
FIXTURES="$(cd "$FIXTURES" && pwd)"

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

expect_no_out() {
    if grep -qF -- "$2" "$WORK_DIR/out"; then
        fail "$1: stdout contains '$2'"
    fi
}

expect_err() {
    if ! grep -qF -- "$2" "$WORK_DIR/err"; then
        fail "$1: stderr does not contain '$2'"
    fi
}

# --- Reading the files written ------------------------------------------------

file_size() {
    wc -c < "$1" | tr -d ' '
}

# fourcc <file> <offset>
fourcc() {
    dd if="$1" bs=1 skip="$2" count=4 2>/dev/null
}

# le <file> <offset> <bytes>: an unsigned little-endian number.
le() {
    local value=0 shift_by=0 byte
    for byte in $(od -An -tu1 -j "$2" -N "$3" "$1"); do
        value=$((value + (byte << shift_by)))
        shift_by=$((shift_by + 8))
    done
    echo "$value"
}

# be <file> <offset> <bytes>: an unsigned big-endian number.
be() {
    local value=0 byte
    for byte in $(od -An -tu1 -j "$2" -N "$3" "$1"); do
        value=$(((value << 8) + byte))
    done
    echo "$value"
}

# webp_frames <file>: walks the RIFF chunks and prints one line per ANMF
# frame, "<width>x<height> <duration-ms>", after an "ANIM" line when the
# animation chunk is present. Prints "not-webp" for anything else.
webp_frames() {
    local file="$1" size offset id length payload
    size="$(file_size "$file")"
    if [[ "$size" -lt 12 || "$(fourcc "$file" 0)" != "RIFF" ||
          "$(fourcc "$file" 8)" != "WEBP" ]]; then
        echo "not-webp"
        return
    fi
    offset=12
    while [[ $((offset + 8)) -le "$size" ]]; do
        id="$(fourcc "$file" "$offset")"
        length="$(le "$file" $((offset + 4)) 4)"
        payload=$((offset + 8))
        case "$id" in
            ANIM) echo "ANIM" ;;
            ANMF)
                echo "$(( $(le "$file" $((payload + 6)) 3) + 1 ))x$(( $(le "$file" $((payload + 9)) 3) + 1 )) $(le "$file" $((payload + 12)) 3)"
                ;;
        esac
        offset=$((payload + length + (length & 1)))
    done
}

# png_size <file>: "<width>x<height>" from the IHDR chunk, or "not-png".
png_size() {
    if [[ "$(file_size "$1")" -lt 24 || "$(dd if="$1" bs=1 skip=1 count=3 2>/dev/null)" != "PNG" ||
          "$(fourcc "$1" 12)" != "IHDR" ]]; then
        echo "not-png"
        return
    fi
    echo "$(be "$1" 16 4)x$(be "$1" 20 4)"
}

# --- The project --------------------------------------------------------------

cp "$FIXTURES/cvbs_video_params_video.orcprj" vp.orcprj
# One PAL frame (with its trailing bytes) of 0x0202 samples: a flat level
# inside the 10-bit range.
head -c 1418758 /dev/zero | tr '\000' '\002' > blank.cvbs

# --- outputs ------------------------------------------------------------------

run 0 "outputs" preview outputs vp.orcprj --node "CVBS Source"
expect_out "outputs" "interlaced_clamped"
expect_out "outputs" "1135 x 625"

run 0 "outputs --json" preview outputs vp.orcprj --node 1 --json
expect_out "outputs --json" '"option": "interlaced_clamped"'
expect_out "outputs --json" '"width": 1135'

run 2 "outputs of a missing node" preview outputs vp.orcprj --node 99

# --- render -------------------------------------------------------------------

run 0 "render" preview render vp.orcprj --node 1 --frame 1 --scale 2 -o f1.png
if [[ "$(png_size f1.png)" != "2270x1250" ]]; then
    fail "render: f1.png is $(png_size f1.png), expected 2270x1250"
fi

run 1 "render frame 0" preview render vp.orcprj --node 1 --frame 0 -o x.png
expect_err "render frame 0" "--frame must be a frame number from 1"
run 1 "render beyond the end" preview render vp.orcprj --node 1 --frame 2 -o x.png
expect_err "render beyond the end" "frames 1 to 1"
run 1 "render an unknown type" preview render vp.orcprj --node 1 --frame 1 \
    --output-type sideways -o x.png
expect_err "render an unknown type" "frame-reversed"
run 1 "render a missing option" preview render vp.orcprj --node 1 --frame 1 \
    --option nonesuch -o x.png
expect_err "render a missing option" "--option interlaced_raw"
run 1 "render a bad scale" preview render vp.orcprj --node 1 --frame 1 \
    --scale 9 -o x.png
run 1 "render without -o" preview render vp.orcprj --node 1 --frame 1
if [[ -e x.png ]]; then
    fail "a refused render wrote x.png"
fi

# --- compare ------------------------------------------------------------------

run 0 "compare" preview compare vp.orcprj --a "CVBS Source" \
    --b "Video Parameters" --frame 1 --diff-output diff.png -o ab.webp
expect_out "compare" "Crop:   0,0,1135,625"
expect_no_out "compare" "identical"
frames="$(webp_frames ab.webp)"
expected_frames="ANIM
2278x625 800
2278x625 800"
if [[ "$frames" != "$expected_frames" ]]; then
    fail "compare: ab.webp is not a two-frame 2278x625 animation at 800 ms; read:
$frames"
fi
if [[ "$(png_size diff.png)" != "1135x625" ]]; then
    fail "compare: diff.png is $(png_size diff.png), expected 1135x625"
fi

run 0 "compare, auto crop" preview compare vp.orcprj --a 1 --b 2 --frame 1 \
    --crop auto --crop-size 64,32 --stretch 0 --scale 2 --interval 500 \
    -o auto.webp
expect_out "compare, auto crop" "Frames: 2 of 128 x 64, 500 ms each"
crop="$(sed -n 's/^  Crop:   //p' "$WORK_DIR/out")"
if ! printf '%s\n' "$crop" | grep -qE '^[0-9]+,[0-9]+,64,32$'; then
    fail "compare, auto crop: the crop is printed as '$crop', not x,y,64,32"
fi
# The printed crop repeats the auto crop exactly.
run 0 "compare, repeated crop" preview compare vp.orcprj --a 1 --b 2 \
    --frame 1 --crop "$crop" --stretch 0 --scale 2 --interval 500 \
    -o repeat.webp
expect_out "compare, repeated crop" "Crop:   $crop"
if ! cmp -s auto.webp repeat.webp; then
    fail "compare, repeated crop: the file differs from the auto crop's"
fi

run 0 "compare a node with itself" preview compare vp.orcprj --a 1 --b 1 \
    --frame 1 --crop 0,0,32,16 -o same.webp
expect_out "compare a node with itself" "A and B are identical inside the crop"

run 1 "compare options that differ" preview compare vp.orcprj --a 1 \
    --b "Video Sink" --frame 1 -o x.webp
expect_err "compare options that differ" "--option phase2_colour_carrier"
run 2 "compare a missing node" preview compare vp.orcprj --a 1 --b 99 \
    --frame 1 -o x.webp
run 1 "compare beyond the end" preview compare vp.orcprj --a 1 --b 2 \
    --frame 2 -o x.webp
run 1 "compare a crop outside the picture" preview compare vp.orcprj --a 1 \
    --b 2 --frame 1 --crop 1100,0,64,32 -o x.webp
run 1 "compare --crop-size without auto" preview compare vp.orcprj --a 1 \
    --b 2 --frame 1 --crop-size 64,32 -o x.webp
expect_err "compare --crop-size without auto" "--crop auto"
run 1 "compare an even window" preview compare vp.orcprj --a 1 --b 2 \
    --frame 1 --stretch-window 4 -o x.webp
run 1 "compare a bad crop" preview compare vp.orcprj --a 1 --b 2 \
    --frame 1 --crop 0,0,0,10 -o x.webp
run 1 "compare a bad interval" preview compare vp.orcprj --a 1 --b 2 \
    --frame 1 --interval 0 -o x.webp
run 1 "compare a negative gain" preview compare vp.orcprj --a 1 --b 2 \
    --frame 1 --stretch -1 -o x.webp
if [[ -e x.webp ]]; then
    fail "a refused comparison wrote x.webp"
fi

run 1 "an unknown subcommand" preview sideways
run 0 "help" preview --help

if [[ "$FAILURES" -ne 0 ]]; then
    echo "$FAILURES check(s) failed" >&2
    exit 1
fi
echo "orc-cli preview checks passed"
