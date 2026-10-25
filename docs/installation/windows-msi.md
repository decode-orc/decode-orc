# Windows MSI Installation

## Prerequisite: Microsoft Visual C++ Redistributable

Decode-Orc for Windows is built with MSVC and links against the Microsoft Visual C++ runtime
(`MSVCP140.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`). The MSI does **not** bundle
the runtime, so it must be installed separately.

Most Windows systems already have it, because it is installed alongside many other
applications. A fresh Windows installation with few other programs on it usually does not.

1. Download the **latest supported X64 Visual C++ Redistributable** from Microsoft:
   [Microsoft Visual C++ Redistributable latest supported downloads](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
2. Run the downloaded `vc_redist.x64.exe` and follow the prompts.
3. Reboot if the installer asks you to.

Install the redistributable before installing Decode-Orc; if you have already installed
Decode-Orc there is no need to reinstall it afterwards.

## Download the latest release

1. Open the GitHub releases page: [Decode-Orc Releases](https://github.com/decode-orc/decode-orc/releases)
2. In the latest release, download the MSI named like `Decode-Orc-<version>-windows-x64.msi`.

## Install from the MSI

1. Double-click the downloaded MSI file.
2. Follow the Windows Installer wizard:
   - Review the license agreement, tick the box to accept it, and click "Next".
   - Choose the installation location (default: `Program Files\Decode-Orc`).
   - Click "Install" to begin installation. A Start Menu shortcut and a
     Desktop shortcut are both created.
3. Wait for the installation to complete and click "Finish".

## Unsigned MSI - Additional Steps Required

The MSI installer is **unsigned**, which means Windows Defender SmartScreen may block installation. Follow these steps:

### Option A: Install via Windows Defender SmartScreen Prompt (Recommended)

1. When you try to run the MSI, you may see a "Windows Defender SmartScreen prevented an unrecognized app from starting" dialog.
2. Click **"More info"** to expand the dialog.
3. Click **"Run anyway"** to proceed with installation.
4. The installer will then launch normally.

### Option B: Disable SmartScreen (Not Recommended)

If you don't see the prompt, you may need to manually disable SmartScreen protection temporarily:

1. Open **Windows Security**.
2. Go to **App & browser control**.
3. Under **SmartScreen for Microsoft Edge**, toggle to **Off**.
4. Run the MSI installer.
5. Re-enable SmartScreen protection after installation.

### Option C: Override via Command Line (Advanced)

If the GUI method doesn't work, you can install via PowerShell or Command Prompt:

```powershell
# In PowerShell as Administrator:
msiexec /i "C:\path\to\Decode-Orc-<version>-windows-x64.msi" /quiet
```

Replace `C:\path\to\Decode-Orc-<version>-windows-x64.msi` with the actual path to the downloaded MSI file.

## Launching the Application

After installation, you can launch Decode-Orc:

- **Via Start Menu**: Search for "Decode-Orc" and click the application.
- **Via Desktop Shortcut**: Double-click the "Decode-Orc" shortcut on your desktop.
- **Via File Explorer**: Navigate to `Program Files\Decode-Orc\bin\` and double-click `orc-gui.exe`.

## Using `orc-cli` from the command line

The command-line tool `orc-cli` is included in the installation. You can run it from the Command Prompt or PowerShell:

```cmd
"C:\Program Files\Decode-Orc\bin\orc-cli.exe" --help
```

To use `orc-cli` from any directory, add the installation directory to your system PATH:

1. Press `Win + X` and select **System**.
2. Click **Advanced system settings**.
3. Click **Environment Variables**.
4. Under **System variables**, select **Path** and click **Edit**.
5. Click **New** and add: `C:\Program Files\Decode-Orc\bin`
6. Click **OK** on all dialogs.
7. Restart your terminal.

You can then run:

```cmd
orc-cli --help
```

## Troubleshooting

### "MSVCP140.dll was not found"

If launching `orc-gui.exe` produces one or more system errors of the form:

```text
The code execution cannot proceed because MSVCP140.dll was not found.
Reinstalling the program may fix this problem.
```

(or the same message naming `VCRUNTIME140.dll` or `VCRUNTIME140_1.dll`), the Microsoft
Visual C++ Redistributable is missing. Install it as described in
[Prerequisite: Microsoft Visual C++ Redistributable](#prerequisite-microsoft-visual-c-redistributable),
then launch Decode-Orc again. Reinstalling Decode-Orc itself will not fix it.

## Uninstalling

To remove Decode-Orc:

1. Open **Control Panel** > **Programs** > **Programs and Features**.
2. Find **Decode-Orc** in the list.
3. Click **Uninstall** and confirm.
4. Follow the uninstall wizard.

Alternatively, you can uninstall via the MSI:

```cmd
msiexec /x "Decode-Orc-<version>-windows-x64.msi"
```
