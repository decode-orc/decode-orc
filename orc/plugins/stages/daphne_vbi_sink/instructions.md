# Daphne VBI Sink

Writes per-field VBI data to a binary `.vbi` file in the format required by the Daphne arcade LaserDisc emulator. Use this sink when preserving a LaserDisc for Daphne emulation.

## When to use

Add this sink to your pipeline when you are archiving a LaserDisc title for use with the Daphne arcade LaserDisc emulator. The `.vbi` format carries the per-field VBI metadata that Daphne requires to emulate the disc's interactivity correctly.

## What it does

Writes a `.vbi` file in the layout of the Daphne VBIInfo specification (documented at the Daphne VBIInfo wiki page): a four-byte `1VBI` header followed by one 10-byte record per field — the white flag and the three biphase words of lines 16, 17 and 18, each stored big-endian with `0x7FFFFF` for a line that could not be read. This is a pure sink — no output is passed downstream.

The stage decodes the biphase code and white flag itself from the frames on its input, so it needs no other stage upstream. Records are written in field order, two per frame. A field with no readable biphase code gets all three lines as zero, and a frame the input cannot supply still gets its two records so that later fields stay at the right offset. The white flag is an NTSC feature and is always `0` on a PAL disc.

## Parameters

### output_path (string)
Path to the output `.vbi` file. Required.

`-` writes the stream to the CLI process's standard output instead of a file — runs only via the CLI; settable here for a project you'll execute there.

## Notes

- This sink produces a file specific to the Daphne emulation project and is not a general-purpose VBI archive format.
- The `.vbi` format is documented at the Daphne VBIInfo wiki page.
- No output is forwarded downstream; connect other sinks in parallel if you also need video output.
- This sink does not support an unbounded (piped/live) upstream source: the VBI decode does not report when a frame is missing, so the stage has no signal to detect the input's real end from. Triggering against an unbounded source (e.g. a stream source left at `input_path=-` with no frame count) is refused with a diagnostic rather than run indefinitely.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
