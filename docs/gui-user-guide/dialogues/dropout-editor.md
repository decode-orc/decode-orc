# Dropout Editor

The **Dropout Map Editor** is where the dropout regions of a [Dropout Map](../stages/transform-stages.md#dropout-map) stage are drawn, moved, resized and removed by hand. It shows each frame of the stage's input with the source's dropout regions laid over it, and records your changes as the stage's `dropout_map` parameter, so that stages downstream — dropout correction in particular — work from the corrected map.

---

## Opening the editor

* Right-click a **Dropout Map** stage
* Open **Stage Tools**
* Choose **Edit Dropout Map**

The editor is a window of its own: the graph, the preview and the rest of the application stay usable while it is open. Frames are rendered in the background through the same pipeline as the Preview window, so the editor stays responsive while a frame is on its way.

---

## Navigating frames

The row along the top mirrors the Preview window:

| Control | Description |
|---------|-------------|
| **<<** / **<** / **>** / **>>** | First, previous, next and last frame. Hold **<** or **>** to step continuously. |
| Number box | Jump straight to a frame. Frame numbers are 1-based, as everywhere in the GUI. |
| Slider | Scrub through the frames. Frames that carry edits are marked on the slider in orange. |
| **◀ Edit** / **Edit ▶** | Jump to the previous or next frame that carries edits. |

**Page Up** and **Page Down** also step back and forward one frame. The status bar shows the frame number and how many additions and removals it carries.

## View controls

| Control | Description |
|---------|-------------|
| **Aspect** | **1:1 (Square)** or **4:3 (Display)** (the default, matching the preview). |
| **Zoom** **−** / **1:1** / **Fit** / **+** | Zoom out, back to 100 %, to fit the window, or in. The current zoom is shown beside the buttons. |

The frame is shown with its lines in sequence, top to bottom. **Ctrl+mouse wheel** zooms at the pointer; the plain wheel, the scroll bars, and the cursor keys (when no addition is selected) pan. **+**, **−** and **0** zoom in, zoom out and reset to 1:1.

---

## Editing

Editing is direct: there is no mode to switch into.

| Region | Colour | What you can do |
|--------|--------|-----------------|
| **Source dropout** — detected in the capture | red | Select it, then **Delete** (or right-click → **Mark dropout removed**) to mark it removed; do the same again to restore it. Drag a handle at either end to resize it: the editor marks the original removed and creates an addition covering the new extent, in one step. |
| **Addition** — a region you drew | green | Drag it to move it, drag a handle to resize it, or nudge it a sample or line at a time with the cursor keys. **Delete** removes it. |
| **Removal** — a source dropout you marked removed | grey, struck through | Restore it as above. |

* Drag across an empty part of the picture to add a dropout region.
* Click a region, or its row in the **Dropout Regions** table, to select it. The table lists every region on the frame with its status, line and start and end samples.
* The **Delete** button's label changes to say what it will do to the selection — **Delete Addition**, **Mark Removed**, **Restore Dropout** or **Delete Removal**.
* **Clear Frame Edits** removes every addition and removal on the current frame.
* Right-click on the picture for the same actions, plus **Undo** and **Redo**.

A removal left in a hand-edited project file without a matching source dropout is shown struck through and can only be deleted.

### Undo and redo

Every edit can be undone: **Undo** and **Redo** buttons, **Ctrl+Z** and **Ctrl+Shift+Z** (the platform's usual redo key also works), or the right-click menu. Undoing an edit on a frame other than the one on show moves to that frame first, so you can see what was undone. Successive nudges of the same region count as one edit.

---

## Saving the changes

| Button | Action |
|--------|--------|
| **Apply** | Writes the edited map to the stage and leaves the editor open. The pipeline is rebuilt and the preview updates. |
| **OK** | Applies, then closes the editor. |
| **Cancel** | Closes the editor. Edits not yet applied are discarded. |

Applying marks the project as modified; use **File → Save Project** to keep the change. The stage's `dropout_map` parameter can also be edited as text in **Edit Parameters…** — see the [Dropout Map stage](../stages/transform-stages.md#dropout-map) for its format.

Analysis sinks see the edited map only when they are connected downstream of the Dropout Map stage.
