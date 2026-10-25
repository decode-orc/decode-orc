# Closed Caption Sink

Extracts one EIA-608 service from NTSC Line 21 and writes it as Scenarist SCC, SubRip subtitles, plain text or an HTML transcript. Use this stage in parallel with the TBC Sink stage when archiving captioned NTSC LaserDiscs.

## When to use

Add this sink when you are processing an NTSC LaserDisc that carries Line 21 closed captions and you want to preserve or inspect the caption data. SCC is the archival format and loads into caption-authoring tools; SRT plays in any modern media player; plain text is for reading; HTML keeps the column layout, which is what makes a text service such as a listings or scores page readable.

## What it does

For each field in the pipeline, the stage reads the two caption bytes embedded in VBI Line 21 from the incoming stream, selects the pairs belonging to the chosen service, and writes them in the chosen format.

Line 21 does not carry one caption stream but four, multiplexed into the same two bytes per field: **CC1** and **CC2** (captions) and **TEXT1** and **TEXT2** (pages of text such as schedules, scores or station information). Which service a byte pair belongs to is decided by the control codes that came before it. Exported without demultiplexing, a recording that used more than one produces garbled output — a caption running through a page of listings. This is what the `service` parameter is for.

## Parameters

### output_path (string)
Path to the closed-caption output file. Required. Conventionally `.scc`, `.srt`, `.txt` or `.html` to match the format.

### service (string)
Which of the services multiplexed onto Line 21 to export. Values: `CC1` (the primary caption service, and what most recordings use), `CC2` (a second caption service, often a translation), `TEXT1` and `TEXT2` (text services). Default: `CC1`.

Only the first field's four services are offered. CC3, CC4, TEXT3 and TEXT4 ride on Line 21 of the second field, which the host's closed-caption observer does not decode.

To export more than one service, add a second Closed Caption Sink with its own output path.

### format (string)
Export format. Default: `Scenarist SCC`.

| Value | Output |
|-------|--------|
| `Scenarist SCC` | Scenarist SCC V1.0: `HH:MM:SS:FF` timecodes and the selected service's byte pairs exactly as transmitted, channel bits and the duplicate copy of each control code included. Consumers of an SCC file de-duplicate for themselves. |
| `SubRip SRT` | Numbered cues with `HH:MM:SS,mmm` times |
| `Plain Text` | Each caption under a `[HH:MM:SS:FF]` timestamp |
| `HTML` | A monospaced transcript, each caption in a `<pre>` block beside its timestamp |

In the three decoded formats the rows of the caption display stay on separate lines and keep their indent, so a text service's columns still line up.

## Notes

CC data must be present in the source capture — this stage cannot synthesise captions. It handles NTSC Line 21 only; PAL sources do not carry Line 21 CC data. Do not use a Mask Line transform to blank line 21 before this stage, as that will destroy the caption payload. If the source fields contain no CC data the output file will be empty but the stage will not abort.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
