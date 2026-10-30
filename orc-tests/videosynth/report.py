#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
"""Write the videosynth suite's run report, and update its baselines.

Runs as the videosynth_report CTest fixture cleanup, after every test of the
run. Each test leaves <output-dir>/results/<Suite.Test>.json (written by
harness/videosynth_harness.cpp); the manifest lists every registered test and
its cases, so tests that did not run are reported too.

Writes <output-dir>/report.md and report.json. With
ORC_VIDEOSYNTH_UPDATE_BASELINES=1 it also rewrites baselines.json from the
run: each test that passed gets its metrics as measured, keeping any
tolerance already recorded.
"""

import argparse
import datetime
import json
import os
import sys
from pathlib import Path


def read_manifest(path):
    tests = []
    for line in Path(path).read_text().splitlines():
        if not line.strip():
            continue
        name, _, cases = line.partition(" ")
        tests.append((name, [c for c in cases.split(",") if c]))
    return tests


def read_generation(output_dir, case):
    """The case's generation record, or None when it was never generated."""
    case_dir = output_dir / case
    if not (case_dir / ".videosynth-stamp").exists():
        return None
    record = {}
    generation = case_dir / ".videosynth-generation"
    if generation.exists():
        for line in generation.read_text().splitlines():
            key, _, value = line.partition("=")
            record[key] = value
    return {
        "generated_at": record.get("generated_at"),
        "generation_ms": int(record["generation_ms"])
        if "generation_ms" in record
        else None,
    }


def collect(output_dir, manifest):
    results_dir = output_dir / "results"
    tests = []
    for name, cases in manifest:
        case_records = [
            {"name": case, "generation": read_generation(output_dir, case)}
            for case in cases
        ]
        result_path = results_dir / f"{name}.json"
        if result_path.exists():
            entry = json.loads(result_path.read_text())
        else:
            missing = [c["name"] for c in case_records if c["generation"] is None]
            if missing:
                reason = (
                    "not run: never generated "
                    + ", ".join(missing)
                    + " (its generation failed, or the test was not selected)"
                )
            else:
                reason = "not run: not selected in this run, or the test crashed"
            entry = {"test": name, "status": "not run", "reason": reason,
                     "elapsed_ms": None, "notes": [], "metrics": []}
        entry["cases"] = case_records
        tests.append(entry)
    return tests


def fmt(value):
    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.4g}"
    return str(value)


def generation_text(generation):
    if generation is None:
        return "not generated"
    if generation["generation_ms"] is None:
        return "no record"
    return str(generation["generation_ms"])


def difference(metric):
    if "baseline" not in metric or metric.get("value") is None:
        return None
    return metric["value"] - metric["baseline"]


def write_markdown(path, report):
    lines = ["# videosynth suite report", ""]
    lines.append(f"Run finished {report['finished_at']}.")
    lines.append("")
    if report["native_arch"]:
        lines.append(
            "> **Warning:** built with ORC_NATIVE_ARCH=ON. Floating-point "
            "metrics are not comparable with baselines.json."
        )
        lines.append("")
    if report["updating_baselines"]:
        lines.append(
            "> ORC_VIDEOSYNTH_UPDATE_BASELINES=1: baselines.json was "
            "rewritten from this run. Review the diff."
        )
        lines.append("")

    counts = {}
    for test in report["tests"]:
        counts[test["status"]] = counts.get(test["status"], 0) + 1
    lines.append(
        "| Status | Tests |\n|---|---|\n"
        + "\n".join(f"| {k} | {v} |" for k, v in sorted(counts.items()))
    )
    lines.append("")

    lines.append("| Test | Status | Run (ms) | Cases (generation ms) | Reason |")
    lines.append("|---|---|---|---|---|")
    for test in report["tests"]:
        cases = ", ".join(
            f"{c['name']} ({generation_text(c['generation'])})"
            for c in test["cases"]
        )
        reason = test.get("reason", "").replace("|", "\\|")
        lines.append(
            f"| {test['test']} | {test['status']} | {fmt(test.get('elapsed_ms'))}"
            f" | {cases} | {reason} |"
        )
    lines.append("")

    for test in report["tests"]:
        if not test.get("metrics") and not test.get("notes"):
            continue
        lines.append(f"## {test['test']}")
        lines.append("")
        for note in test.get("notes", []):
            lines.append(f"> {note}")
            lines.append("")
        if not test.get("metrics"):
            continue
        lines.append("| Metric | Value | Baseline | Tolerance | Difference | Better | Verdict |")
        lines.append("|---|---|---|---|---|---|---|")
        for m in test["metrics"]:
            lines.append(
                f"| {m['name']} | {fmt(m.get('value'))} | {fmt(m.get('baseline'))}"
                f" | {fmt(m.get('tolerance'))} | {fmt(difference(m))}"
                f" | {m['better']} | {m['verdict']} |"
            )
        lines.append("")
    path.write_text("\n".join(lines))


def write_baselines(path, baselines):
    """baselines.json with one line per metric, so a change reads as one line
    of the diff."""
    out = ["{"]
    if "_comment" in baselines:
        out.append('  "_comment": ' + json.dumps(baselines["_comment"], indent=4)
                   .replace("\n]", "\n  ]") + ",")
    out.append('  "tests": {')
    tests = sorted(baselines["tests"].items())
    for t, (test, metrics) in enumerate(tests):
        out.append(f"    {json.dumps(test)}: {{")
        items = sorted(metrics.items())
        for m, (name, entry) in enumerate(items):
            body = ", ".join(
                f"{json.dumps(k)}: {json.dumps(entry[k])}"
                for k in ("value", "tolerance", "better")
            )
            comma = "," if m + 1 < len(items) else ""
            out.append(f"      {json.dumps(name)}: {{{body}}}{comma}")
        out.append("    }" + ("," if t + 1 < len(tests) else ""))
    out.append("  }")
    out.append("}")
    path.write_text("\n".join(out) + "\n")


def update_baselines(path, tests):
    baselines = json.loads(path.read_text())
    recorded = baselines.setdefault("tests", {})
    updated, held = [], []
    for test in tests:
        if not test.get("metrics"):
            continue
        if test["status"] != "passed":
            held.append(test["test"])
            continue
        previous = recorded.get(test["test"], {})
        entries = {}
        for m in test["metrics"]:
            if m.get("value") is None:
                continue
            tolerance = previous.get(m["name"], {}).get(
                "tolerance", m["new_tolerance"])
            entries[m["name"]] = {"value": m["value"], "tolerance": tolerance,
                                  "better": m["better"]}
        recorded[test["test"]] = entries
        updated.append(test["test"])
    write_baselines(path, baselines)
    print(f"Updated {path}: {len(updated)} tests")
    for name in held:
        print(f"  not updated, it failed: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--baselines", required=True, type=Path)
    parser.add_argument("--native-arch", action="store_true")
    args = parser.parse_args()

    updating = os.environ.get("ORC_VIDEOSYNTH_UPDATE_BASELINES") == "1"
    tests = collect(args.output_dir, read_manifest(args.manifest))
    report = {
        "finished_at": datetime.datetime.now(datetime.timezone.utc)
        .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "native_arch": args.native_arch,
        "updating_baselines": updating,
        "tests": tests,
    }
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    write_markdown(args.output_dir / "report.md", report)
    print(f"Wrote {args.output_dir / 'report.md'}")
    if args.native_arch:
        print("Warning: ORC_NATIVE_ARCH=ON; metrics are not comparable with "
              "the baselines")
    if updating:
        if args.native_arch:
            print("Not updating the baselines from an ORC_NATIVE_ARCH build")
            return 1
        update_baselines(args.baselines, tests)
    return 0


if __name__ == "__main__":
    sys.exit(main())
