#!/usr/bin/env python3
#
# File:        phase9_dropouts.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 9 item 4: the decoders near lines that
#              Dropout Correction rebuilt
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase9_dropouts.py build SIDECAR DIR
#       Copies of the noisy bench with dropouts cut into its stills, one
#       per CASE, each with a dropout sidecar (SIDECAR, any real one, gives
#       the schema), and a project per copy: source -> Dropout Correction
#       (interpolate) -> pal2d, transform2d and domesday2d. The bench as it
#       is goes through the same chain as "none".
#   phase9_dropouts.py decode DIR [--orc-cli PATH]
#   phase9_dropouts.py score DIR [--json FILE]
#       Each decoder against the truth in a band around the repairs, and
#       the same band of its decode of "none".
#
# Domesday2D reads each line's 312-line partner from the other field, and
# its transform runs on the woven frame. A repair rebuilds a run from the
# lines above and below it in its own field, which a field decoder sees as
# a plausible line and a frame decoder sees as a line that no longer
# matches its partner. Real Community South dropouts (DS1, the whole side)
# come about 9 to a frame, median 14 samples, 99 % under 103 and none over
# 220: none a whole line. The plan asked for runs of 1-3 lines; they are
# the "lines" case, beside the real rate and ten times it.

import argparse
import json
import os
import shutil
import sqlite3
import subprocess
import sys

import numpy as np
from scipy.ndimage import binary_dilation

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import phase7_real  # noqa: E402

DECODERS = phase7_real.DECODERS
STEM = "videosynth_pal_stills"
# name: (runs per frame, (shortest, longest) samples, or None for whole
# lines: events per frame of 1-3 field lines)
CASES = {"none": (0, (1, 1)),
         "real": (9, (1, 220)),
         "real_x10": (90, (1, 220)),
         "lines": (4, None)}
# The real lengths (Community South DS1): a geometric fit to median 14.
MEDIAN_LENGTH = 14.0
ACTIVE = (157, 1105)
FIELD_LINES = (22, 310)  # 0-based field lines of the picture
# The band scored around a repair, on the truth grid: rows and samples.
BAND = (4, 16)


def _line_offset(line):
    """Sample offset of transmission line |line| (0-624) in a frame."""
    return line * bench.LINE + (2 if line > 312 else 0)


def _events(rng, case):
    """[(field 0/1, field line, start sample, length)] for one frame."""
    count, lengths = CASES[case]
    out = []
    if lengths is None:
        for _ in range(count):
            field = int(rng.integers(2))
            first = int(rng.integers(FIELD_LINES[0], FIELD_LINES[1] - 3))
            for k in range(int(rng.integers(1, 4))):
                out.append((field, first + k, ACTIVE[0],
                            ACTIVE[1] - ACTIVE[0]))
        return out
    p = np.log(2.0) / MEDIAN_LENGTH
    for _ in range(int(rng.poisson(count))):
        n = int(np.clip(rng.geometric(p), *lengths))
        field = int(rng.integers(2))
        line = int(rng.integers(*FIELD_LINES))
        start = int(rng.integers(ACTIVE[0], ACTIVE[1] - n))
        out.append((field, line, start, n))
    return out


def build(sidecar, folder):
    src = os.path.join(bench.OUTPUT, bench.CASES["noisy"][0], STEM)
    rng = np.random.default_rng(9104)
    layout = {}
    for case in CASES:
        base = os.path.join(folder, case)
        os.makedirs(base, exist_ok=True)
        cvbs = os.path.join(base, STEM + ".cvbs")
        shutil.copy(src + ".cvbs", cvbs)
        shutil.copy(src + ".meta", os.path.join(base, STEM + ".meta"))
        side = os.path.join(base, STEM + ".dropouts.meta")
        shutil.copy(sidecar, side)
        db = sqlite3.connect(side)
        db.execute("DELETE FROM dropout_run")
        data = np.memmap(cvbs, dtype="<u2", mode="r+")
        events = []
        for frame, _ in bench.STILLS:
            for field, line, start, n in _events(rng, case):
                tline = line if field == 0 else 313 + line
                at = frame * bench.CVBS_FRAME + _line_offset(tline) + start
                data[at:at + n] = rng.integers(200, 900, n)
                db.execute("INSERT OR REPLACE INTO dropout_run VALUES "
                           "(1, ?, ?, ?, 100)",
                           (frame, _line_offset(tline) + start, n))
                events.append((frame, field, line, start, n))
        data.flush()
        db.commit()
        db.close()
        layout[case] = events
        project = phase7_real.Project(f"Phase 9 dropouts {case}")
        s = project.add("PAL_CVBS_Source", "SOURCE", "CVBS Source", case, 0,
                        0, {"input_path": STEM + ".cvbs"})
        doc = project.add("dropout_correct", "TRANSFORM",
                          "Dropout Correction", "DOC", 200, 0,
                          {"method": "interpolate"})
        project.edge(s, doc)
        for i, d in enumerate(DECODERS):
            sink = project.add("video_sink", "SINK", "Video Sink", d, 400,
                               100 * i,
                               {"decoder_type": d, "output_path": f"{d}.yuv",
                                "output_mode": "raw", "raw_format": "yuv",
                                "output_padding": 1})
            project.edge(doc, sink)
        project.write(os.path.join(base, "dropouts.orcprj"))
        print(case, len(events), flush=True)
    with open(os.path.join(folder, "layout.json"), "w") as f:
        json.dump(layout, f)


def decode(folder, orc_cli):
    for case in CASES:
        base = os.path.join(folder, case)
        subprocess.run([orc_cli, "dropouts.orcprj", "--process",
                        "--log-level", "warn"], cwd=base, check=True,
                       capture_output=True)
        print(case, flush=True)


def _band(events, frame):
    """The truth-grid samples within BAND of a repair on |frame|."""
    hit = np.zeros((bench.TRUTH_ROWS, bench.TRUTH_WIDTH), dtype=bool)
    for f, field, line, start, n in events:
        if f != frame:
            continue
        row = 2 * line + field - bench.TRUTH_ROW0
        if not 0 <= row < bench.TRUTH_ROWS:
            continue
        a = max(0, start - bench.TRUTH_FIRST_SAMPLE)
        b = min(bench.TRUTH_WIDTH, start + n - bench.TRUTH_FIRST_SAMPLE)
        if b > a:
            hit[row, a:b] = True
    return binary_dilation(hit, np.ones((2 * BAND[0] + 1, 2 * BAND[1] + 1),
                                        dtype=bool))


def score(folder, json_path):
    with open(os.path.join(folder, "layout.json")) as f:
        layout = json.load(f)
    results = {}
    keys = ("delta_e_mean", "delta_e_p99", "y_rms_pct", "u_rms_pct",
            "v_rms_pct")
    for case in CASES:
        if case == "none":
            continue
        rows = {}
        for frame, name in bench.STILLS:
            features = bench.TruthFeatures(bench.truth("clean", frame))
            band = _band(layout[case], frame) & bench.masks(frame)[0]
            if band.sum() < 100:
                continue
            row = {"samples": int(band.sum())}
            for d in DECODERS:
                for which in (case, "none"):
                    path = os.path.join(folder, which, f"{d}.yuv")
                    m = bench.measure(bench.decoded(path, frame), features,
                                      band)
                    row[f"{d} {which}"] = {k: m[k] for k in keys}
            rows[name] = row
        results[case] = rows
        print(f"== {case}")
        for name, row in rows.items():
            print(f"  {name:20s} " + "  ".join(
                f"{d} {row[f'{d} none']['delta_e_mean']:.2f}->"
                f"{row[f'{d} {case}']['delta_e_mean']:.2f}"
                for d in DECODERS))
        for d in DECODERS:
            a = np.mean([r[f"{d} none"]["delta_e_mean"] for r in rows.values()])
            b = np.mean([r[f"{d} {case}"]["delta_e_mean"]
                         for r in rows.values()])
            ya = np.mean([r[f"{d} none"]["y_rms_pct"] for r in rows.values()])
            yb = np.mean([r[f"{d} {case}"]["y_rms_pct"] for r in rows.values()])
            print(f"  mean {d:12s} dE {a:.3f} -> {b:.3f}   Y {ya:.3f} -> {yb:.3f}")
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("build")
    s.add_argument("sidecar")
    s.add_argument("dir")
    s = sub.add_parser("decode")
    s.add_argument("dir")
    s.add_argument("--orc-cli", default=os.path.join(
        bench.ROOT, "build", "bin", "orc-cli"))
    s = sub.add_parser("score")
    s.add_argument("dir")
    s.add_argument("--json")
    args = ap.parse_args()
    if args.cmd == "build":
        build(args.sidecar, args.dir)
    elif args.cmd == "decode":
        decode(args.dir, args.orc_cli)
    else:
        score(args.dir, args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
