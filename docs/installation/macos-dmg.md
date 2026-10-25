# MacOS DMG Installation

## Download the latest release

1. Open the GitHub releases page: [Decode-Orc Releases](https://github.com/decode-orc/decode-orc/releases){target="_blank"}
2. In the latest release, download the DMG named like `Decode-Orc-<version>-macos-arm64.dmg`.
   The DMG is built for Apple Silicon Macs.

## Install the app from the DMG

1. Open the downloaded DMG.
2. Drag `orc-gui.app` to the `Applications` folder.
3. Eject the DMG when the copy completes.

## Run a self-signed DMG or app

Important: The app is only ad-hoc signed (it is not signed with an Apple Developer ID or notarised), so MacOS Gatekeeper may block the first launch.

Option A: Open via Finder (recommended)

1. In Finder, open `Applications`.
2. Right-click `orc-gui.app` and choose **Open**.
3. Confirm the prompt to open the app.

Option B: Allow in System Settings

1. Try to open the app once (it will be blocked).
2. Go to **System Settings** -> **Privacy & Security**.
3. Under **Security**, click **Open Anyway** for Decode Orc.

Option C: Remove the quarantine attribute (advanced)

Run this once in Terminal:

```bash
xattr -dr com.apple.quarantine "/Applications/orc-gui.app"
```

## Use `orc-cli` from the command line

The command-line tool is bundled inside the app:

```
/Applications/orc-gui.app/Contents/MacOS/orc-cli
```

You can run it directly, or add a symlink so it is on your PATH:

```bash
sudo ln -s "/Applications/orc-gui.app/Contents/MacOS/orc-cli" /usr/local/bin/orc-cli
```

After that, you can run:

```bash
orc-cli --help
```
