# SNR Analysis Sink

Measures the signal-to-noise ratio of the incoming video stream on its vertical interval test signals (VITS) and displays white SNR and black PSNR per frame. Use this sink to quantify noise improvement from stacking or to compare the SNR of different capture setups.

## When to use

Connect this stage after each source or after the Stacker stage when you want to measure the noise level in the signal. Comparing SNR before and after stacking shows the noise reduction achieved. Comparing SNR between two capture setups identifies which has better signal quality. Higher SNR means less noise and a better decode quality.

## What it does

Measures noise on the VITS lines of each frame:

- **White SNR** — on the VITS white flag (PAL line 19; NTSC line 20 at two positions, falling back to line 13). The slice starts 2 µs after the flag's nominal start (PAL 14–20 µs from the sync edge; NTSC 14–26 µs and 52–60 µs on line 20, 14–28 µs on line 13), so the flag's rising edge never reaches it: a slice on the edge measures where a capture's samples fall rather than its noise, and a stack inherits its first input's. A slice is used only if its mean is between 90 and 110 IRE, so a frame without a white flag has no white value. Figures from versions before this change (12–20 µs on PAL) read several dB lower and depend on the input order of a stack; do not compare across the change.
- **Black PSNR** — on the VITS black-level slice (PAL line 22; NTSC line 1), against a 100 IRE reference. A noiseless slice is capped at 80 dB.

Frames are read in order, and the frames just ahead are produced on the other threads, so a stacked input is stacked on every core.

Each figure is measured on both fields and averaged into one value for the frame. The stage needs a known recording length: a piped or live source that leaves its frame count open makes the trigger fail with a message asking for an explicit `frame_count` on the source. After triggering, the SNR Analysis tool is automatically invoked to display the results in a chart window. Results are consistent across comparable pipelines, allowing meaningful cross-capture comparison. The dataset is cached and can be retrieved from the Stage Tools menu after the trigger completes.

## Parameters

### output_path (file path)
Destination CSV file for the SNR metrics. Leave empty to skip file output.

### write_csv (bool)
Enable writing the results to CSV at trigger time. The CSV is written only when this is on and `output_path` is set. Default: `false`.

### mode (choice: white, black, both)
Selects which metrics to measure: white SNR only, black PSNR only, or both. A metric that is not measured is left empty in the CSV. Default: `both`.

## CSV output

The CSV is written from the canonical per-frame dataset — **one row per frame**;
the stage analyses every frame. Each row carries the frame's *true* frame
number. An absent metric is written as an **empty field** — never the string
`nan`. The CSV always contains full-resolution per-frame data and is never
affected by the display decimation used to draw the graph.

Columns (units are carried in the header names; values are plain numbers):

| Column | Unit | Meaning |
|--------|------|---------|
| `frame_number` | — | Frame number (1-based) |
| `white_snr_db` | dB | White SNR for the frame (empty if not measured) |
| `black_psnr_db` | dB | Black PSNR for the frame (empty if not measured) |

Example (`mode = white` measures white SNR only, so the black column is empty):

```csv
frame_number,white_snr_db,black_psnr_db
1,42.5,
2,42.1,
3,41.9,
```

## Tools

### SNR Analysis
Displays white SNR and black PSNR (dB) over time in a chart window. A **White / Black / Both** selector above the chart chooses which series to show; it changes only the display, not what was measured. Long recordings are decimated into bars; clicking a bar shows its frame range and figures. This tool is automatically invoked after the stage is triggered.

The dataset stays with the stage afterwards, so closing the window and picking **SNR Analysis** from the **Stage Tools** menu re-opens it immediately, reading what the last trigger produced without measuring again. That menu entry only ever reads: on a stage that has not been triggered it says there is nothing to show rather than starting the analysis itself, because deciding when to spend that time is what **Trigger Stage** is for. Editing any stage's parameters rebuilds the graph and discards every stage's results, closing the open windows with them — trigger again for a dataset that matches the new settings.

### From the command line
`orc-cli tools run <project> snr_analysis --node <this stage>` triggers the stage and prints a summary of its results. `--json` adds every frame's figures, and `--csv <file>` writes the stage's CSV for that run without changing the saved `output_path` / `write_csv` settings.

## Notes

- The measurement needs VITS on the recording (as on most LaserDiscs); a capture without vertical-interval test signals gives empty or meaningless values.
- SNR results are comparable across pipelines using the same measurement methodology, making cross-capture comparison meaningful.
- Connect one instance after each source and one after the Stacker to see the SNR improvement from stacking.
- This stage does not modify the video stream; it is a pure sink.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
