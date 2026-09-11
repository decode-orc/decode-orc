{
  description = "Decode-Orc - LaserDisc and tape decoding orchestration framework";

  # Upstream dependencies for the flake
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";
    qtnodes = {
      # Pinned commit; keep in sync with io.github.decode_orc.decode-orc.yml
      # and the QtNodes fetch steps in .github/workflows/package-{macos,windows}.yml.
      url = "github:paceholder/nodeeditor/1b173f885b52e4fd9616f663ea288435ccf1d0d8";
      flake = false;
    };
    ezpwd = {
      url = "github:pjkundert/ezpwd-reed-solomon/62a490c13f6e057fbf2dc6777fde234c7a19098e";
      flake = false;
    };
  };

  # Build outputs for each supported system
  outputs = { self, nixpkgs, flake-utils, qtnodes, ezpwd }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        # Import Nixpkgs for this system
        pkgs = import nixpkgs {
          inherit system;
          config = {
            allowUnfree = true; # In case any dependencies require it
          };
        };

        # Helper to get version from git or use a default
        # For clean commits in CI/releases: include branch + short rev
        # For local dirty builds: use a fixed fallback
        branch =
          if (self ? sourceInfo && self.sourceInfo ? ref)
          then self.sourceInfo.ref
          else "detached";

        commit = if (self ? shortRev) then self.shortRev else null;

        rawVersion =
          if (commit != null)
          then "${branch}-${commit}"
          else "0.0.0-dirty";

        version = builtins.replaceStrings ["\n" "/" " "] ["" "-" "-"] rawVersion;

        # Use the nixpkgs default stdenv. Legacy SDK override patterns
        # (`overrideSDK`, `apple_sdk_11_0`) were removed in nixpkgs.
        stdenv = pkgs.stdenv;

        # Filtered ezpwd headers-only derivation (avoids VERSION file collisions)
        ezpwd-headers = pkgs.runCommand "ezpwd-headers" {} ''
          mkdir -p $out
          cp -r ${ezpwd}/c++ $out/ 2>/dev/null || true
          if [ -d "$out/c++/ezpwd" ]; then
            ln -s c++/ezpwd $out/ezpwd
          fi
        '';

        # Build QtNodes as a separate package (no external package needed)
        qtNodes = stdenv.mkDerivation {
          pname = "qtnodes";
          version = "3.0.0";

          src = qtnodes;

          strictDeps = true;

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            qt6.wrapQtAppsHook
          ];

          buildInputs = with pkgs; [
            qt6.qtbase
          ];

          cmakeFlags = [
            "-GNinja"
            "-DUSE_QT6=ON"
            "-DBUILD_TESTING=OFF"
            "-DBUILD_EXAMPLES=OFF"
            "-DBUILD_SHARED_LIBS=OFF"
          ];

          meta = with pkgs.lib; {
            description = "Qt-based library for node graph editing";
            homepage = "https://github.com/paceholder/nodeeditor";
            license = licenses.bsd3;
          };
        };

        # Python environment for MkDocs documentation tooling
        mkdocsPythonEnv = pkgs.python312.withPackages (ps: [
          ps.mkdocs
          ps.mkdocs-material
          ps."mkdocs-awesome-nav"
        ]);

        # Qt Multimedia dlopens libpipewire-0.3.so.0 at runtime (PipeWire capture
        # support) but nixpkgs' qtmultimedia carries no RPATH entry for it, so
        # anything linking Qt6::Multimedia logs
        #   qt.multimedia.symbolsresolver: Couldn't load pipewire-0.3 library
        # unless PipeWire is on the library search path. Used on Linux only.
        pipewireLibPath = "${pkgs.pipewire}/lib";

        # Build the decode-orc package (primary output).
        mkDecodeOrc = {}: stdenv.mkDerivation {
          pname = "decode-orc";
          version = version;

          src = builtins.path {
            path = ./.;
            name = "decode-orc-docs-src";
          };

          strictDeps = true;

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            qt6.wrapQtAppsHook
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            wrapGAppsHook3
            autoPatchelfHook
          ];

          # Wrap Qt binaries and include gapps runtime settings on Linux.
          dontWrapGApps = pkgs.stdenv.isLinux;
          
          # On macOS, the .app bundle is already properly structured,
          # so we skip the automatic Qt wrapping which causes issues
          dontWrapQtApps = pkgs.stdenv.isDarwin;

          qtWrapperArgs =
            pkgs.lib.optionals pkgs.stdenv.isDarwin [
              "--set"
              "QT_QPA_PLATFORM"
              "cocoa"
            ]
            ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
              # Do not inherit potentially incompatible user-profile GIO modules.
              "--set"
              "GIO_EXTRA_MODULES"
              "${pkgs.glib-networking}/lib/gio/modules"
              # Let Qt Multimedia resolve libpipewire-0.3 (see pipewireLibPath).
              "--prefix"
              "LD_LIBRARY_PATH"
              ":"
              pipewireLibPath
            ];

          preFixup = pkgs.lib.optionalString pkgs.stdenv.isLinux ''
            qtWrapperArgs+=("''${gappsWrapperArgs[@]}")
          '';

          buildInputs = with pkgs; [
            # Core dependencies from vcpkg.json
            spdlog
            sqlite
            yaml-cpp
            libpng
            fftw

            # FFmpeg components
            ffmpeg

            # High-quality sinc resampling for PAL CVBS fractional sample normalization
            soxr

            # HTTP client for downloading remote plugins
            curl

            # Qt6 for GUI
            qt6.qtbase
            qt6.qttools

            # Audio output device for preview playback (QAudioSink)
            qt6.qtmultimedia

            # qsb, which compiles the GPU render surfaces' GLSL sources into
            # the .qsb bundles Qt's RHI loads at run time (ORC_GUI_GPU_RENDER).
            qt6.qtshadertools

            # QtNodes built from flake input
            qtNodes

            # Automated testing
            gtest

          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            # Provide GTK schemas/settings used by Qt's native integration paths.
            gtk3
            gsettings-desktop-schemas
            glib-networking
          ];

          cmakeFlags = [
            "-GNinja"
            "-DCMAKE_BUILD_TYPE=Release"
            "-DBUILD_UNIT_TESTS=OFF"
            "-DBUILD_GUI=ON"
            "-DBUILD_DOCS=OFF"
            "-DBUILD_ENCODE_ORC=OFF"
            # Tell CMake where to find QtNodes
            "-DQtNodes_DIR=${qtNodes}/lib/cmake/QtNodes"
            # Tell CMake where to find the ezpwd Reed-Solomon headers
            "-DEZPWD_INCLUDE_DIR=${ezpwd-headers}"
            # Pass git version to CMake since .git dir isn't available in Nix builds
            "-DPROJECT_VERSION_OVERRIDE=${version}"
            # Define NODE_EDITOR_STATIC to match QtNodes static build
            "-DCMAKE_CXX_FLAGS=-DNODE_EDITOR_STATIC"
          ];

          # Patch scripts for Nix sandbox compatibility
          postPatch = ''
            # Patch shell script shebangs to use Nix bash
            patchShebangs cmake/check_mvp_architecture.sh
            patchShebangs encode-tests.sh || true
          '';

          # Make ffprobe available during tests
          preCheck = ''
            export PATH=${pkgs.ffmpeg}/bin:$PATH
          '';

          doInstallCheck = true;

          installCheckPhase = ''
            ctest --output-on-failure -R MVPArchitectureCheck
          '';

          # Fix RPATH for standalone execution (nix profile, etc.)
          postFixup = 
            pkgs.lib.optionalString pkgs.stdenv.isLinux ''
              for binary in $out/bin/orc-*; do
                if [ -f "$binary" ]; then
                  ${pkgs.patchelf}/bin/patchelf \
                    --set-rpath "$out/lib:$out/lib/orc-stage-plugins:${pkgs.lib.makeLibraryPath [
                      pkgs.stdenv.cc.cc
                      pkgs.qt6.qtbase
                      pkgs.qt6.qtmultimedia
                      pkgs.curl
                      pkgs.ffmpeg
                      pkgs.soxr
                      pkgs.fftw
                      pkgs.yaml-cpp
                      pkgs.sqlite
                      pkgs.spdlog
                      pkgs.libpng
                    ]}" \
                    --set-interpreter "$(cat $NIX_CC/nix-support/dynamic-linker)" \
                    "$binary" || true
                fi
              done
            ''
            + pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
              # On macOS, the .app bundle has:
              #   - Libraries in: orc-gui.app/Contents/Frameworks/
              #   - Plugins in: orc-gui.app/Contents/PlugIns/orc-stage-plugins/
              #   - Executable in: orc-gui.app/Contents/MacOS/orc-gui
              #
              # We need to rewrite all references to use @loader_path for relocatability.
              # From the executable (@loader_path is MacOS/):
              #   - Frameworks are at: ../Frameworks/
              #   - Plugins are at: ../PlugIns/orc-stage-plugins/
              # From a Framework dylib (@loader_path is Frameworks/):
              #   - Other frameworks are at: ./
              #   - Plugins are at: ../PlugIns/orc-stage-plugins/
              
              app_path="$out/orc-gui.app"
              main_exe="$app_path/Contents/MacOS/orc-gui"
              frameworks_dir="$app_path/Contents/Frameworks"
              plugins_dir="$app_path/Contents/PlugIns/orc-stage-plugins"
              
              # Helper function to extract store paths from otool output
              get_store_dylibs() {
                local binary="$1"
                otool -L "$binary" 2>/dev/null | grep -v ":" | grep -v "^$" | grep -v "^[[:space:]]*$" | awk '{print $1}' | grep "\.dylib$"
              }
              
              # Rewrite dylibs in Frameworks directory
              if [ -d "$frameworks_dir" ]; then
                find "$frameworks_dir" -name "*.dylib" -type f | while read dylib; do
                  # Rewrite all .dylib references in this framework
                  get_store_dylibs "$dylib" | while read lib; do
                    libname=$(basename "$lib")
                    if [ -f "$frameworks_dir/$libname" ]; then
                      # Reference to another framework: use @loader_path
                      install_name_tool -change "$lib" "@loader_path/$libname" "$dylib" 2>/dev/null || true
                    fi
                    # Check if it's a plugin reference
                    if [ -f "$plugins_dir/$libname" ]; then
                      # Reference to a plugin: use path relative to Frameworks
                      install_name_tool -change "$lib" "@loader_path/../PlugIns/orc-stage-plugins/$libname" "$dylib" 2>/dev/null || true
                    fi
                  done
                done
              fi
              
              # Rewrite dylibs in plugins directory
              if [ -d "$plugins_dir" ]; then
                find "$plugins_dir" -name "*.dylib" -type f | while read dylib; do
                  # Plugins might reference frameworks and core library
                  get_store_dylibs "$dylib" | while read lib; do
                    libname=$(basename "$lib")
                    if [ -f "$frameworks_dir/$libname" ]; then
                      # Reference to a framework: use path relative to plugin location
                      install_name_tool -change "$lib" "@loader_path/../../Frameworks/$libname" "$dylib" 2>/dev/null || true
                    fi
                    # Plugin referencing another plugin (unlikely but handle it)
                    if [ -f "$plugins_dir/$libname" ]; then
                      install_name_tool -change "$lib" "@loader_path/$libname" "$dylib" 2>/dev/null || true
                    fi
                  done
                done
              fi
              
              # Rewrite the main executable
              if [ -f "$main_exe" ]; then
                get_store_dylibs "$main_exe" | while read lib; do
                  libname=$(basename "$lib")
                  if [ -f "$frameworks_dir/$libname" ]; then
                    # Reference to a framework
                    install_name_tool -change "$lib" "@loader_path/../Frameworks/$libname" "$main_exe" 2>/dev/null || true
                  fi
                  if [ -f "$plugins_dir/$libname" ]; then
                    # Reference to a plugin (shouldn't happen but handle it)
                    install_name_tool -change "$lib" "@loader_path/../PlugIns/orc-stage-plugins/$libname" "$main_exe" 2>/dev/null || true
                  fi
                done
              fi
            '';

          meta = with pkgs.lib; {
            description = "Decode-Orc - LaserDisc and tape decoding orchestration framework";
            homepage = "https://github.com/decode-orc/decode-orc";
            license = licenses.gpl3Plus;
            platforms = platforms.linux ++ platforms.darwin;
            maintainers = [ ];
          };
        };

        # Full build with ONNX Runtime (default, for local development).
        decode-orc = mkDecodeOrc {};

        # Build MkDocs documentation as a separate flake package
        decode-orc-docs = pkgs.stdenv.mkDerivation {
          pname = "decode-orc-docs";
          version = version;

          src = self;

          nativeBuildInputs = [
            mkdocsPythonEnv
          ];

          buildPhase = ''
            mkdocs build
          '';

          installPhase = ''
            mkdir -p $out
            cp -r site/* $out/
          '';

          meta = with pkgs.lib; {
            description = "Decode-Orc documentation site";
            homepage = "https://github.com/decode-orc/decode-orc";
            license = licenses.gpl3Plus;
            platforms = platforms.all;
            maintainers = [ ];
          };
        };

      in
      {
        # Packages that can be built with `nix build`
        packages = {
          default = decode-orc;
          decode-orc = decode-orc;
          docs = decode-orc-docs;
        };

        # Apps that can be run with `nix run`
        apps = {
          default = {
            type = "app";
            program = "${decode-orc}/bin/orc-gui";
          };
          orc-gui = {
            type = "app";
            program = "${decode-orc}/bin/orc-gui";
          };
          orc-cli = {
            type = "app";
            program = "${decode-orc}/bin/orc-cli";
          };
        };

        # Development shell with all dependencies for `nix develop`
        devShells.default = pkgs.mkShell.override { inherit stdenv; } {
          inputsFrom = [ decode-orc ];

          packages = with pkgs; [
            # Additional development tools
            cmake-format
            clang-tools
            ccache
            doxygen
            graphviz
            mkdocsPythonEnv
            ninja
            zip

            # Version control
            git
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            gdb
            valgrind
            perf
            hotspot
            heaptrack
            mold
          ] ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [
            lldb
          ];

          shellHook = ''
            echo "decode-orc nix development environment"
            echo ""
            echo "CMake version: $(cmake --version | head -n1)"
            echo "Qt version: ${pkgs.qt6.qtbase.version}"
            echo ""

            # Set up ccache if available
            export CMAKE_CXX_COMPILER_LAUNCHER=ccache

            # Expose ezpwd headers for manual cmake runs inside nix develop
            export EZPWD_INCLUDE_DIR=${ezpwd-headers}

            # Build CMAKE_PREFIX_PATH from all build inputs so that IDEs (e.g. CLion)
            # launched from this shell can run cmake without extra configuration.
            export CMAKE_PREFIX_PATH="$(echo $buildInputs $nativeBuildInputs | tr ' ' '\n' | tr '\n' ':')"
            ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
            # Qt Multimedia dlopens libpipewire-0.3 (see pipewireLibPath).
            export LD_LIBRARY_PATH="${pipewireLibPath}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
            ''}

            # Ensure build directory exists
            mkdir -p build
          '';

          # Environment variables
          CMAKE_EXPORT_COMPILE_COMMANDS = 1;
          # Default to Ninja when no -G is given (existing build trees keep
          # their configured generator; a Makefiles tree must be recreated).
          CMAKE_GENERATOR = "Ninja";
          QT_QPA_PLATFORM = pkgs.lib.optionalString pkgs.stdenv.isLinux "xcb"; # For Linux
        };

        # CI shell for streamlined workflow tools.
        devShells.ci = pkgs.mkShell.override { inherit stdenv; } {
          inputsFrom = [ decode-orc ];

          packages = with pkgs; [
            cmake-format
            clang-tools
            # Object cache for the CI build; the workflow persists $CCACHE_DIR
            # between runs and sets CMAKE_CXX_COMPILER_LAUNCHER.
            ccache
            ninja
            zip
            git
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            # The root CMakeLists sets CMAKE_LINKER_TYPE=MOLD when it finds mold
            # on PATH; without it here CI linked the debug test binaries with
            # BFD ld. Matches the default dev shell.
            mold
          ];

          shellHook = ''
            export EZPWD_INCLUDE_DIR=${ezpwd-headers}
            export CMAKE_PREFIX_PATH="$(echo $buildInputs $nativeBuildInputs | tr ' ' '\n' | tr '\n' ':')"
            ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
            # Qt Multimedia dlopens libpipewire-0.3 (see pipewireLibPath).
            export LD_LIBRARY_PATH="${pipewireLibPath}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
            ''}
            mkdir -p build
          '';

          CMAKE_EXPORT_COMPILE_COMMANDS = 1;
          # Without this CI configured with the Unix Makefiles generator, whose
          # per-directory recursion also gives every translation unit a
          # different working directory -- which ccache folds into its hash when
          # debug info is on, so the two generators cannot share a cache.
          # Matches the default dev shell.
          CMAKE_GENERATOR = "Ninja";
          QT_QPA_PLATFORM = pkgs.lib.optionalString pkgs.stdenv.isLinux "xcb";
        };

      }
    );
}
