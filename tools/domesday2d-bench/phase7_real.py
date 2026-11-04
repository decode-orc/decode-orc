#!/usr/bin/env python3
#
# File:        phase7_real.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 7: real-disc validation of pal2d,
#              transform2d and domesday2d on Community South
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase7_real.py setup STACK.orcprj OUT [--orc-cli PATH]
#       Calibrates every disc used here over its whole side (Channel
#       Calibration on a Channel Equalise stage, then Domesday2D Calibration
#       on a video sink behind it; the projects are kept in OUT/calibration),
#       and writes OUT/held.orcprj and OUT/cross.orcprj. Each disc there runs
#       source -> its disc map (copied from STACK) -> the pictures used ->
#       Channel Equalise -> one video sink per decoder, raw YUV to
#       OUT/decodes; held.orcprj also writes the equalised composite there,
#       for the reference. Paths are relative to OUT, so the projects open in
#       the GUI wherever the side's folder is mounted.
#   phase7_real.py held OUT [--json FILE]
#       Step 1: each held still's reference from its run (no decoder), and
#       every decoder's single-frame decodes against it.
#   phase7_real.py cross OUT [--json FILE]
#       Step 2: decodes of the same single-frame still from different
#       pressings against each other, within and across pressing groups.
#   phase7_real.py panel OUT PNG_DIR
#       Step 3: whole frames side by side, pal2d | transform2d | domesday2d,
#       at the decoders' own resolution: of the held stills, with their
#       reference first, to PNG_DIR/held; of the single-frame stills, which
#       have no reference, to PNG_DIR/single.
#   phase7_real.py panel-bench YUV_DIR PNG_DIR
#       The same for the videosynth still bench, with its picture truth
#       first, to PNG_DIR/synthetic/{noisy,clean}. YUV_DIR holds the video
#       sink's decodes pal_stills[_clean]_<decoder>.yuv (raw yuv,
#       output_padding 1).
#
# The held reference. On a held still the picture repeats exactly, and on
# the 4fsc grid the carrier inverts every two frames. Averaging each of the
# four carrier phases over the run, two phases 180 degrees apart give
# Y = (A + B) / 2 and C = (A - B) / 2 exactly; C is demodulated row by row
# on its own burst-fitted lattice with PalColour's horizontal filter. Its
# line-alternating error is taken out with the matrix fitted to the run's
# own exact chroma: on Domesday it is the still's (it repeats across
# pressings, and differs between stills), and the same correction is used
# whichever decoder is scored. The frame being scored is left out of its
# reference, so its noise is not in both, and samples that move within the
# run (a pointer) are left out of the score. The reference keeps the
# chroma's full vertical resolution, as the still bench's truth does.

import argparse
import itertools
import json
import os
import subprocess
import sys

import numpy as np
import yaml
from scipy.ndimage import maximum_filter, uniform_filter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import hanover  # noqa: E402
import proto  # noqa: E402

DECODERS = ["pal2d", "transform2d", "palstill"]

# Held stills: (name, first picture, last picture), runs found on the
# stacked side and trimmed to frames held both sides.
HELD = [("test_card_f", 3001, 3020),
        ("uk_map", 1600, 1623),
        ("england_map", 45060, 45083),
        ("menu", 46400, 46423),
        ("uk_colour_map", 47220, 47243),
        ("os_map", 47560, 47583),
        ("town_map", 48340, 48363),
        ("map_key", 49860, 49883),
        ("photograph", 52400, 52423)]
HELD_DISCS = ["DS1", "DS6"]

# Single-frame stills, and the discs compared, by pressing group: P and Q
# are two pressings of one mother, R and S other masters (the Community
# South stacking comparison).
STILLS = [8501, 10500, 14511, 16500, 22535, 25597, 31502, 34500, 35500,
          39500,
          8137, 9337, 11137, 12937, 14137, 14812, 15337, 15937, 17748, 18937,
          23738, 27337, 32137, 32737, 33337, 33937, 35137, 36937, 38738,
          39937]
GROUPS = {"DS1": "P", "DS2": "P", "DS6": "Q", "DS9": "Q", "DS3": "R",
          "DS5": "S", "DS11": "S"}

# The held still each panel of a held run shows: the middle of the run.
PANEL_DISC = "DS1"


# ── Projects ──────────────────────────────────────────────────────────────

def _param(value):
    if isinstance(value, bool):
        return {"type": "bool", "value": value}
    if isinstance(value, int):
        return {"type": "int32", "value": value}
    if isinstance(value, float):
        return {"type": "double", "value": value}
    return {"type": "string", "value": value}


class Project:
    def __init__(self, name):
        self.name = name
        self.nodes = []
        self.edges = []

    def add(self, stage, node_type, display, label, x, y, params):
        node_id = len(self.nodes) + 1
        self.nodes.append({
            "id": node_id, "stage": stage, "node_type": node_type,
            "display_name": display, "user_label": label, "x": x, "y": y,
            "parameters": {k: _param(v) for k, v in params.items()}})
        return node_id

    def edge(self, a, b):
        self.edges.append({"from": a, "to": b})

    def write(self, path):
        doc = {"project": {"name": self.name, "version": 2.0,
                           "video_format": "PAL",
                           "source_format": "Composite",
                           "amplitude_unit": "10bit"},
               "dag": {"nodes": self.nodes, "edges": self.edges}}
        with open(path, "w") as f:
            f.write("# ORC Project File\n# Version: 2.0\n\n")
            yaml.safe_dump(doc, f, sort_keys=False)


def _disc_maps(stack):
    """{disc: (capture path, disc map ranges)} from a stack project."""
    with open(stack) as f:
        doc = yaml.safe_load(f)
    base = os.path.dirname(os.path.abspath(stack))
    sources, maps = {}, {}
    for node in doc["dag"]["nodes"]:
        label = node.get("user_label", "")
        params = node.get("parameters") or {}
        if node["stage"] == "PAL_CVBS_Source":
            sources[label] = os.path.normpath(
                os.path.join(base, params["input_path"]["value"]))
        elif node["stage"] == "frame_map" and label.endswith(" map"):
            maps[label[:-4]] = params["ranges"]["value"]
    return {d: (sources[d], maps[d]) for d in sources if d in maps}


def _chain(project, out, disc, capture, disc_map, pictures, cal, row,
           name, reference=False):
    """One disc's source, maps, equaliser and sinks on |row|."""
    y = row * 450
    src = project.add("PAL_CVBS_Source", "SOURCE", "CVBS Source", disc,
                      0, y, {"input_path": os.path.relpath(capture, out)})
    dmap = project.add("frame_map", "TRANSFORM", "Frame Map",
                       f"{disc} map", 200, y, {"ranges": disc_map})
    pick = project.add("frame_map", "TRANSFORM", "Frame Map",
                       f"{disc} pictures", 400, y, {"ranges": pictures})
    eq = project.add("channel_equalise", "TRANSFORM", "Channel Equalise",
                     f"{disc} equalise", 600, y,
                     {"smear_fraction": cal["smear_fraction"],
                      "smear_tau_samples": cal["smear_tau_samples"]})
    project.edge(src, dmap)
    project.edge(dmap, pick)
    project.edge(pick, eq)
    sinks = [(d, {"decoder_type": d}) for d in DECODERS]
    for i, (decoder, params) in enumerate(sinks):
        params.update({"output_path": f"decodes/{name}_{disc}_{decoder}.yuv",
                       "output_mode": "raw", "raw_format": "yuv",
                       "output_padding": 1})
        if decoder == "palstill":
            for k in ("hanover_uu", "hanover_uv", "hanover_vu",
                      "hanover_vv"):
                params[k] = cal[k]
        sink = project.add("video_sink", "SINK", "Video Sink",
                           f"{disc} {decoder}", 850, y + 100 * i, params)
        project.edge(eq, sink)
    if reference:
        sink = project.add("CVBSSink", "SINK", "CVBS Sink",
                           f"{disc} equalised composite", 850, y + 300,
                           {"output_path": f"decodes/{name}_{disc}.cvbs"})
        project.edge(eq, sink)


def _run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"{' '.join(cmd)} failed:\n{result.stdout}"
                         f"{result.stderr}")


def _calibrate(out, disc, capture, disc_map, orc_cli):
    """Channel and Domesday2D calibration over the whole side, kept as a
    project in OUT/calibration."""
    folder = os.path.join(out, "calibration")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, f"{disc}.orcprj")
    if not os.path.exists(path):
        p = Project(f"{disc} calibration")
        src = p.add("PAL_CVBS_Source", "SOURCE", "CVBS Source", disc, 0, 0,
                    {"input_path": os.path.relpath(capture, folder)})
        dmap = p.add("frame_map", "TRANSFORM", "Frame Map", f"{disc} map",
                     200, 0, {"ranges": disc_map})
        eq = p.add("channel_equalise", "TRANSFORM", "Channel Equalise",
                   f"{disc} equalise", 400, 0, {})
        sink = p.add("video_sink", "SINK", "Video Sink", f"{disc} domesday2d",
                     600, 0, {"decoder_type": "palstill",
                              "output_path": f"{disc}.yuv",
                              "output_mode": "raw", "raw_format": "yuv"})
        p.edge(src, dmap)
        p.edge(dmap, eq)
        p.edge(eq, sink)
        p.write(path)
        _run([orc_cli, "tools", "run", path, "channel_calibration",
              "--node", str(eq), "--apply", "--quiet"])
        _run([orc_cli, "tools", "run", path, "domesday2d_calibration",
              "--node", str(sink), "--apply", "--quiet"])
    with open(path) as f:
        doc = yaml.safe_load(f)
    cal = {}
    for node in doc["dag"]["nodes"]:
        for k, v in (node.get("parameters") or {}).items():
            if k.startswith("smear_") or k.startswith("hanover_"):
                cal[k] = float(v["value"])
    return cal


def setup(stack, out, orc_cli):
    out = os.path.abspath(out)
    os.makedirs(os.path.join(out, "decodes"), exist_ok=True)
    maps = _disc_maps(stack)
    discs = list(dict.fromkeys(HELD_DISCS + list(GROUPS)))
    cal = {}
    for disc in discs:
        capture, disc_map = maps[disc]
        cal[disc] = _calibrate(out, disc, capture, disc_map, orc_cli)
        print(disc, " ".join(f"{k} {v:.4f}" for k, v in cal[disc].items()),
              flush=True)
    with open(os.path.join(out, "calibration.json"), "w") as f:
        json.dump(cal, f, indent=1)

    held = ",".join(f"{a}-{b}" for _, a, b in HELD)
    project = Project("Domesday2D Phase 7 held stills")
    for row, disc in enumerate(HELD_DISCS):
        _chain(project, out, disc, *maps[disc], held, cal[disc], row,
               "held", reference=True)
    project.write(os.path.join(out, "held.orcprj"))

    stills = ",".join(str(p) for p in STILLS)
    project = Project("Domesday2D Phase 7 single-frame stills")
    for row, disc in enumerate(GROUPS):
        _chain(project, out, disc, *maps[disc], stills, cal[disc], row,
               "cross")
    project.write(os.path.join(out, "cross.orcprj"))


# ── Reading ───────────────────────────────────────────────────────────────

def read_raster(path, frame):
    """Frame |frame| of a .cvbs as bench.frame_raster() lays it out. The
    CVBS sink stores the internal samples bitwise, so what an equaliser
    pushes below 0 (sync tips) is negative: the file is read as int16."""
    data = np.memmap(path, dtype="<i2", mode="r")
    raw = np.asarray(data[frame * bench.CVBS_FRAME:
                          (frame + 1) * bench.CVBS_FRAME], dtype=np.float64)
    out = np.empty((625, 1137))
    at = 0
    for line in range(625):
        n = 1137 if line in (312, 624) else bench.LINE
        out[line, :n] = raw[at:at + n]
        out[line, n:] = raw[at + n - 1]
        at += n
    return bench.frame_raster(out)


def held_index(picture):
    """Output frame of |picture| in held.orcprj's sinks."""
    at = 0
    for _, a, b in HELD:
        if a <= picture <= b:
            return at + picture - a
        at += b - a + 1
    raise KeyError(picture)


# ── Step 1: held-run reference ────────────────────────────────────────────

def _phase_class(raster):
    """The frame's carrier phase in quarter turns (0-3) on the first
    active row, where the lattice puts it at 45 degrees plus a multiple of
    90."""
    phi, _ = proto.lattice(raster)
    return int(np.round((phi[proto.FIRST_ACTIVE] - 45.0) / 90.0)) % 4


class Run:
    """A held run: its frames' carrier classes, the line-alternating
    matrix of its chroma, and the samples where it is static."""

    def __init__(self, rasters):
        self.rasters = rasters
        self.classes = [_phase_class(r) for r in rasters]
        means = self._means(-1)
        pairs = []
        for q in (0, 1):
            c = (means[q] - means[q + 2]) / 2
            phi, s = proto.lattice(c)
            pairs.append(hanover.flat_pairs(
                *adaptive.demodulate_rows(c, phi, s), s))
        # The Hanover error is the picture's (it repeats across pressings,
        # and differs between stills), so the reference takes the run's
        # own: fitted on its exact chroma, the same for every decoder.
        self.matrix, self.pairs = hanover.fit_matrix(pairs)
        self.correction = hanover.corrector(self.matrix)
        # What moves within the run (a pointer on a menu) is not a held
        # picture: samples whose same-phase spread over the run is three
        # times the median, widened.
        resid = np.empty((len(rasters),) + rasters[0].shape)
        for i, r in enumerate(rasters):
            resid[i] = r - means[self.classes[i]]
        spread = np.sqrt(uniform_filter((resid ** 2).mean(axis=0),
                                        size=(5, 15)))
        grid = bench.from_raster(spread, spread, spread)[0]
        self.static = ~maximum_filter(grid > 3 * np.median(grid),
                                      size=(9, 25))

    def _means(self, leave_out):
        means = {}
        for q in range(4):
            members = [r for i, r in enumerate(self.rasters)
                       if self.classes[i] == q and i != leave_out]
            if members:
                means[q] = np.mean(members, axis=0)
        return means

    def reference(self, leave_out=-1):
        """(Y, U, V) on the truth grid from the run without frame
        |leave_out|."""
        means = self._means(leave_out)
        ys, us, vs = [], [], []
        for q in (0, 1):
            if q not in means or q + 2 not in means:
                continue
            a, b = means[q], means[q + 2]
            ys.append((a + b) / 2)
            c = (a - b) / 2
            phi, s = proto.lattice(c)
            u, v = adaptive.demodulate_rows(c, phi, s,
                                            correction=self.correction)
            us.append(u)
            vs.append(v)
        if not ys:
            raise ValueError("the run has no two carrier phases 180 apart")
        return bench.from_raster(np.mean(ys, axis=0), np.mean(us, axis=0),
                                 np.mean(vs, axis=0))


def held(out, json_path):
    results = {}
    for disc in HELD_DISCS:
        cvbs = os.path.join(out, "decodes", f"held_{disc}.cvbs")
        for name, a, b in HELD:
            frames = [held_index(p) for p in range(a, b + 1)]
            run = Run([read_raster(cvbs, f) for f in frames])
            mask = bench.picture_mask() & run.static
            scores = {d: [] for d in DECODERS}
            for i, frame in enumerate(frames):
                features = bench.TruthFeatures(run.reference(i))
                for d in DECODERS:
                    path = os.path.join(out, "decodes",
                                        f"held_{disc}_{d}.yuv")
                    scores[d].append(bench.measure(
                        bench.decoded(path, frame), features, mask))
            row = {"matrix": run.matrix.tolist(),
                   "static_pct": float(run.static.mean() * 100)}
            for d in DECODERS:
                keys = scores[d][0].keys()
                row[d] = {k: float(np.mean([s[k] for s in scores[d]
                                            if k in s])) for k in keys}
            results[f"{disc} {name}"] = row
            print(f"{disc} {name:14s} M {np.round(run.matrix, 3).tolist()} "
                  + "  ".join(
                      f"{d} dE {row[d]['delta_e_mean']:.2f} "
                      f"Y {row[d]['y_rms_pct']:.2f}"
                      for d in DECODERS), flush=True)
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


# ── Step 2: cross-disc agreement ──────────────────────────────────────────

def _matched(da, db, m):
    """|db| with its luma gain and offset, and its chroma's complex gain,
    fitted to |da| over |m|: pressings differ in level (Community South's
    DS3 sits 7 % high), which every decoder carries alike."""
    ya, yb = da[0][m], db[0][m]
    g, o = np.linalg.lstsq(np.stack([yb, np.ones_like(yb)], 1), ya,
                           rcond=None)[0]
    wa = da[1][m] + 1j * da[2][m]
    wb = db[1][m] + 1j * db[2][m]
    k = (np.conj(wb) * wa).sum() / (np.abs(wb) ** 2).sum()
    w = k * (db[1] + 1j * db[2])
    return g * db[0] + o, w.real, w.imag


def _disagreement(da, db, mask):
    m = mask.copy()
    m[:, :bench.EDGE_EXCLUSION] = False
    m[:, -bench.EDGE_EXCLUSION:] = False
    db = _matched(da, db, m)
    out = {}
    for name, x, y in zip("YUV", da, db):
        out[name] = float(np.sqrt(((x - y)[m] ** 2).mean()) * 100)
    de = bench.ciede2000(bench.yuv_to_lab(*(x[m] for x in da)),
                         bench.yuv_to_lab(*(x[m] for x in db)))
    out["dE"] = float(de.mean())
    return out


def cross(out, json_path):
    mask = bench.picture_mask()
    discs = list(GROUPS)
    sums = {(d, kind): [] for d in DECODERS for kind in ("within", "across")}
    for k, picture in enumerate(STILLS):
        decodes = {(disc, d): bench.decoded(
            os.path.join(out, "decodes", f"cross_{disc}_{d}.yuv"), k)
            for disc in discs for d in DECODERS}
        line = []
        for d in DECODERS:
            per = {"within": [], "across": []}
            for a, b in itertools.combinations(discs, 2):
                kind = "within" if GROUPS[a] == GROUPS[b] else "across"
                per[kind].append(_disagreement(decodes[(a, d)],
                                               decodes[(b, d)], mask))
            for kind in per:
                sums[(d, kind)].extend(per[kind])
            ac = per["across"]
            line.append(f"{d} dE {np.mean([x['dE'] for x in ac]):.3f} "
                        f"U {np.mean([x['U'] for x in ac]):.3f} "
                        f"V {np.mean([x['V'] for x in ac]):.3f}")
        print(f"{picture:6d}  " + "  ".join(line), flush=True)
    results = {}
    for d in DECODERS:
        results[d] = {kind: {m: float(np.mean([x[m] for x in sums[(d, kind)]]))
                             for m in ("Y", "U", "V", "dE")}
                      for kind in ("within", "across")}
        print(d, json.dumps(results[d]))
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


# ── Step 3: panels ────────────────────────────────────────────────────────

def _rgb(y, u, v):
    r, g, b = bench.yuv_to_rgb(y, u, v)
    return np.clip(np.stack([r, g, b], -1) * 255 + 0.5, 0, 255).astype(
        np.uint8)


def _sheet(columns, path):
    """|columns| [(label, (Y, U, V))], whole frames side by side with their
    labels, to |path|: one pixel per decoded sample and frame line, the
    4fsc grid (922 samples by 576 lines), not cropped or scaled."""
    from PIL import Image, ImageDraw
    rows, cols = columns[0][1][0].shape
    gap, head = 8, 20
    sheet = Image.new("RGB", (len(columns) * (cols + gap) - gap,
                              rows + head), (32, 32, 32))
    draw = ImageDraw.Draw(sheet)
    for i, (label, planes) in enumerate(columns):
        x = i * (cols + gap)
        sheet.paste(Image.fromarray(_rgb(*planes)), (x, head))
        draw.text((x + 4, 4), label, fill=(255, 255, 255))
    sheet.save(path)


def panel(out, png_dir):
    """Every held still (the reference first, then the run's middle frame
    from each decoder) to PNG_DIR/held, and every single-frame still to
    PNG_DIR/single, all from PANEL_DISC."""
    disc = PANEL_DISC
    for folder in ("held", "single"):
        os.makedirs(os.path.join(png_dir, folder), exist_ok=True)
    cvbs = os.path.join(out, "decodes", f"held_{disc}.cvbs")
    for name, a, b in HELD:
        frames = [held_index(p) for p in range(a, b + 1)]
        middle = frames[len(frames) // 2]
        columns = [("reference", Run([read_raster(cvbs, f)
                                      for f in frames]).reference())]
        for d in DECODERS:
            path = os.path.join(out, "decodes", f"held_{disc}_{d}.yuv")
            columns.append((d, bench.decoded(path, middle)))
        _sheet(columns, os.path.join(png_dir, "held", f"{name}.png"))
        print(name, flush=True)
    for k, picture in enumerate(STILLS):
        columns = [(d, bench.decoded(os.path.join(
            out, "decodes", f"cross_{disc}_{d}.yuv"), k)) for d in DECODERS]
        _sheet(columns, os.path.join(png_dir, "single",
                                     f"picture_{picture}.png"))
        print(picture, flush=True)


def panel_bench(yuv_dir, png_dir):
    """Every still of the still bench, its truth first, from
    YUV_DIR/<case>_<decoder>.yuv for pal_stills (the Domesday noise model)
    and pal_stills_clean, to PNG_DIR/synthetic/noisy and /clean."""
    for case, stem in (("noisy", "pal_stills"), ("clean", "pal_stills_clean")):
        folder = os.path.join(png_dir, "synthetic", case)
        os.makedirs(folder, exist_ok=True)
        for frame, name in bench.STILLS:
            columns = [("truth", bench.truth("clean", frame))]
            for d in DECODERS:
                path = os.path.join(yuv_dir, f"{stem}_{d}.yuv")
                columns.append((d, bench.decoded(path, frame)))
            _sheet(columns, os.path.join(folder, f"{name}.png"))
            print(case, name, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup")
    s.add_argument("stack")
    s.add_argument("out")
    s.add_argument("--orc-cli", default=os.path.join(
        bench.ROOT, "build", "bin", "orc-cli"))
    for name in ("held", "cross"):
        s = sub.add_parser(name)
        s.add_argument("out")
        s.add_argument("--json")
    s = sub.add_parser("panel")
    s.add_argument("out")
    s.add_argument("png_dir")
    s = sub.add_parser("panel-bench")
    s.add_argument("yuv_dir")
    s.add_argument("png_dir")
    args = ap.parse_args()
    if args.cmd == "setup":
        setup(args.stack, args.out, args.orc_cli)
    elif args.cmd == "held":
        held(args.out, args.json)
    elif args.cmd == "cross":
        cross(args.out, args.json)
    elif args.cmd == "panel":
        panel(args.out, args.png_dir)
    else:
        panel_bench(args.yuv_dir, args.png_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
