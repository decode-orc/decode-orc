# Burst Level Analysis Sink

Measures the colour burst amplitude of each frame in the incoming video stream and displays the results in a chart. Use this sink to assess colour burst stability across a capture or to compare burst levels between multiple sources.

## When to use

Connect this stage when you want to compare colour burst stability between two captures of the same disc, or diagnose why colours look unstable in a particular region. Connect one instance per source in parallel and trigger each. The chart shows burst amplitude over time, making it easy to spot regions of instability or compare sources side by side.

## What it does

Reads each frame from the incoming stream and samples the burst window on three lines of each field (near the top, middle and bottom of the field). Each line's peak amplitude is taken from its RMS, readings above 30 IRE are discarded as implausible, and the median of the rest is recorded as the frame's burst level. A frame with no usable burst line has no value. The stage needs a known recording length: a piped or live source that leaves its frame count open makes the trigger fail with a message asking for an explicit `frame_count` on the source. After triggering, the Burst Level Analysis tool is automatically invoked to display the results in a chart window. The dataset is cached in the stage instance and can be retrieved from the Stage Tools menu at any time after the trigger completes.

## Parameters

### output_path (file path)
Destination CSV file for the burst metrics. Leave empty to skip file output.

### write_csv (bool)
Enable writing the results to CSV at trigger time. The CSV is written only when this is on and `output_path` is set. Default: `false`.

## CSV output

The CSV is written from the canonical per-frame dataset — **one row per frame**;
the stage analyses every frame. Each row carries the frame's *true* frame
number. An absent value is written as an **empty field** — never the string
`nan`. The CSV always contains full-resolution per-frame data and is never
affected by the display decimation used to draw the graph.

Columns (units are carried in the header name; values are plain numbers):

| Column | Unit | Meaning |
|--------|------|---------|
| `frame_number` | — | Frame number (1-based) |
| `median_burst_10bit` | 10-bit sample units | Median colour-burst amplitude for the frame (empty if not measured) |

Example:

```csv
frame_number,median_burst_10bit
1,200
2,205.5
3,201
```

## Tools

### Burst Level Analysis
Displays per-frame colour-burst amplitude measurements in a chart window, in the project's amplitude display unit (10-bit samples, IRE or mV, set in Project Properties); the CSV is always in 10-bit sample units. Long recordings are decimated into bars; clicking a bar shows its exact frame range and burst level. This tool is automatically invoked after the stage is triggered.

The dataset stays with the stage afterwards, so closing the window and picking **Burst Level Analysis** from the **Stage Tools** menu re-opens it immediately, reading what the last trigger produced without measuring again. That menu entry only ever reads: on a stage that has not been triggered it says there is nothing to show rather than starting the analysis itself, because deciding when to spend that time is what **Trigger Stage** is for. Editing any stage's parameters rebuilds the graph and discards every stage's results, closing the open windows with them — trigger again for a dataset that matches the new settings.

### From the command line
`orc-cli tools run <project> burst_level_analysis --node <this stage>` triggers the stage and prints a summary of its results. `--json` adds every frame's figures, and `--csv <file>` writes the stage's CSV for that run without changing the saved `output_path` / `write_csv` settings.

## Notes

- The chart updates only after the trigger completes; it does not display live results during processing.
- Connect one instance before and one after the Stacker stage to compare burst stability across captures.
- This stage does not modify the video stream; it is a pure sink.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
