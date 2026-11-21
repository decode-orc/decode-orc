#!/usr/bin/env bash
#
# File:        baseline_runs.sh
# Module:      tools/perf-benchmark
# Purpose:     Run the issue #375 baseline cases on real captures
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# A research tool, outside the build and not run in CI.
#
#   baseline_runs.sh RESULTS_DIR [CASE...]
#
# Cases (all by default):
#   stack   STACK_PROJECT (an .orcprj as build-pack.sh writes it), cut to
#           frames 20000-20499 in front of its video sink, FFV1 to local disk.
#           Two cold runs (perf stat) and one profile run.
#   single  SINGLE_CVBS: one LaserDisc capture through the stack's per-capture
#           chain (Frame Map, FM Beat Cancel, Dropout Correct, Picture Band
#           Limit, Channel Equalise) to the same sink, same 500 frames.
#   tape    TAPE_TBC (+ TAPE_PCM): tbc_source -> Dropout Correct -> video
#           sink with audio, composite. Two cold runs and one profile run.
#   tape-yc TAPE_YC_TBC + TAPE_YC_CHROMA (+ TAPE_YC_PCM): the same, Y/C.
#
# Every run starts cold: the inputs are dropped from the page cache (and the
# NFS client cache) first. Outputs go to RESULTS_DIR/out.

set -eu
here="$(cd "$(dirname "$0")" && pwd)"
res="$(mkdir -p "$1" && cd "$1" && pwd)"
shift
cases="${*:-stack single tape tape-yc}"
out="$res/out"
mkdir -p "$out"
ORC_CLI="${ORC_CLI:-$here/../../build/perf-release/bin/orc-cli}"

bench() {
    python3 "$here/bench.py" run --orc-cli "$ORC_CLI" --workdir "$res" \
        --clean "$out/*" "$@"
}

inputs_of() {
    # Inputs and their sidecars, for --evict-list.
    local list="$1"
    shift
    : >"$list"
    for p in "$@"; do
        [ -n "$p" ] || continue
        for f in "$p" "${p%.*}.meta" "${p%.*}.dropouts.meta" "$p.db" \
            "${p%.*}_audio_0.wav"; do
            if [ -e "$f" ]; then echo "$f" >>"$list"; fi
        done
    done
}

triple() {
    # Two cold perf-stat runs, then a profile run.
    local label="$1" evict="$2"
    shift 2
    bench --label "$label" --out "$res/$label.json" --repeat 2 \
        --evict-list "$evict" --watch "$out/*.mkv*" -- "$@"
    bench --label "$label-profile" --out "$res/$label-profile.json" --profile \
        --evict-list "$evict" --watch "$out/*.mkv*" -- "$@"
}

for c in $cases; do
    case "$c" in
    stack)
        python3 "$here/cut_project.py" "${STACK_PROJECT:?}" "$res/stack-cut.orcprj" \
            --range 20000-20499 --output-dir "$out"
        triple stack "$res/stack-cut.orcprj.inputs" "$res/stack-cut.orcprj" --process
        ;;
    single)
        inputs_of "$res/single.inputs" "${SINGLE_CVBS:?}"
        triple single "$res/single.inputs" \
            -i "PAL_CVBS_Source=input_path=$SINGLE_CVBS" \
            -f "frame_map=ranges=${SINGLE_MAP:-2-54003}, frame_map=ranges=20000-20499, fm_beat_cancel=carrier_black_mhz=${SINGLE_CARRIER:-7.0927}, dropout_correct=method=interpolate:match_chroma_phase=true, picture_band_limit=cutoff_mhz=5.3628254059999998:rolloff_mhz=0.4, channel_equalise=smear_fraction=0.2324:smear_tau_samples=6.223" \
            -o "video_sink=output_path=$out/single.mkv:decoder_type=transform3d:ffmpeg_format=mkv-ffv1-bt601:display_aspect_ratio='4:3':embed_audio=true:embed_chapter_metadata=true:embed_disc_metadata=true:noise_reduction_mode=still_only"
        ;;
    tape)
        inputs_of "$res/tape.inputs" "${TAPE_TBC:?}" "${TAPE_PCM:-}"
        triple tape "$res/tape.inputs" \
            -i "tbc_source=input_path=$TAPE_TBC${TAPE_PCM:+:pcm_path=$TAPE_PCM}" \
            -f dropout_correct \
            -o "video_sink=output_path=$out/tape.mkv:decoder_type=transform2d:ffmpeg_format=mkv-ffv1:embed_audio=true"
        ;;
    tape-yc)
        inputs_of "$res/tape-yc.inputs" "${TAPE_YC_TBC:?}" "${TAPE_YC_CHROMA:?}" "${TAPE_YC_PCM:-}"
        triple tape-yc "$res/tape-yc.inputs" \
            -i "tbc_source=y_path=$TAPE_YC_TBC:c_path=$TAPE_YC_CHROMA${TAPE_YC_PCM:+:pcm_path=$TAPE_YC_PCM}" \
            -f dropout_correct \
            -o "video_sink=output_path=$out/tape-yc.mkv:decoder_type=pal2d:ffmpeg_format=mkv-ffv1:embed_audio=true"
        ;;
    *)
        echo "unknown case: $c" >&2
        exit 2
        ;;
    esac
done

python3 "$here/bench.py" report "$res"/*.json
