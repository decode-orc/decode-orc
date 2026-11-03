#!/usr/bin/env python3
#
# File:        phase9_stack.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 9 item 1: the decoders on the full
#              Domesday chain (Stacker, Dropout Correction, Picture Band
#              Limit, Channel Equalise)
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase9_stack.py setup STACK.orcprj OUT [--orc-cli PATH]
#       From a side's stack project (sources, disc maps, FM Beat Cancel,
#       Source Align, Stacker, Dropout Correction, Picture Band Limit),
#       writes OUT/held.orcprj: the whole stack, Phase 7's held pictures,
#       Channel Equalise, one video sink per decoder and a CVBS sink for
#       the reference; and OUT/cross.orcprj: each pressing group stacked
#       on its own, through the same chain, over Phase 7's single-frame
#       stills. Each chain's Channel Equalise and domesday2d hanover_* are
#       calibrated on that chain's own output (OUT/calibration).
#   phase9_stack.py held OUT [--json FILE]
#   phase9_stack.py cross OUT [--json FILE]
#       phase7_real.py's steps 1 and 2 on these decodes: the held runs
#       against a reference built from the stack's own run, and the group
#       stacks against each other.
#
# A group is compared with the other groups, never with a disc it holds:
# a stack and its own discs share their noise.

import argparse
import json
import os
import sys

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import phase7_real  # noqa: E402

DECODERS = phase7_real.DECODERS
# Community South's pressings (the stacking comparison): two pressings of
# one mother (P, Q) and two other masters (R, S). DS7 is not in the stack.
GROUPS = {"P": ["DS1", "DS2", "DS4", "DS8", "DS12"],
          "Q": ["DS6", "DS9", "DS10"],
          "R": ["DS3"],
          "S": ["DS5", "DS11"]}
CHAIN_STAGES = ("stacker", "dropout_correct", "picture_band_limit")


class Raw(phase7_real.Project):
    """A project that also takes nodes' typed parameters as they are."""

    def add_raw(self, node, label, x, y, params=None):
        node_id = len(self.nodes) + 1
        self.nodes.append({
            "id": node_id, "stage": node["stage"],
            "node_type": node["node_type"],
            "display_name": node["display_name"], "user_label": label,
            "x": x, "y": y,
            "parameters": params if params is not None else
            node.get("parameters") or {}})
        return node_id


def _stack_nodes(stack):
    """{disc: (source node, map node, beat node)} and {stage: node} of the
    chain behind the Stacker, from a stack project."""
    with open(stack) as f:
        doc = yaml.safe_load(f)
    base = os.path.dirname(os.path.abspath(stack))
    by_id = {n["id"]: n for n in doc["dag"]["nodes"]}
    after = {}
    for e in doc["dag"]["edges"]:
        after.setdefault(e["from"], []).append(e["to"])
    discs = {}
    chain = {}
    for node in doc["dag"]["nodes"]:
        if node["stage"] == "PAL_CVBS_Source":
            source = dict(node)
            params = dict(node["parameters"])
            path = os.path.normpath(os.path.join(
                base, params["input_path"]["value"]))
            params["input_path"] = {"type": "string", "value": path}
            source["parameters"] = params
            dmap = by_id[after[node["id"]][0]]
            beat = by_id[after[dmap["id"]][0]]
            discs[node["user_label"]] = (source, dmap, beat)
        elif node["stage"] in CHAIN_STAGES:
            chain[node["stage"]] = node
    return discs, chain


def _chain(project, out, nodes, discs, tag, row, repeats=True):
    """|discs| stacked through the chain, on |row|; returns the Picture
    Band Limit node. |repeats| False turns the Stacker's still-frame
    stacking off."""
    disc_nodes, chain = nodes
    y = row * 600
    beats = []
    for i, disc in enumerate(discs):
        source, dmap, beat = disc_nodes[disc]
        params = dict(source["parameters"])
        params["input_path"] = {"type": "string", "value": os.path.relpath(
            params["input_path"]["value"], out)}
        s = project.add_raw(source, disc, 0, y + 60 * i, params)
        m = project.add_raw(dmap, f"{disc} map", 200, y + 60 * i)
        b = project.add_raw(beat, f"{disc} beat", 400, y + 60 * i)
        project.edge(s, m)
        project.edge(m, b)
        beats.append(b)
    align = project.add("source_align", "COMPLEX", "Source Align",
                        f"{tag} align", 600, y,
                        {"alignmentMap": ", ".join(
                            f"{i + 1}+0" for i in range(len(discs))),
                         "alignmentMode": "pad_for_alignment"})
    for b in beats:
        project.edge(b, align)
    last = align
    for k, stage in enumerate(CHAIN_STAGES):
        params = dict(chain[stage].get("parameters") or {})
        if stage == "stacker" and not repeats:
            params["still_frame_stacking"] = {"type": "bool", "value": False}
        n = project.add_raw(chain[stage], f"{tag} {stage}", 800 + 200 * k, y,
                            params)
        project.edge(last, n)
        last = n
    return last


def _calibrate(out, nodes, discs, tag, orc_cli):
    folder = os.path.join(out, "calibration")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, f"{tag}.orcprj")
    if not os.path.exists(path):
        # Without the still-frame search, which costs about 12 s a frame
        # of eleven discs: the VITS line the smear is fitted on lies
        # outside the active picture the copies cover, and the Hanover fit
        # gains only noise from them.
        p = Raw(f"{tag} calibration")
        band = _chain(p, folder, nodes, discs, tag, 0, repeats=False)
        eq = p.add("channel_equalise", "TRANSFORM", "Channel Equalise",
                   f"{tag} equalise", 1600, 0, {})
        sink = p.add("video_sink", "SINK", "Video Sink", f"{tag} domesday2d",
                     1800, 0, {"decoder_type": "domesday2d",
                               "output_path": f"{tag}.yuv",
                               "output_mode": "raw", "raw_format": "yuv"})
        p.edge(band, eq)
        p.edge(eq, sink)
        p.write(path)
        phase7_real._run([orc_cli, "tools", "run", path,
                          "channel_calibration", "--node", str(eq),
                          "--apply", "--quiet"])
        phase7_real._run([orc_cli, "tools", "run", path,
                          "domesday2d_calibration", "--node", str(sink),
                          "--apply", "--quiet"])
    with open(path) as f:
        doc = yaml.safe_load(f)
    cal = {}
    for node in doc["dag"]["nodes"]:
        if node["stage"] not in ("channel_equalise", "video_sink"):
            continue
        for k, v in (node.get("parameters") or {}).items():
            if k.startswith("smear_") or k.startswith("hanover_"):
                cal[k] = float(v["value"])
    return cal


def _decoders(project, eq, cal, name, tag, x, y, reference):
    for i, d in enumerate(DECODERS):
        params = {"decoder_type": d,
                  "output_path": f"decodes/{name}_{tag}_{d}.yuv",
                  "output_mode": "raw", "raw_format": "yuv",
                  "output_padding": 1}
        if d == "domesday2d":
            for k in ("hanover_uu", "hanover_uv", "hanover_vu", "hanover_vv"):
                params[k] = cal[k]
        sink = project.add("video_sink", "SINK", "Video Sink", f"{tag} {d}",
                           x, y + 100 * i, params)
        project.edge(eq, sink)
    if reference:
        sink = project.add("CVBSSink", "SINK", "CVBS Sink",
                           f"{tag} equalised composite", x, y + 300,
                           {"output_path": f"decodes/{name}_{tag}.cvbs"})
        project.edge(eq, sink)


def _picked(project, band, pictures, cal, tag, row):
    y = row * 600
    pick = project.add("frame_map", "TRANSFORM", "Frame Map",
                       f"{tag} pictures", 1400, y, {"ranges": pictures})
    eq = project.add("channel_equalise", "TRANSFORM", "Channel Equalise",
                     f"{tag} equalise", 1600, y,
                     {"smear_fraction": cal["smear_fraction"],
                      "smear_tau_samples": cal["smear_tau_samples"]})
    project.edge(band, pick)
    project.edge(pick, eq)
    return eq


def setup(stack, out, orc_cli):
    out = os.path.abspath(out)
    os.makedirs(os.path.join(out, "decodes"), exist_ok=True)
    nodes = _stack_nodes(stack)
    every = [d for g in GROUPS.values() for d in g]
    chains = {"stack": every}
    chains.update({f"group_{g}": discs for g, discs in GROUPS.items()})
    cal = {}
    for tag, discs in chains.items():
        cal[tag] = _calibrate(out, nodes, discs, tag, orc_cli)
        print(tag, " ".join(f"{k} {v:.4f}" for k, v in cal[tag].items()),
              flush=True)
    with open(os.path.join(out, "calibration.json"), "w") as f:
        json.dump(cal, f, indent=1)

    held = ",".join(f"{a}-{b}" for _, a, b in phase7_real.HELD)
    project = Raw("Domesday2D Phase 9 held stills, stacked")
    band = _chain(project, out, nodes, every, "stack", 0)
    eq = _picked(project, band, held, cal["stack"], "stack", 0)
    _decoders(project, eq, cal["stack"], "held", "stack", 1850, 0, True)
    project.write(os.path.join(out, "held.orcprj"))

    stills = ",".join(str(p) for p in phase7_real.STILLS)
    project = Raw("Domesday2D Phase 9 single-frame stills, group stacks")
    for row, g in enumerate(GROUPS):
        tag = f"group_{g}"
        band = _chain(project, out, nodes, GROUPS[g], tag, row)
        eq = _picked(project, band, stills, cal[tag], tag, row)
        _decoders(project, eq, cal[tag], "cross", tag, 1850, row * 600,
                  False)
    project.write(os.path.join(out, "cross.orcprj"))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup")
    s.add_argument("stack")
    s.add_argument("out")
    s.add_argument("--orc-cli", default=os.path.join(
        bench.ROOT, "build", "bin", "orc-cli"))
    args = ap.parse_args()
    setup(args.stack, args.out, args.orc_cli)
    return 0


if __name__ == "__main__":
    sys.exit(main())
