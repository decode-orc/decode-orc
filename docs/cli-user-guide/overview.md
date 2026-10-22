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
orc-cli nodes <subcommand> <project> [options]
orc-cli tools <subcommand> <project> [options]
```

### Required Arguments

- `<project-file>`: Path to an Orc project file (`.orcprj`) — not needed by the
  `plugins` and `stages` subcommands, which do not run a pipeline. The `nodes`
  and `tools` subcommands name their project after the subcommand instead (see
  [Editing project nodes](#editing-project-nodes) and
  [Running stage tools](#running-stage-tools)).

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

A stream source reading stdin or a named pipe can feed several sinks at
once, using the `[label]` fan-out syntax. The input is still read once; each
sink gets the whole stream and they run side by side, at the pace of the
slowest:

```bash
producer | orc-cli -i "ntsc_cvbs_stream_source=input_path=-:sample_encoding=CVBS_U10_4FSC[s]" \
  -o "[s] CVBSSink=output_path=copy.cvbs; [s] video_sink=output_path=preview.mkv"
```

Stage tools cannot run on a project that reads from a stream source
(`*_tbc_stream_source`, `*_cvbs_stream_source`). The Disc Mapper and Source
Alignment read their input more than once, and a stream can be read only once,
so `orc-cli tools run` refuses with exit code `5`. Decode to files first, then
map and align (see [Running stage tools](#running-stage-tools)).

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

## Editing project nodes

`orc-cli nodes` reads and edits the stages of an existing project, so a script
can take a template project made in the GUI and point it at new captures
without opening the GUI. It is the command-line form of the DAG editor and the
**Edit Parameters...** dialog.

```bash
orc-cli nodes list disc.orcprj
orc-cli nodes get disc.orcprj "Disc 1"
orc-cli nodes set disc.orcprj "Disc 1" input_path=/captures/disc1.tbc
orc-cli nodes set template.orcprj 1 input_path=disc2.tbc --output disc2.orcprj
```

### Naming a stage

A `<node>` is either the stage's id or its name, the label shown on the stage in
the GUI. An argument made only of digits is an id; anything else is a name,
matched exactly (case and spaces included). Names are unique within a project
and never made only of digits, so either form names exactly one stage. Give
your template's stages names you can script against, such as "Disc 1" or
"Disc 1 map", with **Rename Stage...** in the GUI.

A name or id that matches no stage exits with code `2` and lists the
project's stages with their ids and names.

### Listing stages

`nodes list` shows each stage's id, name, stage type and configuration status,
and the stage tools that apply to it. The status is the colour of the dot the
GUI draws on the stage: `green` (configured), `yellow` (partly configured) or
`red` (not configured).

For a CVBS source (`PAL_CVBS_Source`, `NTSC_CVBS_Source`, `PAL_M_CVBS_Source`)
it also reports the capture's signal state and `sequence_continuous`, read
from the `.meta` file beside the configured capture. `sequence_continuous`
false means the capture has at least one discontinuity, usually a player skip,
and needs the Disc Mapper. Both are `unknown` (`null` in JSON) when no capture
is configured or its `.meta` cannot be read.

### Showing a stage's parameters

`nodes get` shows every parameter the stage takes, with the value the project
stores, or the stage's default marked `(default)` when the project stores
none. Values are the stored form, as written in the project file: frame and
line numbers are 0-based here, unlike the GUI's parameter dialog and
`stages info`, which show them 1-based.

### Setting parameters

`nodes set` takes one or more `key=value` assignments. Only the first `=`
separates the key, so a value may contain `=`. Each value is checked before
anything is saved:

- **Unknown keys are refused** (exit code `2`). The valid keys are the ones the
  stage offers in this project. A source in a Y/C project, for instance, takes
  `y_path` and `c_path` but not `input_path`, and the message says so.
- **Values are converted to the parameter's type**: a whole number for an
  integer, a number for a decimal, and `true`/`false` (or `yes`/`no`, `on`/`off`,
  `1`/`0`) for a switch. A value that does not convert is refused (exit `1`).
- **Values are checked against the parameter's range and allowed values**, the
  same limits the GUI's parameter dialog applies (exit `1`).
- **The project checks the result.** A change the project refuses, such as a
  source whose capture does not match the other sources, is reported with exit
  code `5`.

If any assignment is refused, nothing is changed. Otherwise the new values are
merged into the stage's existing ones and the project is saved: over the
project file by default, or to the file named by `--output`, leaving the
original untouched.

Strings and paths are stored exactly as typed. A relative path or a
`${PROJECT_ROOT}` path is resolved against the directory of the saved project
file when the project runs, so it keeps working when the project file and its
captures move together. When you save with `--output` to another directory,
use absolute paths or make sure the relative ones still lead to the captures.

`nodes list` and `nodes get` never modify the project file. `nodes set` saves
the whole project, so it also writes the renamed labels if an older project
had duplicate stage names (see [Project Files](#project-files)).

`--process` runs the project afterwards:

```bash
orc-cli nodes set disc.orcprj "Disc 1" input_path=/captures/disc1.tbc \
  && orc-cli disc.orcprj --process
```

## Running stage tools

`orc-cli tools` runs the stage tools from the GUI's **Stage Tools** menu
against a project's stages. The ones that matter for scripting work out
parameter values from the captures themselves: the Disc Mapper writes a Frame
Map's frame list, and Source Alignment writes the alignment of the discs in a
stack. Without them a script would need the GUI for every new set of captures.

```bash
orc-cli tools list disc.orcprj
orc-cli tools run disc.orcprj field_mapping --node "Disc 1 map"
orc-cli tools run disc.orcprj field_mapping --node "Disc 1 map" --apply
```

Stages are named as for `nodes` (see [Naming a stage](#naming-a-stage)).

### Listing tools

`tools list <project>` shows, for every stage that has tools, each tool's id,
name and kind, whether it runs from the command line, and the parameters it
takes with their types and defaults. `tools list <project> <node>` shows one
stage.

| Tool id | Stage | What it does | From the CLI |
|---|---|---|---|
| `field_mapping` | Frame Map | Disc Mapper: builds the frame list from the picture numbers or CLV time codes, padding player skips | `--apply` writes `ranges` |
| `frame_map_range` | Frame Map | Frame Map Range Finder: the frames between `startAddress` and `endAddress` | `--apply` writes `ranges` |
| `frame_corruption` | Frame Map | Frame Corruption Generator: skip and repeat patterns for testing the Disc Mapper | `--apply` writes `ranges` and `seed` |
| `source_alignment` | Source Align | Aligns the discs of a stack by picture number or CLV time code | `--apply` writes `alignmentMap` and `alignmentMode` |
| `dropout_analysis`, `snr_analysis`, `burst_level_analysis` | The analysis sinks | Run the sink and report its per-frame results | Report only; `--csv` writes them |
| `mask_line_config`, `ffmpeg_preset_config` | Mask Line, video sinks | Preset pickers | No: set the parameters with `nodes set` |
| `dropout_editor` | Dropout Map | Marking dropouts by hand | No: GUI only |
| `teletext_analysis`, `nabts_analysis` | Teletext and NABTS sinks | Browse the decoded pages and records | No: the sinks write their files with `--process` |

The interactive tools stay in the GUI because they need a person looking at
the picture or choosing from a browser. `tools run` refuses them (exit `1`)
and names what to use instead.

### Running a tool

`tools run <project> <tool> --node <node>` runs the tool and reports what it
found: its summary, its statistics and, for a tool that writes parameters, the
values it would set. On its own this is a **dry run** and the project file is
not touched:

```bash
orc-cli tools run disc.orcprj field_mapping --node "Disc 1 map"
```

`--apply` writes those values to the stage and saves the project, over the
project file or to the file named by `--output`:

```bash
orc-cli tools run disc.orcprj field_mapping --node "Disc 1 map" --apply
orc-cli tools run disc.orcprj source_alignment --node Align --apply \
  --output aligned.orcprj
```

The values are merged into the stage's other parameters, so a Frame Map keeps
its padding settings when the Disc Mapper writes its frame list.

`--param key=value` sets one of the tool's own parameters (repeat it for
more), checked the same way as `nodes set` (exit `2` for a key the tool does
not take, exit `1` for a value that does not fit). Parameters you leave out
start from the defaults the GUI's tool dialog shows:

```bash
orc-cli tools run disc.orcprj field_mapping --node "Disc 1 map" \
  --param include_lead_in_out=false --apply
orc-cli tools run disc.orcprj frame_map_range --node "Disc 1 map" \
  --param startAddress=1000 --param endAddress=2000 --apply
orc-cli tools run stack.orcprj source_alignment --node Align \
  --param alignmentMode=first_common_frame --apply
```

A tool that should produce values and produces none, such as the Disc Mapper
on a capture with no readable picture numbers, counts as failed (exit `6`), so
a disc that cannot be mapped is never stacked silently.

Map each disc before aligning the stack: Source Alignment reads the discs
through their Frame Maps. [Refreshing a project](#refreshing-a-project) runs
them in that order for you.

### Checks before a tool runs

Before any analysis starts, `tools run` refuses (exit `5`) when:

- a source the stage reads from is not configured;
- the stage reads from a stream source (see
  [Piping between processes](#piping-between-processes));
- the stage reads from both `.tbc` and `.cvbs` captures. A stack takes all its
  discs from `tbc_source` or all from the CVBS sources, never a mix, and the
  message names the sources of each kind.

### Analysis sinks

The Dropout, SNR and Burst Level analysis sinks have one tool each. Running it
processes the sink's input, the same work as **Trigger Stage** on the sink, and
reports a summary (frames analysed, and the mean, minimum and maximum of each
measure) plus every frame's figures in `--json`. `--csv <file>` also writes
the per-frame results as CSV, in the sink's own format, without changing the
sink's saved settings:

```bash
orc-cli tools run stack.orcprj snr_analysis --node "SNR Analysis Sink" \
  --json --csv stack-snr.csv
```

A script can use these to drop a poor capture before stacking, for example a
disc whose mean white SNR is well below the others'.

### Progress and cancelling

Progress lines go to stderr, at most one every few per cent. `--quiet` turns
them off and limits the log to warnings and errors. Ctrl-C asks the tool to
stop and exits with code `130`; while the stages before the tool are still
being prepared the request waits, and a second Ctrl-C stops at once.

## Refreshing a project

A template can record which tool works out each stage's values, so one
command re-runs them all on new captures:

```bash
orc-cli tools refresh stack.orcprj
orc-cli tools refresh stack.orcprj --json --process > stack.report.json
```

For a stack this maps every disc and then aligns them, in the right order, and
saves the project once at the end. See
[Stacking multiple discs](multi-disc-stacking.md) for the whole workflow.

### Choosing a stage's refresh tool

Each stage can have one refresh tool, stored in the project with the tool
parameters to run it with. Only a tool that writes its result to the stage
can be one: the Disc Mapper, the Frame Map Range Finder and the Frame
Corruption Generator on a Frame Map, and Source Alignment on a Source Align.

In the GUI, open the tool from the stage's **Stage Tools** menu, set its
parameters, tick **Use this tool when refreshing from the CLI**, and press
**Apply to Stage** or **OK**. Save the project to keep the choice.

From the command line, use `tools refresh-config`:

```bash
orc-cli tools refresh-config stack.orcprj "Disc 1 map" field_mapping
orc-cli tools refresh-config stack.orcprj "Disc 2 map" field_mapping \
  --param include_lead_in_out=false
orc-cli tools refresh-config stack.orcprj Align source_alignment
orc-cli tools refresh-config stack.orcprj "Disc 2 map" --clear
```

`--param` is checked as for `tools run`; parameters you leave out take the
tool's defaults when the refresh runs. A tool the stage does not offer exits
with code `2`; a tool that writes nothing to the stage, such as an analysis
sink's, exits with `1`. The project is saved, over the project file or to the
file named by `--output`. `nodes list` and `nodes get` show each stage's
refresh tool ("Refreshed by").

### What a refresh does

`tools refresh <project>`:

1. **Checks everything first.** Each refresh tool must still be one the stage
   offers, its stored parameters must still exist, and the stage must pass the
   [checks before a tool runs](#checks-before-a-tool-runs). A problem stops
   the refresh before any tool runs, with the same exit codes as `tools run`.
2. **Runs the tools in pipeline order.** A stage runs after every stage it
   reads from, so each Frame Map is mapped before the Source Align that reads
   the discs through it. Stages the order leaves free run lowest id first.
3. **Applies each result before the next tool runs**, so Source Alignment sees
   the frame lists the Disc Mapper has just worked out.
4. **Stops at the first tool that fails**, produces no values (exit `6`), has
   its result refused (exit `5`) or is cancelled (exit `130`). The tools after
   it do not run.
5. **Saves once, at the end, only if every tool succeeded**, over the project
   file or to `--output`. A refresh never leaves a half-refreshed project: the
   file is either fully refreshed or unchanged.

A refresh always runs every refresh tool, even when the captures have not
changed since the last one.

Options:

- `--only <tool>[,<tool>...]` runs only the stages refreshed by those tools,
  in the same order. A tool no stage uses exits with code `2`.
- `--dry-run` runs every tool and applies each result in memory, so later
  tools see it, but saves nothing. It cannot be combined with `--output` or
  `--process`.
- `--process` processes the saved project afterwards, exactly as
  `orc-cli <project> --process` does. It runs only when the refresh saved; a
  processing failure exits with code `1`.
- `--json` writes the report described under
  [Machine-readable output](#machine-readable-output); `--quiet` turns off
  the progress lines.

Progress lines name the step (`[2/3] field_mapping on 'Disc 2 map'`).
Ctrl-C works as for `tools run`, and leaves the project unchanged.

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
orc-cli nodes list disc.orcprj --json
orc-cli nodes get disc.orcprj "Disc 1" --json
orc-cli tools list disc.orcprj --json
orc-cli tools run disc.orcprj field_mapping --node "Disc 1 map" --json
orc-cli tools refresh stack.orcprj --json
```

`plugins list`, `plugins search`, `plugins info`, `plugins doctor`,
`nodes get`, `tools run` and `tools refresh` emit one object; `plugins updates`, `stages list`,
`nodes list` and `tools list` emit an array. Keys are the
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
- **A project node object** carries `id`, `label`, `stage` and `status`
  (`green` / `yellow` / `red`); pass `id` or `label` back as a `<node>`.
  `nodes list` and `nodes get` add `tool_refresh`: the stage's refresh tool as
  `tool` and `parameters` (the stored values, typed), or `null` when it has
  none. `nodes list` adds `tools`, the ids of the stage tools that apply, and
  for a CVBS source `signal_state` and `sequence_continuous` (`null` when
  unknown).
  `nodes get` adds `parameters`, one object per parameter with `name`, `type`,
  `value` (typed as declared, the stored 0-based form), `set` (false when the
  value is the stage's default) and `declared` (false for a stored value the
  stage no longer takes).
- **A `tools list` entry** is a stage (`id`, `label`, `stage`) with `tools`:
  per tool `id`, `name`, `kind` (`analysis`, `batch_analysis`,
  `config_dialog`, `non_modal_editor`, `preview_utility` or
  `catalogue_browser`), `runnable`, `not_runnable_reason` when it is not,
  `can_apply`, and `parameters` (`name`, `type`, `default`, `allowed`,
  `description`).
- **A `tools run` report** is written whenever the tool ran, including when it
  failed or was cancelled; a refusal before the run prints only the error.

  ```json
  {
    "project": "disc.orcprj",
    "nodes": [
      {
        "id": 4, "node": "Disc 1 map", "stage": "frame_map",
        "tool": "field_mapping",
        "status": "success",
        "summary": "…",
        "statistics": { "paddingFrames": 12, "…": 0 },
        "items": [],
        "parameter_changes": { "ranges": "0-12345" },
        "sources": [
          { "id": 1, "label": "Disc 1", "stage": "NTSC_CVBS_Source",
            "signal_state": "STANDARD_STABLE_LOCKED",
            "sequence_continuous": false }
        ]
      }
    ],
    "applied": false,
    "saved": false,
    "saved_to": null
  }
  ```

  `status` is `success`, `failed` or `cancelled`. `parameter_changes` holds
  the values `--apply` writes (or wrote), and is `null` for a tool that writes
  none. `items` lists the tool's findings (`type`, `message`, `start_frame`,
  `end_frame`). `sources` lists the `.tbc` and `.cvbs` sources the stage reads
  from; a CVBS source adds `signal_state` and `sequence_continuous` (`null`
  when unknown). `nodes` is an array so a report covering several stages has
  the same shape. An analysis sink's report adds `results`: `kind` (`dropout`,
  `snr` or `burst_level`), `total_frames`, and `frames`, one object per frame
  with `frame` (1-based) and that sink's figures (`dropout_count` and
  `dropout_length_samples`; `white_snr` and `black_psnr`; or
  `median_burst_10bit`), `null` where a frame has no reading.
- **A `tools refresh` report** has one `nodes` entry per refreshed stage, in
  the order the tools ran, each in the `tools run` shape. A stage after a
  failure has `status` `skipped`. Instead of `applied` it carries:

  ```json
  {
    "project": "stack.orcprj",
    "dry_run": false,
    "nodes": [ { "node": "Disc 1 map", "tool": "field_mapping", "…": "…" } ],
    "completed": true,
    "saved": true,
    "saved_to": "stack.orcprj",
    "processed": null
  }
  ```

  `completed` is true when every tool succeeded. `processed` is `true` or
  `false` after `--process`, and `null` without it or when nothing was saved.
  Like the `tools run` report, it is written whenever a tool ran; a refusal
  before the first tool prints only the error.

## Exit Codes

- `0`: Success - all operations completed successfully
- `1`: Error - processing failed or invalid arguments

The `plugins`, `stages`, `nodes` and `tools` subcommands add more specific
codes so a script can tell the cases apart:

- `2`: Not found - no plugin matched the selector, or it matched more than one;
  or no stage of that name is registered (near matches are listed); or no
  stage in the project has that id or name; or `nodes set`,
  `tools run --param` or `tools refresh-config --param` named a parameter the
  stage or tool does not take; or the stage does not offer that tool (its tools are
  listed); or a stage's refresh tool, or one of its stored parameters, no
  longer exists; or `tools refresh --only` named a tool no stage is refreshed
  by
- `3`: Unavailable - the curated index or a release could not be reached
- `4`: Trust declined - confirmation was required and not given; nothing was
  recorded
- `5`: Project rejected - the project refused a `nodes set` change or a tool's
  result, for example a source that does not match the project's other
  sources; or `tools run` or `tools refresh` found a source not configured, a
  stream source, or `.tbc` and `.cvbs` sources mixed in one stack. Nothing was
  saved
- `6`: Analysis failed - the tool ran but failed, or produced none of the
  values it should; nothing was saved
- `130`: Cancelled - interrupted with Ctrl-C; nothing was saved

For `nodes` and `tools`, `1` covers a project that cannot be loaded or saved,
a value that does not fit its parameter's type, range or allowed values, and
for `tools run` an interactive tool, `--apply` on a tool with nothing to apply,
and options that do not go together (`--output` without `--apply`, `--csv`
with a tool that is not an analysis sink's). For `tools refresh-config` it
covers a tool that writes nothing to the stage; for `tools refresh`, a project
with no refresh tools, `--dry-run` with `--output` or `--process`, and a
`--process` run that fails. `3` and `4` never come from `nodes` or `tools`.

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

Each node's `user_label` (the name shown on the stage in the GUI) is unique
within the project, and is never empty or made only of digits. A project whose
labels break this rule, such as one saved by an older version or edited by
hand, still loads: duplicates get an ascending number appended ("Frame Map 1",
"Frame Map 2", … in node-id order). Processing a project, `nodes list` and
`nodes get` apply this in memory and do not rewrite the file; `nodes set`
saves the project and so writes the new names with it. See
[Migration](../misc/migration.md#unique-stage-names).

A node may carry an optional `tool_refresh` block naming the tool
`orc-cli tools refresh` runs for it, with the tool parameters in the same
typed form as the node's own `parameters`:

```yaml
- id: 4
  stage: frame_map
  user_label: Disc 1 map
  tool_refresh:
    tool: field_mapping
    parameters:
      include_lead_in_out: { type: bool, value: false }
  parameters:
    ranges: { type: string, value: "0-4999" }
```

Set it with the GUI's **Use this tool when refreshing from the CLI** option or
`orc-cli tools refresh-config` rather than by hand (see
[Refreshing a project](#refreshing-a-project)). Projects without it are
unaffected. A version of Decode-Orc older than the refresh feature opens such
a project but drops the block when it saves it.

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

### Stacking several discs

To decode several captures of one title with ld-decode, or capture them as
`.cvbs`, and stack them without opening the GUI for each batch, build one
template project in the GUI and drive it with `nodes set` and
`tools refresh`. [Stacking multiple discs](multi-disc-stacking.md) walks
through it for both capture formats, with complete scripts.

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
