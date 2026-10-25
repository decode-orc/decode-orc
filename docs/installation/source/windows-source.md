# Compiling from source using cmake (Windows)

The project requires [Nix](https://nixos.org/) for deterministic, reproducible builds.  Since Nix is not supported on Windows it is recommended to only build on Windows in order to test specific Windows functionality.  Any Windows-specific code will be built by Nix and tested during CI/CD so it is important to retest using Nix after making any changes under Windows.

## Required Environment

Install the following tools:

- **Visual Studio 2022 Build Tools** (C++ workload)
  - MSVC v143 toolset
  - Windows 10/11 SDK
- **CMake** (3.20+)
- **Git for Windows** (provides `git` and `bash`)
- **Qt 6** (6.7 or newer; the release builds use Qt 6.9) for MSVC 2022 64-bit,
  from the Qt online installer, including the **Qt Multimedia** and
  **Qt Shader Tools** add-on modules. Without Qt Multimedia the preview's audio
  playback is left out of the build, and without Qt Shader Tools the GPU render
  surfaces are; CMake warns and carries on in both cases.
- **ImageMagick** (`magick`) for Windows icon generation (recommended)

Optional but useful:

- **Ninja** (the `windows-gui-release-ninja` preset uses it; the other MSVC
  presets use the Visual Studio generator)
- **PowerShell 7**
- **7-Zip**

## Required Source Dependencies

Three dependencies are cloned by hand into the repository. None of them is a
git submodule (the repository's only submodules are the specification
documents under `docs-tech/`).

From repo root:

```powershell
# vcpkg: the presets expect it at .\vcpkg. It must be a real git clone, not a
# ZIP download, because it reads its own history to resolve the baseline.
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat

# QtNodes (node editor) - expected at external\qtnodes
git clone https://github.com/paceholder/nodeeditor.git external/qtnodes

# ezpwd-reed-solomon headers (header-only)
git clone --depth 1 https://github.com/pjkundert/ezpwd-reed-solomon external/ezpwd-reed-solomon
```

The CI packaging workflow pins QtNodes and ezpwd to the same commits as
`flake.nix`; see `.github/workflows/package-windows.yml` if you need to match a
release build exactly.

## vcpkg Packages

The remaining C/C++ dependencies (spdlog, yaml-cpp, sqlite3, libpng, fftw3,
pkgconf, FFmpeg, soxr, gtest and curl) are listed in `vcpkg.json` and installed
automatically in manifest mode the first time you configure with one of the
Windows presets, which point `CMAKE_TOOLCHAIN_FILE` at
`vcpkg\scripts\buildsystems\vcpkg.cmake`. The first configure therefore takes
a long time (FFmpeg is built from source). Qt is not taken from vcpkg.

Tell CMake where Qt and the ezpwd headers are, either as environment variables
before configuring:

```powershell
$env:CMAKE_PREFIX_PATH = "C:\Qt\6.9.3\msvc2022_64"   # your Qt install
$env:EZPWD_INCLUDE_DIR = "$PWD\external\ezpwd-reed-solomon\c++"
```

or by adding `-DCMAKE_PREFIX_PATH=...` and `-DEZPWD_INCLUDE_DIR=...` to the
configure commands below.

## Build Using CMake Presets (Recommended)

This repository provides Windows presets in CMakePresets.json.

### Configure + build GUI (Debug)

From repo root

```powershell
cmake --preset windows-gui-debug
cmake --build --preset build-windows-gui-debug
```

To run the resulting executable:

```
.\build\windows-gui-debug\bin\Debug\orc-gui.exe  --log-level debug --log-file /tmp/orc-gui.log
```

### Configure + build CLI-only (Debug)

```powershell
cmake --preset windows-cli-debug
cmake --build --preset build-windows-cli-debug
```

### Release builds

```powershell
cmake --preset windows-gui-release
cmake --build --preset build-windows-gui-release

cmake --preset windows-cli-release
cmake --build --preset build-windows-cli-release
```

To run the resulting executable:

```
.\build\windows-gui-release\bin\Release\orc-gui.exe  --log-level debug --log-file /tmp/orc-gui.log
```

The Windows MSI GitHub Action uses the Ninja variant of this preset,
`windows-gui-release-ninja` + `build-windows-gui-release-ninja`, run from a
Visual Studio developer shell so it builds with whichever MSVC that shell
provides. To match it locally, open a "Developer PowerShell for VS 2022" and
run:

```powershell
cmake --preset windows-gui-release-ninja
cmake --build --preset build-windows-gui-release-ninja
```

Both release presets keep `BUILD_GUI=ON`, which guarantees `orc-gui.exe` is
built.

A build tree is not runnable on its own: the Qt, FFmpeg and other third-party
DLLs are not copied next to the executables by `cmake --build`. Install it
(`cmake --install`), run `windeployqt` on the installed `orc-gui.exe`, and copy
the DLLs from the preset's `vcpkg_installed\x64-windows\bin` beside it, as the
packaging workflow does. `BUILD.md` (Troubleshooting) has the details.

### Run tests

```powershell
ctest --preset test-windows-gui-debug
# or
ctest --preset test-windows-cli-debug
```

## Manual Build Commands (Without Presets)

If needed, configure explicitly:

```powershell
cmake -S . -B build-vs-gui -G "Visual Studio 17 2022" -A x64 -DBUILD_GUI=ON -DBASH_EXECUTABLE="C:/Program Files/Git/bin/bash.exe" -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DEZPWD_INCLUDE_DIR="$PWD/external/ezpwd-reed-solomon/c++"
cmake --build build-vs-gui --config Debug -- /m
```

CLI-only:

```powershell
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64 -DBUILD_GUI=OFF -DBASH_EXECUTABLE="C:/Program Files/Git/bin/bash.exe" -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DEZPWD_INCLUDE_DIR="$PWD/external/ezpwd-reed-solomon/c++"
cmake --build build-vs --config Debug -- /m
```

## Output Paths

- GUI executable: `build/windows-gui-debug/bin/Debug/orc-gui.exe`
- CLI executable: `build/windows-cli-debug/bin/Debug/orc-cli.exe`
