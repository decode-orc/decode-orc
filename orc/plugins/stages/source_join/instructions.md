# Source Join

Joins between 1 and 16 connected sources end to end into a single continuous output sequence, in an order you specify. Output frames are renumbered from the start of the join, so the result behaves like one long capture. With a single input the stage is a passthrough.

## When to use

Use Source Join when the material you want as one output is spread across several decodes of the same physical medium. The common case is a tape recorded at more than one speed, or on more than one machine: each region has to be decoded separately so the correct filter profile is applied, which leaves you with several sources that between them cover one recording.

Feed each decode through a **Frame Map** stage to trim it to the frames that region actually contributes, then connect the trimmed outputs to Source Join and set the order. One export then produces the whole tape.

Example — a tape whose first half is SP and second half is LP:

* Node 12: SP decode → Frame Map (`1-4500`)
* Node 14: LP decode → Frame Map (`4501-9000`)
* Source Join with `input_order` = `12,14`

## What it does

Source Join builds a lookup table from output frame ID to (input, input frame ID) at execution time and hands out frames through it. No sample data is copied, so joining is cheap regardless of how long the inputs are.

Everything a frame carries travels with it: dropout hints (renumbered to the output frame), audio channel pairs, EFM and AC3 RF data all come from the input that supplied the frame. The joined output takes its channel-pair layout from the first joined input; an input that carries fewer channel pairs contributes silence for the ones it lacks, so a pair never goes short mid-sequence. For NTSC and PAL-M, the audio window of each output frame is trimmed or silence-padded by at most one stereo pair where the join lands a frame at a different position in the five-frame audio sequence (SMPTE 272M-1994 §14.3); PAL is constant-cadence and is always sample-exact.

All joined inputs must describe the same signal geometry — the same video system, nominal frame width and frame height — and must all be composite or all be Y/C. The stage fails with an explanatory error rather than producing an output whose geometry changes part way through.

## Parameters

### input_order (string)
Comma-separated list of **source node IDs**, giving the order the connected sources are joined in — for example `16,2,4`. A node's ID is the number drawn in the corner of the node in the graph editor.

The parameter dialog lists the nodes currently connected to this stage, with their IDs and names, so the numbers to enter are in front of you as you type them.

Default: `""` (empty), meaning the sources are joined in the order their connections were made.

An ID may appear only once, and every ID must be a whole number. The list is what the output is made of:

* An ID that is not connected to this stage is ignored, with a warning in the log.
* A source that is connected but is not named in the list contributes no frames, with a warning in the log and a `source_join.inputs_skipped` observation.
* If the list names no connected node at all, the stage fails rather than guessing.

### input_node_ids (string)
Host-supplied and not user-editable; it does not appear in the parameter dialog or in the project file. Decode-Orc fills it in with the node IDs of the sources connected to this stage so the stage can match `input_order` against the graph it is actually in.

## Tools

This stage has no interactive tools. Standard GUI previews of the joined output are supported.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

For Source Join specifically, Yellow means either that `input_order` is empty (the sources will be joined in connection order) or that the order no longer names exactly the sources connected to the stage — which is what you will see after adding, removing or rewiring an input. Re-open the parameters and update the order to clear it.

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
