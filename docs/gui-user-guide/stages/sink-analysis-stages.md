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

The burst level, dropout and SNR sinks measure the signal and present a chart with an optional CSV export.

A batch-analysis dialog is not the same thing as an analysis sink, and two stages that offer one live elsewhere: the **Teletext Sink** and the **NABTS Sink** are documented under [Sink Stages](sink-core-stages.md) because writing the packet stream is their product, and their page and record viewers are by-products of the same pass rather than the reason for it.

**CSV output format.** Each CSV is written from the full-resolution, canonical per-frame dataset — **one row per frame** (every analysis sink analyses every frame), with the frame's true (1-based) frame number in the first column. Units are carried in the header names (`_samples`, `_db`, `_10bit`) and values are plain numbers. A metric that was not measured for a frame is written as an **empty field** (never the string `nan`). The CSV is independent of the display decimation used to draw the chart, so it always contains every frame regardless of the on-screen point count.

---

## Burst Level Analysis Sink

| | |
|-|-|
| **Stage id** | `burst_level_analysis_sink` |
| **Stage name** | Burst Level Analysis Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Measure colour burst level stability across fields |

**Use this stage when:**

* Evaluating chroma signal stability
* Comparing multiple captures of the same source
* Diagnosing colour amplitude fluctuations or capture issues

**What it does**

This stage measures the amplitude of the colour burst for each field and generates statistics describing burst level variation over time (per-field measurements plus aggregate mean, variance, min/max). After triggering, the Burst Level Analysis chart is opened automatically.

**Parameters**

* `output_path` (file path)
    - Destination CSV file for burst metrics. Leave empty to skip file output.
* `write_csv` (bool)
    - Enable writing results to CSV at trigger time.

**CSV columns**

`frame_number, median_burst_10bit` — median colour-burst amplitude (10-bit sample units) per frame; the value column is empty when not measured.

**Stage tools**

* **Burst Level Analysis** — displays per-frame colour-burst amplitude measurements in a chart window. Opens automatically after triggering, and re-opens from the Stage Tools menu without recomputing.

**From a script**

`orc-cli tools run <project> burst_level_analysis --node "Burst Level Analysis Sink"` runs the sink and reports its summary; add `--json` for every frame's figures and `--csv <file>` to write the CSV for that run only. See [Analysis sinks](../../cli-user-guide/overview.md#analysis-sinks) in the CLI guide.

**Notes**

* Results are meaningful only if colour burst timing is correct upstream.
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

This stage reads dropout hints present in the stream (originating from the source or modified by transform stages such as `dropout_map`) and generates statistical summaries: total dropout count, per-field counts, size distributions, and line/field density metrics. After triggering, the Dropout Analysis chart is opened automatically.

It does **not** perform dropout detection or correction itself.

**Parameters**

* `output_path` (file path)
    - Destination CSV file for dropout metrics. Leave empty to skip file output.
* `write_csv` (bool)
    - Enable writing results to CSV at trigger time.
* `mode` (choice: `full`, `visible`, default `full`)
    - `full` counts dropouts across the whole field; `visible` restricts the count to the active picture area.
* `write_report` (bool)
    - Enable writing a per-dropout detail report at trigger time (one entry per dropout run).
* `report_path` (file path)
    - Destination file for the detail report. Leave empty to skip report output.
* `report_format` (choice: `csv`, `text`, default `csv`)
    - `csv` writes one row per dropout run; `text` writes a human-readable report grouped by frame.

**CSV columns**

`frame_number, dropout_count, dropout_length_samples` — one row for **every** frame (the sink analyses every frame). A zero row means the frame was analysed and had no dropouts; an absent frame number means the frame was not analysed.

**Per-dropout detail report**

Separate from the per-frame CSV, the detail report records *where* each individual dropout sits within its frame. It is written only when `write_report` is enabled with a `report_path`, is always full-resolution (never decimated), and honours the same `mode`. Coordinates are frame-flat: `line_number` is a 0-based line within the frame and `sample_start` / `sample_end` are 0-based, inclusive sample indices within that line, derived from the nominal samples-per-line (PAL 1135, NTSC 910). Unlike the per-frame CSV, **frames with no dropouts do not appear**.

* `csv` — one row per run: `frame_number, line_number, sample_start, sample_end, length_samples`.
* `text` — grouped by frame, e.g. `Frame 1: 2 dropouts, 50 samples total` followed by one indented `line N, samples A-B (L samples)` line per run.

**Stage tools**

* **Dropout Analysis** — displays dropout frequency, size, and distribution charts. Opens automatically after triggering, and re-opens from the Stage Tools menu without recomputing.

**Where the analysis reads its data**

The analysis reports the dropout state **at the sink's input** — the dropout hints visible on the representation connected to it. To include edits made in the **Dropout Map** stage (added or removed dropout regions), the sink must be connected **downstream of the `dropout_map` stage**. A sink placed upstream of, or on a branch that bypasses, `dropout_map` sees only the original sidecar hints. Editing the map and re-triggering re-analyses from scratch, so the chart and CSV never show pre-edit data after a re-trigger.

**From a script**

`orc-cli tools run <project> dropout_analysis --node "Dropout Analysis Sink"` runs the sink and reports its summary; add `--json` for every frame's figures and `--csv <file>` to write the CSV for that run only. See [Analysis sinks](../../cli-user-guide/overview.md#analysis-sinks) in the CLI guide.

**Notes**

* Results depend on the quality of upstream dropout detection.
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

This stage estimates signal-to-noise ratio using spatial and temporal analysis of the incoming video stream, reporting **white SNR** and **black SNR** per field along with aggregate statistics. Results are consistent across comparable pipelines, allowing meaningful cross-capture comparison. After triggering, the SNR Analysis chart is opened automatically.

**Parameters**

* `output_path` (file path)
    - Destination CSV file for SNR metrics. Leave empty to skip file output.
* `write_csv` (bool)
    - Enable writing results to CSV at trigger time.
* `mode` (choice: `white`, `black`, `both`, default `both`)
    - Selects which SNR metrics to measure.

**CSV columns**

`frame_number, white_snr_db, black_psnr_db` — white SNR and black PSNR (dB) per frame. A column is empty when that metric was not measured (for example, `mode = white` leaves `black_psnr_db` empty).

**Stage tools**

* **SNR Analysis** — displays white SNR and black SNR metrics over time in a chart window. Opens automatically after triggering, and re-opens from the Stage Tools menu without recomputing.

**From a script**

`orc-cli tools run <project> snr_analysis --node "SNR Analysis Sink"` runs the sink and reports its summary; add `--json` for every frame's figures and `--csv <file>` to write the CSV for that run only. See [Analysis sinks](../../cli-user-guide/overview.md#analysis-sinks) in the CLI guide.

**Notes**

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
