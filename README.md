# Decode Orc

**Decode-Orc** is a cross-platform orchestration and processing framework for LaserDisc and tape decoding workflows.

![](assets/decode-orc_logotype-1024x286.png)

It aims to brings structure and consistency to complex decoding processes, making them easier to run, repeat, and understand.

> [!IMPORTANT]
> [Click here for the documentation](https://decode-orc.github.io/decode-orc/)

`Decode-Orc` is a direct replacement for the existing ld-decode-tools, coordinating each step of the process and keeping track of inputs, outputs, and results.

The project aims to:
- Make advanced LaserDisc and tape workflows (from TBC to end-user video) easier to manage
- Reduce manual steps and error-prone command sequences
- Help users reproduce the same results over time

Both a graphical interface (orc-gui) and command-line interface (orc-cli) are implemented for orchestrating workflows.  These commands contain minimal business logic and, instead, rely on the same orc-core following a MVP architecture (Model–View–Presenter) wherever possible.

# Documentation

## User documentation

The user documentation is available via [GitHub Pages](https://decode-orc.github.io/decode-orc/)

# Installation

The Decode-Orc project provides ready-to-install packages for Mac OS (DMG).

The release packages can be found in the [release section](https://github.com/decode-orc/decode-orc/releases) of the Github repository.

For instructions on how to install please see the documentation for your preferred platform:

- [Linux Flatpak installation](https://decode-orc.github.io/decode-orc/installation/linux-flatpak/)
- [MacOS DMG installation](https://decode-orc.github.io/decode-orc/installation/macos-dmg/)
- [Windows MSI installation](https://decode-orc.github.io/decode-orc/installation/windows-msi/)

## Building from Source

For comprehensive build instructions covering Nix, CMake, and platform-specific setup, please see [BUILD.md](BUILD.md).

**Quick reference:** Use `nix develop` for reproducible builds with all dependencies pre-configured, or follow the CMake/vcpkg instructions for system package manager setups.

## Plugin Development

Third-party plugins are external repositories and should follow the naming
convention `orc-plugin_<stage-name>`. They build against the standalone Plugin
SDK package alone — no decode-orc source tree or host dependency stack is
required.

Canonical starter template repository:

- `https://github.com/decode-orc/orc-plugin_skeleton`

Decode-Orc host integration consumes pre-compiled plugin artifacts from release
assets (or a local pre-compiled path for development/testing).

Documentation:

- [Plugin Author Guide](docs/technical/plugin-author-guide.md) — end-to-end
  walkthrough from an empty directory to a plugin loaded in the host.
- [Plugin Publishing Guide](docs/technical/plugin-publishing.md) — release-asset
  naming, digests, ABI-rebuild expectations, and submitting a curated-index entry.
- [Plugin SDK Developer Guide](docs/technical/plugin-sdk.md) — tiered header
  reference, CMake integration, and the versioning/ABI policy.
- [Plugin Architecture](docs/technical/plugin-architecture.md) — runtime flow,
  compatibility gating, registry, and the curated index.

# Using Agentic Coding AI

This repository includes repository-wide instructions for agentic coding AI tools in [AGENTS.md](AGENTS.md). These instructions are **essential** for AI agents to understand the project architecture, build process, testing strategy, and CI/CD requirements. All AI agents must read and follow them before performing any task.

Key guidance includes:
- **MVP Architecture:** Core, presenters, view-types, and UI layers must remain decoupled. Run `ctest -R MVPArchitectureCheck` before submitting PRs.
- **Build & Test:** Use `nix develop` for reproducible setup, then `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNIT_TESTS=ON` and `ctest --test-dir build --output-on-failure`.
- **C++ Style:** Follow the Google C++ Style Guide; use `clang-format` for formatting and `clang-tidy` for static analysis (both available in the Nix environment).
- **Testing Philosophy:** Unit test coverage is the primary methodology; mock all dependencies; no filesystem/network/clock access in unit tests.
- **Multi-OS CI/CD:** Changes must align with Linux (Nix), macOS (DMG packaging), Windows (MSI packaging), and Flatpak workflows.

# Crash Reporting

If the application crashes unexpectedly, it will automatically create a diagnostic bundle to help identify and fix the issue. This bundle contains:

- System information (OS, CPU, memory)
- Stack backtrace showing where the crash occurred
- Application logs
- Coredump file (when available)

The crash bundle is saved as a ZIP file in:
- **orc-gui**: `~/Documents/decode-orc-crashes/` (or your home directory)
- **orc-cli**: Current working directory

When reporting a crash issue on [GitHub Issues](https://github.com/decode-orc/decode-orc/issues), please:
1. Attach the crash bundle ZIP file (or upload to a file sharing service if too large)
2. Describe what you were doing when the crash occurred
3. Include the contents of `crash_info.txt` from the bundle in your issue description

This information is invaluable for diagnosing and fixing crashes quickly.

# Credits

Decode-Orc was designed and written by Simon Inns.  Decode-Orc's development heavily relied on the original GPLv3 ld-decode-tools which contained many contributions from others.

- Simon Inns (2018-2025) - Extensive work across all tools
- Adam Sampson (2019-2023) - Significant contributions to core libraries, chroma decoder and tools
- Chad Page (2014-2018) - Filter implementations and original NTSC comb filter
- Ryan Holtz (2022) - Metadata handling
- Phillip Blucas (2023) - VideoID decoding
- ...and others (see the original ld-decode-tools source)

It should be noted that the original code for the observers is also based heavily on the ld-decode python code-base (written by Chad Page et al).
