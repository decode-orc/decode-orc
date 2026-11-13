#!/usr/bin/env python3
#
# File:        cut_project.py
# Module:      tools/perf-benchmark
# Purpose:     Make a benchmark cut of an .orcprj project
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# A research tool, outside the build and not run in CI. It reproduces the cut
# of issue #375: a Frame Map node with the given range is inserted in front of
# every sink (or of one node with --before), every relative *_path parameter
# is made absolute against the project's directory, and every output_path is
# moved into --output-dir. The input files are listed in OUT.inputs, one per
# line, for "bench.py run --evict-list".
#
# The project file is edited as text, so no YAML library is needed: .orcprj
# files are written in one fixed layout (orc/core/project_io).

import argparse
import os
import re
import sys

NODE_RE = re.compile(r"^    - id: (\d+)\s*$")


def split_nodes(lines):
    """Return (head, [node line lists], tail) around the nodes block."""
    start = next(i for i, l in enumerate(lines) if l.rstrip() == "  nodes:") + 1
    end = next(i for i, l in enumerate(lines) if l.rstrip() == "  edges:")
    head, body, tail = lines[:start], lines[start:end], lines[end:]
    nodes, cur = [], None
    for l in body:
        if NODE_RE.match(l):
            cur = [l]
            nodes.append(cur)
        elif cur is not None:
            cur.append(l)
    return head, nodes, tail


def node_field(node, name):
    for l in node:
        m = re.match(rf"^      {name}: (.*)$", l)
        if m:
            return m.group(1).strip()
    return None


def unquote(v):
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
        return v[1:-1]
    return v


def rewrite_paths(node, project_dir, output_dir, inputs):
    out, param = [], None
    for l in node:
        m = re.match(r"^        (\w+):\s*$", l)
        if m:
            param = m.group(1)
        m = re.match(r"^(          value: )(.*)$", l)
        if m and param and param.endswith("_path"):
            value = unquote(m.group(2).strip())
            if value and value != "-":
                if param == "output_path":
                    value = os.path.join(output_dir, os.path.basename(value))
                else:
                    if not os.path.isabs(value):
                        value = os.path.normpath(os.path.join(project_dir, value))
                    inputs.append(value)
                l = f"{m.group(1)}{value}\n"
        out.append(l)
    return out


def frame_map_node(node_id, ranges, x, y):
    return [
        f"    - id: {node_id}\n",
        "      stage: frame_map\n",
        "      node_type: TRANSFORM\n",
        "      display_name: Frame Map\n",
        "      user_label: Benchmark cut\n",
        f"      x: {x}\n",
        f"      y: {y}\n",
        "      parameters:\n",
        "        ranges:\n",
        "          type: string\n",
        f"          value: {ranges}\n",
    ]


def main():
    ap = argparse.ArgumentParser(description="Make a benchmark cut of a project.")
    ap.add_argument("project")
    ap.add_argument("out")
    ap.add_argument("--range", required=True,
                    help="frame_map ranges value, 0-based, e.g. 20000-20499")
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--before", type=int, action="append",
                    help="node id to cut in front of (default: every sink)")
    args = ap.parse_args()

    with open(args.project) as f:
        lines = f.readlines()
    project_dir = os.path.dirname(os.path.abspath(args.project))
    output_dir = os.path.abspath(args.output_dir)
    os.makedirs(output_dir, exist_ok=True)

    head, nodes, tail = split_nodes(lines)
    inputs = []
    nodes = [rewrite_paths(n, project_dir, output_dir, inputs) for n in nodes]

    ids = [int(NODE_RE.match(n[0]).group(1)) for n in nodes]
    targets = args.before or [i for i, n in zip(ids, nodes)
                              if node_field(n, "node_type") == "SINK"]
    if not targets:
        print("no sink found to cut in front of", file=sys.stderr)
        return 1

    edges_text = "".join(tail)
    edges = [(int(a), int(b)) for a, b in
             re.findall(r"- from: (\d+)\s+to: (\d+)", edges_text)]
    next_id = max(ids) + 1
    new_edges = list(edges)
    for target in targets:
        node = nodes[ids.index(target)]
        x = int(node_field(node, "x") or 0)
        y = int(node_field(node, "y") or 0)
        nodes.append(frame_map_node(next_id, args.range, x - 110, y + 120))
        new_edges = [(a, next_id) if b == target else (a, b) for a, b in new_edges]
        new_edges.append((next_id, target))
        next_id += 1

    tail = ["  edges:\n"] + [f"    - from: {a}\n      to: {b}\n" for a, b in new_edges]
    with open(args.out, "w") as f:
        f.writelines(head)
        for n in nodes:
            f.writelines(n)
        f.writelines(tail)
    with open(args.out + ".inputs", "w") as f:
        for p in inputs:
            f.write(p + "\n")
            stem = os.path.splitext(p)[0]
            for sidecar in (".meta", ".dropouts.meta", ".db", "_audio_0.wav",
                            ".efm"):
                for candidate in (p + sidecar, stem + sidecar):
                    if os.path.exists(candidate):
                        f.write(candidate + "\n")
    print(f"wrote {args.out}: cut {args.range} in front of {targets}, "
          f"{len(inputs)} inputs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
