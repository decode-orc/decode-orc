#!/usr/bin/env python3
#
# File:        phase9_motion.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 9 item 5: motion that the decoder schedule
#              sends to domesday2d
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase9_motion.py pan DIR [--orc-cli PATH] [--json FILE]
#       DIR holds videosynth's render of pal_pan.yaml (a photograph and a
#       map panned 2 samples a field: as interlaced motion, whose fields
#       are different instants, and as film, whose two fields are one;
#       SECTIONS frames each, with the still bench's noise). Decodes it with
#       pal2d, transform2d and domesday2d and scores every frame against
#       the picture truth.
#   phase9_motion.py count MKV HELD_JSON [--step N] [--json FILE]
#       Every Nth frame of a side's stacked video, each frame scored for
#       whether its two fields are one instant; the threshold is set on the
#       frames of the side's held runs (HELD_JSON: [[first, last], ...]),
#       which are one instant. Counts the frames not held whose fields are
#       two instants: what `domesday2d, held:transform3d` combs.
#
# The decoder guide's schedule decodes with domesday2d every frame the
# Stacker did not find held, which on a still-frame disc is every
# single-frame still, and on a side with video is its motion too.

import argparse
import json
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import phase7_real  # noqa: E402

DECODERS = phase7_real.DECODERS
STEM = "videosynth_pal_pan"
SECTIONS = [("photograph", "interlaced"), ("photograph", "film"),
            ("map", "interlaced"), ("map", "film")]
# --fast: pal_pan.yaml with the clips panned 8 and 24 samples a field.
FAST_SECTIONS = [("photograph", "interlaced 8"),
                 ("photograph", "interlaced 24"),
                 ("map", "interlaced 8"), ("map", "interlaced 24")]
FRAMES = 12


def _truth(folder, frame):
    plane = bench.TRUTH_WIDTH * bench.TRUTH_ROWS
    data = np.memmap(os.path.join(folder, STEM + ".truth"), dtype="<f4",
                     mode="r")
    block = np.asarray(data[frame * 3 * plane:(frame + 1) * 3 * plane],
                       dtype=np.float64).reshape(3, bench.TRUTH_ROWS,
                                                 bench.TRUTH_WIDTH)
    return block[0] / 700.0, block[1] / 700.0, block[2] / 700.0


def pan(folder, orc_cli, json_path, sections=SECTIONS):
    cvbs = os.path.join(folder, STEM + ".cvbs")
    for d in DECODERS:
        path = os.path.join(folder, f"pan_{d}.yuv")
        if not os.path.exists(path):
            subprocess.run([orc_cli, "--source",
                            f"PAL_CVBS_Source=input_path={cvbs}", "--sink",
                            f"video_sink=output_path={path}:output_mode=raw:"
                            f"raw_format=yuv:decoder_type={d}:"
                            "output_padding=1", "--log-level", "warn"],
                           check=True, capture_output=True)
    mask = bench.picture_mask()
    results = {}
    for k, (picture, kind) in enumerate(sections):
        scores = {d: [] for d in DECODERS}
        # The first and last frames of a section are left out: a decoder
        # that reads neighbouring frames would see the next section.
        for f in range(k * FRAMES + 1, (k + 1) * FRAMES - 1):
            features = bench.TruthFeatures(_truth(folder, f))
            for d in DECODERS:
                scores[d].append(bench.measure(bench.decoded(
                    os.path.join(folder, f"pan_{d}.yuv"), f), features,
                    mask))
        row = {d: {m: float(np.mean([s[m] for s in scores[d]]))
                   for m in ("delta_e_mean", "delta_e_p99", "y_rms_pct",
                             "y_hf_rms_pct", "u_rms_pct", "v_rms_pct")}
               for d in DECODERS}
        results[f"{picture} {kind}"] = row
        print(f"{picture:10s} {kind:13s} " + "  ".join(
            f"{d} dE {row[d]['delta_e_mean']:.2f} p99 "
            f"{row[d]['delta_e_p99']:.1f} Y {row[d]['y_rms_pct']:.2f}"
            for d in DECODERS), flush=True)
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


# ── Counting motion on a side ─────────────────────────────────────────────

def comb_score(frame):
    """How far the frame's two fields are from one instant: each row of
    one field against the average of the other field's rows either side,
    relative to the same row against its own field's rows either side.
    One instant reads about 1 or less (more vertical detail from the
    other field than from its own); two instants of motion read above."""
    y = frame.astype(np.float64)[8:-8, 16:-16]
    inter = np.abs(y[1:-1] - 0.5 * (y[:-2] + y[2:]))[1::2]
    intra = np.abs(y[2:-2] - 0.5 * (y[:-4] + y[4:]))[1::2]
    a, b = inter.mean(), intra.mean()
    return float(a / b) if b > 0.5 else 0.0


def count(mkv, held_json, step, json_path):
    with open(held_json) as f:
        held = [tuple(x) for x in json.load(f)]
    cmd = ["ffmpeg", "-nostdin", "-loglevel", "error", "-threads", "4",
           "-i", mkv, "-map", "0:v:0", "-vf",
           f"select=not(mod(n\\,{step})),format=gray", "-fps_mode",
           "passthrough", "-f", "rawvideo", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = 720 * 576
    scores = []
    n = 0
    while True:
        buf = proc.stdout.read(size)
        if len(buf) < size:
            break
        frame = np.frombuffer(buf, dtype=np.uint8).reshape(576, 720)
        scores.append((n * step, comb_score(frame),
                       float(frame.mean())))
        n += 1
    proc.wait()
    is_held = np.zeros(scores[-1][0] + 1, dtype=bool)
    for a, b in held:
        is_held[a:b + 1] = True
    s = np.array([x[1] for x in scores])
    level = np.array([x[2] for x in scores])
    at = np.array([x[0] for x in scores])
    in_run = is_held[at]
    lit = level > 30
    threshold = float(np.percentile(s[in_run & lit], 99))
    moving = lit & ~in_run & (s > threshold)
    result = {"frames_scored": int(len(s)), "lit": int(lit.sum()),
              "in_held_runs": int((in_run & lit).sum()),
              "threshold": threshold,
              "two_instants_not_held": int(moving.sum()),
              "share_of_lit": float(moving.sum() / max(1, lit.sum())),
              "held_scores_p50_p99": [float(np.percentile(s[in_run & lit],
                                                          50)), threshold]}
    print(json.dumps(result, indent=1))
    if json_path:
        with open(json_path, "w") as f:
            json.dump(result, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("pan")
    s.add_argument("dir")
    s.add_argument("--orc-cli", default=os.path.join(
        bench.ROOT, "build", "bin", "orc-cli"))
    s.add_argument("--json")
    s.add_argument("--fast", action="store_true")
    s = sub.add_parser("count")
    s.add_argument("mkv")
    s.add_argument("held_json")
    s.add_argument("--step", type=int, default=25)
    s.add_argument("--json")
    args = ap.parse_args()
    if args.cmd == "pan":
        pan(args.dir, args.orc_cli, args.json,
            FAST_SECTIONS if args.fast else SECTIONS)
    else:
        count(args.mkv, args.held_json, args.step, args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
