# perf-benchmark

Research tools for measuring decode-orc throughput and memory, kept outside the build and not run in CI. They measured the baselines and results for [issue #375](https://github.com/decode-orc/decode-orc/issues/375).

The tools need Python 3 (standard library only), `perf`, and a Release build of `orc-cli`. The scripts default to `build/perf-release/bin/orc-cli`:

```
cmake -S . -B build/perf-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DORC_ENABLE_CLANG_TIDY=OFF -DCMAKE_CXX_FLAGS=-g -DCMAKE_C_FLAGS=-g
cmake --build build/perf-release
```

`-g` adds symbols for the profile; it does not change the generated code.

## Measuring on a shared host

Compare task-clock and cycles, not wall time. Other jobs on the host change the wall time but barely move the cycle count of a run.

`orc-cli` is started under `perf`. `perf_event_paranoid` 2 allows user-space counters (`cycles:u`) for our own children only, and `ptrace_scope` 1 forbids attaching to a process that is already running.

Each run starts cold: `--evict-list` drops the inputs from the page cache and the NFS client cache with `posix_fadvise(DONTNEED)`.

## Tools

| Tool | Purpose |
| --- | --- |
| `bench.py run` | Runs `orc-cli` once or more (`--repeat`). Records task-clock, cycles, instructions, peak RSS, bytes read, thread count, an RSS timeline, and the time to first output (`--watch`). `--profile` runs `perf record` instead and gives self-time shares by stage library, by category, and by symbol. `--memory-max` runs inside a systemd user scope with that limit and no swap, so a run that would exhaust the host is killed and recorded. `--stop-after` ends a run that many seconds after its first output. |
| `bench.py report` | A Markdown table of result files. |
| `cut_project.py` | The issue #375 cut: puts a Frame Map in front of every sink, makes input paths absolute, moves outputs to a local directory, and lists the inputs for `--evict-list`. |
| `baseline_runs.sh` | The real-capture cases: the 11-capture stack, a single LaserDisc capture, and tape-style composite and Y/C chains. Capture paths come from environment variables (see the script header). |
| `make_long_fixture.py` | A sparse fixture of any length (six hours by default) in every source form: `.tbc` composite and Y/C with `.tbc.db` and `.pcm`, a 44.1 kHz WAV for `audio_import`, and `.cvbs` with `.meta`, `.dropouts.meta` and a channel-pair WAV. The picture and audio files are sparse and read as zeros, so the fixture measures memory and index cost, not quality. |
| `fixture_runs.sh` | Every source-to-sink chain against a fixture: TBC, CVBS, audio and video sinks. |

Peak RSS comes from `wait4()` on `orc-cli`, so it is exact even when the peak falls at the very end of a run. The time to first output is when the watched output files first hold more than 1 MiB between them.

## Six-hour fixture

```
make_long_fixture.py /local/disk/fx-540k              # 540,000 PAL frames
make_long_fixture.py /local/disk/fx-54k --frames 54000
fixture_runs.sh /local/disk/fx-540k results fx540k
```

A six-hour fixture takes about 1.7 GB of real disk, mostly the dropout tables (20 runs a frame by default). Its sparse files appear as about 2.4 TB. Put it on a local disk: some tools copy sparse files in full.

A six-hour 48 kHz/24-bit stereo WAV holds 6.2 GB of samples, more than a RIFF header can state. The fixture's CVBS channel-pair WAV therefore carries its sizes modulo 2^32, as a 32-bit writer produces them. See [cvbs-file-format-specification#16](https://github.com/decode-orc/cvbs-file-format-specification/issues/16).
