# Daphne VBI Sink

Writes per-field VBI data to a binary `.vbi` file in the format required by the Daphne arcade LaserDisc emulator. Use this sink when preserving a LaserDisc for Daphne emulation.

## When to use

Add this sink to your pipeline when you are archiving a LaserDisc title for use with the Daphne arcade LaserDisc emulator. The `.vbi` file produced by this stage carries the per-field VBI metadata that Daphne requires to emulate the disc's interactivity correctly.

## What it does

Reads VBI data from each frame in the incoming stream and writes binary VBI records field by field to a `.vbi` file according to the Daphne VBIInfo specification (documented at the Daphne VBIInfo wiki page). This is a pure sink — no output is passed downstream.

## Parameters

### output_path (string)
Path to the output `.vbi` file. Required.

`-` writes the stream to the CLI process's standard output instead of a file — CLI only; the GUI rejects this value.

## Notes

- This sink produces a file specific to the Daphne emulation project and is not a general-purpose VBI archive format.
- The `.vbi` format is documented at the Daphne VBIInfo wiki page.
- No output is forwarded downstream; connect other sinks in parallel if you also need video output.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
