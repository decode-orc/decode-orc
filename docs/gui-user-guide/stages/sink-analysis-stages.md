# Analysis Sink Stages

Analysis sink stages are **terminal stages** that generate diagnostics, metrics, and reports rather than producing media or hardware output. They consume processed data from upstream stages and emit **analysis results** intended for comparison, validation, or debugging.

Analysis sinks:

* Do not modify video, audio, or metadata
* Do not produce outputs that can be connected further downstream
* Display their results in a dialog of their own and write them to a file

They are typically used to:

* Compare capture quality across multiple sources
* Validate signal stability and decode quality
* Quantify the effects of transform stages such as stacking or dropout correction

Every analysis sink works the same way: trigger the stage to compute the dataset, after which its dialog opens automatically. The dataset stays with the stage, so closing the dialog and picking the tool again from the **Stage Tools** menu re-opens it immediately, reading what the last trigger produced without recomputing. That menu entry only ever reads: on a node that has not been triggered it says so rather than starting a decode, because deciding when to spend that time is what **Trigger Stage** is for. Editing any stage's parameters rebuilds the graph and discards every stage's results, closing the open viewers with them — trigger again to get a dataset that matches the new settings.

The burst level, dropout and SNR sinks measure the signal frame by frame and present a chart with an optional CSV export. Each chart plots one point per frame on short recordings; on longer ones it is decimated into bars, and a line under the chart says which view is showing. Clicking a bar prints its exact frame range and figures under the chart. The chart can be zoomed and panned. Triggering the stage shows the usual progress dialog and can be cancelled; a cancelled run leaves no dataset.

The burst level and SNR sinks need a known recording length: a piped or live source that leaves its frame count open makes the trigger fail with a message asking for an explicit `frame_count` on the source.

**Status indicator.** Each analysis sink shows green once its CSV `output_path` is set and yellow while it is empty. Yellow is not an error — the stage still analyses and shows its chart; it simply has nowhere to write the CSV.

A batch-analysis dialog is not the same thing as an analysis sink, and two stages that offer one live elsewhere: the **Teletext Sink** and the **NABTS Sink** are documented under [Sink Stages](sink-core-stages.md) because writing the packet stream is their product, and their page and record viewers are by-products of the same pass rather than the reason for it.

**CSV output format.** Each CSV is written from the full-resolution, canonical per-frame dataset — **one row per frame** (every analysis sink analyses every frame), with the frame's true (1-based) frame number in the first column. Units are carried in the header names (`_samples`, `_db`, `_10bit`) and values are plain numbers. A metric that was not measured for a frame is written as an **empty field** (never the string `nan`). The CSV is independent of the display decimation used to draw the chart, so it always contains every frame regardless of the on-screen point count.

---

## Burst Level Analysis Sink

| | |
|-|-|
| **Stage id** | `burst_level_analysis_sink` |
| **Stage name** | Burst Level Analysis Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Measure colour burst level stability across frames |

**Use this stage when:**

* Evaluating chroma signal stability
* Comparing multiple captures of the same source
* Diagnosing colour amplitude fluctuations or capture issues

**What it does**

This stage measures the colour burst amplitude of every frame and plots it over time. For each frame it samples the burst window on three lines of each field (near the top, middle and bottom of the field), takes each line's peak amplitude from its RMS, discards implausible readings above 30 IRE, and records the median of what remains as the frame's burst level. A frame with no usable burst line has no value. After triggering, the Burst Level Analysis chart is opened automatically.

**Parameters**

* `output_path` (file path, default empty)
    - Destination CSV file for burst metrics. Leave empty to skip file output.
* `write_csv` (bool, default `false`)
    - Enable writing results to CSV at trigger time. The CSV is written only when this is on **and** `output_path` is set.

**CSV columns**

`frame_number, median_burst_10bit` — median colour-burst amplitude (10-bit sample units) per frame; the value column is empty when not measured.

**Stage tools**

* **Burst Level Analysis** — displays per-frame colour-burst amplitude measurements in a chart window, in the project's amplitude display unit (10-bit samples, IRE or mV, set in Project Properties); the CSV is always in 10-bit sample units. Opens automatically after triggering, and re-opens from the Stage Tools menu without recomputing.

**From a script**

`orc-cli tools run <project> burst_level_analysis --node "Burst Level Analysis Sink"` runs the sink and reports its summary; add `--json` for every frame's figures and `--csv <file>` to write the CSV for that run only. See [Analysis sinks](../../cli-user-guide/overview.md#analysis-sinks) in the CLI guide.

**Notes**

* Results are meaningful only if colour burst timing is correct upstream.
* A standard-amplitude burst has a peak excursion of roughly 20 IRE around blanking.
* Masking or altering the burst region before this stage will invalidate results.
* Connect one instance before and one after the Stacker stage to compare burst stability across captures.

---

## Dropout Analysis Sink

| | |
|-|-|
| **Stage id** | `dropout_analysis_sink` |
| **Stage name** | Dropout Analysis Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Produce statistics describing dropout frequency, size, and distribution |

**Use this stage when:**

* Comparing dropout levels between captures
* Evaluating the effectiveness of stacking or dropout correction
* Identifying problematic regions of a capture

**What it does**

This stage reads the dropout hints present in the stream (originating from the source or modified by transform stages such as `dropout_map`) and, for every frame, counts the dropout runs and totals their length in samples. After triggering, the Dropout Analysis chart is opened automatically.


**Parameters**

* `output_path` (file path, default empty)
    - Destination CSV file for dropout metrics. Leave empty to skip file output.
* `write_csv` (bool, default `false`)
    - Enable writing results to CSV at trigger time. The CSV is written only when this is on **and** `output_path` is set.
* `mode` (choice: `full`, `visible`, default `full`)
    - `full` counts every dropout run in the frame; `visible` excludes runs outside the active picture area and clamps partially-visible runs to the active sample range, so only their visible length is counted.
* `write_report` (bool, default `false`)
    - Enable writing a per-dropout detail report at trigger time (one entry per dropout run). The report is written only when this is on **and** `report_path` is set.
* `report_path` (file path, default empty)
    - Destination file for the detail report. Leave empty to skip report output.
* `report_format` (choice: `csv`, `text`, default `csv`)
    - `csv` writes one row per dropout run; `text` writes a human-readable report grouped by frame.

**CSV columns**

`frame_number, dropout_count, dropout_length_samples` — one row for **every** frame (the sink analyses every frame). A zero row means the frame was analysed and had no dropouts; an absent frame number means the frame was not analysed.

**Per-dropout detail report**

Separate from the per-frame CSV, the detail report records *where* each individual dropout sits within its frame. It is written only when `write_report` is enabled with a `report_path`, is always full-resolution (never decimated), and honours the same `mode`. Coordinates are frame-flat: `line_number` is a 0-based line within the frame and `sample_start` / `sample_end` are 0-based, inclusive sample indices within that line, derived from the nominal samples-per-line (PAL 1135, NTSC 910). A run that crosses a line boundary is reported against its start line, so its `sample_end` may exceed the nominal line width. Unlike the per-frame CSV, **frames with no dropouts do not appear**.

* `csv` — one row per run: `frame_number, line_number, sample_start, sample_end, length_samples`.
* `text` — grouped by frame, e.g. `Frame 1: 2 dropouts, 50 samples total` followed by one indented `line N, samples A-B (L samples)` line per run.

**Stage tools**

* **Dropout Analysis** — charts the total dropout length (in samples) per frame; clicking a bar shows its frame range, dropout length and dropout count. Opens automatically after triggering, and re-opens from the Stage Tools menu without recomputing.

**Where the analysis reads its data**

The analysis reports the dropout state **at the sink's input** — the dropout hints visible on the representation connected to it. To include edits made in the **Dropout Map** stage (added or removed dropout regions), the sink must be connected **downstream of the `dropout_map` stage**. A sink placed upstream of, or on a branch that bypasses, `dropout_map` sees only the original sidecar hints. Editing the map and re-triggering re-analyses from scratch, so the chart and CSV never show pre-edit data after a re-trigger.

**From a script**

`orc-cli tools run <project> dropout_analysis --node "Dropout Analysis Sink"` runs the sink and reports its summary; add `--json` for every frame's figures and `--csv <file>` to write the CSV for that run only. See [Analysis sinks](../../cli-user-guide/overview.md#analysis-sinks) in the CLI guide.

**Notes**

* This stage reads existing dropout hints; it does not detect or correct dropouts itself.
* Results depend on the quality of upstream dropout detection, and are meaningful only if the upstream pipeline carries dropout hints (for example from a source stage that provides them).
* Removing or adding dropouts upstream will directly affect analysis output.
* Connect one instance before the Stacker and one after to see the dropout reduction achieved by stacking.

---

## SNR Analysis Sink

| | |
|-|-|
| **Stage id** | `snr_analysis_sink` |
| **Stage name** | SNR Analysis Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Produce signal-to-noise metrics for capture quality comparison |

**Use this stage when:**

* Comparing multiple captures of the same material
* Quantifying improvements from stacking or filtering
* Evaluating capture hardware or settings

**What it does**

This stage measures noise on the vertical interval test signals (VITS) and reports two figures per frame:

* **White SNR** — measured on the VITS white flag (PAL line 19; NTSC line 20 at two positions, falling back to line 13). The slice starts 2 µs after the flag's nominal start (PAL 14–20 µs from the sync edge; NTSC 14–26 µs and 52–60 µs on line 20, 14–28 µs on line 13), so the flag's rising edge never reaches it: a slice on the edge measures where a capture's samples fall rather than its noise, and a stack inherits its first input's. A slice is used only if its mean sits between 90 and 110 IRE, so a frame without a white flag has no white value. Figures from versions before this change (12–20 µs on PAL) read several dB lower and depend on the input order of a stack; do not compare across the change.
* **Black PSNR** — the noise on the VITS black-level slice (PAL line 22; NTSC line 1), expressed against a 100 IRE reference. A noiseless slice is capped at 80 dB.

Each figure is measured on both fields and averaged into one value for the frame. Because the measurement method is fixed, results are comparable across pipelines, allowing meaningful cross-capture comparison. Higher is better. After triggering, the SNR Analysis chart is opened automatically.

**Parameters**

* `output_path` (file path, default empty)
    - Destination CSV file for SNR metrics. Leave empty to skip file output.
* `write_csv` (bool, default `false`)
    - Enable writing results to CSV at trigger time. The CSV is written only when this is on **and** `output_path` is set.
* `mode` (choice: `white`, `black`, `both`, default `both`)
    - Selects which metrics to measure: white SNR only, black PSNR only, or both. A metric that is not measured is left empty in the CSV.

**CSV columns**

`frame_number, white_snr_db, black_psnr_db` — white SNR and black PSNR (dB) per frame. A column is empty when that metric was not measured (for example, `mode = white` leaves `black_psnr_db` empty).

**Stage tools**

* **SNR Analysis** — displays white SNR and black PSNR (dB) over time in a chart window. A **White / Black / Both** selector above the chart chooses which series to show; it changes only the display, not what was measured. Opens automatically after triggering, and re-opens from the Stage Tools menu without recomputing.

**From a script**

`orc-cli tools run <project> snr_analysis --node "SNR Analysis Sink"` runs the sink and reports its summary; add `--json` for every frame's figures and `--csv <file>` to write the CSV for that run only. See [Analysis sinks](../../cli-user-guide/overview.md#analysis-sinks) in the CLI guide.

**Notes**

* The measurement needs VITS on the recording (as on most LaserDiscs). A capture without test signals in the vertical interval gives empty or meaningless values.
* Meaningful SNR comparison requires aligned sources.
* Use `source_align` and `stacker` appropriately upstream when comparing captures.
* Stacking improves SNR only where sources contain independent noise; identical sources will not show an SNR improvement on dropout-free areas.

---

## Notes on Analysis Sink Stages

* Analysis sink stages terminate pipeline branches.
* Multiple analysis sinks may consume the same upstream output.
* Analysis sinks are side-effect-free with respect to media data.
* Results are intended for diagnostics, comparison, and validation—not for further pipeline processing.
* From a script, `orc-cli tools run` runs any of these sinks and reports its results as text or JSON, so a batch job can check each capture's figures before stacking it.
* Each sink reads frames in order, with the frames just ahead produced on the other threads, so analysing a stacked input stacks on every core. On 140 frames of eleven captures stacked with still-frame repeats, an analysis took about 13 s on 16 threads, of which about 4 s was loading the project.
