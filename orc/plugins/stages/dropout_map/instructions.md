# Dropout Map

Overrides the dropout hints attached to individual frames without altering the underlying video sample data. The stage can add new dropout regions that the automatic detector missed, remove false-positive regions, or adjust the boundaries of existing regions on a per-frame basis. Downstream stages such as Dropout Correction see the modified hints and act accordingly.

## When to use

Use Dropout Map when the automatic dropout detection in the source stage has produced incorrect results on specific frames. Example: the automatic detector missed a physical scratch across frame 42, or incorrectly flagged a valid high-chroma region on frame 100 as a dropout. Add Dropout Map after the source stage, open the Edit Dropout Map tool to draw corrections interactively, then let Dropout Correction fix the marked regions.

## What it does

The stage wraps the incoming frame representation and intercepts calls to `get_dropout_hints()`. For each frame that has an entry in the `dropout_map` parameter, additions are merged into the hint list and removals are subtracted before the list is returned to the caller. Frames without an entry are forwarded with their original hints unchanged. No pixel data is read or modified by this stage.

## Parameters

### dropout_map (string)
Per-frame dropout override specification encoded as a JSON-like array. Each element identifies a frame and lists regions to add or remove. Default: `[]` (no overrides). Example: `[{frame:1,add:[{line:10,start:100,end:200}],remove:[{line:15,start:50,end:75}]}]`. Frame numbers are entered 1-based in the GUI parameter dialog, matching the preview window; the project file (YAML) stores them 0-based and the conversion is automatic. Line, start, and end values are frame-flat 0-based coordinates; `start` and `end` are sample positions within the line, both inclusive. Use the Edit Dropout Map tool to build and edit this string interactively rather than writing it by hand.

## Tools

### Edit Dropout Map
Opens the interactive dropout map editor. Select this tool from the stage's Stage Tools menu to visually draw new dropout regions or delete existing ones on a per-frame basis. Edits are written to the `dropout_map` parameter when you press Apply or OK; Cancel discards them.

The editor displays the frame exactly as the preview window does (same rendering pipeline, sequential frame layout) with the same aspect-ratio options (1:1 square samples or 4:3 display). Dropout regions are drawn as overlays on the frame:

- **Red** — dropouts detected by the source
- **Green** — regions added in the editor
- **Grey (struck-through)** — source dropouts marked for removal

Editing is modeless and works by direct manipulation:

- **Add** — click and drag along a line on an empty area of the frame.
- **Select** — click any region (or its row in the region table; selection stays in sync both ways).
- **Move / resize** — drag a selected addition to move it; drag the square handles at its ends to resize it; arrow keys nudge it one line/sample at a time.
- **Resize a source dropout** — a selected red source dropout also shows resize handles (unless it is already marked removed). Dragging a handle marks the source dropout as removed and creates an addition covering the new extent, as a single undoable edit; the replacement can then be moved and resized like any other addition.
- **Delete / remove** — press `Delete` (or use the right-click menu): on an addition it deletes the region; on a red source dropout it toggles the removed mark; on a struck-through region it restores it.
- **Undo / redo** — `Ctrl+Z` / `Ctrl+Shift+Z` (or the Undo/Redo buttons) step through every edit, including edits on other frames.

All regions on the frame are listed in a single table with their status (Source / Removed / Added), line (1-based, matching the rest of the GUI), and start/end samples. Zoom with the toolbar buttons (including 1:1 and Fit) or Ctrl+mouse-wheel, which zooms at the cursor position; the plain wheel pans, and the arrow keys pan when no addition is selected.

Frame navigation matches the preview window: first/previous/next/last buttons, a scrub slider, a 1-based jump box, and Page Up/Page Down keys. Frames that carry edits are marked in orange on the slider, and the *◀ Edit* / *Edit ▶* buttons jump between them. The pipeline executes and frames render in the background — the editor opens immediately and shows progress in its status bar.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
