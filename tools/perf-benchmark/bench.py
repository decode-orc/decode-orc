#!/usr/bin/env python3
#
# File:        bench.py
# Module:      tools/perf-benchmark
# Purpose:     Measure one orc-cli run: CPU, cycles, peak RSS, reads, profile
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# A research tool, outside the build and not run in CI. The development host
# is shared, so a run is judged by its CPU time and cycles (perf stat), not by
# wall time. orc-cli is started under perf: perf_event_paranoid 2 allows
# user-space counters on our own children only, and ptrace_scope 1 forbids
# attaching to a running process.
#
#   bench.py run --label NAME --out results/NAME.json [options] -- ORC_CLI_ARGS
#   bench.py report results/*.json
#
# The process tree is: bench.py -> perf -> bench.py exec-child -> orc-cli. The
# exec-child step reaps orc-cli with wait4(), which gives the exact peak RSS
# even when the peak falls in the last moments of the run (sinks that write
# their audio at the end do that). While orc-cli runs, bench.py samples its
# RSS, thread count and read counters from /proc four times a second.
#
# A watched output (--watch) gives the time to the first frame: the moment the
# watched files first hold more than --first-output-bytes between them.
# --stop-after then ends the run that many seconds later with SIGTERM
# (orc-cli ignores SIGINT), for runs too long to finish, such as the six-hour
# fixture. --memory-max runs the whole tree in a systemd user scope with that
# memory limit and no swap, so a run that would exhaust the host is killed and
# recorded as such instead.

import argparse
import glob
import json
import os
import platform
import re
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# Profile categories: (name, dso regex, symbol regex), first match wins, on
# self time. Matched against "perf report --sort dso,sym" rows.
DEFAULT_CATEGORIES = [
    ("FM Beat Cancel", r".", r"fm_beat::|FmBeat|BeatCancel"),
    ("Medians", r"stacker", r"median_sample|ConsensusLuma|nth_element|__introselect|insertion_sort"),
    ("Rest of the stacker", r"stacker", r"."),
    ("Picture Band Limit", r"picture[-_]band[-_]limit", r"."),
    ("FM Beat Cancel", r"fm[-_]beat", r"."),
    ("Chroma decode", r"libfftw", r"."),
    ("Chroma decode", r"sink", r"Transform|PalColour|Comb|Chroma|decodeLine|PalStill|Secam|Mono[A-Z]|Decoder"),
    ("Video sink (other)", r"video[-_]sink", r"."),
    ("Dropout Correct", r"dropout[-_]correct", r"."),
    ("Channel Equalise", r"channel[-_]equalise", r"."),
    ("Sources", r"(cvbs|tbc|vbi)[-_](stream[-_])?source", r"."),
    ("Other stages", r"orc-stage-plugin", r"."),
    ("Audio resample", r".", r"soxr|resample"),
    ("FFmpeg encode", r"libavcodec|libswscale|libavformat|libavfilter|libx26|libavutil", r"."),
    ("SQLite", r"libsqlite", r"."),
    ("Maths library (sincos, hypot)", r"libm\.so", r"."),
    ("Memory and C library", r"libc\.so|libc-|libstdc\+\+|ld-linux", r"."),
    ("Core (other)", r"liborc-core", r"."),
    ("Kernel", r"kernel", r"."),
]


# --------------------------------------------------------------------------
# exec-child: run the command, reap it with wait4, write its rusage.
# --------------------------------------------------------------------------

def exec_child(args):
    pid = os.fork()
    if pid == 0:
        try:
            os.execvp(args.cmd[0], args.cmd)
        except OSError as e:
            print(f"exec failed: {e}", file=sys.stderr)
            os._exit(127)
    with open(args.pidfile, "w") as f:
        f.write(str(pid))
    # Forward SIGTERM to the child so a stopped run still reports.
    signal.signal(signal.SIGTERM, lambda *_: os.kill(pid, signal.SIGTERM))
    while True:
        try:
            _, status, ru = os.wait4(pid, 0)
            break
        except InterruptedError:
            continue
    result = {
        "maxrss_kb": ru.ru_maxrss,
        "utime_s": ru.ru_utime,
        "stime_s": ru.ru_stime,
        "majflt": ru.ru_majflt,
        "minflt": ru.ru_minflt,
        "inblock": ru.ru_inblock,
        "nvcsw": ru.ru_nvcsw,
        "nivcsw": ru.ru_nivcsw,
        "exit_code": os.WEXITSTATUS(status) if os.WIFEXITED(status) else None,
        "signal": os.WTERMSIG(status) if os.WIFSIGNALED(status) else None,
    }
    with open(args.rusage, "w") as f:
        json.dump(result, f)
    if os.WIFSIGNALED(status):
        return 128 + os.WTERMSIG(status)
    return os.WEXITSTATUS(status)


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------

def read_proc(pid):
    out = {}
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                k, _, v = line.partition(":")
                if k in ("VmRSS", "VmHWM", "Threads", "RssAnon", "RssFile"):
                    out[k] = int(v.split()[0])
    except OSError:
        return None
    try:
        with open(f"/proc/{pid}/io") as f:
            for line in f:
                k, _, v = line.partition(":")
                out[k.strip()] = int(v)
    except OSError:
        pass
    return out


def evict(paths):
    """Drop the page cache (and NFS client cache) for the given files."""
    n = 0
    for p in paths:
        try:
            fd = os.open(p, os.O_RDONLY)
        except OSError:
            continue
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            n += 1
        finally:
            os.close(fd)
    return n


def watched_bytes(patterns):
    total = 0
    for pat in patterns:
        for p in glob.glob(pat):
            try:
                total += os.stat(p).st_size
            except OSError:
                pass
    return total


def git_state():
    try:
        head = subprocess.run(["git", "-C", REPO, "rev-parse", "--short=10", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", REPO, "status", "--porcelain",
                                "--untracked-files=no"],
                               capture_output=True, text=True).stdout.strip()
        return head + ("-dirty" if dirty else "")
    except OSError:
        return None


def parse_perf_stat(path):
    out = {}
    try:
        with open(path) as f:
            for line in f:
                if not line.strip() or line.startswith("#"):
                    continue
                parts = line.strip().split(",")
                if len(parts) < 3:
                    continue
                value, _unit, event = parts[0], parts[1], parts[2]
                try:
                    v = float(value)
                except ValueError:
                    v = None  # "<not counted>" / "<not supported>"
                out[event.split(":")[0]] = v
    except OSError:
        pass
    return out


def perf_rows(perf_data, time_window=None):
    """(pct, dso, sym) self-time rows, optionally within a time window."""
    base = ["perf", "report", "-i", perf_data, "--stdio", "--no-children",
            "--sort", "dso,sym", "--percent-limit", "0.05", "-q"]
    if time_window:
        base += ["--time", time_window]
    rows = []
    for extra in (["--comms", "orc-cli"], []):
        # Worker threads keep the orc-cli comm; fall back to every comm.
        rep = subprocess.run(base + extra, capture_output=True, text=True)
        for line in rep.stdout.splitlines():
            m = re.match(r"\s*([\d.]+)%\s+(\S+)\s+\[[.k]\]\s+(.*)$", line)
            if m:
                rows.append((float(m.group(1)), m.group(2), m.group(3).strip()))
        if rows:
            break
    return rows


def profile_shares(perf_data, categories, time_window=None):
    """Self-time shares by DSO, by category and top symbols."""
    rows = perf_rows(perf_data, time_window)
    by_dso, by_cat = {}, {}
    for pct, dso, sym in rows:
        by_dso[dso] = by_dso.get(dso, 0.0) + pct
        cat = "Other"
        for name, dso_re, sym_re in categories:
            if re.search(dso_re, dso) and re.search(sym_re, sym):
                cat = name
                break
        by_cat[cat] = by_cat.get(cat, 0.0) + pct
    covered = sum(p for p, _, _ in rows)
    return {
        "time_window": time_window,
        "covered_pct": round(covered, 2),
        "by_category": {k: round(v, 2) for k, v in
                        sorted(by_cat.items(), key=lambda kv: -kv[1])},
        "by_dso": {k: round(v, 2) for k, v in
                   sorted(by_dso.items(), key=lambda kv: -kv[1])[:15]},
        "top_symbols": [{"pct": p, "dso": d, "sym": s[:120]} for p, d, s in rows[:25]],
    }


# --------------------------------------------------------------------------
# run
# --------------------------------------------------------------------------

def one_run(args, run_index, workdir):
    for pat in args.clean:
        for p in glob.glob(pat):
            if os.path.isfile(p):
                os.remove(p)
    evicted = 0
    if args.evict_list:
        with open(args.evict_list) as f:
            evicted = evict([l.strip() for l in f if l.strip()])

    pidfile = os.path.join(workdir, f"pid{run_index}")
    rusage = os.path.join(workdir, f"rusage{run_index}.json")
    statfile = os.path.join(workdir, f"stat{run_index}.csv")
    perf_data = os.path.join(workdir, f"perf{run_index}.data")

    child = [sys.executable, os.path.abspath(__file__), "exec-child",
             "--pidfile", pidfile, "--rusage", rusage, "--", args.orc_cli] + args.cmd
    if args.profile:
        perf = ["perf", "record", "-q", "-F", str(args.profile_freq), "-e", "cycles:u",
                "-k", "CLOCK_MONOTONIC", "-o", perf_data, "--"]
    else:
        perf = ["perf", "stat", "-x", ",", "-o", statfile,
                "-e", "task-clock,cycles:u,instructions:u", "--"]
    cmd = perf + child
    if args.memory_max:
        cmd = ["systemd-run", "--user", "--scope", "-q",
               "-p", f"MemoryMax={args.memory_max}", "-p", "MemorySwapMax=0",
               "--"] + cmd

    load_start = os.getloadavg()
    log = open(os.path.join(workdir, f"console{run_index}.log"), "w")
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                            cwd=args.cwd or None)
    pid = None
    peak = {"VmRSS": 0, "Threads": 0}
    last = {}
    timeline = []
    ttff = None
    stop_at = None
    stopped_by_bench = None
    while proc.poll() is None:
        now = time.monotonic() - t0
        if pid is None and os.path.exists(pidfile):
            try:
                pid = int(open(pidfile).read().strip())
            except ValueError:
                pid = None
        if pid is not None:
            s = read_proc(pid)
            if s:
                # Counters only grow; an exiting process can return
                # status without io, so keep the largest value seen.
                for k in ("rchar", "read_bytes"):
                    last[k] = max(last.get(k, 0), s.get(k, 0))
                peak["VmRSS"] = max(peak["VmRSS"], s.get("VmRSS", 0))
                peak["Threads"] = max(peak["Threads"], s.get("Threads", 0))
                if not timeline or now - timeline[-1][0] >= args.timeline_step:
                    timeline.append((round(now, 2), s.get("VmRSS", 0) // 1024,
                                     s.get("rchar", 0) // (1 << 20)))
        if ttff is None and args.watch and \
                watched_bytes(args.watch) > args.first_output_bytes:
            ttff = now
            if args.stop_after is not None:
                stop_at = now + args.stop_after
        if pid is not None and stopped_by_bench is None:
            if (stop_at is not None and now >= stop_at) or \
                    (args.timeout and now >= args.timeout):
                stopped_by_bench = "stop-after" if stop_at is not None and now >= stop_at \
                    else "timeout"
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        time.sleep(0.25)
    wall = time.monotonic() - t0
    log.close()

    ru = {}
    if os.path.exists(rusage):
        with open(rusage) as f:
            ru = json.load(f)
    run = {
        "run": run_index,
        "wall_s": round(wall, 2),
        "ttff_s": round(ttff, 2) if ttff is not None else None,
        "stopped_by_bench": stopped_by_bench,
        "exit_code": ru.get("exit_code", proc.returncode),
        "signal": ru.get("signal"),
        "killed_oom": ru.get("signal") == signal.SIGKILL and stopped_by_bench is None,
        "max_rss_mb": round(ru.get("maxrss_kb", 0) / 1024, 1) if ru else None,
        "sampled_peak_rss_mb": round(peak["VmRSS"] / 1024, 1),
        "threads_max": peak["Threads"],
        "user_s": round(ru.get("utime_s", 0), 2),
        "sys_s": round(ru.get("stime_s", 0), 2),
        "rchar_mb": round(last.get("rchar", 0) / (1 << 20), 1),
        "read_bytes_mb": round(last.get("read_bytes", 0) / (1 << 20), 1),
        "major_faults": ru.get("majflt"),
        "evicted_files": evicted,
        "load_start": [round(x, 1) for x in load_start],
        "load_end": [round(x, 1) for x in os.getloadavg()],
        "rss_timeline": thin(timeline, 60),
    }
    if args.profile:
        run["profile"] = profile_shares(perf_data, DEFAULT_CATEGORIES)
        if ttff is not None:
            # The decode phase: from the first output to the end. perf's
            # timestamps use CLOCK_MONOTONIC (-k), the clock of t0.
            run["profile_after_first_output"] = profile_shares(
                perf_data, DEFAULT_CATEGORIES, f"{t0 + ttff:.6f},")
        if not args.keep_perf_data:
            os.remove(perf_data)
    else:
        st = parse_perf_stat(statfile)
        tc = st.get("task-clock")
        run["task_clock_s"] = round(tc / 1000.0, 2) if tc else None
        run["cycles_g"] = round(st["cycles"] / 1e9, 2) if st.get("cycles") else None
        run["instructions_g"] = round(st["instructions"] / 1e9, 2) \
            if st.get("instructions") else None
        run["cpus_utilised"] = round(tc / 1000.0 / wall, 2) if tc else None
    return run


def thin(timeline, n):
    if len(timeline) <= n:
        return timeline
    step = len(timeline) / n
    out = [timeline[int(i * step)] for i in range(n)]
    out.append(timeline[-1])
    return out


def cmd_run(args):
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    workdir = tempfile.mkdtemp(prefix="bench-", dir=args.workdir)
    result = {
        "label": args.label,
        "notes": args.notes,
        "orc_cli": os.path.abspath(args.orc_cli),
        "args": args.cmd,
        "git": git_state(),
        "host": {"nproc": os.cpu_count(), "kernel": platform.release(),
                 "node": platform.node()},
        "memory_max": args.memory_max,
        "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "workdir": workdir,
        "runs": [],
    }
    for i in range(args.repeat):
        run = one_run(args, i, workdir)
        result["runs"].append(run)
        print(json.dumps({k: v for k, v in run.items()
                          if k not in ("rss_timeline", "profile")}), flush=True)
        with open(args.out, "w") as f:
            json.dump(result, f, indent=1)
    if args.profile:
        for run in result["runs"]:
            print(json.dumps(run["profile"]["by_category"]))
    return 0


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------

def fmt(v, nd=1):
    if v is None:
        return "—"
    if isinstance(v, float):
        return f"{v:.{nd}f}"
    return str(v)


def cmd_report(args):
    rows = []
    for path in args.files:
        with open(path) as f:
            r = json.load(f)
        for run in r["runs"]:
            rows.append((r["label"], run))
    print("| Case | Run | Task-clock (s) | Cycles (G) | Wall (s) | CPUs | Peak RSS (MB) "
          "| Read (MB) | First output (s) | End |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |")
    for label, run in rows:
        if "profile" in run:
            continue
        end = "ok" if run.get("exit_code") == 0 else \
            ("memory limit" if run.get("killed_oom") else
             (run.get("stopped_by_bench") or f"exit {run.get('exit_code')}"))
        print(f"| {label} | {run['run']} | {fmt(run.get('task_clock_s'))} "
              f"| {fmt(run.get('cycles_g'), 2)} | {fmt(run.get('wall_s'))} "
              f"| {fmt(run.get('cpus_utilised'), 2)} | {fmt(run.get('max_rss_mb'), 0)} "
              f"| {fmt(run.get('rchar_mb'), 0)} | {fmt(run.get('ttff_s'))} | {end} |")
    profiles = [(l, r) for l, r in rows if "profile" in r]
    for label, run in profiles:
        whole = run["profile"]["by_category"]
        decode = (run.get("profile_after_first_output") or {}).get("by_category")
        print(f"\nProfile, {label} (self time, cycles:u):\n")
        if decode:
            print("| Where | Whole run | After first output |")
            print("| --- | ---: | ---: |")
            for k in sorted(set(whole) | set(decode), key=lambda k: -whole.get(k, 0)):
                print(f"| {k} | {whole.get(k, 0):.1f}% | {decode.get(k, 0):.1f}% |")
        else:
            print("| Where | Whole run |")
            print("| --- | ---: |")
            for k, v in whole.items():
                print(f"| {k} | {v:.1f}% |")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Measure orc-cli runs.")
    sub = ap.add_subparsers(dest="command", required=True)

    ec = sub.add_parser("exec-child")
    ec.add_argument("--pidfile", required=True)
    ec.add_argument("--rusage", required=True)
    ec.add_argument("cmd", nargs=argparse.REMAINDER)

    r = sub.add_parser("run")
    r.add_argument("--label", required=True)
    r.add_argument("--out", required=True, help="result JSON")
    r.add_argument("--notes", default="")
    r.add_argument("--orc-cli", default=os.path.join(REPO, "build", "perf-release",
                                                     "bin", "orc-cli"))
    r.add_argument("--repeat", type=int, default=1)
    r.add_argument("--profile", action="store_true",
                   help="perf record instead of perf stat")
    r.add_argument("--profile-freq", type=int, default=199)
    r.add_argument("--keep-perf-data", action="store_true")
    r.add_argument("--memory-max", help="e.g. 24G: run inside a memory-limited scope")
    r.add_argument("--watch", action="append", default=[],
                   help="output file glob for the time to first output")
    r.add_argument("--first-output-bytes", type=int, default=1 << 20)
    r.add_argument("--stop-after", type=float,
                   help="SIGTERM this many seconds after the first output")
    r.add_argument("--timeout", type=float, help="SIGTERM after this many seconds")
    r.add_argument("--clean", action="append", default=[],
                   help="file glob removed before each run")
    r.add_argument("--evict-list", help="file listing inputs to evict from cache")
    r.add_argument("--cwd")
    r.add_argument("--workdir", help="where per-run logs and perf files go")
    r.add_argument("--timeline-step", type=float, default=1.0)
    r.add_argument("cmd", nargs=argparse.REMAINDER)

    rep = sub.add_parser("report")
    rep.add_argument("files", nargs="+")

    args = ap.parse_args()
    if getattr(args, "cmd", None) and args.cmd and args.cmd[0] == "--":
        args.cmd = args.cmd[1:]
    if args.command == "exec-child":
        return exec_child(args)
    if args.command == "run":
        return cmd_run(args)
    return cmd_report(args)


if __name__ == "__main__":
    sys.exit(main())
