# Compiling from source using Nix (Linux and Mac)

The project requires [Nix](https://nixos.org/) for deterministic, reproducible builds.  You must install nix in order to build from source.

## Entering the nix development environment

Use the following command to enter a nix development environment which will provided the required dependencies:
```
nix develop
```

## Build the project using nix

Use the following command to build the project (builds orc-gui, orc-cli and integrates encode-orc as a component):

```
nix build
```

Note: encode-orc is built and integrated into orc-gui and orc-cli, not as a separate executable.

## Running the GUI application

Use the following command to run the orc-gui application:

```
nix run .#orc-gui
```

## Hardware acceleration on Linux distributions other than NixOS

A Nix build of Orc-GUI cannot reach the graphics driver your distribution
installed. Nix builds against its own C library, which deliberately ignores
`/etc/ld.so.cache` and the system library directories, and the OpenGL
dispatch library it uses looks for a driver in exactly one place outside the
Nix store: `/run/opengl-driver/lib`, which only NixOS creates. So on Ubuntu,
Fedora, Arch and the rest, the driver in `/usr/lib` is invisible to the
application, OpenGL offers no usable configuration at all, and Orc-GUI draws
the preview and scopes on the CPU. It checks for this at startup and falls
back by itself, so the application starts and works normally; only the GPU
acceleration is missing. **Help → About Orc GUI…** says which drawing path is
in use.

To get the GPU back, run the portable build, which carries its own copy of
Mesa:

```
nix run .#orc-gui-portable
```

or install it with `nix profile install .#decode-orc-portable`. It uses the
system's driver when there is one, so it is also safe to use on NixOS. Two
things to know before choosing it:

- It adds about a gigabyte to the download, most of it LLVM, which is why the
  ordinary build does not include it.
- It gives real hardware acceleration on Intel and AMD graphics, where Mesa
  talks to the kernel directly. On NVIDIA's proprietary driver it falls back
  to software rendering, because the driver's user-space half has to match the
  installed kernel module and only [nixGL](https://github.com/nix-community/nixGL)
  can supply it:

```
nix run --impure github:nix-community/nixGL -- nix run .#orc-gui
```

None of this applies to NixOS, where `/run/opengl-driver/lib` is present and
the ordinary build uses the system's driver.

A binary you built yourself inside `nix develop` has the same limitation, and
the dev shell deliberately does not carry Mesa - it would add that gigabyte to
every developer's shell. Point it at one for the current shell when you need
the GPU path:

```
export LD_LIBRARY_PATH="$(nix build --no-link --print-out-paths nixpkgs#mesa)/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

## Installing the application

To install the application persistently to your user profile:

```
nix profile install .
```

This will:
- Install both `orc-gui` and `orc-cli` to your PATH
- Install the desktop file to `~/.nix-profile/share/applications/`
- Make the application appear in your desktop environment's application menu

After installation, you can run the applications directly:

```
orc-gui
orc-cli
```

To uninstall:

```
nix profile remove decode-orc
```

### NixOS system-wide installation

On NixOS, you can install system-wide by adding to your `configuration.nix`:

```nix
environment.systemPackages = [
  (pkgs.callPackage /path/to/decode-orc {})
];
```