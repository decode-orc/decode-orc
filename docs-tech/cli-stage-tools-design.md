# CLI Stage Tools Design

Running stage tools (Disc Mapper, Source Alignment and the rest) from
`orc-cli`, so that multi-disc LaserDisc decodes can be mapped, aligned, stacked
and exported without opening the GUI. Both `.tbc` (ld-decode / vhs-decode) and
`.cvbs` (decode-orc container) captures are covered.

## 1. Problem

A typical batch job:

1. Decode N copies of the same disc with ld-decode (`.tbc`) or capture them
   into the CVBS container (`.cvbs`).
2. Per disc: map the frames (`frame_map`, Disc Mapper tool).
3. Across discs: align the sources (`source_align`, Source Alignment tool).
4. Stack (`stacker`) and export for emulation (e.g. `video_sink`,
   `daphne_vbi_sink`, `efm_sink`).

Steps 2 and 3 produce parameter values (`frame_map.ranges`,
`source_align.alignmentMap` / `alignmentMode`) that depend on the content of
each capture. Today they can only be produced through the GUI's stage tool
dialogs. `orc-cli` can run a finished project (`orc-cli <project> --process`)
or build one from a filtergraph, but it cannot:

- change a parameter on an existing project,
- run a stage tool, or
- apply a tool's result to the project and save it.

A script therefore cannot take a template project, point it at fresh
captures, refresh the content-derived parameters and process it.

## 2. What already exists

The building blocks are all Qt-free and already linked into `orc-cli`:

| Piece | Location | Notes |
|---|---|---|
| Tool interface | `orc/core/analysis/analysis_tool.h` | `analyze()` → `AnalysisResult`; `applyToGraph()` fills `parameterChanges` without touching the project |
| Tool registry | `orc/core/analysis/analysis_registry.h` | singleton; `findById`; applicability via `isApplicableToStage(stage_name)` |
| Plugin tool declarations | `orc/sdk/include/orc/plugin/orc_stage_tooling.h` | `StageToolProvider::get_stage_tools()` (kind: ConfigDialog, NonModalEditor, BatchAnalysis, CatalogueBrowser, …) |
| Generic apply | `AnalysisToolPresenter::applyResultToGraph` (`orc/presenters/src/analysis_tool_presenter.cpp`) | runs `applyToGraph`, then `project_io::set_node_parameters`, then invalidates the DAG; does **not** save |
| Tool presenters | `DiscMapperPresenter`, `SourceAlignmentPresenter`, … in `orc/presenters` | constructed from `ProjectPresenter::getCoreProjectHandle()`; `runAnalysis(node, params, progress_cb, cancel_cb)` |
| Tool discovery | `AnalysisPresenter::getToolsForStage` / `getToolParameters` | |
| Project editing | `ProjectPresenter` | `loadProject`, `saveProject`, `getNodes`, `getNodeParameters`, `setNodeParameters`, `getStageParameters` |
| Tool → presenter selection | `orc/gui/generic_analysis_dialog.cpp` | **GUI-only today**; picks the concrete presenter by tool id |

Both the disc mapper and source alignment execute the DAG up to their
inputs themselves (`DAGExecutor::execute_to_node`) and run their observers
on the decoded signal, so they need nothing the GUI provides.

## 3. Stage tool inventory

Every stage tool in the tree, from core `isApplicableToStage` and plugin
`get_stage_tools()`:

| Stage | Tool id | Declared by | Purpose | Writes | Automatable |
|---|---|---|---|---|---|
| `frame_map` | `field_mapping` (Disc Mapper) | core | Frame map from VBI picture numbers / CLV timecode and signal quality | `ranges` | **Yes** |
| `frame_map` | `frame_map_range` | core | Range from `startAddress` / `endAddress` (picture number or CLV timecode) | `ranges` | **Yes** |
| `frame_map` | `frame_corruption` | core | Skip/repeat/gap patterns for testing the disc mapper | `ranges`, `seed` | No (test aid) |
| `source_align` | `source_alignment` | core | Align sources by VBI frame number / CLV timecode | `alignmentMap`, `alignmentMode` | **Yes** |
| `mask_line` | `mask_line_config` | core + plugin | Preset selection | mask config | No (use `node set`) |
| `video_sink` (all video sinks) | `ffmpeg_preset_config` | core + plugin | Encoder preset selection | sink config | No (use `node set`) |
| `dropout_map` | `dropout_editor` | core + plugin | Interactive dropout editing | dropout lists | No (manual) |
| — | `vectorscope` | core | Preview view; applicable to no stage | — | No |
| `dropout_analysis_sink` | `dropout_analysis` | plugin | Batch dropout statistics | results only | Export only |
| `snr_analysis_sink` | `snr_analysis` | plugin | Batch SNR statistics | results only | Export only |
| `burst_level_analysis_sink` | `burst_level_analysis` | plugin | Batch burst level statistics | results only | Export only |
| `teletext_sink` | `teletext_analysis` | plugin | Page catalogue browser | — | No (sink writes files) |
| `nabts_sink` | `nabts_analysis` | plugin | Record catalogue browser | — | No (sink writes files) |

Three tools write `frame_map.ranges`, so "refresh this node" is ambiguous
unless the tool is named. This drives the per-node configuration in §6.4.

Two GUI-only **parameter helpers** have the same "derived from upstream"
character, although they are not tools (`orc/gui/stage_parameter_context.cpp`):

- `video_params`: "reset to source metadata" (`sourceParametersToVideoParamsStageValues`).
- `audio_channel_map`, `audio_align`, `tbc_sink`: channel-pair choices
  narrowed to the pairs the input carries.

## 4. Source formats: `.tbc` and `.cvbs`

The disc mapper and source alignment both work on the decoded signal
(VBI and biphase observers on frames), not on source-specific metadata. They
run unchanged on either source stage, and both are gated on
`AnalysisSourceType::LaserDisc`. The differences that matter to automation
are in how the source node is configured and what it tells the script.

### 4.1 `tbc_source` (`.tbc`)

- **Paths:** `input_path` (composite `.tbc`), or `y_path` + `c_path`
  (`.tbcy` / `.tbcc`) for Y/C. Sidecar paths: `pcm_path`, `efm_path`,
  `ac3rf_path`.
- **Metadata:** `.tbc.db`, falling back to `.tbc.json`. Required: without it
  the node is yellow and cannot run. A script should check for it before
  queuing a disc.
- **Video format:** read from metadata. A `tbc_source` never implies one to
  the project, so a template must carry `video_format` (or the CLI's
  `--video-format` must be passed when a template is built from a
  filtergraph).
- **Player skips:** ld-decode's `pad` fields become padding frames. The disc
  mapper reports them (`statistics.paddingFrames`); the stacker skips them.
- **EFM:** carried only when `.efm` exists **and** the metadata has per-field
  T-value counts. Relevant if `efm_sink` is part of the emulation export.

### 4.2 CVBS sources (`.cvbs`)

The CVBS source plugin (`orc/plugins/stages/cvbs_source`) registers one
stage per system: `PAL_CVBS_Source`, `NTSC_CVBS_Source` and
`PAL_M_CVBS_Source`. Unlike `tbc_source`, the stage itself fixes the video
format, so a CVBS template needs no `video_format` override.

- **Paths:** `input_path` (composite `.cvbs`), or `y_path` + `c_path`
  (`.cvbsy` / `.cvbsc`) for Y/C. The paths offered follow the project's
  `source_format`. Sidecars (`_audio_N.wav`, EFM, AC3) are found
  automatically; there are no sidecar path parameters.
- **Metadata:** `.meta` SQLite sidecar. Optional if `sample_encoding` is set
  manually, but then the signal state is assumed and audio pair names are
  derived. Batch jobs should leave `sample_encoding` at `From metadata`.
- **Signal state:** only `STANDARD_STABLE_LOCKED` / `STANDARD_STABLE_UNLOCKED`
  are accepted. Anything else is a configuration error before any samples
  are read, so a script should treat it as a hard per-disc failure.
- **Sequence continuity:** `sequence_continuous = FALSE` means the capture
  has at least one discontinuity (usually a player skip), and the Disc Mapper
  is required. `ProjectPresenter` already reads this column for the GUI
  advisory. The CLI should surface it in `nodes list --json` and in the tool
  run report (§6.6).

### 4.3 Stream sources are out of scope

`tbc_stream_source` and `cvbs_stream_source` read standard input or a pipe
once, in strict forward order. The disc mapper and source alignment both
execute the whole upstream and then re-read it (the observer workers run on
cloned DAGs), so they cannot work against a stream. `tools run` /
`tools refresh` must refuse with a clear error when a tool's upstream
contains a stream source. The automation flow is "decode to files, then
run", not "decode into orc-cli".

### 4.4 Templates per format

Because the path parameters differ by stage and by composite/Y/C, a script
needs one template per combination it uses:

| Template | Source stage | Path keys set per disc |
|---|---|---|
| TBC composite | `tbc_source` | `input_path` (+ optional `pcm_path`, `efm_path`, `ac3rf_path`) |
| TBC Y/C | `tbc_source` | `y_path`, `c_path` (+ sidecars) |
| CVBS composite | `<SYS>_CVBS_Source` | `input_path` |
| CVBS Y/C | `<SYS>_CVBS_Source` | `y_path`, `c_path` |

`node set` (§6.2) validates keys against the stage's parameter descriptors
**for the project's source format**. Setting `input_path` on a Y/C project
therefore fails loudly instead of producing a node that silently has no
input.

### 4.5 One source format per stack

A stack takes all its inputs from `.tbc` captures or all from `.cvbs`
captures, never a mix. `tools run source_alignment` and `tools refresh`
check the source stages upstream of each `source_align` node. If they are
not all `tbc_source` or all CVBS source stages (`*_CVBS_Source`), they stop with a project error
(exit 5) naming the offending nodes, before any analysis runs.

## 5. Goals and non-goals

**Goals**

- Run any stage tool that writes parameters against a node of an existing
  project, and optionally apply and save the result.
- Read and set node parameters on an existing project from the CLI.
- One command that refreshes every content-derived parameter of a project
  in dependency order, driven by the project, not the script.
- Machine-readable output and meaningful exit codes for scripting.
- GUI and CLI share one tool-dispatch path.

**Non-goals**

- CLI forms of the interactive tools (dropout editor, catalogue browsers,
  vectorscope).
- Building complete projects from scratch on the CLI beyond what the
  filtergraph front end already offers. Templates are made in the GUI or
  with `--export-project`.
- Running tools against stream sources (§4.3).

## 6. Design

### 6.1 Shared tool presenter factory

Move the tool-id → presenter selection out of
`orc/gui/generic_analysis_dialog.cpp` into `orc/presenters`:

```cpp
// orc/presenters/include/analysis_tool_presenter_factory.h
std::unique_ptr<AnalysisToolPresenter>
makeAnalysisToolPresenter(const std::string& tool_id, void* project_handle);
```

The GUI dialog and the CLI both call it, so a tool added for one front end is
available to the other. Tools without a dedicated presenter fall back to the
generic `AnalysisToolPresenter`.

### 6.2 Node commands

These follow the existing group-first shape (`plugins …`, `stages …`):

```
orc-cli nodes list <project> [--json]
orc-cli nodes get  <project> <node> [--json]
orc-cli nodes set  <project> <node> key=value [key=value …] [--output <file>]
```

- `<node>` is a node id (all digits) or a `user_label`. Labels are unique
  within a project and never all digits (§6.9), so a label always resolves
  to exactly one node. A label that matches no node is an error that lists
  the project's labels.
- `set` converts each value to the type its parameter descriptor declares,
  checks it against the descriptor's constraints and allowed strings, and
  rejects unknown keys. Without `--output` it saves in place.
- `list --json` includes, per node: id, stage, label, configuration status
  (the green/yellow/red the GUI shows), the applicable tools from §3, and the
  `tool_refresh` entry from §6.4. For CVBS sources it also includes
  `signal_state` and `sequence_continuous`.
- Strings are stored as given, so templates can keep `${PROJECT_ROOT}`-relative
  paths. A per-disc directory holding a copy of the template then needs no
  `nodes set` at all.

These map directly onto `ProjectPresenter::getNodes`, `getNodeParameters`,
`getStageParameters`, `setNodeParameters` and `saveProject`.

### 6.3 Tool commands

```
orc-cli tools list <project> [<node>] [--json]
orc-cli tools run  <project> <tool_id> --node <node>
                   [--param key=value …] [--apply] [--output <file>] [--json]
```

- `list` shows, per node (or for one node), every tool from §3 with its kind,
  whether it can apply to the graph, and its parameter descriptors.
- `run` without `--apply` is a dry run. It prints the summary, statistics
  and proposed `parameterChanges`, and leaves the project untouched.
- `run --apply` calls `applyResultToGraph` and then `saveProject`, in place
  unless `--output` is given.
- `--param` values are converted using the tool's `parametersForContext()`
  descriptors (e.g. `include_lead_in_out=true`, `alignmentMode=first_common_frame`,
  `startAddress=…`).
- `run` on a tool whose kind is NonModalEditor or CatalogueBrowser (or on
  `vectorscope`) is rejected with a message naming the GUI tool to use.
- `run` on a BatchAnalysis tool (dropout / SNR / burst level) triggers the
  analysis sink and reports its results. `--apply` does not apply; `--json`
  writes the result data (§6.7).

### 6.4 Per-node tool refresh configuration

The project, not the script, records which tool computes which node's
parameters. This lives in an optional block on the node:

```yaml
- id: 4
  stage: frame_map
  user_label: disc1_map
  tool_refresh:
    tool: field_mapping
    parameters:
      include_lead_in_out: { type: bool, value: false }
  parameters:
    ranges: { type: string, value: "…" }
```

- `tool` must be applicable to the node's stage and must `canApplyToGraph()`.
  The tool parameters use the existing typed-parameter YAML form.
- Projects without the block are unaffected. No format version change is
  needed, because the reader ignores unknown node keys. An older build will
  drop the block when it saves the project, which is acceptable for an
  opt-in field. Readers must still be checked to confirm unknown node keys
  are ignored.
- GUI: the stage tool dialog gets an "Use this tool when refreshing from the
  CLI" checkbox. It stores the tool id and the parameters the user ran it
  with. `nodes set` / a `tools refresh-config` subcommand set the same block
  from the CLI.

### 6.5 Refresh

```
orc-cli tools refresh <project> [--only <tool_id>[,…]] [--dry-run]
                      [--output <file>] [--json] [--process]
```

1. Load the project and collect the nodes that have `tool_refresh`.
2. Order them topologically. Every node's `frame_map` is applied before the
   `source_align` that consumes them. This is required: source alignment
   executes its inputs and reads VBI through the frame maps.
3. For each node: run the tool, apply the result, and invalidate the DAG so
   downstream tools see the new parameters. Stop at the first failure and
   leave the project file unchanged (all-or-nothing save).
4. Save once at the end, or write to `--output`.
5. `--process` then triggers all sinks, the same as `--process` today, so a
   whole batch is one command.

`--only` limits the run to the named tool ids. `--dry-run` runs everything
in order but applies changes only to the in-memory project and never saves.

### 6.6 Output and exit codes

Human-readable progress goes to stderr. With `--json`, stdout gets one JSON
document:

```json
{
  "project": "stack.orcprj",
  "nodes": [
    {
      "node": "disc1_map", "stage": "frame_map", "tool": "field_mapping",
      "status": "success",
      "summary": "…",
      "statistics": { "paddingFrames": 12, "…": 0 },
      "parameter_changes": { "ranges": "…" },
      "source": { "stage": "NTSC_CVBS_Source", "sequence_continuous": false }
    }
  ],
  "saved": true
}
```

The codes extend the ones `orc-cli` already documents (`0`, `1`, and `2`–`4`
for `plugins` / `stages`) and keep the existing meanings where they overlap:

| Exit | Meaning |
|---|---|
| 0 | Success (all tools ran; changes applied if requested) |
| 1 | Error: invalid arguments, project load/save failed, or `--process` failed (as today) |
| 2 | Not found: no node with that id or label, no such tool, or the tool does not apply to that node's stage; unknown parameter key |
| 5 | Project rejected: source not configured, stream source upstream of a tool, mixed `.tbc` / `.cvbs` stack |
| 6 | Analysis failed, or returned no parameter changes for a tool that should produce them |
| 130 | Cancelled (SIGINT) |

`3` and `4` are not used by these commands, so a script never has to
interpret "unavailable" or "trust declined" here.

A disc whose mapping fails (no VBI, wrong source type) must not be stacked
silently, so "no parameter changes" counts as a failure.

### 6.7 Progress, cancellation and batch results

- The tool presenters' `progress_cb` drives a stderr progress line. It is
  throttled, and suppressed with `--quiet`.
- SIGINT sets the flag behind `cancel_cb`, the same cancellation chain the
  GUI analysis dialogs use. Upstream DAG execution before the analysis is
  still not cancellable, which matches the GUI.
- BatchAnalysis results (dropout / SNR / burst level) are written as JSON,
  or as CSV with `--csv <file>`. A script can use them to drop a bad capture
  before stacking (e.g. mean SNR below a threshold).

### 6.8 `video_params` reset

Move `sourceParametersToVideoParamsStageValues` and its caller logic from
`orc/gui/stage_parameter_context.cpp` into presenters. Then add:

```
orc-cli nodes reset <project> <node>
```

For `video_params` this sets the values from the source metadata, exactly
as the GUI's reset does. This is optional, and only needed if per-disc
active-area or level values must follow each capture.

### 6.9 Unique node labels

Node labels become unique within a project. This is enforced in core
(`orc/core/project.cpp`), so the GUI and the CLI share one rule.

Today every new node takes its stage's display name as its label
(`add_node`), and a node loaded without one falls back to the display name.
A project with three Frame Map nodes therefore already has three nodes
labelled "Frame Map". The rule has to cover new nodes, edits and existing
projects:

- **Uniqueness** is an exact string comparison across all nodes in the
  project. Labels may not be empty or all digits, so a label can never be
  mistaken for a node id.
- **New nodes** (`add_node`) take the display name if it is free, otherwise
  the first free of "<display name> 2", "<display name> 3", …
- **Renaming** (`set_node_label`) throws when the label is empty, all digits
  or already used by another node. The GUI's caption edit
  (`OrcGraphModel::setNodeData`) already returns `false` when the presenter
  throws, so the edit is refused. The model keeps the presenter's message
  (`OrcGraphModel::lastCaptionError`), and the Rename Stage prompt shows it
  and reopens with the refused name.
- **Loading** a project with duplicate labels renames them
  deterministically. Every node in a group of duplicates gets the label
  followed by a space and a number. The numbers ascend from 1 in node-id
  order, so three nodes labelled "Frame Map" become "Frame Map 1",
  "Frame Map 2" and "Frame Map 3". If one of those names is already taken by
  another node, that number is skipped and the next free one is used. Labels
  that were already unique are left alone. Each rename is logged and the
  project is marked modified, so the change is saved the next time the user
  saves. Loading never fails because of labels. A stored label that is
  empty or all digits is first replaced with the display name, then numbered
  with the rest if that name is taken.
- **Saving** asserts the invariant. With the rules above it cannot be
  broken, so a violation is an internal error, not a user-facing one.
- **Filtergraph import** (`import_filtergraph_into_project`) goes through
  `add_node`, so it gets unique labels for free.

## 7. Example workflows

### 7.1 Three `.tbc` decodes, composite PAL

```bash
#!/usr/bin/env bash
set -euo pipefail
tmpl=templates/pal-tbc-3disc.orcprj   # tool_refresh on disc{1,2,3}_map + align
out=work/title.orcprj

cp "$tmpl" "$out"
for n in 1 2 3; do
  tbc="caps/disc$n.tbc"
  [[ -f "$tbc.db" || -f "${tbc%.tbc}.tbc.json" ]] || { echo "no metadata: $tbc"; exit 1; }
  orc-cli nodes set "$out" "disc${n}_src" input_path="$tbc" efm_path="caps/disc$n.efm"
done
orc-cli nodes set "$out" video_out output_path="out/title.mkv"
orc-cli tools refresh "$out" --json --process > work/title.report.json
```

### 7.2 Three `.cvbs` captures, Y/C NTSC

```bash
cp templates/ntsc-cvbs-yc-3disc.orcprj work/title.orcprj
for n in 1 2 3; do
  orc-cli nodes set work/title.orcprj "disc${n}_src" \
      y_path="caps/disc$n.cvbsy" c_path="caps/disc$n.cvbsc"
done
# Inspect continuity before deciding anything else
orc-cli nodes list work/title.orcprj --json \
  | jq '.nodes[] | select(.stage=="NTSC_CVBS_Source") | {label, signal_state, sequence_continuous}'
orc-cli tools refresh work/title.orcprj --process
```

### 7.3 Single tool, dry run first

```bash
orc-cli tools run disc.orcprj field_mapping --node disc1_map --json | jq .parameter_changes
orc-cli tools run disc.orcprj field_mapping --node disc1_map --apply
```

## 8. GUI/CLI parity manifest

`orc/plugin_ux_capabilities.yaml` must record every new CLI form, and the
`PluginUxCapabilityParity` gate checks it against `--help`. The gate
currently knows the `plugins` and `stages` help groups. It needs extending
to `nodes` and `tools`. New entries:

| id | cli | gui |
|---|---|---|
| `nodes.list` | `orc-cli nodes list <project>` | DAG editor (`none` with reason if no single control) |
| `nodes.parameters.get` / `.set` | `orc-cli nodes get/set …` | Edit Parameters... |
| `nodes.parameters.reset` | `orc-cli nodes reset …` | video_params reset button |
| `tools.list` | `orc-cli tools list …` | node context menu Tools |
| `tools.run` | `orc-cli tools run …` | stage tool dialog Run / Apply |
| `tools.refresh` | `orc-cli tools refresh …` | `none`: batch refresh is a scripting feature |
| `tools.refresh.config` | `orc-cli tools refresh-config …` | "Use this tool when refreshing" checkbox |
| `tools.dropout_editor` | `none`: interactive editing | Dropout Editor |
| `tools.catalogue` | `none`: browsing; the sinks write files | Teletext Pages / NABTS Records |
| `tools.vectorscope` | `none`: preview view | Vectorscope |

## 9. Testing

**Unit** (`unit`, plus `cli` / `gui-logic` as appropriate):

- The factory returns the right presenter per tool id, and the generic one
  for others.
- `nodes set` type conversion and constraint checks, unknown-key and
  wrong-source-format rejection (`input_path` on a Y/C project), and
  resolution by id and by label.
- Label uniqueness (core, `orc-tests/core/unit`):
  - `add_node` suffixes a taken display name ("Frame Map 2").
  - `set_node_label` rejects a duplicate, an empty label and an all-digit
    label.
  - Loading YAML with three "Frame Map" nodes yields "Frame Map 1", "2" and
    "3" in node-id order and marks the project modified.
  - A number already taken (an existing "Frame Map 2") is skipped.
  - Unique labels are untouched.
  - These are YAML-string loads with no file access.
- `--param` conversion against tool descriptors.
- Refresh ordering through a mocked tool/presenter seam: a `frame_map` before
  the `source_align` it feeds, all-or-nothing save on failure, and `--only`
  filtering.
- `tool_refresh` YAML round trip, and that projects without it are unchanged.
- The stream-source refusal, and the mixed-format refusal (a `tbc_source`
  and a `*_CVBS_Source` feeding one `source_align`).
- Exit-code mapping.

**Functional** (`functional`):

- `tools run field_mapping --apply` on a short `.tbc` fixture writes `ranges`
  to the saved YAML.
- The same on a short `.cvbs` fixture with `sequence_continuous = FALSE`.
- `tools refresh` on a two-source project produces `alignmentMap` and
  processes to a sink, run once with two `.tbc` fixtures and once with two
  `.cvbs` fixtures.

**GUI** (`gui-widget`):

- The analysis dialog still selects the same presenter after the factory
  move.
- The refresh-config checkbox round-trips.
- A duplicate caption edit is refused and the node keeps its old label.

## 10. Implementation phases

The work lands in four phases, plus one optional phase. Each phase is
complete in itself: it carries its own unit and functional tests, its
parity manifest entries and the `docs/` / `instructions.md` changes for what
it adds (§11). The documentation therefore never describes a command that
does not exist yet.

### 10.1 Unique node labels

**Status:** done.

**Scope:**
- The core rule (§6.9): `add_node` suffixing, `set_node_label` rejection,
  and the ascending-number migration on load.
- The GUI caption edit is refused with the reason shown.

**Docs:**
- `docs/gui-user-guide/dialogues/main.md` (Renaming a Stage).
- `docs/misc/migration.md`.
- The Project Files note in the CLI guide.

**Why first:** CLI label addressing depends on it, and it is the only phase
that changes users' existing projects. Landing it alone keeps any fallout
from the renaming separate from the new CLI surface.

### 10.2 Tool presenter factory and node commands

**Status:** done.

**As built:**
- `runAnalysis` is a public pure virtual on `AnalysisToolPresenter`, with
  no default arguments (clang-tidy `google-default-arguments`). The GUI
  dialog now holds one factory-made presenter and applies every tool through
  `applyResultToGraph`, which refuses tools that cannot apply to the graph.
- Text-to-value conversion moved from the filtergraph import into
  `orc/presenters/include/node_editing.h` (`parseParameterText`), shared by
  both. `nodes set` adds the range and allowed-string checks
  (`checkParameterConstraints`), comparing only the stored value of a
  labelled allowed-string entry.
- Source-format-aware keys: `ProjectPresenter::getStageParameters` now passes
  the project's declared `source_format`, falling back to the source type its
  configured sources imply. `tbc_source` narrows its path descriptors by
  source type, as the CVBS source already did. The GUI parameter dialog shows
  the same narrowing.
- `nodes set` uses exit 5 when core rejects the change (for example a source
  that conflicts with the project's other sources). Exits 6 and 130 are
  defined in `cli_exit_codes.h` for phase 3.
- The parity gate checks `plugins`, `stages` and `nodes` as required groups.
  It checks `tools` once `orc-cli tools --help` offers it, so phase 3 only
  adds manifest entries.
- `nodes get` shows stored (0-based) values, not the GUI's 1-based frame and
  line numbers.

**Scope:**
- The shared tool presenter factory (§6.1), with
  `generic_analysis_dialog.cpp` switched over to it.
- `nodes list` / `get` / `set` (§6.2), with typed conversion and
  source-format-aware key validation.
- The exit-code scheme (§6.6).
- The parity gate extended to the `nodes` and `tools` help groups (§8), so
  later phases only add manifest entries.

**Docs:**
- CLI guide: Command Syntax, "Editing project nodes" and Exit Codes.
- The TBC Source / CVBS Source parameter-key notes in
  `docs/gui-user-guide/stages/source-stages.md`.

**Result:** a script can point a template at new `.tbc` or `.cvbs` captures.

### 10.3 Tool commands

**Scope:**
- `tools list` / `tools run` (§6.3): dry run, `--apply`, `--param`, the
  `--json` report, progress and cancellation (§6.7).
- The refusals: interactive tools, stream sources upstream (§4.3), and
  mixed `.tbc` / `.cvbs` stacks (§4.5).
- Result export from the batch analysis sinks.

**Docs:**
- CLI guide: "Running stage tools", Machine-readable output, and the stream
  source note in Piping between processes.
- GUI stage pages: the CLI equivalents for Frame Map, Source Align and the
  analysis sinks.
- The `instructions.md` Tools updates (§11.3).

**Result:** a script can map, align and stack by calling each tool in turn.

### 10.4 Refresh

**Scope:**
- The `tool_refresh` node block (§6.4).
- The GUI "Use this tool when refreshing from the CLI" option, and
  `tools refresh-config`.
- `tools refresh` (§6.5), with pipeline-order execution, an all-or-nothing
  save, `--only`, `--dry-run` and `--process`.

**Docs:**
- CLI guide: "Refreshing a project" and the Project Files `tool_refresh`
  note.
- The refresh option in the Frame Map / Source Align GUI pages.
- The new `docs/cli-user-guide/multi-disc-stacking.md` page. It comes last
  because it describes the finished end-to-end flow for both capture
  formats.

**Result:** one template plus one command per batch.

### 10.5 `video_params` reset (optional)

**Scope:**
- Move the reset logic into presenters, and switch the GUI over to it.
- Add `nodes reset` (§6.8).

**Docs:**
- The CLI guide's "Editing project nodes".
- The Video Parameters GUI page.

Nothing else depends on this phase. Do it only when per-disc video
parameters are needed.

## 11. Documentation

User-facing documentation in `docs/` is updated in the same change as the
code it describes. Stage help (`instructions.md`) follows AGENTS.md §9.1.

### 11.1 CLI user guide

`docs/cli-user-guide/overview.md`:

- **Command Syntax:** add `orc-cli nodes <subcommand>` and
  `orc-cli tools <subcommand>`.
- **New section "Editing project nodes":**
  - `nodes list` / `get` / `set` / `reset`;
  - addressing by id or label;
  - type conversion and rejection of unknown keys;
  - `--output` versus saving in place.
- **New section "Running stage tools":**
  - `tools list` / `run`;
  - dry run versus `--apply`;
  - `--param`;
  - which tools can run from the CLI, with the §3 table trimmed to
    user-relevant columns;
  - why the interactive tools stay GUI-only.
- **New section "Refreshing a project":**
  - the `tool_refresh` block and how to set it from the GUI or with
    `tools refresh-config`;
  - pipeline order;
  - all-or-nothing save;
  - `--only`, `--dry-run`, `--process`.
- **Piping between processes:** state that stage tools cannot run on
  `tbc_stream_source` / `cvbs_stream_source` projects. Decode to files first.
- **Machine-readable output:** the `--json` report shape from §6.6.
- **Exit Codes:** add `5`, `6` and `130`, and the extended meaning of `2`
  for these commands.
- **Project Files:**
  - the optional `tool_refresh` node block;
  - unique labels, including the automatic renaming of duplicates when an
    older project is opened.
- **Common Use Cases:** replace the generic batch loop with a pointer to the
  workflow page below.

New page `docs/cli-user-guide/multi-disc-stacking.md`, added to
`docs/cli-user-guide/.nav.yml`, covering the end-to-end workflow for both
capture formats:

- Building a template in the GUI: label the nodes, enable refresh on each
  Frame Map and on Source Align, and save one template per format and
  composite/Y/C combination (§4.4).
- **`.tbc` captures:**
  - the paths to set (`input_path` or `y_path` / `c_path`, plus `pcm_path`,
    `efm_path`, `ac3rf_path`);
  - that the `.tbc.db` / `.tbc.json` metadata must exist;
  - padding frames from player skips.
- **`.cvbs` captures:**
  - the paths to set (`input_path` or `y_path` / `c_path`);
  - the `.meta` sidecar;
  - accepted signal states;
  - checking `sequence_continuous` in `nodes list --json`.
- The one-format-per-stack rule.
- The complete scripts from §7, with exit-code handling.

### 11.2 GUI user guide

- `docs/gui-user-guide/dialogues/main.md`, **Renaming a Stage:**
  - labels are unique;
  - a duplicate, empty or all-digit name is refused, with the reason shown;
  - new stages get a numeric suffix when the name is taken;
  - duplicates in an older project are renamed when it is opened.
- `docs/gui-user-guide/stages/transform-stages.md`:
  - **Frame Map:**
    - the Disc Mapper and Frame Map Range Finder entries gain the "Use this
      tool when refreshing from the CLI" option;
    - each gets a one-line CLI equivalent (`orc-cli tools run field_mapping …`).
  - **Source Align:**
    - the same additions for Source Alignment Analysis;
    - the one-format-per-stack rule.
  - **Stacker:** link to the CLI workflow page.
- `docs/gui-user-guide/stages/source-stages.md`:
  - **TBC Source** and **CVBS Source:** the parameter keys a script sets
    with `nodes set`;
  - **CVBS Source:** the `sequence_continuous` paragraph also points at the
    CLI report.
- `docs/gui-user-guide/stages/sink-analysis-stages.md`: the Burst Level,
  Dropout and SNR analysis sinks gain their CLI result export
  (`tools run … --json` / `--csv`).
- `docs/misc/migration.md`: a short entry for the label change, saying that
  duplicate labels in existing projects get an ascending number appended
  when the project is opened ("Frame Map" ×3 → "Frame Map 1", "2" and "3",
  in node-id order), and the new labels are saved on the next save.

### 11.3 Stage help (`instructions.md`)

Update the Tools section of each of these files with the CLI form and the
refresh option:

- `frame_map`
- `source_align`
- `dropout_analysis_sink`
- `snr_analysis_sink`
- `burst_level_analysis_sink`

The Frame Map entry covers both the Disc Mapper and the Frame Map Range
Finder.

`tbc_source` and `cvbs_source`: no behaviour changes, so no update is
required. The existing `sequence_continuous` advice in `cvbs_source`
("run the Disc Mapper") gains "or `orc-cli tools run field_mapping`".

### 11.4 Internal documentation

- `orc/plugin_ux_capabilities.yaml` entries (§8).
- `orc-cli --help` and the `nodes --help` / `tools --help` texts, which the
  parity gate checks.

## 12. Decisions

1. **Label uniqueness:** enforced project-wide in core (§6.9), not only when
   the CLI resolves an address.
2. **Refresh when nothing changed:** not in the initial implementation.
   `tools refresh` always runs every configured tool. Skipping unchanged
   inputs (keyed on source path, size and mtime) can be added later without
   changing the command line.
3. **Disc-mapper advisory for CVBS:** refresh does not skip the disc mapper
   for CVBS sources declaring `sequence_continuous = TRUE`. The template
   decides which tools run. The report carries the flag for scripts that
   want to act on it.
