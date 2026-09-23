# Building Decode-Orc

This guide covers building Decode-Orc from source for development and production use.

## Quick Start

### Using Nix (Recommended for Reproducible Builds)

The project supports [Nix](https://nixos.org/) for deterministic, reproducible builds with all dependencies pre-configured:

```bash
# Enter development environment
nix develop

# Build the project
nix build

# Run the application
nix run .#orc-gui
```

If you want to pass command line options when using `nix run`:

```bash
nix run git+file:///home/pathtoproject/decode-orc#orc-gui -- --log-level debug --log-file /tmp/orc-gui.log
```

### CMake with Presets (Cross-Platform)

For Linux, macOS, and Windows developers using system package managers or vcpkg:

```bash
# Configure with tests enabled (development)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNIT_TESTS=ON

# Build
cmake --build build -j

# Run tests
ctest --test-dir build --output-on-failure
```

## Detailed Build Instructions

### Prerequisites

#### Nix-Based Setup

- [Nix](https://nixos.org/) (recommended for reproducible builds)
- `flake.nix` defines all dependencies and development environment

#### CMake-Based Setup

- **CMake**: 3.20 or later
- **C++ Compiler**: C++17 support
  - Linux: GCC 9+ or Clang 9+
  - macOS: Apple Clang (Xcode 12+)
  - Windows: MSVC 2022
- **Qt6**: For GUI builds (`BUILD_GUI=ON`)
- **Dependencies**: Managed via vcpkg or system packages (see `vcpkg.json` for the dependency list)
- **EZPWD Headers**: Reed-Solomon library headers (automatically provided in Nix; for manual setups, set `EZPWD_INCLUDE_DIR` environment variable)

### Nix Development Environment

The `nix develop` shell includes all build tools and dependencies:

```bash
# Enter the development shell
nix develop

# Inside the shell, you have access to:
# - cmake, ninja (Ninja is the default generator via CMAKE_GENERATOR)
# - Qt6 (qtbase, qttools)
# - FFmpeg, spdlog, yaml-cpp, sqlite, libpng, fftw
# - Google Test (gtest) for unit testing
# - pkg-config for dependency discovery
# - Clang tools, ccache, doxygen, graphviz (on Linux/macOS)
# - mold linker (Linux; picked up automatically by CMake when present)
# - gdb/lldb debuggers

# Typical workflow:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNIT_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Manual CMake Build (Without Nix)

If you prefer to use system package managers or vcpkg manually:

#### 1. Install Dependencies

**Linux (Ubuntu/Debian):**

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build pkg-config \
  qtbase6-dev qttools6-dev \
  libspdlog-dev libfmt-dev \
  libsqlite3-dev libyaml-cpp-dev libpng-dev libfftw3-dev \
  libffmpeg-dev google-gtest-dev
```

**macOS (Homebrew):**

```bash
brew install cmake ninja qt@6 spdlog fmt sqlite yaml-cpp libpng fftw ffmpeg
```

**Windows (vcpkg):**
See `vcpkg.json` for the manifest-based dependency list and CI workflow details.

**Windows: UTF-8 code page**

`orc-gui.exe` and `orc-cli.exe` embed `cmake/windows/orc-utf8.manifest`, which
sets the process active code page to UTF-8 (Windows 10 1903+). File paths
travel through the codebase as UTF-8 `std::string`, and without this the CRT
and `std::filesystem` decode them as CP1252, so any path containing a
non-ASCII character appears to be missing. `orc_apply_utf8_manifest()` in
`cmake/WindowsUtf8Manifest.cmake` attaches it (linker manifest input under
MSVC, an `RT_MANIFEST` resource under MinGW); a new Windows executable target
should call it, and stage plugin DLLs need nothing as they inherit the code
page of the process that loads them.

**Windows (MinGW-w64/gcc):**
Building with MinGW-w64/gcc instead of MSVC needs vcpkg cloned into the repo
root and bootstrapped, and QtNodes cloned manually (see step 2 below — despite
the error message calling it a submodule, it is not one):

```powershell
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
git clone https://github.com/paceholder/nodeeditor.git external/qtnodes
```

vcpkg must be a real `git clone`, not a downloaded ZIP archive, since it
reads its own commit history to resolve dependency baselines.

`orc/core/include/plugin_safe_call.h` uses `__try`/`__except` (Structured
Exception Handling) on its Windows branch, an MSVC-only language extension
that GCC does not implement. Building with MinGW needs that branch split so
MSVC keeps SEH and GCC instead uses `AddVectoredExceptionHandler` (a plain
Win32 API) with `setjmp`/`longjmp`, mirroring the existing POSIX
implementation.

#### 2. Provide EZPWD Headers

The project requires the `ezpwd-reed-solomon` headers (header-only library):

```bash
# Clone the ezpwd repository
git clone --depth 1 https://github.com/pjkundert/ezpwd-reed-solomon external/ezpwd-reed-solomon

# Set the environment variable (or pass via CMake)
export EZPWD_INCLUDE_DIR="$PWD/external/ezpwd-reed-solomon/c++"
```

Alternatively, pass it directly to CMake:

```bash
cmake -S . -B build -DEZPWD_INCLUDE_DIR="/path/to/ezpwd-reed-solomon/c++" ...
```

#### 3. Configure with CMake

```bash
# Debug build with unit tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNIT_TESTS=ON

# Or use a preset for your platform (see CMakePresets.json):
# Linux:
cmake --preset linux-gui-debug

# macOS:
cmake --preset macos-gui-debug

# Windows:
cmake --preset windows-gui-debug

# Windows (MinGW-w64/gcc):
cmake --preset windows-mingw-gui-release
```

**Common CMake options:**

- `CMAKE_BUILD_TYPE`: `Debug` or `Release`
- `BUILD_GUI`: `ON` (default) or `OFF` (CLI only)
- `BUILD_UNIT_TESTS`: `ON` (development) or `OFF` (release)
- `BUILD_INTEGRATION_TESTS`: `OFF` (default) or `ON` to compile integration suites
- `BUILD_DOCS`: `OFF` (default) or `ON` (generate Doxygen docs)
- `ORC_NATIVE_ARCH`: `ON` (default for a local build) compiles for the CPU of
  the machine doing the build (`-march=native`), so the auto-vectoriser can use
  AVX2/AVX-512 instead of the SSE2 baseline every x86-64 binary must otherwise
  assume. `OFF` under CI (recognised by the `CI` environment variable), in every
  release preset and in the packaging manifests, because a shipped binary must
  run on any CPU. Inside `nix develop` the flake sets `NIX_ENFORCE_NO_NATIVE=0`
  for you; without that the Nix compiler wrapper silently drops the flag.
- `ORC_ENABLE_LTO`: `ON` (default for a local build) enables link-time
  optimisation for Release configurations; `OFF` under CI, in the release
  presets and in packaging. Debug builds are never affected.
- `EZPWD_INCLUDE_DIR`: Path to ezpwd headers (or set `EZPWD_INCLUDE_DIR` environment variable)

#### 5. Build

If you configured manually into `build/` (as in the `-B build` example
above):

```bash
# Build using the configured generator
cmake --build build -j
```

# Or use make/ninja directly (depending on your generator)

make -C build -j
ninja -C build

If you configured with a preset (as in step 3 above), each preset has its own
build directory (`build/<preset-name>/`), so build with the matching build
preset instead:

```bash
cmake --build --preset build-linux-gui-debug     # match whichever configure preset you used
cmake --build --preset build-windows-mingw-gui-release
```

#### 6. Run Tests

```bash
# Run all tests (unit tests + MVP architecture check)
ctest --test-dir build --output-on-failure

# Run integration tests (requires -DBUILD_INTEGRATION_TESTS=ON at configure time)
ctest --test-dir build -L integration --output-on-failure

# Run only unit tests (skip MVP check)
ctest --test-dir build -E MVPArchitectureCheck --output-on-failure

# Run only MVP architecture check
ctest --test-dir build -R MVPArchitectureCheck --output-on-failure

# Run a specific test by name
ctest --test-dir build -R "test_name_pattern" --output-on-failure
```

### Build Outputs

After a successful build, executables and libraries are placed in:

```
build/
├── bin/
│   ├── orc-gui          # GUI application (if BUILD_GUI=ON)
│   └── orc-cli          # CLI application
├── lib/
│   ├── liborc-core.*    # Shared core runtime library
│   ├── [other library files]
│   └── orc-stage-plugins/
│       └── [runtime stage plugin libraries]
├── generated/
│   └── version.h        # Generated version header
└── ... (other CMake artifacts)
```

### Development Workflow

**Recommended development workflow:**

```bash
# Enter Nix shell
nix develop

# Configure once
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNIT_TESTS=ON

# Edit code, then rebuild (fast iteration)
cmake --build build -j

# Run tests
ctest --test-dir build --output-on-failure

# Validate MVP architecture
ctest --test-dir build -R MVPArchitectureCheck

# Clean build if needed
rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNIT_TESTS=ON && cmake --build build -j
```

## Architecture Notes

The project follows this CMake structure:

- **`orc/core/`** — Core business logic library (no UI dependencies)
- **`orc/presenters/`** — Presenter layer (bridges core to UI)
- **`orc/view-types/`** — Shared data structures for presentation
- **`orc/cli/`** — CLI application

## Stage Plugin Layout

Phase 3 of the stage plugin migration introduces a shared `orc-core` runtime library plus separately built stage plugin modules.

- Build tree plugin output:
  - `build/lib/orc-stage-plugins/`
- Linux install layout:
  - `lib/orc-stage-plugins/`
- macOS app bundle layout:
  - `orc-gui.app/Contents/PlugIns/orc-stage-plugins/`
- Windows install layout:
  - `bin/orc-stage-plugins/`

At runtime, Decode-Orc now looks for stage plugins in the persistent YAML registry, `ORC_STAGE_PLUGIN_PATHS`, and the default build/install plugin directory layout.

- **`orc/gui/`** — Qt6 GUI application (optional)
- **`orc-tests/`** — Unified test tree with source-aligned subdirectories such as `core/unit/` and `gui/unit/`

MVP architecture boundaries are enforced via `ctest -R MVPArchitectureCheck`. See [TESTING.md](TESTING.md) for testing strategy details.

## CI/CD & Multi-Platform Builds

The repository includes GitHub Actions workflows that test and package for multiple platforms:

- **Linux + Nix:** Primary CI gate (`.github/workflows/build-and-test.yml`)
- **macOS:** DMG packaging (`.github/workflows/package-macos.yml`)
- **Windows:** MSI packaging (`.github/workflows/package-windows.yml`)
- **Linux Flatpak:** Flatpak bundle (`.github/workflows/package-flatpak.yml`)

See [`.github/workflows/`](.github/workflows/) for details on platform-specific build configurations and dependencies.

## Troubleshooting

### CMake not found

Ensure CMake 3.20+ is installed and in your PATH:

```bash
cmake --version
```

### Qt6 not found

Install Qt6 via your package manager or set `CMAKE_PREFIX_PATH`:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/usr/lib/cmake/Qt6
```

### EZPWD headers not found

Set the `EZPWD_INCLUDE_DIR` environment variable or pass it to CMake:

```bash
export EZPWD_INCLUDE_DIR="$PWD/external/ezpwd-reed-solomon/c++"
```

### `orc-gui.exe` starts but complains about missing DLLs (Qt6Core, libcurl, ffmpeg, ...)

This happens when running straight from the raw `build/` tree: third-party
runtime DLLs (Qt, ffmpeg, curl, fmt, spdlog, sqlite3, soxr, ...) are never
copied next to the executables or plugins during a plain `cmake --build`, on
any Windows toolchain including MSVC. The official Windows release only works
because its packaging step (see `.github/workflows/package-windows.yml`) runs
`cmake --install`, then `windeployqt` for Qt, then copies every DLL from
vcpkg's manifest install `bin/` next to `orc-gui.exe`. Reproduce those two
steps rather than running the executable directly from the build directory.
`windeployqt` handles Qt; the copy below handles everything else vcpkg
installed (adjust the build directory to whichever preset you configured):

```powershell
cmake --install build/windows-mingw-gui-release --prefix package-root
Copy-Item build/windows-mingw-gui-release/vcpkg_installed/x64-mingw-dynamic/bin/*.dll package-root/bin/ -Force
```

### Unit tests fail with exit code `0xc0000135` during a MinGW build

This is the same missing-runtime-DLL issue as above, but hit earlier: with
`BUILD_UNIT_TESTS=ON`, CMake tries to run each test executable right after
linking it (`gtest_discover_tests(... DISCOVERY_MODE POST_BUILD)`) to enumerate
its test cases, and that run fails for the same reason a bare `orc-gui.exe`
would — no `--install` step has happened yet to collect its runtime DLLs. The
`windows-mingw-gui-debug`/`-release` presets set `BUILD_UNIT_TESTS=OFF` for
this reason. If you need unit tests on MinGW, either run them against a
separate `cmake --install`'d + DLL-collected copy, or accept a slow
`cmake --install` step before every test-affecting rebuild.

### Tests failing to compile

Ensure Google Test (gtest) is available. In Nix, use `nix develop`. For manual setups, install:

```bash
# Linux
sudo apt-get install google-gtest-dev

# macOS
brew install google-benchmark
```

### Build is slow

Use ccache to speed up incremental builds:

```bash
export CMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNIT_TESTS=ON
```

The default ccache cache size (5 GiB) is too small for this project's Debug
objects and causes constant eviction; raise it once with:

```bash
ccache --max-size=30G
```

Inside `nix develop`, ccache is enabled automatically, the Ninja generator is
the default (`CMAKE_GENERATOR=Ninja`), and on Linux the mold linker is used
automatically when CMake ≥ 3.29 finds it. A build tree configured with the
old "Unix Makefiles" generator must be recreated once (`rm -rf build`) before
the Ninja default applies.

## Further Reading

- [TESTING.md](TESTING.md) — Testing strategy and unit test patterns
- [CONTRIBUTING.md](CONTRIBUTING.md) — Contribution guidelines
- [CMakePresets.json](CMakePresets.json) — Available build presets for all platforms
- [flake.nix](flake.nix) — Nix development environment and reproducible build configuration
