# Daphne VBI Sink

Writes per-field VBI data to a binary `.vbi` file in the format required by the Daphne arcade LaserDisc emulator. Use this sink when preserving a LaserDisc for Daphne emulation.

## When to use

Add this sink to your pipeline when you are archiving a LaserDisc title for use with the Daphne arcade LaserDisc emulator (see the current limitation below). The `.vbi` format carries the per-field VBI metadata that Daphne requires to emulate the disc's interactivity correctly.

## What it does

Writes a `.vbi` file in the layout of the Daphne VBIInfo specification (documented at the Daphne VBIInfo wiki page): a four-byte `1VBI` header followed by one 10-byte record per field — the white flag and the three biphase words of lines 16, 17 and 18, each stored big-endian with `0x7FFFFF` for a line that could not be read. This is a pure sink — no output is passed downstream.

**Current limitation:** the per-field records are not yet written. The stage has no access to the decoded biphase observations at export time, so the file it produces holds only the `1VBI` header. The log says so at debug level ("Wrote VBI header for N frames"). A file from this stage is therefore not yet usable by Daphne on its own.

## Parameters

### output_path (string)
Path to the output `.vbi` file. Required.

`-` writes the stream to the CLI process's standard output instead of a file — runs only via the CLI; settable here for a project you'll execute there.

## Notes

- This sink produces a file specific to the Daphne emulation project and is not a general-purpose VBI archive format.
- The `.vbi` format is documented at the Daphne VBIInfo wiki page.
- No output is forwarded downstream; connect other sinks in parallel if you also need video output.
- This sink does not support an unbounded (piped/live) upstream source: because of the limitation above it never reads per-frame data from the input at all, only counting frames, so it has no signal to detect the input's real end from. Triggering against an unbounded source (e.g. a stream source left at `input_path=-` with no frame count) is refused with a diagnostic rather than run indefinitely.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
