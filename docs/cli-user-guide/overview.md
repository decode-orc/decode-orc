# CLI User Guide

## Overview

`orc-cli` is the command-line interface for Decode Orc, a cross-platform orchestration and processing framework for LaserDisc and tape decoding workflows. It provides a text-based interface for batch processing video projects defined in `.orcprj` files.

The CLI uses the same core processing library as the GUI (`orc-gui`), ensuring that project files created in the graphical interface can be executed unchanged via the command line, and vice versa.

## Key Features

- **Batch Processing**: Process complete DAG pipelines without user interaction
- **Automation**: Integrate into scripts and automated workflows
- **Reproducibility**: Execute the same project file consistently across runs
- **Progress Tracking**: Real-time progress updates during processing
- **Flexible Logging**: Configurable logging levels and output destinations
- **Crash Reporting**: Automatic diagnostic bundle creation for troubleshooting

## Basic Usage

### Command Syntax

```bash
orc-cli <project-file> [options]
orc-cli --source/--filters/--sink <...>
orc-cli plugins <subcommand> [options]
orc-cli stages <subcommand> [options]
```

### Required Arguments

- `<project-file>`: Path to an Orc project file (`.orcprj`) — not needed by the
  `plugins` and `stages` subcommands, which do not run a pipeline

### Options

| Option | Description | Default |
|--------|-------------|---------|
| `--process` | Process the complete DAG pipeline (trigger all sink nodes) | Required |
| `--source GRAPH`, `-i GRAPH` | Input (source) stage(s), for the source/filters/sink triad | - |
| `--filters GRAPH`, `-f GRAPH` | Processing stage(s), for the triad | - |
| `--sink GRAPH`, `-o GRAPH` | Output (sink) stage(s), for the triad | - |
| `--export-project FILE` | Save the assembled filtergraph as a `.orcprj` file instead of running it | - |
| `--video-format NTSC\|PAL\|PAL-M` | Set the video format if no stage implies one (works when running directly too, but only `--export-project` requires it) | - |
| `--source-type composite\|yc` | Same idea, for the source signal type | - |
| `--log-level LEVEL` | Set logging verbosity level | `info` |
| `--log-file FILE` | Write logs to specified file | None (console only) |
| `--log-out console\|file\|both` | Where log output is sent | `both` |
| `--help`, `-h` | Display help message and exit | - |

### Log Levels

Available log levels (from most to least verbose):

- `trace`: Extremely detailed debugging information
- `debug`: Detailed debugging information
- `info`: General informational messages
- `warn`: Warning messages
- `error`: Error messages
- `critical`: Critical errors only
- `off`: Disable logging

### Log Destinations

`--log-out` selects where log records are written:

- `console`: console only — any `--log-file` is ignored
- `file`: log file only — nothing is written to the console
- `both` (default): console, plus the log file when `--log-file` is given

`file` and `both` only reach a file when `--log-file` is also given. Asking for
`--log-out file` without a log file leaves nothing to write to, so logging
falls back to the console and a warning is emitted rather than discarding the
log silently.

The default (`both` with no `--log-file`) is plain console logging, which is
the behaviour of earlier releases.

## Examples

### Basic Processing

Process a project file with default settings:

```bash
orc-cli my-project.orcprj --process
```

### Detailed Logging

Enable debug logging for troubleshooting:

```bash
orc-cli my-project.orcprj --process --log-level debug
```

### Log to File

Save all log output to a file:

```bash
orc-cli my-project.orcprj --process --log-file processing.log
```

### Log to File Only

Keep the console clear and send every log record to the file instead:

```bash
orc-cli my-project.orcprj --process --log-file processing.log --log-out file
```

### Combined Options

Process with debug logging saved to file:

```bash
orc-cli my-project.orcprj --process --log-level debug --log-file debug.log
```

## Filtergraph Mode

As an alternative to authoring a `.orcprj` file, a decode pipeline can be
described directly on the command line with `--source`, `--filters`, and
`--sink` (short forms `-i`, `-f`, `-o`). This builds the same in-memory DAG a
`.orcprj` file would and triggers all sink nodes, so results are identical.
The `.orcprj` workflow itself is unchanged.

Video format (NTSC/PAL/PAL-M) and source signal type (composite/Y-C) are
**usually detected automatically** from the stage modules used — a stage
that is exclusively NTSC-compatible implies NTSC; a source stage with both
`y_path` and `c_path` set implies Y/C; one with `input_path` set implies
composite. If two stages imply conflicting formats, that is reported as an
error before anything runs. Some stages (`tbc_source` in particular) are
format-agnostic — they read their own format from a metadata sidecar file
rather than declaring one — so no stage in the graph may give any hint at
all; running in memory tolerates this, but see
[Exporting instead of running](#exporting-instead-of-running) below for the
one case where it matters.

### The source/filters/sink triad

`--source`, `--filters`, and `--sink` enforce that stages go where they
belong: **every stage named in `--source` must be a source, every stage in
`--sink` must be a sink, and every stage in `--filters` must be neither** (a
transform or similar processing stage). Putting a sink in `--source`, for
example, is rejected with a clear error naming the correct flag — this is
checked against each stage's real role (the same metadata the GUI uses), not
guessed from its name, so it works for any third-party plugin stage too.

```
stage_name=key=value:key=value, stage_name=key=value
```

- **Stages** within a single `--source`/`--filters`/`--sink` value are
  separated by `,` (or `;`, for a separate filterchain) and are
  auto-connected in order.
- **Values** may be wrapped in single quotes (`'...'`) **or** double quotes
  (`"..."`) — whichever is more convenient for your shell — to include `:`
  `,` `;` and spaces literally; the two quote styles are interchangeable, and
  a value quoted with one may freely contain the other. A value may also be
  escaped with a backslash (`\`) instead of quoting, and may itself contain
  `=`.

```bash
orc-cli \
  --source "tbc_source=input_path=capture.tbc" \
  --filters "dropout_correct" \
  --sink "video_sink=output_path=capture.mp4"
```

The same thing with short options and a CVBS source:

```bash
orc-cli -i "NTSC_CVBS_Source=input_path=capture.cvbs" -o "video_sink=output_path=capture.mp4"
```

Any of the three may be omitted, but at least one must be non-empty.

### Windows paths

Unquoted Windows paths (drive letters and backslashes) are parsed correctly:

```bash
orc-cli --source "tbc_source=input_path=C:\Users\me\capture.tbc" --sink video_sink
```

If a value needs to rule out any ambiguity — for instance it contains a
literal comma or semicolon — wrap it in single **or** double quotes,
whichever your shell passes through more conveniently.

### Non-linear graphs

Fan-in (multiple sources into one stage, e.g. a stacker) and fan-out (one
stage feeding several sinks) are fully supported, using `[label]` link
syntax to connect stages across `--source`/`--filters`/`--sink`:

```bash
orc-cli --source "tbc_source=input_path=a.tbc[a]; tbc_source=input_path=b.tbc[b]; tbc_source=input_path=c.tbc[c]" \
  --filters "[a][b][c] stacker" \
  --sink video_sink
```

### Piping between processes

A stage's `output_path`/`input_path` (any `FILE_PATH` parameter) may be set
to `-` instead of a real path, meaning the CLI process's own standard
input/output — so pipelines can be chained through the shell instead of
always going through an intermediate file:

```bash
orc-cli -i "tbc_source=input_path=capture.tbc" -o "CVBSSink=output_path=-" \
  | orc-cli -i "ntsc_cvbs_stream_source=input_path=-:sample_encoding=CVBS_U10_4FSC" \
            -o "video_sink=output_path=-" \
  | ffplay -
```

Running it is **CLI only** — a GUI process has no stdin/stdout of its own
to redirect to. The GUI's own parameter editor still accepts and saves `-`,
since the officially supported workflow is to build a project in the GUI
and run it with `orc-cli project.orcprj --process`; the GUI just cannot
preview or trigger a node configured that way itself, and shows it as
unconfigured rather than ready to run. Not every stage or every
configuration can honour it: at most one node in the graph
may target stdin and at most one may target stdout, and every stage between
a piped source and a piped sink must be able to run in a single forward
pass with no seeking back (a Y/C CVBS export needs two streams, for
example, so it refuses to pipe; MP4/MOV need to patch a header at the end,
so `video_sink` falls back to a pipe-safe container such as `nut-ffv1`
instead). An incompatible combination is rejected with a specific error
before anything runs, not partway through. See the stage's own
`instructions.md` (`orc-cli stages help <stage>`) for whether and how it
supports `-`, and
[plugin-architecture.md's Stdio Piping Convention](../technical/plugin-architecture.md#stdio-piping-convention)
for the full mechanism, if you're authoring a plugin stage of your own.

A stream source reading stdin can feed several sinks at once, using the
`[label]` fan-out syntax. stdin is still read once; each sink gets the
whole stream and they run side by side, at the pace of the slowest:

```bash
producer | orc-cli -i "ntsc_cvbs_stream_source=input_path=-:sample_encoding=CVBS_U10_4FSC[s]" \
  -o "[s] CVBSSink=output_path=copy.cvbs; [s] video_sink=output_path=preview.mkv"
```

### Exporting instead of running

`--export-project` builds the project exactly as `--source`/`--filters`/
`--sink` normally would, but saves it as a `.orcprj` file instead of
triggering it — using the same `saveProject()` writer the GUI's "Save As"
uses, so this isn't a new save format, just a different way to reach the
existing one:

```bash
orc-cli --source "tbc_source=input_path=capture.tbc" --sink video_sink \
  --export-project capture.orcprj
```

Useful for building a project quickly from the command line and then
opening it in the GUI, or reusing it later with `--process`.

A saved `.orcprj` file requires an explicit video format *and* source signal
type — unlike running in memory, which tolerates either being undetermined.
If no stage implies one (a format-agnostic source like `tbc_source` reads
its own format from its metadata sidecar file rather than implying one),
`--video-format` and/or `--source-type` set them explicitly:

```bash
orc-cli --source "tbc_source=input_path=capture.tbc" --sink video_sink \
  --export-project capture.orcprj --video-format NTSC --source-type composite
```

`--video-format`/`--source-type` also work without `--export-project` — set
them when running a graph directly and no stage implies a format, and the
graph gets the same format-specific parameter defaults it would after being
exported and reprocessed with `--process`, rather than only the
exported/reprocessed path ever getting a concrete value. `--export-project`
is the only thing that actually *requires* one, since it's the `.orcprj`
file format itself that demands an explicit value, not the pipeline.

### Discovering stages and their parameters

Every stage's parameter names, types, and which ones are required come from
the same descriptors the GUI uses (`getStageParameters()`), so a mismatch
between a filtergraph and what a stage actually accepts is caught before
anything runs — see the "Missing required parameter" and "not recognised"
errors below. Ask `orc-cli stages` for the exact name and parameters of any
stage this build can run; see [Stage introspection](#stage-introspection).

## Processing Workflow

When you run `orc-cli --process`, the following occurs:

1. **Project Loading**: The `.orcprj` file is loaded and validated
2. **DAG Construction**: The processing pipeline is built from the project definition
3. **Validation**: Input files and parameters are verified
4. **Sink Triggering**: All sink nodes in the DAG are triggered sequentially
5. **Progress Reporting**: Real-time progress updates are displayed (every 5%)
6. **Completion**: Exit code indicates success (0) or failure (non-zero)

### Progress Output

During processing, you'll see progress updates like:

```
[2026-02-08 10:15:23.456] [cli] [info] Loading project: my-project.orcprj
[2026-02-08 10:15:23.789] [cli] [info] Project loaded: My Video Project
[2026-02-08 10:15:24.012] [cli] [info] [Progress: 0%] Starting decoding...
[2026-02-08 10:15:45.234] [cli] [info] [Progress: 5%] Processing frames...
[2026-02-08 10:16:12.567] [cli] [info] [Progress: 10%] Processing frames...
...
[2026-02-08 10:25:34.890] [cli] [info] [Progress: 100%] Decode complete
```

## Plugin management

`orc-cli plugins` edits the same registry file the GUI's **Plugin Manager**
edits,
and uses the same words for the same things. Changes take effect at the next
application launch, never mid-run.

### Selectors

Every identifier `plugins list` prints is accepted verbatim by the commands
that take one, so a line of listing output is usable as input without editing.
A **selector** is the entry's plugin id when it has one, and otherwise a
`path:<path>` or `url:<asset-url>` handle; bare paths and asset URLs are
accepted too. A selector that matches more than one entry is an error listing
the candidates, never a guess.

### Installed plugins

```bash
orc-cli plugins list                    # plugins you installed
orc-cli plugins list --core             # ...and the ones that ship with Orc
orc-cli plugins list --check-updates    # ...with each entry's update status
```

**Core plugins are hidden by default**, matching the Plugin Manager's unticked
**Show core plugins**; `--core` (or `--all`) includes them. Each entry reports
its `selector`, `source` and a single `status` — `Enabled` when it will load at
the next launch, otherwise the one reason it will not (`Disabled`,
`Not trusted yet`, `Needs a rebuild` — with the Orc ABI number it needs a
rebuild for — `Binary missing`, `Core plugin`).

`plugins list` never goes to the network unless `--check-updates` is passed.

### Trust

Plugins execute code locally, so **adding, installing, updating and enabling a
plugin ask for confirmation before the binary is allowed to run** — the same
warning the GUI shows. Pass `--yes` to confirm without prompting. When stdin is
not a terminal and `--yes` is absent, the command fails immediately naming
`--yes` rather than waiting for an answer that cannot arrive.

```bash
orc-cli plugins add /path/to/plugin.so --yes
orc-cli plugins add --url https://github.com/owner/repo/releases --yes
orc-cli plugins enable com.example.myplugin --yes
orc-cli plugins remove --dry-run com.example.myplugin   # resolve, write nothing
```

Withdrawing a permission grants nothing, so the commands that only take
capability away never prompt (each still accepts `--yes`, so a script can
pass the flag uniformly):

```bash
orc-cli plugins disable com.example.myplugin
orc-cli plugins untrust com.example.myplugin
orc-cli plugins remove com.example.myplugin
```

`plugins trust` and `plugins untrust` remain as explicit primitives for
scripts. `trust` is what `enable` does implicitly, so it asks the same
question; the GUI has no separate trust control, because there adding,
installing and ticking **Enabled** are the trust-granting actions.

### Available plugins

```bash
orc-cli plugins search                       # the whole curated index
orc-cli plugins search chroma --compatible   # ...matching, with a build for this host
orc-cli plugins search --installed           # ...already installed
orc-cli plugins info org.example.plugin
orc-cli plugins install org.example.plugin --yes
```

Entries are annotated `installed`, `incompatible` or `unreachable`, exactly as
the GUI's **Browse Plugins…** dialog labels them. The id printed for an entry
is what `info` and `install` take, and for an installed entry it is also its
registry selector.

### Inspecting one plugin

```bash
orc-cli plugins info com.example.myplugin        # installed, indexed, or both
orc-cli plugins info path:/plugins/example.so    # an entry with no plugin id
```

`plugins info` accepts any selector `plugins list` prints and any id
`plugins search` prints: it describes the installed copy, the index entry, or
both when a plugin is both offered and installed. The fields it prints — and
their order — are what the Plugin Manager's **Details** pane and the **Browse
Plugins…** details pane show, so the two front ends describe a plugin
identically; for an installed copy the `installed` field says which version you
have and whether a newer release is published. An id found in neither place
fails with a message naming both places searched. A `path:` or `url:` selector
is registry-only, so it never goes to the network.

### Diagnostics

```bash
orc-cli plugins doctor
```

Reports the registry path, the runtime plugin search paths, and every
diagnostic the plugin runtime recorded while loading — the same lines the
Plugin Manager's **Diagnostics** section shows, prefixed `Info`, `Warning` or
`Error`. It reports rather than judges: finding problems is the answer to the
question, so it still exits `0`.

### Updating

```bash
orc-cli plugins updates                      # check every registered plugin
orc-cli plugins update com.example.myplugin --yes
orc-cli plugins update --all --yes           # every plugin with an update
```

An update downloads a fresh binary, so it is confirmed like an install;
`--all` confirms once for the whole batch and lists what it will update first.

## Stage introspection

`orc-cli stages` answers, without opening the GUI, what a stage is called, what
it takes and what it does — the same information the GUI's Add Stage menu,
parameter dialog and **Help...** dialog show, from the same source.

A **stage name** is the internal token — `tbc_source`, not "TBC Source". It is
what `stages info`, `stages help` and `--source`/`--filters`/`--sink` all
accept; the display name is reported as a separate field and is never accepted
where a name belongs.

### Listing stages

```bash
orc-cli stages list --core                 # every stage this build can run
orc-cli stages list --kind source --core   # just the source stages
orc-cli stages list --plugin com.example.myplugin
orc-cli stages list --format PAL --core    # only stages usable with PAL
```

**Core stages are hidden by default**, matching `plugins list` and the Plugin
Manager's unticked **Show core plugins**; `--core` (or `--all`) includes them,
and a note reports how many were hidden. `--kind` takes `source`, `filter`
(equivalently `transform`), `analysis` or `sink` — the categories the GUI's Add
Stage menu groups by. `sink` lists everything the `--sink` slot accepts,
analysis sinks included; `analysis` narrows to those alone. The plugin id each entry reports is a plugin selector, so
it feeds `plugins info` and `stages list --plugin` unchanged.

### Describing one stage

```bash
orc-cli stages info tbc_source
orc-cli stages info video_sink --format NTSC
```

Reports the stage's identity, then every parameter with its display name,
description, type, whether it is required, its default, any minimum, maximum or
allowed values, what it depends on, and the file-extension hint — the same
descriptor data the GUI's parameter dialog renders. `--format` reports the
defaults that video format selects, as the GUI does for a project of that
format.

Frame and line numbers are shown 1-based, as the GUI shows them, even though a
project file stores them 0-based.

### Pasting a stage into a project or a filtergraph

```bash
orc-cli stages info frame_map --yaml         # a .orcprj parameter block
orc-cli stages info frame_map --filtergraph  # stage=key=value:key=value
```

`--yaml` emits a node `parameters:` block with every default filled in, shaped
and indented exactly as the project writer emits one under `dag: nodes:`, so
it loads unmodified when pasted under a node in a `.orcprj` file. `--filtergraph` emits the form
`--source`/`--filters`/`--sink` take, quoted so it runs unmodified. Both carry
the **0-based** values a project file and the filtergraph parser read back,
not the 1-based numbers `stages info` displays.

### Stage instructions

```bash
orc-cli stages help tbc_source
```

Prints the stage's `instructions.md` as Markdown — the same document the GUI's
**Help...** context-menu action renders, read at runtime from beside the plugin
binary. A stage that ships no instructions reports that and exits non-zero,
rather than succeeding with nothing to say.

## Machine-readable output

Every query command takes `--json`:

```bash
orc-cli plugins list --json
orc-cli plugins search --json
orc-cli plugins info com.example.myplugin --json
orc-cli plugins updates --json
orc-cli plugins doctor --json
orc-cli stages list --core --json
orc-cli stages info tbc_source --json
```

`plugins list`, `plugins search`, `plugins info` and `plugins doctor` emit one
object; `plugins updates` and `stages list` emit an array. Keys are the
presenter's own field names — lower case with underscores, never a display
label — and a field with nothing to say is still there, as `""`, `[]` or
`null`, so every entry has the same shape.

```bash
# Enable everything that is registered but not trusted yet
orc-cli plugins list --json \
  | python3 -c 'import json,sys
for e in json.load(sys.stdin)["entries"]:
    if e["load_state"] == "not_trusted": print(e["selector"])' \
  | xargs -rn1 orc-cli plugins enable --yes
```

Points to script against:

- **Identifiers come back verbatim.** Every plugin object carries `selector`
  and every stage object carries `name`; pass either straight back to the
  commands that take one. No string surgery on the other fields.
- **States are stable ids, not the words the table prints.** `load_state` is
  `will_load` / `disabled` / `not_trusted` / `abi_mismatch` / `file_missing` /
  `core`; an update `status` is `up_to_date` / `update_available` /
  `unreachable` / `unknown` / `not_applicable`; a diagnostic `severity` is
  `info` / `warning` / `error`; a stage `kind` is the word `--kind` accepts.
  The labels stay in the human output, where they can be reworded.
- **Booleans and numbers are JSON booleans and numbers**, not the `yes`/`no`
  the table prints.
- **Parameter defaults are the stored, 0-based values**, matching `--yaml` and
  `--filtergraph` rather than the 1-based frame and line numbers `stages info`
  displays — this side is read by a machine, so it carries what a project file
  stores.
- **Only the document is on stdout.** Runtime log lines go to stderr in this
  mode, so `--json` output can be piped straight into a parser.
- `stages info` emits one format at a time: `--json`, `--yaml` or
  `--filtergraph`, not several.

## Exit Codes

- `0`: Success - all operations completed successfully
- `1`: Error - processing failed or invalid arguments

The `plugins` and `stages` subcommands add more specific codes so a script can
tell the cases apart:

- `2`: Not found - no plugin matched the selector, or it matched more than one;
  or no stage of that name is registered (near matches are listed)
- `3`: Unavailable - the curated index or a release could not be reached
- `4`: Trust declined - confirmation was required and not given; nothing was
  recorded

Always check the exit code in scripts:

```bash
if orc-cli project.orcprj --process; then
    echo "Processing successful!"
else
    echo "Processing failed!" >&2
    exit 1
fi
```

## Project Files

### Project File Format

Project files (`.orcprj`) are YAML-based files that define:

- Source configurations (input TBC files)
- DAG structure (processing nodes and connections)
- Node parameters (decoder settings, output formats, etc.)
- Project metadata (name, description, video format)
- Optional `required_plugins` metadata for third-party plugin-backed stages

When a project contains stages supplied by third-party plugins, Decode-Orc now
saves a root-level `required_plugins` block in the `.orcprj` file. Each entry
records the plugin identity, repository or release URL metadata, ABI
expectation, and the stage names from that plugin that are still used by the
project.

This block is refreshed every time the project is saved:

- Plugin entries are kept only if at least one of their stage names is still present in the current DAG
- Plugin metadata is refreshed from the current local plugin registry when available
- Stale entries are removed automatically if the corresponding plugin-backed stages were deleted while editing

If a project is opened on a machine where one of those stages is unavailable,
Decode-Orc uses the saved `required_plugins` metadata to give a more specific
missing-stage error that can point the user at the expected plugin and its
repository URL.

### Creating Projects

Project files are typically created using `orc-gui`, but they can also be:

- Hand-edited (with care - see technical documentation)
- Generated programmatically
- Version controlled (recommended for reproducibility)

### Project Compatibility

Projects created in the GUI can be executed in the CLI without modification. This ensures:

- Consistent results across interfaces
- Batch processing of GUI-created projects
- Easy integration into automated workflows

## Common Use Cases

### Batch Processing Multiple Files

Process multiple projects in a loop:

```bash
for project in *.orcprj; do
    echo "Processing $project..."
    orc-cli "$project" --process --log-level info
done
```

### Automated Workflow

Integrate into a processing pipeline:

```bash
#!/bin/bash
set -e

# Process video
orc-cli capture1.orcprj --process --log-file capture1.log

# Check for errors
if [ $? -ne 0 ]; then
    echo "Processing failed, check capture1.log"
    exit 1
fi

# Continue with next step...
```

### Monitoring Progress

Capture and monitor progress in real-time:

```bash
orc-cli project.orcprj --process 2>&1 | tee -a processing.log
```

## Error Handling

### Common Errors

**Project file not found:**
```
Error: No project file specified
```
→ Ensure the `.orcprj` file path is correct

**Missing command:**
```
Error: No command specified. You must use --process
```
→ Add the `--process` flag

**Processing failure:**
```
Failed to load project: <reason>
```
→ Check project file syntax and input file paths

**Filtergraph parse error:**
```
Failed to parse filtergraph: <reason> (at offset N)
```
→ Check the filtergraph syntax near character `N`; quote values containing
`:` `,` `;` or spaces (single or double quotes both work)

**Unknown stage:**
```
Unknown stage '<name>'.
```
→ Use a stage name that exists in your build

**Stage used in the wrong triad category:**
```
--source: stage 'video_sink' (Video Sink) is an output (sink) stage — it
belongs under --sink, not --source.
```
→ Move the stage to the flag matching its actual role

**Missing required parameter:**
```
Stage '<name>': missing required parameter '<param>'.
```
→ Supply the parameter

**Cannot export — no video format:**
```
Cannot export: none of the stages used imply a video format (NTSC/PAL/PAL-M),
so the saved project would fail to reload. Pass --video-format NTSC|PAL|PAL-M,
or run the pipeline directly instead of exporting.
```
→ Add `--video-format`, or add a format-specific source stage to the graph

**Cannot export — no source signal type:**
```
Cannot export: none of the stages used imply a source signal type
(composite/Y-C), so the saved project would fail to reload. Pass
--source-type composite|yc, or run the pipeline directly instead of
exporting.
```
→ Add `--source-type`, or ensure a source stage's parameters reveal its
signal type (`y_path`+`c_path`, or `input_path`)

### Crash Diagnostics

If `orc-cli` crashes unexpectedly, it automatically creates a diagnostic bundle containing:

- System information (OS, CPU, memory)
- Stack backtrace showing crash location
- Application logs
- Core dump file (when available)

The crash bundle is saved as a ZIP file in the current working directory, on
Linux, macOS and Windows alike:

```
crash_bundle_YYYYMMDD_HHMMSS.zip
```

When reporting issues, attach this bundle to your bug report on [GitHub Issues](https://github.com/decode-orc/decode-orc/issues).
See [Issue Reporting](../misc/issue-reporting.md) for full details of the bundle
contents and the locations used by `orc-gui`.
