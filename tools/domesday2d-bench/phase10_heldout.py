#!/usr/bin/env python3
#
# File:        phase10_heldout.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 10 item 4: a held-out still set, never used
#              in tuning, rendered and scored like the still bench
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase10_heldout.py pictures PHOTOS ASSETS FONTS OUT
#       OUT/exr/<NAME>.exr for the twelve stills of STILLS. PHOTOS holds
#       the photographs (PHOTOS below: CC0 files from Wikimedia Commons,
#       not committed); ASSETS is videosynth-assets (its OSM extract and
#       map renderer); FONTS the Liberation fonts' truetype directory.
#       Needs ffmpeg and exrstdattr (openexr) on PATH.
#   phase10_heldout.py write OUT
#       OUT/ho_rot{0..3}[_clean].yaml: pal_stills.yaml with its twelve
#       stills replaced by these, each keeping the bench section's noise,
#       and moved through the PAL sequence as phase7_rotation.py moves the
#       bench. Then: videosynth --project OUT/<name>.yaml --output-root OUT,
#       and decode each with the video sink to OUT/<name>_<decoder>.yuv.
#   phase10_heldout.py score OUT [--yuv-tag TAG] [--json FILE]
#       DecoderStillTruth's metrics for every still at every rotation,
#       for the decodes OUT/<name>_<decoder><TAG>.yuv.
#   phase10_heldout.py detail OUT
#       Each still's luma share at 3.5-5.5 MHz, beside the bench's.
#
# The bench's gains did not carry over to the discs (Phase 9 item 8)
# because its stills are sharper and more synthetic than Domesday's. So the
# photographs and maps here are softened as a camera and a 1980s PAL chain
# soften them (a Gaussian of CAMERA_SIGMA pixels on the 720-pixel line, in
# linear light), and their 3.5-5.5 MHz luma share is reported beside the
# discs' (Phase 9: 0.1-2.6 %). The captions, chart and data map are
# electronic, as Domesday's were, and stay sharp.

import argparse
import copy
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
import yaml
from PIL import Image, ImageDraw, ImageFont, ImageOps
from scipy.ndimage import gaussian_filter, maximum_filter
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import phase7_rotation  # noqa: E402

WIDTH, HEIGHT = 720, 576
CAMERA_SIGMA = 0.6
DECODERS = ["pal2d", "transform2d", "domesday2d"]

# The photographs: Wikimedia Commons files under CC0, fetched as their
# 1920-pixel thumbnails, with the 4:3 crop centre (x, y) used here.
PHOTOS = {
    "pub.jpg": ("File:Royal Oak Inn, Wood Street Village, Guildford "
                "(April 2014).jpg", 0.5, 0.5),
    "portrait.jpg": ("File:Woman in a headwrap in Quebec City.jpg", 0.5,
                     0.5),
    "boat.jpg": ("File:Blue fishing boat harbour Eretria Euboea "
                 "Greece.jpg", 0.45, 0.5),
    "cheshire.jpg": ("File:Rural Cheshire (Explored) - Flickr - Oneterry "
                     "AKA Terry Kearney.jpg", 0.5, 0.5),
    "livingroom.jpg": ("File:Living room of a typical rural house in "
                       "northeast Brazil.jpg", 0.5, 0.5),
    "fishmonger.jpg": ("File:Fishmonger smiling, Maracaibo street market, "
                       "Venezuela.jpg", 0.6, 0.4),
}

# (name, class, kind); the class picks the primary metric.
STILLS = [
    ("PHOTO_PUB", "photograph", ("photo", "pub.jpg")),
    ("PHOTO_PORTRAIT", "photograph", ("photo", "portrait.jpg")),
    ("PHOTO_BOAT", "photograph", ("photo", "boat.jpg")),
    ("PHOTO_FIELDS", "photograph", ("photo", "cheshire.jpg")),
    ("PHOTO_INTERIOR", "photograph", ("photo", "livingroom.jpg")),
    ("PHOTO_MONO", "monochrome", ("photo", "fishmonger.jpg")),
    ("MAP_STREETS", "map", ("osm", "streets")),
    ("MAP_COUNTRY", "map", ("osm", "country")),
    ("MAP_DATA", "graphic", ("data_map",)),
    ("TEXT_CAPTIONS", "text", ("captions",)),
    ("GRAPHIC_CHART", "graphic", ("chart",)),
    ("PHOTO_CAPTIONED", "photograph", ("captioned", "pub.jpg")),
]


# ── Pictures ──────────────────────────────────────────────────────────────


def _srgb_to_linear(v):
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def _linear_to_srgb(v):
    return np.where(v <= 0.0031308, v * 12.92,
                    1.055 * np.power(v, 1.0 / 2.4) - 0.055)


def _soften(rgb):
    """The camera's and chain's softness, in linear light."""
    lin = _srgb_to_linear(rgb)
    lin = np.stack([gaussian_filter(lin[:, :, k], CAMERA_SIGMA,
                                    mode="nearest") for k in range(3)], 2)
    return _linear_to_srgb(np.clip(lin, 0.0, 1.0))


def photo(path, cx, cy):
    """videosynth's convert_photo.py: a 4:3 crop at (cx, cy), downscaled
    in linear light with a triangle filter. Then softened."""
    image = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    w, h = image.size
    if w / h > 4.0 / 3.0:
        cw, ch = round(h * 4.0 / 3.0), h
    else:
        cw, ch = w, round(w * 3.0 / 4.0)
    left = min(max(round(cx * w - cw / 2.0), 0), w - cw)
    top = min(max(round(cy * h - ch / 2.0), 0), h - ch)
    image = image.crop((left, top, left + cw, top + ch))
    lin = _srgb_to_linear(np.asarray(image, dtype=np.float64) / 255.0)
    planes = [np.asarray(Image.fromarray(lin[:, :, k].astype(np.float32),
                                         mode="F").resize(
        (WIDTH, HEIGHT), Image.BILINEAR)) for k in range(3)]
    return _soften(_linear_to_srgb(np.clip(np.stack(planes, 2), 0.0, 1.0)))


def _osm_module(assets):
    path = os.path.join(assets, "scripts", "render_osm_map.py")
    spec = importlib.util.spec_from_file_location("render_osm_map", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def osm(assets, fonts, which):
    """A map from the bench's OSM extract, of ground the bench's two maps
    do not centre on: west of the town at 1.6 km (streets) and south-west
    at 3.5 km (country). Then softened, as a filmed print."""
    m = _osm_module(assets)
    town, rural = m.MAPS
    if which == "streets":
        spec = dict(town, name="MAP_STREETS", centre=(51.3405, -2.2700),
                    ground_width_m=1600.0, grid_m=200.0)
    else:
        spec = dict(rural, name="MAP_COUNTRY", centre=(51.3330, -2.2900),
                    ground_width_m=3500.0, grid_m=500.0)
    areas, lines, nodes = m.load_features(
        os.path.join(assets, "sources", "osm", "bradford_on_avon.json"))
    return _soften(np.clip(m.render_map(spec, areas, lines, nodes, fonts),
                           0.0, 1.0))


def _font(fonts, size, bold=False):
    name = "LiberationSans-Bold.ttf" if bold else "LiberationSans-Regular.ttf"
    return ImageFont.truetype(os.path.join(fonts, name), size)


def _supersampled(draw_at, scale=4):
    """An RGB picture drawn by |draw_at|(ImageDraw, scale) at |scale|
    times the raster and box-reduced in linear light (sub-pixel edges, no
    aliasing, no further softening)."""
    big = Image.new("RGB", (WIDTH * scale, HEIGHT * scale))
    draw_at(ImageDraw.Draw(big), scale)
    lin = _srgb_to_linear(np.asarray(big, dtype=np.float64) / 255.0)
    lin = lin.reshape(HEIGHT, scale, WIDTH, scale, 3).mean(axis=(1, 3))
    return _linear_to_srgb(np.clip(lin, 0.0, 1.0))


def captions(fonts):
    """Coloured captions as a 1980s character generator set them, in an
    anti-aliased face: on solid panels, white, yellow, cyan and green on
    blue, red, black and magenta, at three sizes."""
    panels = [((16, 16, 352, 280), (0, 0, 160), [(255, 255, 255),
                                                 (255, 255, 0)]),
              ((368, 16, 704, 280), (170, 0, 0), [(255, 255, 255),
                                                  (0, 255, 255)]),
              ((16, 296, 352, 560), (0, 0, 0), [(0, 255, 0),
                                                (255, 0, 255)]),
              ((368, 296, 704, 560), (200, 0, 200), [(255, 255, 0),
                                                     (255, 255, 255)])]
    lines = [("Population 1981", 30, True), ("Households by tenure", 22,
                                              False),
             ("Owner occupied 58%", 18, False), ("Rented 34%  Other 8%",
                                                 18, False),
             ("Source: Census", 14, False), ("Wiltshire  Avon  Dorset",
                                             14, True)]

    def draw_at(d, k):
        d.rectangle((0, 0, WIDTH * k, HEIGHT * k), fill=(40, 40, 40))
        for (x0, y0, x1, y1), back, inks in panels:
            d.rectangle((x0 * k, y0 * k, x1 * k, y1 * k), fill=back)
            y = y0 + 12
            for i, (text, size, bold) in enumerate(lines):
                d.text(((x0 + 14) * k, y * k), text,
                       font=_font(fonts, size * k, bold),
                       fill=inks[i % len(inks)])
                y += size + 12
    return _supersampled(draw_at)


def chart(fonts):
    """A saturated bar chart and pie, as Domesday's data graphics: fully
    saturated fills on black and white, with outlines and labels."""
    colours = [(255, 0, 0), (0, 200, 0), (0, 0, 255), (255, 255, 0),
               (0, 255, 255), (255, 0, 255)]
    values = [0.82, 0.55, 0.67, 0.31, 0.93, 0.44]

    def draw_at(d, k):
        d.rectangle((0, 0, WIDTH * k, HEIGHT * k), fill=(255, 255, 255))
        d.rectangle((0, 0, 360 * k, HEIGHT * k), fill=(0, 0, 0))
        base, top = 500, 80
        for i, (c, v) in enumerate(zip(colours, values)):
            x0 = 30 + i * 52
            y0 = base - v * (base - top)
            d.rectangle((x0 * k, y0 * k, (x0 + 40) * k, base * k), fill=c,
                        outline=(255, 255, 255), width=2 * k)
            d.text((x0 * k, (base + 10) * k), f"{1971 + 2 * i}",
                   font=_font(fonts, 14 * k), fill=(255, 255, 255))
        d.text((30 * k, 30 * k), "Unemployment, %",
               font=_font(fonts, 22 * k, True), fill=(255, 255, 0))
        start = 0.0
        total = sum(values)
        for c, v in zip(colours, values):
            end = start + 360.0 * v / total
            d.pieslice((400 * k, 120 * k, 690 * k, 410 * k), start, end,
                       fill=c, outline=(0, 0, 0), width=2 * k)
            start = end
        d.text((400 * k, 450 * k), "Share by region",
               font=_font(fonts, 22 * k, True), fill=(0, 0, 0))
    return _supersampled(draw_at)


def data_map(fonts):
    """A choropleth: irregular regions in five saturated classes, black
    boundaries, a key, as National's data maps."""
    rng = np.random.default_rng(1986)
    seeds = rng.uniform((40, 40), (540, 540), size=(60, 2))
    classes = rng.integers(0, 5, size=len(seeds))
    palette = [(255, 255, 160), (255, 200, 0), (255, 110, 0), (220, 0, 0),
               (120, 0, 60)]
    k = 4
    gy, gx = np.mgrid[0:HEIGHT * k, 0:WIDTH * k] / k
    # The two nearest seeds: their distances' difference is under the
    # boundary's width on a boundary.
    dist, index = cKDTree(seeds).query(
        np.stack([gx.ravel(), gy.ravel()], 1), k=2)
    nearest = index[:, 0].reshape(gx.shape)
    gap = (dist[:, 1] - dist[:, 0]).reshape(gx.shape)
    land = ((gx - 290) / 250) ** 2 + ((gy - 290) / 260) ** 2 + \
        0.15 * np.sin(gx / 23.0) * np.cos(gy / 31.0) < 1.0
    rgb = np.zeros((HEIGHT * k, WIDTH * k, 3))
    rgb[:] = (90, 150, 230)  # sea
    fill = np.array(palette)[classes[nearest]]
    rgb[land] = fill[land]
    rgb[land & (gap < 1.2)] = 0.0
    big = Image.fromarray(rgb.astype(np.uint8))
    d = ImageDraw.Draw(big)
    for i, c in enumerate(palette):
        y = 120 + i * 60
        d.rectangle((590 * k, y * k, 630 * k, (y + 36) * k), fill=c,
                    outline=(0, 0, 0), width=2 * k)
        d.text((640 * k, (y + 6) * k), f"{10 * i}+",
               font=_font(fonts, 20 * k, True), fill=(0, 0, 0))
    lin = _srgb_to_linear(np.asarray(big, dtype=np.float64) / 255.0)
    lin = lin.reshape(HEIGHT, k, WIDTH, k, 3).mean(axis=(1, 3))
    return _linear_to_srgb(np.clip(lin, 0.0, 1.0))


def captioned(path, fonts):
    """A photograph with a caption strip keyed over it, as National's
    photographs with text overlays."""
    rgb = photo(path, 0.3, 0.55)
    img = Image.fromarray((rgb * 255.0 + 0.5).astype(np.uint8))
    k = 4
    big = img.resize((WIDTH * k, HEIGHT * k), Image.NEAREST)
    d = ImageDraw.Draw(big)
    d.rectangle((0, 470 * k, WIDTH * k, 550 * k), fill=(0, 0, 140))
    d.text((24 * k, 480 * k), "The Royal Oak, Wood Street",
           font=_font(fonts, 28 * k, True), fill=(255, 255, 255))
    d.text((24 * k, 516 * k), "Village inn, Surrey",
           font=_font(fonts, 20 * k), fill=(255, 255, 0))
    lin = _srgb_to_linear(np.asarray(big, dtype=np.float64) / 255.0)
    lin = lin.reshape(HEIGHT, k, WIDTH, k, 3).mean(axis=(1, 3))
    return _linear_to_srgb(np.clip(lin, 0.0, 1.0))


def write_exr(rgb, path):
    """videosynth-assets' write_exr.sh: planar float32 G, B, R through
    ffmpeg, then the BT.601 625-line metadata."""
    assert rgb.min() >= 0.0 and rgb.max() <= 1.0
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "frame.gbrpf32le")
        with open(raw, "wb") as f:
            for channel in (1, 2, 0):
                f.write(np.ascontiguousarray(rgb[:, :, channel],
                                             dtype="<f4").tobytes())
        encoded = os.path.join(tmp, "frame.exr")
        subprocess.run(["ffmpeg", "-v", "error", "-f", "rawvideo",
                        "-pix_fmt", "gbrpf32le", "-s:v",
                        f"{WIDTH}x{HEIGHT}", "-i", raw, "-frames:v", "1",
                        "-c:v", "exr", "-compression", "none", "-format",
                        "float", "-gamma", "1", "-y", encoded], check=True)
        subprocess.run(["exrstdattr", "-framesPerSecond", "25", "1",
                        "-pixelAspectRatio", "1.094017", encoded, path],
                       check=True)


def pictures(photos, assets, fonts, out):
    os.makedirs(os.path.join(out, "exr"), exist_ok=True)
    for name, _, kind in STILLS:
        if kind[0] == "photo":
            _, cx, cy = PHOTOS[kind[1]]
            rgb = photo(os.path.join(photos, kind[1]), cx, cy)
        elif kind[0] == "osm":
            rgb = osm(assets, fonts, kind[1])
        elif kind[0] == "data_map":
            rgb = data_map(fonts)
        elif kind[0] == "captions":
            rgb = captions(fonts)
        elif kind[0] == "chart":
            rgb = chart(fonts)
        else:
            rgb = captioned(os.path.join(photos, kind[1]), fonts)
        write_exr(rgb, os.path.join(out, "exr", f"{name}.exr"))
        Image.fromarray((rgb * 255.0 + 0.5).astype(np.uint8)).save(
            os.path.join(out, "exr", f"{name}.png"))
        print(name, flush=True)


# ── Projects ──────────────────────────────────────────────────────────────


def write(out):
    out = os.path.abspath(out)
    with open(phase7_rotation.PROJECT) as f:
        doc = yaml.safe_load(f)
    for i, (name, _, _) in enumerate(STILLS):
        section = doc["sections"][1 + i]
        section["name"] = name.title().replace("_", "")
        section["source"] = os.path.join(out, "exr", f"{name}.exr")
        section.pop("chroma_line_sign", None)
    for r in phase7_rotation.ROTATIONS:
        for clean in (False, True):
            d = copy.deepcopy(doc)
            stills = d["sections"][1:1 + phase7_rotation.STILL_COUNT]
            moved = [None] * phase7_rotation.STILL_COUNT
            for i, section in enumerate(stills):
                moved[phase7_rotation.rotated_frame(i + 1, r) - 1] = section
            d["sections"][1:1 + phase7_rotation.STILL_COUNT] = moved
            stem = f"ho_rot{r}" + ("_clean" if clean else "")
            if clean:
                for section in d["sections"]:
                    section.pop("noise", None)
            d["project"]["name"] = f"HeldOutRot{r}" + ("Clean" if clean
                                                       else "")
            d["project"]["description"] = (
                "Domesday2D Phase 10 held-out stills (phase10_heldout.py)")
            d["output"]["video_path"] = f"{{output}}/{stem}.cvbs"
            with open(os.path.join(out, f"{stem}.yaml"), "w") as f:
                yaml.safe_dump(d, f, sort_keys=False)
            print(stem)


# ── Scores ────────────────────────────────────────────────────────────────


def _mask(out, r, frame):
    """The picture, less the samples within reach of a clipped composite
    sample of the clean render (bench.masks() without the stress card)."""
    saved = bench.CASES["clean"]
    bench.CASES["clean"] = (os.path.abspath(out), f"ho_rot{r}_clean")
    try:
        raster = bench.composite("clean", frame)
    finally:
        bench.CASES["clean"] = saved
    first, width = bench.TRUTH_FIRST_SAMPLE, bench.TRUTH_WIDTH
    rows = np.empty((bench.TRUTH_ROWS, width))
    rows[0::2] = raster[22:22 + 288, first:first + width]
    rows[1::2] = raster[335:335 + 288, first:first + width]
    clipped = ((rows <= bench.SYNC_TIP) | (rows >= bench.PEAK)) & \
        bench.picture_mask()
    spread = maximum_filter(clipped.astype(np.uint8),
                            size=(2 * bench.CLIP_MARGIN_ROWS + 1,
                                  2 * bench.CLIP_MARGIN_SAMPLES + 1),
                            mode="constant") > 0
    return bench.picture_mask() & ~spread


def primary(cls):
    return "cross_colour_rms_pct" if cls == "monochrome" else "delta_e_mean"


def score(out, tag, json_path):
    results = {}
    for case in ("clean", "noisy"):
        for i, (name, cls, _) in enumerate(STILLS):
            f = i + 1
            row = {}
            for r in phase7_rotation.ROTATIONS:
                stem = f"ho_rot{r}" + ("_clean" if case == "clean" else "")
                at = phase7_rotation.rotated_frame(f, r)
                truth = phase7_rotation._truth(
                    os.path.join(out, f"{stem}.truth"), at)
                features = bench.TruthFeatures(truth)
                mask = _mask(out, r, at)
                for d in DECODERS:
                    got = bench.decoded(
                        os.path.join(out, f"{stem}_{d}{tag}.yuv"), at)
                    row.setdefault(d, []).append(
                        bench.measure(got, features, mask))
            results[f"{case} {name}"] = {"class": cls, "scores": row}
            key = primary(cls)
            line = "  ".join(
                f"{d} {np.mean([m[key] for m in row[d]]):.3f}"
                for d in DECODERS)
            print(f"{case:5s} {name:16s} {key[:5]}  {line}", flush=True)
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


def _hf_share(y):
    """The luma's 3.5-5.5 MHz power as a share of its AC power."""
    band = bench.filter_rows(y, bench.band_filter(3.5e6, 5.5e6))
    ac = y - y.mean(axis=1, keepdims=True)
    return 100.0 * (band ** 2).sum() / (ac ** 2).sum()


def detail(out):
    for i, (name, _, _) in enumerate(STILLS):
        y = phase7_rotation._truth(os.path.join(out, "ho_rot0_clean.truth"),
                                   i + 1)[0]
        print(f"held-out {name:16s} {_hf_share(y):5.2f} %")
    for f, name in bench.STILLS:
        print(f"bench    {name:16s} {_hf_share(bench.truth('clean', f)[0]):5.2f} %")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("pictures")
    for a in ("photos", "assets", "fonts", "out"):
        s.add_argument(a)
    s = sub.add_parser("write")
    s.add_argument("out")
    s = sub.add_parser("score")
    s.add_argument("out")
    s.add_argument("--yuv-tag", default="")
    s.add_argument("--json")
    s = sub.add_parser("detail")
    s.add_argument("out")
    args = ap.parse_args()
    if args.cmd == "pictures":
        pictures(args.photos, args.assets, args.fonts, args.out)
    elif args.cmd == "write":
        write(args.out)
    elif args.cmd == "score":
        score(args.out, args.yuv_tag, args.json)
    else:
        detail(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
