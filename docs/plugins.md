# Using Plugins

Plugins add new stages to Decode-Orc: a different chroma decoder, a new kind of
source, an extra sink. Once installed, a plugin's stages are used exactly like
the built-in ones: you add them to a project from the graph editor, set their
parameters and preview their output.

This page covers finding, installing, updating and removing plugins, and what
to do when one does not load. The menus and commands it mentions are described
in full in the [Plugin Manager](gui-user-guide/dialogues/main.md#plugin-manager)
section of the GUI guide and in [Plugin management](cli-user-guide/overview.md#plugin-management)
in the CLI guide. If you want to write a plugin yourself, see the
[Plugin Author Guide](technical/plugin-author-guide.md).

## Core plugins and third-party plugins

Every stage that ships with Decode-Orc is itself a plugin, called a **core
plugin**. Core plugins are always loaded and cannot be removed, and the
Plugin Manager hides them unless you tick **Show core plugins**.

Everything else is a **third-party plugin**: a separate program file written
by someone else and published on GitHub. Decode-Orc keeps a curated
**index** of the third-party plugins it knows about, which is what
**Browse Plugins…** lists, but you can also install a plugin that is not in
the index from its GitHub releases page or from a file on disk.

## Only install plugins you trust

A plugin is native code. It runs inside Decode-Orc with the same access to your
files and network as the application itself, and nothing sandboxes it. Being
listed in the index means a plugin has been accepted into the list; it does not
mean its code has been reviewed.

So Decode-Orc never runs a third-party plugin until you have said it may.
Adding, installing, updating and enabling a plugin each ask you to confirm
first. A plugin that arrived some other way, for example through a hand-edited
registry file, stays switched off until you enable it.

## Installing a plugin

### From the index

1. Open **Tools → Plugin Manager…** and click **Browse Plugins…**.
2. Type in the search box to narrow the list, then select a plugin to see its
   description, latest version, license and source repository.
3. Click **Install…** and confirm that the plugin may run. Decode-Orc downloads
   the build of the plugin's latest release for your operating system.
4. Click **OK** in the Plugin Manager. Decode-Orc offers to quit; start it
   again to load the new plugin.

**Install…** is greyed out for a plugin you already have and for one marked
**incompatible**, which means its latest release has no build for your
operating system or for this version of Decode-Orc.

From the command line:

```bash
orc-cli plugins search chroma
orc-cli plugins info org.decodeorc.stage.hvd_chroma_decoder
orc-cli plugins install org.decodeorc.stage.hvd_chroma_decoder
```

### From a GitHub release or a file

If a plugin's author points you at their repository but it is not in the index,
use **Add Plugin…** in the Plugin Manager and give it either:

- the repository's releases URL, such as `https://github.com/owner/repo/releases`.
  Decode-Orc reads the release's `orc-plugin-manifest.yaml`, picks the build for
  your platform, checks it was built for this version of Decode-Orc and
  verifies its checksum before saving it. A release without that manifest
  cannot be installed this way; or
- a plugin file you already have: a `.so` on Linux, a `.dylib` on macOS or a
  `.dll` on Windows.

The command-line equivalents are `orc-cli plugins add --url <releases-url>` and
`orc-cli plugins add <file>`.

A plugin added from a file has no upstream release to check, so Decode-Orc
cannot tell you when it has been updated. To update it, add the new file again.

## Using a plugin's stages

After the restart, the plugin's stages appear in the graph editor's right-click
menu under a separate **Plugins** entry, grouped as Source, Transform, Analysis
and Sink like the built-in stages. As with the built-in stages, only the ones
that work with your project's video system are listed. Right-click a plugin
stage and choose **Help...** to read the plugin author's own instructions for
it.

In `orc-cli`, a plugin stage is named by its stage name, like any other stage.
`orc-cli stages list --plugin <id>` shows which stages a plugin provides.

## Keeping plugins up to date

Each time the Plugin Manager opens, it checks every installed plugin against
its latest GitHub release in the background, and the **Update** column then
shows **Up to date**, **Update available (version)**, **Unreachable**, or **—**
for a plugin with no repository to check. Select a plugin with an update and
click **Update**.

A new version is a new program, so an update asks you to confirm that it may
run, the same as an install.

```bash
orc-cli plugins updates          # what has an update
orc-cli plugins update --all     # install every update
```

### After upgrading Decode-Orc

A plugin is built for one version of Decode-Orc's plugin interface (its
**ABI**). When a new Decode-Orc release changes that interface, plugins built for
the old one stop loading, and the Plugin Manager shows their status as
**Needs a rebuild** with a ⚠ beside the version. Nothing is wrong with your
installation. The plugin's author needs to publish a release built for the new
version. Once they have, **Update** installs it.

## Disabling and removing plugins

Untick **Enabled** to stop a plugin loading while keeping it installed. Tick
it again to bring it back. **Remove** unregisters the plugin entirely.
Both take effect the next time Decode-Orc starts. On the command line, use
`orc-cli plugins disable <id>`, `enable <id>` and `remove <id>`.

## Projects that use plugins

When you save a project that uses a plugin's stages, the project file records
which plugins it needs and where they come from. If you open it on a machine
that is missing one of them, or where the plugin is disabled or failed to load,
Decode-Orc refuses to open the project and names the stages it could not find.
`orc-cli` also names the plugin and its repository when the project recorded
them. Install or re-enable the plugin, restart, and open the project again.

If you send a project to someone else, tell them which plugins it uses.

## When a plugin does not load

The **Status** line in the Plugin Manager's **Details** pane gives the reason:

| Status | Meaning | What to do |
|---|---|---|
| **Enabled** | Will load at the next start | Nothing |
| **Disabled** | Switched off | Tick **Enabled** |
| **Not trusted yet** | You have not yet confirmed it may run | Tick **Enabled** and confirm |
| **Needs a rebuild** | Built for a different Decode-Orc version | Wait for a new release, then **Update** (see [above](#after-upgrading-decode-orc)) |
| **Binary missing** | The plugin file has been moved or deleted | **Remove** it and install it again |

For anything else, open the **Diagnostics** section at the bottom of the Plugin
Manager, or run `orc-cli plugins doctor`. Each line there is a message
recorded while plugins were loading at startup, such as a file that could not
be opened or a stage that was rejected because another plugin already uses
its name.

### If Decode-Orc will not start

If a plugin crashes Decode-Orc or stops it working, start it with only the core
plugins:

```bash
orc-gui --safe-core-plugins
orc-cli --safe-core-plugins plugins list
```

This **empties your plugin registry**, so every third-party plugin is
unregistered and must be installed again afterwards, not just skipped for one
run. Before you use it, try disabling the plugin you suspect from the command
line instead (`orc-cli plugins disable <id>`), which keeps the rest of your
plugins.

To report a problem with a plugin, contact its author: the Details pane shows
its source repository. If you think the problem is in Decode-Orc itself, see
[Reporting issues](misc/issue-reporting.md) and turn on
**Tools → Logging…** before reproducing it.

## Where plugins are kept

The list of installed plugins, called the **registry**, is a file named
`stage-plugins.yaml`. Downloaded plugin files go in a `plugin-cache` folder
beside it:

| System | Folder |
|---|---|
| Linux | `~/.config/decode-orc/` (or `$XDG_CONFIG_HOME/decode-orc/`) |
| Linux Flatpak | `~/.var/app/io.github.decode_orc.decode-orc/config/decode-orc/` |
| macOS | `~/.config/decode-orc/` |
| Windows | `%APPDATA%\decode-orc\` |

The Plugin Manager shows the exact registry path above its table, and
`orc-cli plugins doctor` prints it. The GUI and `orc-cli` share the same
registry, so a plugin installed with one is available in the other.

### Loading plugins from a folder

Setting the `ORC_STAGE_PLUGIN_PATHS` environment variable to one or more
folders (separated by `:`, or `;` on Windows) makes Decode-Orc load every
plugin it finds there at startup. These plugins are not registry entries, so
there is no confirmation step and the Plugin Manager cannot disable them.
This is meant for plugin authors testing their own builds. For everyday use,
install plugins through the Plugin Manager instead. `--safe-core-plugins`
ignores the variable for that run.
