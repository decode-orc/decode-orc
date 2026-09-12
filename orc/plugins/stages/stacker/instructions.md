# Stacker

Combines between 1 and 16 frame-aligned input sources into a single superior output by combining the pixel data available across all sources. When only one input is connected the stage acts as a passthrough. With multiple inputs, corresponding pixels from each source are combined per pixel using the selected stacking mode (mean, median, or outlier-rejecting smart mean), recovering pixels that are in dropout in one source but valid in another.

## When to use

Use Stacker after Source Align when you have multiple captures of the same LaserDisc that contain dropouts. Example: three captures with heavy surface damage — Stacker overlaps each capture's pixel data and, using differential dropout detection, can recover a pixel that is corrupted in one or two captures as long as at least one source has a clean reading. Three or more sources are required for differential dropout detection.

## What it does

For each output frame, Stacker fetches the frame with the *same frame id* from every input source and applies the selected stacking mode pixel by pixel. It relies on Frame Map and Source Align having put the sources on a common frame numbering; it does not search for a matching frame itself. Earlier versions re-aligned each source by looking for a frame whose measured colour frame index matched the reference's, searching up to four frames either way. That measurement is taken against each source's own sample grid, so a source whose line starts sit one sample from the reference's reports a mismatch on the same picture and gets pulled to a neighbouring frame — displacing its video and its audio together. Sample-grid differences are corrected by `sample_align` instead, which shifts samples rather than changing frame. Every mode is an averaging or median policy over the usable source values for that pixel — no mode selects a whole "best" source or frame. Smart modes use a configurable threshold to exclude outlier values before averaging. Differential dropout detection compares dropout flags across sources; if a pixel is flagged as a dropout in fewer than all sources, valid data from the clean sources is used instead. Audio and EFM data can be combined independently of video using their own stacking settings. The stage caches stacked frames in an LRU cache to avoid redundant recomputation during preview navigation.

Note that stacking reduces noise only when the sources contain independent noise (separate captures). Sources that are identical apart from dropouts will stack to the same underlying signal, so signal-to-noise measurements will not improve on dropout-free picture areas.

## Parameters

### mode (string)
Stacking algorithm, applied per pixel to the usable (non-dropout) values across all sources. Values: `Auto`, `Mean`, `Median`, `Smart Mean`, `Smart Neighbor`, `Neighbor`. Default: `Auto`.

| Mode | Behaviour per pixel |
|------|--------------------|
| `Auto` | `Smart Mean` when 3 or more usable source values are available; plain `Mean` otherwise. |
| `Mean` | Arithmetic mean of all usable source values. |
| `Median` | Median of the usable source values (mean of the middle two for an even count). |
| `Smart Mean` | Mean of the values within `smart_threshold` of the median, rejecting outliers; falls back to the median when no value qualifies. |
| `Smart Neighbor` | Not yet implemented — currently behaves as `Median`. |
| `Neighbor` | Not yet implemented — currently behaves as `Median`. |

### smart_threshold (int32)
Pixel-value threshold used by `Smart Mean` (and by `Auto` when it selects `Smart Mean`) to exclude outliers before averaging: only source values within this distance of the per-pixel median are included in the mean. Range: 0–128. Default: `15`. Has no effect for other modes.

### no_diff_dod (bool)
Disable differential dropout detection. When `true`, dropout status from individual sources is not used to guide pixel selection. Default: `false`.

### passthrough (bool)
When `true`, pixels that are in dropout across all sources are passed through unchanged rather than being replaced. Default: `false`.

### sample_align (bool)
Measure each source's 4FSC sample grid against the first input and shift it into line before stacking. Default: `true`.

Independent decodes of the same disc do not necessarily start a line on the same sample. ld-decode locates lines from sync, and PAL's TBC layout — 1135.0064 samples per line, closed with 4 extra samples per frame — does not pin subcarrier phase to sample index the way NTSC's 910 samples per line does. Two captures can therefore sit a sample apart. One 4FSC sample is 90° of subcarrier, so combining a source that is a sample out erodes chroma instead of reinforcing it: the stack comes out desaturated, and the more sources that disagree, the worse it gets.

The offset is a property of the decode and constant for a whole source, so it is measured once from a spread of frames by correlating active picture against the first input, then applied to every frame of that source. Offsets beyond ±4 samples are not corrected — a difference that large is a frame alignment problem, and belongs to Frame Map or Source Align. When a source cannot be measured (no frames in common with the reference, or too little picture detail to correlate) it is stacked unshifted and the reason is logged.

Set to `false` to combine sources on their own grids, as earlier versions did.

### audio_stacking (string)
Method used to combine audio samples from multiple sources. Values: `Disabled`, `Mean`, `Median`. Default: `Mean`. When `Disabled`, audio from the source with the fewest dropouts is used. The method applies per channel pair, to every channel pair present in all inputs; channel pairs not common to all inputs pass through from the source with the fewest dropouts. All pipeline audio is 48 kHz frame-locked 24-bit stereo, so sources are combined sample by sample at the same frame position; combined values saturate at the 24-bit range.

### efm_stacking (string)
Method used to combine EFM t-values from multiple sources. Values: `Disabled`, `Confidence`, `Mean`, `Median`. Default: `Disabled`, which passes the EFM of the source with the fewest dropouts through untouched. That is the default because no combining mode has yet been shown to beat it — see *Choosing a mode* below.

Each EFM byte on the pipeline packs the t-value into its low nibble and the producing source's doubt about that t-value into the high nibble.

| Mode | Behaviour |
|------|-----------|
| `Disabled` | The chosen source's bytes pass through untouched, doubt included. |
| `Confidence` | Sources are aligned on the EFM frame-sync grid and vote on where the transitions are, weighted by their doubt. Emits a doubt of its own. |
| `Mean` | Arithmetic mean of the t-values at each sample index, emitted with zero doubt. |
| `Median` | Median of the t-values at each sample index, emitted with zero doubt. |

### Choosing a mode

`Disabled` is the default and, on the evidence so far, the mode to use. Measured against six independent captures of the same BBC Domesday side (National A, aligned on the EFM stream, 8,000 frames each), passing the best single source through recovered more sectors than any combining mode:

| mode | good sectors | uncorrectable | C1 uncorrectable | C2 uncorrectable |
|------|--------------|---------------|------------------|------------------|
| `Disabled` (best source alone) | 21,695 | 8 | 4,209 | 553 |
| `Confidence`, three captures | 21,577 | 140 | 929 | 10,800 |
| `Mean`, six captures | 5 | 10 | 6,826 | 29,932 |

`Mean` is not merely worse, it is destructive: five good sectors out of some 21,700. On a synthetic three-capture ensemble it made the decode fail outright.

`Confidence` behaves far better than that and improves C1 - it more than halved C1 uncorrectable in the run above - but C2 uncorrectable rises sharply at the same time, and the recovered-sector count falls below a single capture. Combining three copies of one capture reproduces that capture exactly, so the fault is in the voting rather than in the surrounding machinery; the working theory is that the whole-slot alignment is occasionally off by one channel frame, which would yield exactly this signature, a clean copy of the wrong frame. Until that is settled, treat `Confidence` as experimental.

`Mean` and `Median` are kept only for reproducing earlier results, and have two problems `Confidence` does not.

The first is that they average. A t-value is a quantised symbol, not a measurement: where one source reads T3 and another T11, their mean of T7 is a reading neither source made and one that is wrong for both, and the demodulator then decodes it with full confidence. `Confidence` only ever emits a t-value some source actually reported.

The second, and the more serious, is that they combine by sample index. EFM t-values are run lengths, not samples on a shared time axis, so one spurious or missed transition in a capture shifts every following index in it and the two streams are averaged out of step from there on. A video frame carries around 36,600 t-values, so a single early insertion spoils the rest of the frame. `Confidence` combines on the channel bit axis instead, cut into channel frames on each source's own T11+T11 frame sync (IEC 60908 §20.2, 588 bits). An inserted transition splits a run into two that sum to the same length and a missed one merges two runs into one of the same length, so on that axis neither moves anything after it, and the sync grid re-anchors the alignment every 588 bits regardless.

Within a channel frame each source votes on the bit offsets its transitions sit at, with a weight taken from its doubt, and against offsets falling inside its runs. Offsets that win are kept, the result is repaired to legal T3–T11 run lengths, and each output run carries a doubt derived from how strongly its bounding transitions won and from how much the sources that reported it doubted it. Where only one source covers a channel frame, or the vote cannot be repaired into a legal frame, the frame from the source with the fewest dropouts is used unchanged; so are the partial channel frames at each end of a video frame's t-values.

The doubt this mode emits is worth more than the doubt on its inputs, because it is evidence of a different kind: a producer's doubt is one capture's opinion of its own reading, while a disagreement between independent captures is an observation. Both `efm_sink` and `efm_audio_decode` can act on it through their `doubt_erasure_threshold` parameter, which turns doubted t-values into C1/C2 erasure hints.

## Tools

This stage has no interactive tools. A Stacker Configuration stage report is generated automatically after execution.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
