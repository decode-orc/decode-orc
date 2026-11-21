#!/usr/bin/env bash
#
# File:        fixture_runs.sh
# Module:      tools/perf-benchmark
# Purpose:     Run every source/sink chain against a long-capture fixture
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# A research tool, outside the build and not run in CI.
#
#   fixture_runs.sh FIXTURE_DIR RESULTS_DIR [LABEL_PREFIX]
#
# FIXTURE_DIR is written by make_long_fixture.py. Each chain runs once under
# bench.py inside a memory-limited scope (MEMORY_MAX, default 32G) and is
# stopped STOP_AFTER seconds (default 60) after its first output, or after
# TIMEOUT seconds (default 1200) if no output appears. Outputs go to
# RESULTS_DIR/out and are deleted before each run.

set -u
here="$(cd "$(dirname "$0")" && pwd)"
fx="$(cd "$1" && pwd)"
res="$(mkdir -p "$2" && cd "$2" && pwd)"
prefix="${3:-fixture}"
out="$res/out"
mkdir -p "$out"

MEMORY_MAX="${MEMORY_MAX:-32G}"
STOP_AFTER="${STOP_AFTER:-60}"
TIMEOUT="${TIMEOUT:-1200}"
ORC_CLI="${ORC_CLI:-$here/../../build/perf-release/bin/orc-cli}"

tbc="tbc_source=input_path=$fx/tape_composite.tbc:pcm_path=$fx/tape_composite.pcm"
yc="tbc_source=y_path=$fx/tape_yc.tbc:c_path=$fx/tape_yc_chroma.tbc:pcm_path=$fx/tape_yc.pcm"
cvbs="PAL_CVBS_Source=input_path=$fx/long.cvbs"
video="decoder_type=transform2d:ffmpeg_format=mkv-ffv1:embed_audio=true"

run() {
    local name="$1" watch="$2"
    shift 2
    echo "== $prefix-$name"
    python3 "$here/bench.py" run --orc-cli "$ORC_CLI" \
        --label "$prefix-$name" --out "$res/$prefix-$name.json" \
        --workdir "$res" --memory-max "$MEMORY_MAX" \
        --watch "$watch" --clean "$out/*" \
        --stop-after "$STOP_AFTER" --timeout "$TIMEOUT" -- "$@"
}

run tbc-to-tbc "$out/t.tbc" -i "$tbc" -o "tbc_sink=output_path=$out/t.tbc"
run tbc-to-video "$out/v.mkv*" -i "$tbc" -f dropout_correct \
    -o "video_sink=output_path=$out/v.mkv:$video"
# Transform PAL needs a composite source; Y/C takes the 2D PAL decoder.
run yc-to-video "$out/v.mkv*" -i "$yc" -f dropout_correct \
    -o "video_sink=output_path=$out/v.mkv:${video/transform2d/pal2d}"
run tbc-to-audio "$out/a.wav" -i "$tbc" -o "AudioSink=output_path=$out/a.wav"
run tbc-to-cvbs "$out/c.cvbs" -i "$tbc" -o "CVBSSink=output_path=$out/c.cvbs"
run wav-import-to-audio "$out/a.wav" -i "$tbc" \
    -f "audio_import=wav_path=$fx/tape_audio_44k16.wav" \
    -o "AudioSink=output_path=$out/a.wav:channel_pair=1"
run cvbs-to-video "$out/v.mkv*" -i "$cvbs" -f dropout_correct \
    -o "video_sink=output_path=$out/v.mkv:$video"
run cvbs-to-audio "$out/a.wav" -i "$cvbs" -o "AudioSink=output_path=$out/a.wav"

python3 "$here/bench.py" report "$res/$prefix"-*.json
