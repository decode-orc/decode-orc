# Orc GUI — Main Window

## Overview

The main window is the primary interface for orc, a LaserDisc and tape decoding
orchestration framework. It hosts a DAG (Directed Acyclic Graph) editor where
processing pipelines are built by connecting stage nodes. Each node represents
one processing step; connections between nodes define the data flow.

## Toolbar

Directly beneath the menu bar is a toolbar with quick-access buttons that mirror
common menu actions:

| Button | Equivalent Menu Action | Description |
|--------|------------------------|-------------|
| Arrange to grid | View → Arrange DAG to Grid | Automatically arrange all nodes in an ordered grid layout. |
| Show preview | View → Show Preview | Open the Preview Window, or bring it to the front if it is already open. Disabled until a project is loaded. |
| Reload all sources | File → Reload All Sources | Re-read every source in the project from disk. Disabled until the project has a source stage. |
| Theme | Tools → Themes | Cycles the UI theme in the order Auto → Light → Dark. The icon shows the current mode (half-disc for Auto, sun for Light, crescent moon for Dark), and stays in sync with the Tools → Themes submenu. |

The toolbar can be hidden or shown from **View → Show Toolbar**.

## File Menu

| Action | Shortcut | Description |
|--------|----------|-------------|
| New Project... | Ctrl+N | Create a new project, choosing the source type and video standard. |
| Quick Project... | Ctrl+Shift+Q | Create a pre-configured project with a standard pipeline for the chosen source. |
| Open Project... | Ctrl+O | Open an existing .orc project file from disk. |
| Save Project | Ctrl+S | Save the current project to its current file. |
| Save Project As... | Ctrl+Shift+S | Save the current project under a different file name. |
| Edit Project... | — | Edit top-level project settings such as video format, source type, media path, and signal units (10-bit, mV, or IRE). |
| Reload All Sources | Ctrl+R | Re-read every source stage in the project from disk, picking up a capture that has changed or grown since the project was opened. The pipeline itself is left exactly as it is. Works from any window, including the Preview Window. |
| Quit | Ctrl+Q | Exit the application. |

## View Menu

| Action | Shortcut | Description |
|--------|----------|-------------|
| Show Preview | Ctrl+Shift+P | Open the Preview Window, or bring it to the front if already open. |
| Show Preview on Selection | — | When checked, selecting a preview-capable node automatically opens and updates the Preview Window. |
| Arrange DAG to Grid | Ctrl+G | Automatically arrange all nodes in an ordered grid layout. |
| Show Toolbar | — | Toggle visibility of the toolbar beneath the menu bar. |

## Tools Menu

| Action | Description |
|--------|-------------|
| Plugin Manager... | Open the Plugin Manager to load, inspect, and manage runtime stage plugins. |
| Logging... | Turn diagnostic logging to a file on or off, choose how much detail it records, and choose where the file goes. Takes effect immediately and is remembered between runs. |
| Themes | Choose the UI theme: Auto (follow the operating system), Dark, or Light. Overrides the `--theme` command-line option and is remembered between runs. |

### Logging

Records what the application is doing to a file so it can be attached to a bug
report, without having to relaunch from a command line.

| Control | Description |
|---------|-------------|
| Write log messages to a file | Turns file logging on and off. Console output is unaffected. |
| Detail | How much is recorded: trace, debug, info, warn, error, critical, or off. Use debug for a bug report. |
| Log file | Where the file goes. Leave it blank to use the default location shown in the field. |
| Open Log Folder | Opens the folder holding the log file in the system file manager. |

The change takes effect as soon as you click OK - the application does not need
to be restarted, and stage plugins that are already loaded start writing to the
new file too.

The log file is replaced, never appended to, so it only ever holds one capture.
Every run of the application starts it again from empty, as does turning
logging on or choosing a different file. Changing only the detail level while
the same file stays open keeps what has been recorded so far.

Release builds compile trace records out, so trace records nothing more than
debug there; the dialogue says so when that applies.

The settings are remembered for the next run. The `--log-level`, `--log-file`
and `--log-out` command-line options override them for a single run without
changing what is remembered.

## DAG Editor

The central panel is a node-graph editor. Each node represents a processing
stage; edges carry data between stages from left (input ports) to right (output
ports).

### Adding Nodes

Right-click on the empty canvas to open the node creation menu. Node types are
grouped by category such as Source, Transform, Merger, and Sink.

### Connecting Nodes

Drag from an **output port** (right edge of a node) to an **input port** (left
edge of another node). A single output can feed multiple downstream nodes. A
node will not execute until all of its required inputs are connected.

### Selecting Nodes

Click a node to select it. Hold **Shift** and drag on the canvas to draw a
selection rectangle and select multiple nodes at once. If **Show Preview on
Selection** is enabled, selecting a preview-capable node will automatically
update the Preview Window to show that node's output.

### Keyboard Navigation

With the canvas focused, the selection can be moved from the keyboard, which is
quicker than the mouse when stepping back and forth between two stages to
compare their previews.

| Key | Action |
|-----|--------|
| Left / Right / Up / Down | Move the selection to the neighbouring node in that direction. With nothing selected, the node nearest the centre of the view is selected. |
| Tab | Select the next node in the order the stages were added, wrapping at the end. |
| Shift+Tab | Select the previous node in that order. |
| Ctrl+Left / Right / Up / Down | Scroll the canvas, leaving the selection where it is. |
| Delete / Backspace | Delete the selected node or connection. |

Cursor keys pick the node you are pointing at rather than following the
connections: a node level with the current one in the direction pressed always
wins, and a node off to one side is only chosen when nothing is level. The view
scrolls to the newly selected node if it is off screen.

### Moving and Arranging Nodes

Drag a node to reposition it on the canvas. Use **Arrange DAG to Grid**
(Ctrl+G) to tidy all nodes automatically.

### Node Context Menu

Right-click any node for:

| Action | Description |
|--------|-------------|
| Help... | Open the stage's built-in documentation covering its parameters, interactive tools, and behaviour. |
| Set as Preview Source | Route this node's output to the Preview Window without changing the selection. |
| Edit Parameters... | Open the stage parameter dialog to view and edit all configurable parameters. |
| Tools | Open an interactive stage tool, if the stage provides one. Stage tools can also set parameters directly. |
| Delete | Remove the node and all its connections from the DAG. |

## Status Bar

The status bar at the bottom shows informational messages such as plugin load
results, save confirmations, and operation status.
