# Dropout Analysis Sink

Reads dropout hints from the incoming video stream and generates statistical summaries of dropout frequency, size, and distribution. Use this sink to quantify dropout presence before and after stacking, or to compare the dropout profile between multiple captures.

## When to use

Connect this stage when you want to compare the dropout profile between multiple captures to decide which to use as primary, or to verify that stacking has reduced dropout count and size. Connect one instance before and one after the Stacker stage and compare the resulting charts to see the improvement.

## What it does

Reads dropout hints present in the incoming stream and, for every frame, counts the dropout runs and totals their length in samples. This stage does not perform dropout detection or correction — it only analyses hints that were already placed in the stream by earlier stages. After triggering, the Dropout Analysis tool is automatically invoked to display the results. The dataset is cached and can be retrieved from the Stage Tools menu after the trigger completes. Frames are read in order, and the frames just ahead are produced on the other threads, so a stacked input is stacked on every core.

## Parameters

### output_path (file path)
Destination CSV file for the dropout metrics. Leave empty to skip file output.

### write_csv (bool)
Enable writing the results to CSV at trigger time. The CSV is written only when this is on and `output_path` is set. Default: `false`.

### mode (choice: full, visible)
Selects full-field or visible-area dropout analysis. In `visible` mode, dropout
runs outside the active picture area are excluded and partially-visible runs are
clamped to the active sample range. Default: `full`.

### write_report (bool)
Enable writing a per-dropout detail report at trigger time — one entry per
dropout run showing its location within the frame. The report is written only
when this is on and `report_path` is set. Default: `false`.

### report_path (file path)
Destination file for the per-dropout detail report. Leave empty to skip report
output.

### report_format (choice: csv, text)
Format of the detail report: machine-readable `csv` (one row per run) or
human-readable `text` (grouped by frame). Default: `csv`. See the *Dropout
detail report* section below.

## CSV output

The CSV is written from the canonical per-frame dataset — **one row per analysed
frame**. The dropout sink analyses every frame, so the row count equals the
recording's frame count. A zero row is genuine data (the frame was analysed and
had no dropouts); a *missing* frame number means that frame was not analysed.
The CSV always contains full-resolution per-frame data and is never affected by
the display decimation used to draw the graph.

Columns (units are carried in the header names; values are plain numbers):

| Column | Unit | Meaning |
|--------|------|---------|
| `frame_number` | — | Analysed frame number (1-based) |
| `dropout_count` | count | Number of dropout runs in the frame |
| `dropout_length_samples` | samples | Total dropout length in the frame |

Example (frame 2 was analysed and had no dropouts):

```csv
frame_number,dropout_count,dropout_length_samples
1,3,128
2,0,0
3,1,20
```

## Dropout detail report

Where the per-frame CSV summarises *how much* dropout each frame has, the detail
report records *where* each individual dropout is within the frame (issue #214).
It is written only when `write_report` is enabled and `report_path` is set, and
is always full-resolution — it is never affected by the display decimation used
to draw the graph. It honours the same `mode` (full / visible) parameter as the
rest of the analysis, so in `visible` mode runs outside the active area are
omitted and partially-visible runs are clamped.

Coordinates are frame-flat: `line_number` is a 0-based line index within the
frame and `sample_start` / `sample_end` are 0-based sample indices within that
line (both inclusive), derived from the recording's nominal samples-per-line
(PAL 1135, NTSC 910). A run that crosses a line boundary is reported against its
start line; its `sample_end` may therefore exceed the nominal line width.

Unlike the per-frame CSV, **frames with no dropouts do not appear** in the
report.

### csv format

One row per dropout run:

```csv
frame_number,line_number,sample_start,sample_end,length_samples
1,10,100,139,40
1,20,5,14,10
3,0,50,69,20
```

| Column | Unit | Meaning |
|--------|------|---------|
| `frame_number` | — | Analysed frame number (1-based) |
| `line_number` | — | Frame-flat 0-based line of the run's start |
| `sample_start` | — | Sample-within-line of the run's start (inclusive) |
| `sample_end` | — | Sample-within-line of the run's end (inclusive) |
| `length_samples` | samples | Run length (after visible-area clamping) |

### text format

Human-readable, grouped by frame — a heading with the frame's dropout count and
total length, then one line per run:

```text
Frame 1: 2 dropouts, 50 samples total
  line 10, samples 100-139 (40 samples)
  line 20, samples 5-14 (10 samples)
Frame 3: 1 dropout, 20 samples total
  line 0, samples 50-69 (20 samples)
```

## Where the analysis reads its data

The analysis reports the dropout state **at this sink's input** — the dropout
hints visible on the representation connected to it. To include edits made in
the **Dropout Map** stage (added or removed dropout regions), connect this sink
**downstream of the `dropout_map` stage**. A sink wired upstream of, or on a
branch that bypasses, `dropout_map` sees only the original sidecar hints.

Editing the dropout map and re-triggering the sink re-analyses from scratch, so
the graph and CSV always reflect the latest map edits — they never show
pre-edit data after a re-trigger.

## Tools

### Dropout Analysis
Charts the total dropout length (in samples) per frame. Long recordings are decimated into bars; clicking a bar shows its frame range, dropout length and dropout count. This tool is automatically invoked after the stage is triggered.

The dataset stays with the stage afterwards, so closing the window and picking **Dropout Analysis** from the **Stage Tools** menu re-opens it immediately, reading what the last trigger produced without analysing again — which is why re-analysing after a dropout map edit means triggering the stage, as described above. That menu entry only ever reads: on a stage that has not been triggered it says there is nothing to show rather than starting the analysis itself, because deciding when to spend that time is what **Trigger Stage** is for. Editing any stage's parameters rebuilds the graph and discards every stage's results, closing the open windows with them — trigger again for a dataset that matches the new settings.

### From the command line
`orc-cli tools run <project> dropout_analysis --node <this stage>` triggers the stage and prints a summary of its results. `--json` adds every frame's figures, and `--csv <file>` writes the stage's CSV for that run without changing the saved `output_path` / `write_csv` settings.

## Notes

- This stage reads existing dropout hints; it does not detect or correct dropouts itself.
- Results are meaningful only if dropout information is present in the upstream pipeline (e.g. from a source stage that provides dropout hints).
- Connect one instance before the Stacker and one after to see the dropout reduction achieved by stacking.
- This stage does not modify the video stream; it is a pure sink.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
