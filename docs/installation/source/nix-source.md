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
- On Linux, install the desktop file to `~/.nix-profile/share/applications/`
- On Linux, make the application appear in your desktop environment's
  application menu

After installation, you can run the applications directly:

```
orc-gui
orc-cli
```

To uninstall:

```
nix profile remove decode-orc
```

### macOS

On macOS the build installs as an application bundle, `orc-gui.app`, rather
than as loose binaries. `nix profile install .` puts that bundle at
`~/.nix-profile/orc-gui.app` and links both executables inside it into
`~/.nix-profile/bin/`, so `orc-gui` and `orc-cli` are on your PATH exactly as
they are on Linux. There is no desktop file on macOS, so nothing is added to
the application menu.

To launch the GUI as a normal macOS application:

```bash
open ~/.nix-profile/orc-gui.app
```

#### Showing it in Spotlight and Launchpad

Run this once after installing:

```bash
nix run .#install-macos-app
```

That copies the bundle to `~/Applications/orc-gui.app`, makes it writable, and
registers it with Launch Services, after which it appears in Spotlight and
Launchpad and `open -a orc-gui` works. Spotlight lists it under the bundle's
filename, so search for `orc-gui` or `orc` rather than "Decode Orc".

Re-run the same command after `nix profile upgrade` to refresh the copy, and
after `nix-collect-garbage`, which can remove store paths an old copy still
points at. It is safe to run repeatedly. The `orc-gui` and `orc-cli` on your
PATH always come from the profile itself and are unaffected either way.

##### Why this is a separate step

`nix profile install` cannot do it, and neither can any change to `flake.nix`.
Installing a profile only builds a tree of symlinks inside the Nix store and
points `~/.nix-profile` at it; it never runs anything on your machine and never
writes outside the profile. The store lives on `/nix`, a separate APFS volume
mounted `nobrowse` with indexing switched off, so Spotlight never sees the
installed bundle. Nor do the obvious workarounds: Spotlight skips symlinks
entirely, and a Finder alias is indexed as an alias file rather than as an
application. Only a real bundle directory on an indexed volume is registered as
an app, so something has to copy it out of the store — which is what the
command above does.

To do it by hand instead:

```bash
mkdir -p ~/Applications
rm -rf ~/Applications/orc-gui.app
cp -RL ~/.nix-profile/orc-gui.app ~/Applications/orc-gui.app
chmod -R u+w ~/Applications/orc-gui.app
```

Both flags matter. `-L` dereferences the link: `~/.nix-profile/orc-gui.app` is
itself a symlink into the store, so a plain `cp -R` copies the link instead of
the bundle and you are no better off. `chmod -R u+w` makes the result writable;
everything copied out of the Nix store is read-only, and without it you cannot
replace or delete the copy afterwards.

### NixOS system-wide installation

On NixOS, you can install system-wide by adding to your `configuration.nix`:

```nix
environment.systemPackages = [
  (pkgs.callPackage /path/to/decode-orc {})
];
```