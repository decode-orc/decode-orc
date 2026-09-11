# Issue Reporting

## Reporting Crashes

If Decode Orc crashes unexpectedly, it will automatically create a **diagnostic bundle** to help identify and fix the problem.

### What Happens When a Crash Occurs

When the application crashes, you'll see a message showing where the diagnostic bundle was saved:

**For orc-gui:**
- A dialog box will appear with the crash information and bundle location
- Example (Linux): `/home/username/Documents/decode-orc-crashes/crash_bundle_20260111_143022.zip`

**For orc-cli:**
- A message is printed to the terminal showing the bundle location
- Example: `./crash_bundle_20260111_143022.zip` (in your current directory)

The exact directory depends on your operating system - see
[Crash Bundle Locations](#crash-bundle-locations) below.

### Crash Bundle Contents

The crash bundle is a ZIP file containing:

- **crash_info.txt** - Detailed crash report including:
  - Application version (git commit hash)
  - System information (OS, CPU, memory)
  - Stack backtrace showing where the crash occurred
  - Timestamp of the crash
  
- **README.txt** - Instructions for reporting the issue

- **Log files** - Any application log files (if available)

- **Coredump** - Memory dump file (if available, may be large)

### How to Report a Crash

1. Go to the [GitHub Issues page](https://github.com/decode-orc/decode-orc/issues){target="_blank"}

2. Click **"New Issue"** and select the bug report template

3. **Attach the crash bundle ZIP file**
   - If the file is smaller than 25MB, attach it directly to the issue
   - If it's larger, upload it to a file sharing service (Google Drive, Dropbox, etc.) and include the link

4. **Copy the contents of crash_info.txt** into your issue description
   - This helps us quickly see the crash details
   - You can redact any sensitive file paths if needed

5. **Describe what you were doing** when the crash occurred:
   - What project were you working on?
   - What operation did you just start/complete?
   - Can you reproduce the crash?
   - Any recent changes to your system or workflow?

### Privacy Note

The crash bundle may contain:
- File paths from your system
- Project file names
- Log file contents
- Memory contents (in the coredump)

**Please review the contents** of `crash_info.txt` before uploading to ensure you're comfortable sharing this information publicly.

### Crash Bundle Locations

Bundles are named `crash_bundle_YYYYMMDD_HHMMSS.zip`, where the timestamp is
the local time of the crash.

**orc-gui** writes bundles to a `decode-orc-crashes` folder inside your
Documents folder. The folder is created when the application starts, so it may
exist and be empty if you have never had a crash.

| Operating system | Crash bundle directory |
| --- | --- |
| Linux | `~/Documents/decode-orc-crashes/` (or the folder set as `XDG_DOCUMENTS_DIR` in `~/.config/user-dirs.dirs`) |
| macOS | `~/Documents/decode-orc-crashes/` |
| Windows | `C:\Users\<username>\Documents\decode-orc-crashes\` |

!!! note "Windows and OneDrive"
    If your Documents folder is redirected to OneDrive, the bundle is written
    to the redirected location instead, for example
    `C:\Users\<username>\OneDrive\Documents\decode-orc-crashes\`.

!!! note "Linux Flatpak"
    The Flatpak build has access to your home directory, so bundles appear in
    your real `~/Documents/decode-orc-crashes/` folder rather than inside the
    sandbox.

If the Documents folder cannot be determined, orc-gui falls back to
`decode-orc-crashes` in your home directory (`~/decode-orc-crashes/`,
`C:\Users\<username>\decode-orc-crashes\` on Windows), and finally to the
current working directory.

**orc-cli** saves bundles in the current working directory - the directory you
were in when you ran the command - on Linux, macOS and Windows alike.

### Platform Differences in Bundle Contents

- **Windows** - the bundle contains a `minidump_YYYYMMDD_HHMMSS.dmp` file
  written at the moment of the crash. There is no coredump.
- **Linux** - the coredump is written by the system, not by Decode Orc, so it
  is only included if core dumps are enabled. `crash_info.txt` records where
  the system was expected to put it (for example systemd-coredump - list these
  with `coredumpctl list orc-gui` - or apport in `/var/crash/`).
- **macOS** - a coredump is only produced if you ran the application after
  setting `ulimit -c unlimited`, in which case it is written to
  `/cores/core.<pid>`.

On all platforms, if the system ZIP tool is unavailable the bundle is left as a
plain `crash_bundle_YYYYMMDD_HHMMSS` **folder** next to where the ZIP would have
been. Attach the folder's contents, or compress it yourself, when reporting.

### If You Can't Find the Crash Bundle

If the application crashed but you can't find the bundle:

1. Check the terminal output or error dialog for the exact path
2. Search your home directory for files named `crash_bundle_*`:
   - Linux/macOS: `find ~ -name 'crash_bundle_*' 2>/dev/null`
   - Windows (PowerShell): `Get-ChildItem $HOME -Recurse -Filter 'crash_bundle_*'`
3. Look in the current working directory where you ran the command

If no bundle was created, please report the crash manually with:
- Application version (run with `--version` flag)
- Operating system and version
- What you were doing when it crashed
- Any error messages shown

## Reporting Bugs (Non-Crash Issues)

For bugs that don't cause crashes, please provide:

1. **Steps to reproduce** the issue
2. **Expected behavior** vs actual behavior
3. **Application version** and platform
4. **Screenshots or videos** if helpful
5. **Log files** if available. In Orc-GUI, open **Tools → Logging…**, tick
   **Write log messages to a file**, set **Detail** to `debug`, click **OK**,
   then reproduce the problem and attach the file — **Open Log Folder** in the
   same dialog takes you to it. From the command line, use the `--log-file`
   option; add `--log-out file` to keep the console clear and capture
   everything in the file.

For a display problem — a preview or scope that draws wrongly, flickers, or
takes the window down with it — say which drawing path was in use: **Help →
About Orc GUI…** names it. Trying the other path narrows it down quickly:
untick **Draw the preview and scopes on the GPU** in **Tools → Logging…**, or
start the application with `ORC_GUI_GPU_RENDER=0` in the environment, and say
in the report whether that made the problem go away.

## Feature Requests

For new features or enhancements, please describe:

1. **What you want to achieve** and why
2. **Current workarounds** you're using (if any)
3. **Use cases** where this would be helpful

## Getting Help

For questions or help using Decode Orc (not bugs):

- Check the [user documentation](https://decode-orc.github.io/decode-orc/)
- Review existing [GitHub Issues](https://github.com/decode-orc/decode-orc/issues){target="_blank"} (someone may have had the same question)
- [The Domesday86 Discord Server](https://discord.com/invite/pVVrrxd){target="_blank"}