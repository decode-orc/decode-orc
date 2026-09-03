# Stacker

Combines between 1 and 16 frame-aligned input sources into a single superior output by combining the pixel data available across all sources. When only one input is connected the stage acts as a passthrough. With multiple inputs, corresponding pixels from each source are combined per pixel using the selected stacking mode (mean, median, or outlier-rejecting smart mean), recovering pixels that are in dropout in one source but valid in another.

## When to use

Use Stacker after Source Align when you have multiple captures of the same LaserDisc that contain dropouts. Example: three captures with heavy surface damage — Stacker overlaps each capture's pixel data and, using differential dropout detection, can recover a pixel that is corrupted in one or two captures as long as at least one source has a clean reading. Three or more sources are required for differential dropout detection.

## What it does

For each output frame, Stacker fetches the corresponding frame from every input source and applies the selected stacking mode pixel by pixel. Every mode is an averaging or median policy over the usable source values for that pixel — no mode selects a whole "best" source or frame. Smart modes use a configurable threshold to exclude outlier values before averaging. Differential dropout detection compares dropout flags across sources; if a pixel is flagged as a dropout in fewer than all sources, valid data from the clean sources is used instead. Audio and EFM data can be combined independently of video using their own stacking settings. The stage caches stacked frames in an LRU cache to avoid redundant recomputation during preview navigation.

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

### audio_stacking (string)
Method used to combine audio samples from multiple sources. Values: `Disabled`, `Mean`, `Median`. Default: `Mean`. When `Disabled`, audio from the source with the fewest dropouts is used. The method applies per channel pair, to every channel pair present in all inputs; channel pairs not common to all inputs pass through from the source with the fewest dropouts. All pipeline audio is 48 kHz frame-locked 24-bit stereo, so sources are combined sample by sample at the same frame position; combined values saturate at the 24-bit range.

### efm_stacking (string)
Method used to combine EFM t-values from multiple sources. Values: `Disabled`, `Mean`, `Median`. Default: `Mean`. When `Disabled`, EFM from the source with the fewest dropouts is used.

Each EFM byte on the pipeline packs the t-value into its low nibble and the producing source's doubt about that t-value into the high nibble. `Disabled` passes the chosen source's bytes through untouched, doubt included. `Mean` and `Median` combine the t-values alone and emit the combined value with zero doubt: the stacked t-value is a new value that no source vouched for, so the sources' doubt says nothing about it. Combining the whole bytes instead would fold the doubt nibble into the t-value and corrupt it.

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
