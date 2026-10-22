# Migration

## Migrating from Decode-Orc 1.x to 2.0

Decode-Orc 2.0 introduces a new frame-based internal representation (CVBS_U10_4FSC). This is a **breaking change**. Project files from Decode-Orc 1.x are not compatible with 2.0 and will be rejected at load time with a clear error message.

**Project file migration**: There is no automated migration path. Projects must be recreated in Decode-Orc 2.0. This is necessary because the stage names, parameter names, and signal-level domain have all changed in ways that cannot be safely inferred from a 1.x project file.

**Renamed stages**: The following stages were renamed in v2.0. If you recreate a project, use the new stage IDs:

| Old stage ID (1.x) | New stage ID (2.0) | Notes |
|--------------------|--------------------|-------|
| `pal_comp_source` | `tbc_source` | Unified stage; video system detected from metadata |
| `pal_yc_source` | `tbc_source` | |
| `ntsc_comp_source` | `tbc_source` | |
| `ntsc_yc_source` | `tbc_source` | |
| `pal_m_tbc_comp_source` | `tbc_source` | |
| `field_map` | `frame_map` | New parameters: `remove_duplicates`, `pad_gaps`, `pad_strategy` |

**Removed stages**: The following stage was removed in v2.0 with no replacement:

| Removed stage ID | Reason |
|------------------|--------|
| `hackdac_sink` | Hardware-specific stage removed from the core distribution |

**Renamed dialogues**: The GUI dialogues have been renamed to use frame-based terminology:

| Old name (1.x) | New name (2.0) |
|----------------|----------------|
| Line-scope | Frame-scope |
| Field-timing | Frame-timing |

**New stages in v2.0**:

| Stage ID | Description |
|----------|-------------|
| `cvbs_source` | Load CVBS captures in the CVBS file-format family |
| `cvbs_sink` | Write output to the CVBS file-format family in a selectable sample encoding |
| `vbi_source` | Ingest raw VBI teletext captures by laying their line records onto CVBS frames at the timing point and amplitude the standard puts them at |

**Signal level domain**: All internal signal levels in v2.0 are in the CVBS_U10_4FSC 10-bit domain (blanking = 256, white = 844 for PAL; blanking = 240, white = 800 for NTSC). The 16-bit IRE-based domain used in v1.x is gone. The `video_params` stage parameters for level overrides now use 10-bit values.

---

## Unique stage names

Stage names (the label shown on each stage, stored as `user_label` in the project file) must now be unique within a project. Older projects often break this rule, because every new stage used to take its type's name: a project with three Frame Map stages had three stages called "Frame Map".

When such a project is opened, each stage in a group of duplicates gets the name followed by a number. The numbers start at 1 and follow node ID order, so the three Frame Map stages become "Frame Map 1", "Frame Map 2" and "Frame Map 3". A number already used by another stage is skipped. Stage names that were already unique are not changed.

A stage name that is empty or made only of digits is replaced with the stage type's name, and numbered like the others if that name is taken.

The project is marked as modified when it opens, and the new names are written the next time it is saved. Until then the file on disk keeps its old names. `orc-cli` applies the same renaming each time it loads the project. Processing a project, `orc-cli nodes list` and `orc-cli nodes get` do not rewrite the file; `orc-cli nodes set` saves the project, so it writes the new names.

## Processing a project from the command line

`orc-cli <project> --process` now runs each sink with the same settings the GUI uses. Two things change for projects that relied on the old behaviour:

* **Settings the project never stored** take the default for the project's video format, as the sink's parameter dialog shows. Previously a sink got its format-independent default, so a PAL project whose Video Sink had no stored decoder failed with "This decoder is for NTSC video sources only".
* **Relative output paths** are resolved against the directory of the project file, as they are in the GUI and as the [CLI guide](../cli-user-guide/overview.md#setting-parameters) describes. Previously they were taken relative to the directory `orc-cli` was run from. A project processed from its own directory writes to the same place as before; one processed from elsewhere now writes beside the project file. Use an absolute path to write somewhere else.

---

## Migration from legacy vhs-decode builds

## Legacy JSON metadata

The current version of Decode-Orc (and the ld-decode-tools Decode-Orc replaces) require TBC files with metadata in the current SQLite format (previously ld-decode used JSON rather than SQLite).  Although older JSON metadata decodes will be accepted after a warning, they lack some of the newer fields used by Decode-Orc and may cause the tools to emit warnings about missing parameters.

If you have old decodes the recommended course of action is to re-decode the capture with a current version of ld-decode or vhs-decode.  Do not rely on post-decoded files as preservation artifacts as the decoder functionality will vary over time and the post-decode artifact is not deterministic (TL;DR - re-decoding is preservation - keeping old TBC files is not).

## Separate Y/C sources

Legacy versions of vhs-decode produced TBC Y/C pairs with the format `<name>_chroma.tbc` and `<name>.tbc`.

In order to use these files with Decode-Orc you will need to simply rename them.  `<name>_chroma.tbc` should become `<name>.tbcc` and `<name>.tbc` should become `<name>.tbcy`.  This is used by Decode-Orc to distinguish composite and separate sources when using features such as "quick project".
