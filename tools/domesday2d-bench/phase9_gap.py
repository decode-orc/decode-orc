#!/usr/bin/env python3
#
# File:        phase9_gap.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 9: why the decoder gains less on real discs
#              than on the synthetic bench
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase9_gap.py build OUT GAP [--per-run N]
#       From Phase 7's held runs (OUT as phase7_real.py's), writes one copy
#       of each disc's equalised composite per VARIANT to GAP, with N frames
#       of each run replaced, and a project per copy that decodes those
#       frames with pal2d, transform2d and domesday2d.
#   phase9_gap.py decode GAP [--orc-cli PATH]
#       Runs the projects.
#   phase9_gap.py score OUT GAP [--json FILE]
#       Every decode against its truth, beside the exact-separation floor of
#       each noisy variant; and the noise of the real frames against the
#       bench's.
#
# On the noisy bench Domesday2D takes about 40 % off transform2d's ΔE (Test
# Card F 2.75 -> 1.66); on the real held runs, 26 % on the card and under
# 10 % on the maps. The bench's composite is its truth modulated exactly on
# the carrier, plus the Domesday noise model (pal_stills.yaml: capture and
# pressing noise, nothing else). A real frame differs from that in three
# ways that can be taken apart on a held run:
#
#   R   = the real frame
#   X0  = the mean of the run's frames of R's carrier phase: the real
#         channel and picture with the noise mostly averaged out; R - X0 is
#         the frame's real noise
#   X1  = the run's reference (Y, U, V; Hanover error taken out) modulated
#         exactly on R's lattice, as the bench's encoder does: real picture,
#         ideal channel
#
# The variants put those together with the real noise or the bench's (the
# noisy bench less the clean one, frame for frame). Each is scored against
# the run's reference, which is the exact truth of X1. The "oracle" is a
# decode that separates exactly (the variant's own chroma known) and so
# carries only its noise: what no decoder can do better than.

import argparse
import json
import os
import shutil
import subprocess
import sys
from multiprocessing import Pool

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import hanover  # noqa: E402
import phase7_real  # noqa: E402
import proto  # noqa: E402

DECODERS = phase7_real.DECODERS
DISCS = phase7_real.HELD_DISCS
# name: (what is in it, whether the decoder takes the disc's hanover_*)
VARIANTS = {
    "real": ("R", True),
    "mean": ("X0", True),
    "mean_bench": ("X0 + bench noise", True),
    "model": ("X1", False),
    "model_bench": ("X1 + bench noise", False),
    "model_real": ("X1 + real noise", False),
    "model_hanover_real": ("X1 + the run's Hanover error + real noise",
                           True),
}
# The active samples of a row that X1 replaces.
ACTIVE = (157, 1105)
ROWS = (proto.FIRST_ACTIVE, proto.END_ACTIVE)
BENCH_STILLS = [f for f, _ in bench.STILLS]


# ── Rasters ───────────────────────────────────────────────────────────────

def write_raster(path, frame, raster):
    """Puts |raster| (frame_raster() layout) back as frame |frame| of a
    .cvbs, leaving each field's 313th line as it was."""
    data = np.memmap(path, dtype="<u2", mode="r+")
    base = frame * bench.CVBS_FRAME
    codes = np.clip(np.round(raster * (bench.WHITE - bench.BLACK) +
                             bench.BLACK), 0, 1023).astype("<u2")
    at = base
    for line in range(625):
        n = 1137 if line in (312, 624) else bench.LINE
        if line < 312:
            data[at:at + bench.LINE] = codes[2 * line]
        elif 313 <= line:
            data[at:at + bench.LINE] = codes[2 * (line - 313) + 1]
        at += n
    data.flush()


def clipped_copy(src, dst):
    """|src| (the CVBS sink's int16) as 10-bit codes: the source reads
    unsigned samples, and an equalised sync tip can be below 0."""
    data = np.fromfile(src, dtype="<i2")
    np.clip(data, 0, 1023).astype("<u2").tofile(dst)
    for ext in (".meta", ".dropouts.meta"):
        a = src[:-5] + ext
        if os.path.exists(a):
            shutil.copy(a, dst[:-5] + ext)


def bench_noise(k):
    """The noisy bench less the clean one, still |k| (cycled)."""
    f = BENCH_STILLS[k % len(BENCH_STILLS)]
    return (bench.frame_raster(bench.composite("noisy", f)) -
            bench.frame_raster(bench.composite("clean", f)))


class Planes:
    """A held run's reference on the raster, and each frame's class
    mean and exact chroma."""

    def __init__(self, run, leave_out=-1):
        means = run._means(leave_out)
        self.means = means
        ys, us, vs = [], [], []
        self.chroma = {}
        for q in (0, 1):
            a, b = means[q], means[q + 2]
            ys.append((a + b) / 2)
            c = (a - b) / 2
            self.chroma[q], self.chroma[q + 2] = c, -c
            phi, s = proto.lattice(c)
            u, v = adaptive.demodulate_rows(c, phi, s,
                                            correction=run.correction)
            us.append(u)
            vs.append(v)
        self.y = np.mean(ys, axis=0)
        self.u = np.mean(us, axis=0)
        self.v = np.mean(vs, axis=0)

    def truth(self):
        return bench.from_raster(self.y, self.u, self.v)


def model_frame(planes, base, phi, s, correction=None):
    """X1: the reference modulated on (phi, s) over the active picture
    (through |correction|'s inverse if given, putting a Hanover error
    back), |base| elsewhere."""
    out = base.copy()
    c = adaptive.encode(planes.u, planes.v, phi, s, correction)
    r, a = slice(*ROWS), slice(*ACTIVE)
    out[r, a] = planes.y[r, a] + c[r, a]
    return out


def chosen(n, per_run):
    """|per_run| frames of a run of |n|, five apart: each carrier phase in
    turn."""
    return [(5 * i) % n for i in range(per_run)]


# ── Build ─────────────────────────────────────────────────────────────────

def build(out, gap, per_run):
    os.makedirs(gap, exist_ok=True)
    with open(os.path.join(out, "calibration.json")) as f:
        cal = json.load(f)
    layout = {"per_run": per_run, "discs": {}}
    for disc in DISCS:
        src = os.path.join(out, "decodes", f"held_{disc}.cvbs")
        paths = {}
        for name in VARIANTS:
            paths[name] = os.path.join(gap, f"{name}_{disc}.cvbs")
            clipped_copy(src, paths[name])
        frames = []
        k = 0
        for run_name, a, b in phase7_real.HELD:
            index = [phase7_real.held_index(p) for p in range(a, b + 1)]
            rasters = [phase7_real.read_raster(src, f) for f in index]
            run = phase7_real.Run(rasters)
            planes = Planes(run)
            for i in chosen(len(index), per_run):
                r = rasters[i]
                phi, s = proto.lattice(r)
                x0 = planes.means[run.classes[i]]
                x1 = model_frame(planes, x0, phi, s)
                x1h = model_frame(planes, x0, phi, s, run.correction)
                noise = r - x0
                nb = bench_noise(k)
                k += 1
                content = {"mean": x0, "mean_bench": x0 + nb, "model": x1,
                           "model_bench": x1 + nb, "model_real": x1 + noise,
                           "model_hanover_real": x1h + noise}
                for name, raster in content.items():
                    write_raster(paths[name], index[i], raster)
                frames.append(index[i])
            print(disc, run_name, flush=True)
        layout["discs"][disc] = frames
        for name, (_, takes_cal) in VARIANTS.items():
            project = phase7_real.Project(f"Phase 9 gap {name} {disc}")
            src_node = project.add("PAL_CVBS_Source", "SOURCE", "CVBS Source",
                                   disc, 0, 0,
                                   {"input_path": os.path.basename(
                                       paths[name])})
            pick = project.add("frame_map", "TRANSFORM", "Frame Map",
                               "frames", 200, 0,
                               {"ranges": ",".join(str(f) for f in frames)})
            project.edge(src_node, pick)
            for j, d in enumerate(DECODERS):
                params = {"decoder_type": d,
                          "output_path": f"{name}_{disc}_{d}.yuv",
                          "output_mode": "raw", "raw_format": "yuv",
                          "output_padding": 1}
                if d == "palstill" and takes_cal:
                    for key in ("hanover_uu", "hanover_uv", "hanover_vu",
                                "hanover_vv"):
                        params[key] = cal[disc][key]
                sink = project.add("video_sink", "SINK", "Video Sink", d,
                                   400, 100 * j, params)
                project.edge(pick, sink)
            project.write(os.path.join(gap, f"{name}_{disc}.orcprj"))
    with open(os.path.join(gap, "layout.json"), "w") as f:
        json.dump(layout, f, indent=1)


def decode(gap, orc_cli):
    for disc in DISCS:
        for name in VARIANTS:
            project = os.path.join(gap, f"{name}_{disc}.orcprj")
            print(name, disc, flush=True)
            subprocess.run([orc_cli, project, "--process", "--log-level", "warn"],
                           cwd=gap, check=True, capture_output=True)


# ── Score ─────────────────────────────────────────────────────────────────

def _oracle(x, c, noise, correction):
    """A decode of x + noise that knows x's chroma |c| exactly."""
    phi, s = proto.lattice(x + noise)
    u, v = adaptive.demodulate_rows(c + noise, phi, s, correction=correction)
    return bench.from_raster(x + noise - c, u, v)


def _noise_bands(n):
    """R.m.s. (% of white) of a noise raster over the picture: whole,
    luma band (below 3 MHz), chroma band (3-5.9 MHz), and on the blanked
    rows the decoder estimates it from."""
    pic = n[slice(*ROWS), slice(*ACTIVE)]
    low = bench.filter_rows(pic, bench.band_filter(0.0, 3.0e6))
    chroma = bench.filter_rows(pic, adaptive.CHROMA_BAND)
    return {"all": float(np.sqrt((pic ** 2).mean()) * 100),
            "below_3m": float(np.sqrt((low ** 2).mean()) * 100),
            "chroma_band": float(np.sqrt((chroma ** 2).mean()) * 100),
            "blank_rows": adaptive.blank_noise(n) * 100}


def _score_run(job):
    out, gap, disc, run_name, a, b, per_run, k0 = job
    src = os.path.join(out, "decodes", f"held_{disc}.cvbs")
    index = [phase7_real.held_index(p) for p in range(a, b + 1)]
    rasters = [phase7_real.read_raster(src, f) for f in index]
    run = phase7_real.Run(rasters)
    full = Planes(run)
    mask = bench.picture_mask() & run.static
    truth = bench.TruthFeatures(full.truth())
    pick = chosen(len(index), per_run)
    scores = {}
    noise = {"real": [], "bench": []}

    def add(key, d, features):
        scores.setdefault(key, []).append(bench.measure(d, features, mask))

    for j, i in enumerate(pick):
        slot = k0 + j
        r = rasters[i]
        phi, s = proto.lattice(r)
        q = run.classes[i]
        x0 = full.means[q]
        nb = bench_noise(slot)
        noise["real"].append(_noise_bands(r - x0))
        noise["bench"].append(_noise_bands(nb))
        # The real frame against its run without it, as Phase 7 scores.
        loo = Planes(run, leave_out=i)
        loo_truth = bench.TruthFeatures(loo.truth())
        for name in VARIANTS:
            features = loo_truth if name == "real" else truth
            for d in DECODERS:
                path = os.path.join(gap, f"{name}_{disc}_{d}.yuv")
                add((name, d), bench.decoded(path, slot), features)
        # The floors: exact separation, so only the noise is left.
        add(("real", "oracle"),
            _oracle(loo.means[q], loo.chroma[q], r - loo.means[q],
                    run.correction), loo_truth)
        add(("mean_bench", "oracle"),
            _oracle(x0, full.chroma[q], nb, run.correction), truth)
        x1 = model_frame(full, x0, phi, s)
        c1 = full.chroma[q].copy()
        region = (slice(*ROWS), slice(*ACTIVE))
        c1[region] = adaptive.encode(full.u, full.v, phi, s)[region]
        add(("model_bench", "oracle"), _oracle(x1, c1, nb, None), truth)
        add(("model_real", "oracle"), _oracle(x1, c1, r - x0, None), truth)
    return disc, run_name, scores, noise


def score(out, gap, json_path):
    with open(os.path.join(gap, "layout.json")) as f:
        per_run = json.load(f)["per_run"]
    jobs = []
    for disc in DISCS:
        k = 0
        for run_name, a, b in phase7_real.HELD:
            jobs.append((out, gap, disc, run_name, a, b, per_run, k))
            k += per_run
    with Pool(min(len(jobs), 9)) as pool:
        done = pool.map(_score_run, jobs)
    results = {}
    keys = ("delta_e_mean", "delta_e_p99", "y_rms_pct", "y_hf_rms_pct",
            "u_rms_pct", "v_rms_pct")
    for disc, run_name, scores, noise in done:
        row = {}
        for (name, d), xs in scores.items():
            row.setdefault(name, {})[d] = {
                k: float(np.mean([x[k] for x in xs])) for k in keys}
        row["noise"] = {w: {b: float(np.mean([x[b] for x in noise[w]]))
                            for b in noise[w][0]} for w in noise}
        results[f"{disc} {run_name}"] = row
    order = list(VARIANTS)
    for key in results:
        print(key)
        row = results[key]
        for name in order:
            line = "  ".join(
                f"{d} {row[name][d]['delta_e_mean']:.2f}/"
                f"{row[name][d]['y_rms_pct']:.2f}"
                for d in DECODERS + ["oracle"] if d in row[name])
            print(f"  {name:20s} {line}")
    print("mean over the runs (ΔE / Y %):")
    for name in order:
        line = []
        for d in DECODERS + ["oracle"]:
            vals = [r[name][d] for r in results.values() if d in r[name]]
            if vals:
                line.append(f"{d} {np.mean([v['delta_e_mean'] for v in vals]):.3f}/"
                            f"{np.mean([v['y_rms_pct'] for v in vals]):.3f}")
        print(f"  {name:20s} " + "  ".join(line))
    for w in ("real", "bench"):
        bands = results[next(iter(results))]["noise"][w].keys()
        print(f"noise {w}: " + "  ".join(
            f"{b} {np.mean([r['noise'][w][b] for r in results.values()]):.3f}"
            for b in bands))
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("build")
    s.add_argument("out")
    s.add_argument("gap")
    s.add_argument("--per-run", type=int, default=4)
    s = sub.add_parser("decode")
    s.add_argument("gap")
    s.add_argument("--orc-cli", default=os.path.join(
        bench.ROOT, "build", "bin", "orc-cli"))
    s = sub.add_parser("score")
    s.add_argument("out")
    s.add_argument("gap")
    s.add_argument("--json")
    args = ap.parse_args()
    if args.cmd == "build":
        build(args.out, args.gap, args.per_run)
    elif args.cmd == "decode":
        decode(args.gap, args.orc_cli)
    else:
        score(args.out, args.gap, args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
