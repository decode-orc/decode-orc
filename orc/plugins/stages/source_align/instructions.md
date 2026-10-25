# Source Align

Synchronises between 1 and 16 input sources so that output frame 0 corresponds to the same physical disc position across all inputs. Without alignment, independent captures may start at different tracks, making pixel-level stacking impossible. Source Align applies a per-input frame offset — padding the start of the later-starting sources, or trimming the start of the earlier ones — from an alignment map that its Source Alignment Analysis tool works out from the discs' VBI picture numbers or CLV timecodes.

## When to use

Use Source Align whenever you connect more than one source to a Stacker stage and the captures did not start at exactly the same disc position. Example: three captures of the same LaserDisc that each began playing slightly earlier or later — map each with a Frame Map, connect all three to Source Align, run the Source Alignment Analysis tool, then connect the aligned outputs to Stacker.

## What it does

The stage applies the per-input frame offsets given in `alignmentMap`; it does not read the VBI itself. The offsets are worked out by the Source Alignment Analysis tool (below), which reads the VBI picture numbers of CAV discs or the timecodes of CLV discs, or can be entered by hand. Each input named in the map produces one output, and the outputs are frame-aligned with one another. An input the map does not name is left out of the outputs.

With `alignmentMap` empty the stage is not configured (red status): a single input passes through unchanged, but with two or more inputs the stage fails and asks for the map to be generated.

In `pad_for_alignment` mode the leading padding is followed by tail padding: every output is padded at the end to the length of the longest one, so all outputs have the same frame count and a stacker downstream keeps the pictures that only the longer captures carry. Padding frames have the source's full frame geometry, including the two extra samples PAL carries on lines 312 and 624, and are marked as padding so the stacker ignores them. In `first_common_frame` mode nothing is padded.

Audio follows the alignment. Every audio channel pair remaps per frame: trimmed sources serve their audio from the shifted frame IDs, and padded sources carry silence on the leading and trailing padding frames. Each output frame always carries the exact per-frame sample count required by its position in the output timeline (constant on PAL; alternating per the SMPTE 272M five-frame sequence on NTSC/PAL-M), so an offset that breaks the NTSC/PAL-M sequence phase trims or silence-pads each shifted frame's audio by a single trailing stereo pair. Offsets that preserve the phase (multiples of 5) — and all PAL offsets — are sample-exact. Sample values and rates are never changed.

## Parameters

### alignmentMode (string)
How the offsets in `alignmentMap` are applied. `pad_for_alignment` prepends that many padding frames to each input, then pads every output at the tail to the longest one. `first_common_frame` drops that many leading frames from each input and leaves the tails as they are. Default: `pad_for_alignment`.

### alignmentMap (string)
Per-input frame offsets. Format: `INPUT_ID+OFFSET` entries separated by commas, where INPUT_ID is the 1-based input number and OFFSET is a number of frames: the padding frames prepended to that input in `pad_for_alignment` mode, or the leading frames dropped from it in `first_common_frame` mode. Example: `1+2, 2+2, 3+1, 4+1`. Default: `""` (not configured). Use the Source Alignment Analysis tool to generate this value.

## Tools

### Source Alignment Analysis
Analyses all connected sources and determines the optimal per-input offset so that every source starts at the same disc position. Choose an alignment mode: `pad_for_alignment` prepends synthetic padding frames so all sources start from the earliest picture available across all inputs (no frames are lost). In this mode each source is placed by the picture number its first frame holds, taken as the median of (picture number − frame position) over the frames scanned, so a source that opens on a lead-in frame or an unreadable frame, or carries one corrupt early number, still lines up with the others. A source with no picture numbers at all cannot be placed: it is left out of the map, and so out of the stage's outputs, and the report names it. `first_common_frame` trims all sources to the first VBI frame that exists in every input (may discard unique leading content). The tool writes the resulting offset map to `alignmentMap` automatically. The analysis can be cancelled at any time from the tool dialog. Available for LaserDisc sources only. Invoke from the Stage Tools menu.

After placing the sources, the analysis compares them on up to 8 frames of the same picture taken across the stretch every source covers, and adds a **Capture comparison** to its report:

- Each source's noise in 10-bit codes, measured after shifting it onto the first source's sample grid and skipping flagged samples.
- The **pressings**. Discs of one pressing carry the noise recorded into its stamper, which only averages out across pressings. The report groups the sources by pressing, using the same measurement as the Stacker's Weighted mode (A {1, 2, 3}, B {4, 5}). If all the sources come from one pressing, it says that stacking them cannot remove the stamper's noise. Pressings can only be told apart with 4 or more sources. A source alone in its group is either the only disc of its pressing or much noisier than the rest; the two look the same.
- Each source's **response at 5–6 MHz** against the typical source's, with 3 or more sources. A source below 0.75 is reported as soft in a warning: its capture chain loses fine detail, and stacking it with sharper sources blurs the result slightly.

The comparison only reports; it does not change the alignment map.

### From the command line
`orc-cli tools run <project> source_alignment --node <this stage> --apply` runs the analysis and writes `alignmentMap` and `alignmentMode`; without `--apply` it only reports them. Choose the mode with `--param alignmentMode=first_common_frame` or `pad_for_alignment`. Map each disc with the Disc Mapper first. The command refuses a stack whose sources mix `.tbc` and `.cvbs` captures (exit code 5); take all the discs of one stack from the same capture format.

### Refreshing from the command line
The Source Alignment Analysis dialog has a **Use this tool when refreshing from the CLI** option. Tick it and press **Apply to Stage** or **OK** to record in the project that `orc-cli tools refresh` runs the analysis for this stage, with the alignment mode set in the dialog; save the project to keep it. `orc-cli tools refresh-config <project> <this stage> source_alignment` makes the same choice from a script. A refresh maps every disc's Frame Map first, so the alignment always reads the new frame lists.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | `alignmentMap` is set. |
| Red | `alignmentMap` is empty. Run the Source Alignment Analysis tool, or enter the map by hand. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
