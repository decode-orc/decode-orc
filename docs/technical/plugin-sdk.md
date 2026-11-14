# Plugin SDK Developer Guide

This guide covers everything needed to create, build, test, and distribute a
third-party Decode-Orc stage plugin using the public Plugin SDK. No changes to
the Decode-Orc source repository are required.

## Quick Start

First install the SDK package from a decode-orc checkout (see
[Obtaining the SDK](#obtaining-the-sdk) below), then build a plugin against
it. The smallest complete, in-repo example is the CI fixture plugin at
`orc-tests/fixtures/external-stage-plugin/` — three files (CMakeLists.txt,
plugin.cpp, one stage header) that configure, build, and load exactly as an
external plugin:

```bash
cmake -S my-orc-plugin -B my-orc-plugin/build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=/path/to/sdk-prefix
cmake --build my-orc-plugin/build -j
```

Alternatively, clone the official skeleton template for a scaffold with unit
tests and CI workflows (note: the skeleton repository may lag the SDK — the
in-repo fixture above is always in sync with the installed package):

```bash
git clone https://github.com/decode-orc/orc-plugin_skeleton my-orc-plugin
```

## SDK Headers

Most plugin code needs only the public SDK umbrella header:

```cpp
#include <orc/abi/orc_plugin_sdk.h>
```

(The pre-tier spelling `<orc/plugin/orc_plugin_sdk.h>` still works via a
deprecated forwarding shim — see *Deprecated pre-tier include paths* below.)

### Tier model

The SDK surface is split into tiers with distinct stability promises:

- **`orc/abi/`** — the frozen binary contract: descriptor, entrypoints,
  registration, and service tables. Any change here bumps the host ABI
  version. (The `orc/plugin/` plugin-API headers are treated as part of this
  ABI surface until they are relocated under a tier.)
- **`orc/stage/`** — the stage contract: interfaces and data types that cross
  the plugin boundary, grouped by **domain** (`preview/`, `observation/`,
  `dropout/`, `audio/`, `params/`, `tooling/`) with the foundation types at the
  tier root.
  A layout change here bumps the host ABI.
- **`orc/support/`** — compiled-into-plugin utilities. Explicitly **not** ABI:
  changes never require a bump, only a plugin recompile at the author's
  leisure.
- **`orc/view/`** — the contract between `orc-gui` and a plugin's
  [view binary](#view-plugins). It has its own version,
  `kViewPluginAbiVersion`, and is never included by a stage binary.
- **`orc/view-support/`** — Qt widgets `orc-gui` ships in the
  `orc-view-support` shared library for view binaries to link. Versioned
  with the view tier; the only tier that includes Qt.

The complete tiered header surface is generated from the single-source
manifest `orc/sdk/sdk_headers.yaml`:

<!-- BEGIN GENERATED SDK HEADER TABLES (source: orc/sdk/sdk_headers.yaml; regenerate with tools/gen_sdk_header_docs.sh) -->

#### `orc/abi/` — frozen binary contract

Stability: **any change bumps the host ABI version.** Descriptor,
entrypoints, registration, and service tables.

| Header | Provides |
|--------|----------|
| `<orc/abi/orc_plugin_abi.h>` | Host ABI contract for decode-orc stage plugins |
| `<orc/abi/orc_plugin_registration.h>` | Descriptor and entrypoint boilerplate helpers for stage plugins |
| `<orc/abi/orc_plugin_sdk.h>` | Umbrella include for the decode-orc stage plugin SDK |
| `<orc/abi/orc_plugin_services.h>` | Host service table injected into plugins at registration time. |

#### `orc/plugin/` — plugin API surface (transitional)

Plugin-facing stage API and host-services headers not yet relocated under
a tier subdirectory. Treated as ABI: layout changes bump the host ABI.

| Header | Provides |
|--------|----------|
| `<orc/plugin/orc_plugin_services_helpers.h>` | Helper macros for using OrcPluginServices. |
| `<orc/plugin/orc_stage_api.h>` | Stage API — stable types and interfaces for stage plugin |
| `<orc/plugin/orc_stage_preview.h>` | Public preview capability include surface for plugin stage |
| `<orc/plugin/orc_stage_runtime.h>` | Public stage runtime include surface for plugin stage |
| `<orc/plugin/orc_stage_services.h>` | Stable stage service interfaces exposed via OrcPluginServices |
| `<orc/plugin/orc_stage_tooling.h>` | Canonical stage helper/tooling contracts for plugin stages |

#### `orc/stage/` — stage contract

Stage interfaces and data-contract types that cross the plugin boundary,
grouped by domain. A layout change here bumps the host ABI version.

**foundation**

| Header | Provides |
|--------|----------|
| `<orc/stage/artifact.h>` | Artifact implementation |
| `<orc/stage/common_types.h>` | Common type definitions shared across all layers |
| `<orc/stage/cvbs_signal_constants.h>` | Normative CVBS_U10_4FSC signal constants for PAL, NTSC, and |
| `<orc/stage/error_types.h>` | Shared exception types for error classification |
| `<orc/stage/field_id.h>` | Field identifier implementation |
| `<orc/stage/file_io_interface.h>` | Interface(s) for file I/O to make unit testing easier |
| `<orc/stage/frame_descriptor.h>` | Per-frame metadata descriptor for CVBS_U10_4FSC frames |
| `<orc/stage/frame_id.h>` | Frame identifier types for CVBS_U10_4FSC frame-based pipeline |
| `<orc/stage/node_id.h>` | NodeID type definition for DAG nodes |
| `<orc/stage/node_type.h>` | Node type registry |
| `<orc/stage/orc_source_parameters.h>` | Source metadata types |
| `<orc/stage/stage.h>` | Base interface for all stage types |
| `<orc/stage/triggerable_stage.h>` | Triggerable interface for stages that can be manually executed |
| `<orc/stage/video_frame_representation.h>` | VideoFrameRepresentation interface for CVBS_U10_4FSC frames |
| `<orc/stage/video_metadata_types.h>` | Video metadata types exposed through VFR interface |

**audio**

| Header | Provides |
|--------|----------|
| `<orc/stage/audio/audio_channel_pair.h>` | Audio channel-pair model shared by all |
| `<orc/stage/audio/audio_sample_feed.h>` | Pure conversions from the 24-bit-in-int32 pipeline audio |

**dropout**

| Header | Provides |
|--------|----------|
| `<orc/stage/dropout/dropout_decision.h>` | Dropout decision management |
| `<orc/stage/dropout/dropout_run.h>` | Frame-flat dropout descriptor for CVBS_U10_4FSC frames |

**observation**

| Header | Provides |
|--------|----------|
| `<orc/stage/observation/colour_frame_phase_query.h>` | Measure a frame's colour-sequence phase via the colour_frame_phase observer |
| `<orc/stage/observation/observation_context.h>` | Pipeline-scoped observation storage |
| `<orc/stage/observation/observation_context_interface.h>` | Pipeline-scoped observation storage |
| `<orc/stage/observation/observation_schema.h>` | Observation schema definitions |
| `<orc/stage/observation/observation_service_interface.h>` | Host-owned observation service reached via OrcPluginServices |
| `<orc/stage/observation/stage_measurement_store.h>` | Host-kept store for a stage's own measurements of its input |

**params**

| Header | Provides |
|--------|----------|
| `<orc/stage/params/parameter_types.h>` | Stage parameter type definitions shared across all layers |
| `<orc/stage/params/stage_parameter.h>` | Stage Parameter |

**preview**

| Header | Provides |
|--------|----------|
| `<orc/stage/preview/colour_preview_provider.h>` | Interface for stages exposing colour-domain preview carriers. |
| `<orc/stage/preview/orc_preview_carriers.h>` | Typed preview carriers used by the Phase 2 preview pipeline. |
| `<orc/stage/preview/orc_preview_types.h>` | Shared preview taxonomy: video data types, colorimetric |
| `<orc/stage/preview/orc_rendering.h>` | Public API for rendering and preview types |
| `<orc/stage/preview/orc_vectorscope.h>` | Public API for vectorscope visualization data |
| `<orc/stage/preview/preview_stage_types.h>` | Shared lightweight types for stage preview interfaces |
| `<orc/stage/preview/stage_custom_preview_renderer.h>` | Interface for stages with non-standard preview rendering. |
| `<orc/stage/preview/stage_preview_capability.h>` | Capability contracts for stages that expose structured preview |

**tooling**

| Header | Provides |
|--------|----------|
| `<orc/stage/tooling/stage_analysis_tool.h>` | A stage's analysis tools: measure the node's inputs and propose parameter values, run by orc-cli and orc-gui |
| `<orc/stage/tooling/stage_results_report.h>` | A sink's results as summary figures and per-frame rows, printed by orc-cli tools run |
| `<orc/stage/tooling/stage_tool_jobs.h>` | Stage side of a view tool's long jobs: run with the node's inputs, with progress and cancel |
| `<orc/stage/tooling/stage_tool_requests.h>` | Stage side of a view tool: answers requests from the plugin's view binary |

#### `orc/support/` — compiled-into-plugin utilities

NOT part of the binary ABI. Changes never force an ABI bump; recompile the
plugin at the author's convenience.

| Header | Provides |
|--------|----------|
| `<orc/support/cav_picture_number.h>` | IEC 60856/60857 CAV picture number decoding from VBI lines 17 and 18, cross-validated |
| `<orc/support/channel_equalise.h>` | The inverse of the PAL LaserVision CAV edge smear and the burst-referenced chroma gain behind it |
| `<orc/support/clv_picture_number.h>` | IEC 60856/60857 CLV picture number (time code) decoding from the VBI |
| `<orc/support/colour_preview_conversion.h>` | Render-boundary conversion from colour carriers to PreviewImage. |
| `<orc/support/composite_split.h>` | Local split of a composite line into luma and baseband chroma, and re-modulation at another line's phase |
| `<orc/support/dropout_util.h>` | Frame-flat ↔ field/line/sample coordinate conversion utilities |
| `<orc/support/eia608_decoder.h>` | EIA-608 Closed Caption Decoder for timed text conversion |
| `<orc/support/eia608_line21_pairs.h>` | Which line 21 byte pairs a receiver takes: parity, defined codes, field order |
| `<orc/support/eia608_service_demux.h>` | EIA-608 data-channel / caption-vs-text service demultiplexer |
| `<orc/support/fm_beat.h>` | The PAL LaserDisc FM-beat herringbone of one field, predicted from the field itself, fitted and subtracted |
| `<orc/support/frame_cache.h>` | Stage frame cache: shared entries pinned per thread, one build per frame, capacity from the readers' span |
| `<orc/support/frame_content.h>` | Field block signatures, same-picture and colour-phase tests between frames, and frame noise estimates |
| `<orc/support/frame_line_util.h>` | Per-line sample count and offset helpers for 4FSC CVBS flat |
| `<orc/support/frame_read_ahead.h>` | Worker threads that fetch the frames a serial loop is about to read, warming the caches upstream of it |
| `<orc/support/log_destination.h>` | Log output destination selection (console, file, both) |
| `<orc/support/logging.h>` | Logging system implementation |
| `<orc/support/lru_cache.h>` | Thread-safe least-recently-used cache |
| `<orc/support/observation_pass.h>` | The disc-analysis observers swept over a frame range in one pass, on several readers each with a private source |
| `<orc/support/observers/biphase_observer.h>` | Biphase VBI (lines 16-18) observer: picture numbers, time codes, chapters, lead-in/out, status |
| `<orc/support/observers/black_psnr_observer.h>` | Black PSNR (peak signal-to-noise ratio) observer |
| `<orc/support/observers/burst_level_observer.h>` | Colour burst median level observer |
| `<orc/support/observers/colour_frame_phase_observer.h>` | Colour-frame sequence index observer, per field |
| `<orc/support/observers/observer.h>` | Base class of the observers: measure a frame and record observations |
| `<orc/support/observers/white_snr_observer.h>` | White SNR (signal-to-noise ratio) observer |
| `<orc/support/pal_line_locked_resample.h>` | Resampling and coordinate maps between line-locked PAL fields (ld-decode .tbc) and the 4fsc frame lattice |
| `<orc/support/preview_helpers.h>` | Helper functions for stage preview rendering |
| `<orc/support/read_only_file.h>` | Input files opened once per plugin and read at explicit offsets (pread) |
| `<orc/support/scratch_file.h>` | Anonymous on-disk scratch files (unlinked while open) in the per-user cache directory |
| `<orc/support/stack_measurement.h>` | Sample-grid offset, pair noise and pressing noise model between captures |
| `<orc/support/stage_instructions.h>` | Runtime loader for a stage's instructions.md (platform file I/O) |
| `<orc/support/symmetric_fir.h>` | A symmetric FIR over a run of samples, each output summed in tap order, vectorised over the outputs |
| `<orc/support/vbi_bcd.h>` | Binary Coded Decimal decoding for IEC 60856/60857 VBI words |
| `<orc/support/vbi_types.h>` | VBI line data structures shared by the VBI decoder and observers |
| `<orc/support/vbi_utilities.h>` | VBI bit-extraction and manchester/biphase decode helpers |

#### `orc/view/` — view binary contract

The contract between `orc-gui` and a plugin's view binary. Versioned by
`kViewPluginAbiVersion`, not the stage host ABI. No Qt header is included;
a view binary brings its own.

| Header | Provides |
|--------|----------|
| `<orc/view/orc_view_plugin_abi.h>` | View binary contract: descriptor, entry points and widget-factory registration |
| `<orc/view/orc_view_registration.h>` | Descriptor and entry-point boilerplate for a view binary |
| `<orc/view/signal_units.h>` | The amplitude units and video systems a view shows signal levels in |
| `<orc/view/stage_tool_session.h>` | IStageToolSession: a view's requests to its stage, answered on the GUI thread |

#### `orc/view-support/` — widgets for view binaries

Qt widgets `orc-gui` ships in the `orc-view-support` shared library, so a
view draws with the same plot as `orc-gui`'s own windows. Link
`orc::view-support`; at run time the view uses the copy `orc-gui` has
loaded. Versioned with the view tier: a layout change bumps
`kViewPluginAbiVersion`. The only SDK tier that includes Qt.

| Header | Provides |
|--------|----------|
| `<orc/view-support/amplitude_units.h>` | 10-bit sample levels in IRE, millivolts or samples, for views showing signal levels |
| `<orc/view-support/analysis_series_decimator.h>` | Reduce a per-frame analysis series to a bounded set of display buckets |
| `<orc/view-support/analysis_tool_window.h>` | AnalysisToolWindow: the window of a stage's analysis tool (parameters, run, report, Apply to Stage) |
| `<orc/view-support/frame_surface.h>` | IFrameSurface and the raster surface; the host may provide a GPU surface instead |
| `<orc/view-support/frame_view_geometry.h>` | FrameViewGeometry: zoom, aspect, pan and widget<->image mapping for frame viewers |
| `<orc/view-support/frame_viewport_widget.h>` | FrameViewportWidget: zoomable, pannable frame viewport with an overlay hook |
| `<orc/view-support/overlay_primitive_builder.h>` | Overlay geometry for dropout bands, cross-hairs and editor handles |
| `<orc/view-support/overlay_primitives.h>` | Overlay quads and lines a frame surface draws over its frame |
| `<orc/view-support/plot_series_decimation.h>` | Column decimation for plot series wider than the plot |
| `<orc/view-support/plot_widget.h>` | PlotWidget: interactive 2D plot with series, markers, zoom and pan |
| `<orc/view-support/theme_color_tokens.h>` | Theme-aware colours for custom painting, matching orc-gui's |
| `<orc/view-support/view_support_export.h>` | Symbol export macro for the orc-view-support shared library |

#### Deprecated pre-tier include paths

The 27 flat `<orc/plugin/...>` / `<orc/stage/...>` paths that
predate this layout are retained as forwarding shims for one release, gated
by the `ORC_SDK_DEPRECATED_INCLUDE_SHIMS` CMake option (default ON). New
code must use the tiered paths above; building with the option OFF turns any
remaining pre-tier include into a hard compile error.

<!-- END GENERATED SDK HEADER TABLES -->


The manifest is also the source of the enforced include allowlist
(`cmake/sdk_header_allowlist.txt`, generated by
`tools/gen_sdk_header_allowlist.sh`): the gate script
(`cmake/check_plugin_private_includes.sh`) fails the build on any plugin
include outside it, other than plugin-local headers, standard-library and
platform headers, and permitted third-party headers (`fmt`/`spdlog`
unconditionally; other libraries when declared by the plugin's own CMake
target).

### Design Notes on the Allowlist

A few allowlist entries carry rationale worth knowing:

- **Logging surface.** In-tree plugins use the `ORC_LOG_*` macro family from
  the support-tier header `<orc/support/logging.h>`, backed by the host's
  spdlog logger. Out-of-tree plugins should prefer the services-table macros
  (`ORC_PLUGIN_LOG_*` from `<orc/plugin/orc_plugin_services_helpers.h>`) to
  avoid a direct spdlog dependency. The application logging header
  `orc/common/include/logging.h` (`get_app_logger()`) is unrelated and is the
  GUI/CLI surface only; the contract header lives at `<orc/support/logging.h>`
  precisely so the two can never be confused via include-path order.
- **Observer classes were removed (ABI 10).** The concrete
  `<orc/stage/observation/*_observer.h>` classes and the `Observer` base
  (`<orc/stage/observation/observer.h>`), deprecated in ABI 9, are removed from
  the SDK as of ABI 10: they are host-internal and no longer shipped in
  `orc-sdk-support`. Obtain observations through the host-owned
  [`IObservationService`](#observation-service-abi-9), selecting observers by
  stable id. (The remaining `<orc/stage/observation/...>` entries —
  `observation_context*.h`, `observation_schema.h`, and
  `observation_service_interface.h` — stay part of the contract.)
- **`lru_cache.h`** is a self-contained generic container with no host
  coupling, allowlisted because several plugins legitimately use it for
  per-frame values — preferable to each plugin vendoring a copy.
- **`frame_cache.h`** is what a stage keeps the frames it builds in. Use it,
  not `LRUCache`, for anything handed out by pointer:
  - Entries are `shared_ptr`s. `pin()` keeps the entry behind a returned
    pointer alive for the calling thread until that thread pins through the
    same cache and slot again, which is the `VideoFrameRepresentation`
    pointer contract even when another thread evicts the frame meanwhile.
    Give luma and chroma separate slots, so a reader can hold both of one
    frame.
  - `get(id, build)` builds a missing frame once: other threads wanting it
    wait for that build, and a build that throws releases them.
  - A cache created with no capacity takes `frame_cache_capacity()`: the
    frames a sink's decode workers, their decoders' look-around, a
    `FrameReadAhead` and a downstream stacker's still-frame search can want
    at once, on this host. Report `nominal_frame_cache_capacity()` (or the
    smaller of it and your input's) from `max_concurrent_frame_requests()`;
    the video sink fits its worker count to the smallest such bound.
  - `ORC_FRAME_CACHE_FRAMES=<n>` overrides every frame cache's capacity. It
    is a diagnostic: run a stage's tests or a chain under AddressSanitizer
    with `ORC_FRAME_CACHE_FRAMES=2` to catch a reader that keeps a pointer
    past the contract.
- **`read_only_file.h`** is how a source stage reads its capture files. Keep
  a `ReadOnlyFileSet` in the stage's file-access object and read through
  `get(path)->read_at(offset, ...)`:
  - Each file is opened once and read with positional reads, so concurrent
    readers need no lock and a read costs no open (a server round trip on a
    network share). Every set in a plugin shares one descriptor per file, so
    DAG copies do not multiply descriptors.
  - Call `refresh()` where the stage checks its inputs at execute, so a
    file rewritten in place is opened afresh.

### Stability Guarantees

Headers in the public SDK carry an explicit stability commitment:
- Changes to ABI-level contracts require a bump to `kStagePluginHostAbiVersion`.
- Changes to stage API contracts require a bump to `kStagePluginApiVersion`.
- Incompatible plugins are refused by the host with a diagnostic message;
  they are never silently mis-loaded.

## What Plugins Must Not Include

The SDK boundary is an **allowlist**: anything not listed in the SDK Headers
section above (or covered by the plugin-local / standard-library /
third-party allowances) is private and will change without notice. Typical
private surfaces plugins must never reach into:

| Forbidden header path | Reason |
|-----------------------|--------|
| `orc/core/include/*.h` | Internal host implementation headers (e.g. `dag_executor.h`, `preview_renderer.h`, `stage_registry.h`, `buffered_file_io.h`) |
| `orc/core/analysis/*.h`, `orc/core/observers/*.h` | Host preview-scope and observer internals (the observation contracts plugins may use live under `<orc/stage/observation/...>`, and the disc-analysis observers under `<orc/support/observers/...>`) |
| `orc/presenters/*.h` | Presenter layer — GUI/CLI internal |
| `orc/gui/*.h` | GUI internal |
| `orc/cli/*.h` | CLI internal |

The boundary is enforced twice:

1. **At compile time** — the `orc::plugin-sdk` target exposes only the SDK
   include tree (plus spdlog/fmt) and the `orc::orc-sdk-support` static library
   (support-tier helper symbols). It does **not** link the host, so a plugin
   translation unit that includes a private host header fails to compile.
2. **By the allowlist scan** — `check_plugin_private_includes.sh` (a hard CI
   gate, `ctest -L sdk`) fails on any include outside the allowlist even if
   it would compile.

Do not link directly against `orc-core`, `orc-presenters`, `orc-gui`, or
`orc-cli`. Link only against `orc::plugin-sdk`. Third-party libraries a
plugin needs (FFmpeg, SQLite, FFTW, ...) must be declared by the plugin's
own CMake target — they are no longer inherited from the host.

If a capability you need is missing from the SDK, open an issue in the
decode-orc repository. Missing capability is never a reason to include private
headers or link private internals.

## CMake Integration

### Obtaining the SDK

The SDK is installed as part of a decode-orc install tree. From a decode-orc
checkout:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
cmake --install build --prefix /path/to/sdk-prefix
```

The prefix then contains the CMake package
(`lib/cmake/decode-orc-plugin-sdk/`), the public SDK headers
(`include/decode-orc-plugin-sdk/`), the `orc-sdk-support` static library
(`lib/liborc-sdk-support.a`), and the `orc-cli`/`orc-gui` binaries used to load
and test your plugin. Plugins link the SDK alone — the host libraries
(`orc-core`, `orc-common`) are not part of the SDK package.

CI also publishes the SDK on its own as
`decode-orc-plugin-sdk-<version>-linux-x86_64.tar.gz` (headers + static library +
CMake config + enforcement scripts), attached to each tagged release; it is
currently built for Linux x86-64 only. Unpack it anywhere and point
`CMAKE_PREFIX_PATH` at the unpacked directory — no decode-orc source tree or its
FFmpeg / yaml-cpp / PNG / SQLite3 dependency stack is required, only spdlog and
fmt. `FetchContent_Declare(... URL <tarball>)` works the same way.

### Out-of-tree plugin project

```cmake
cmake_minimum_required(VERSION 3.20)
project(my-orc-stage VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(decode-orc-plugin-sdk REQUIRED)

orc_add_stage_plugin(
    my-orc-stage-my-filter
    OUTPUT_NAME    orc-plugin_my-filter_my-stage
    PLUGIN_VERSION "${PROJECT_VERSION}"
    SOURCES        plugin.cpp my_filter_stage.cpp
)
```

Configure with the SDK prefix on `CMAKE_PREFIX_PATH`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=/path/to/sdk-prefix
cmake --build build -j
```

The built plugin lands in `<build-dir>/lib/orc-stage-plugins/`. Load it into
the host with the plugin search path environment variable:

```bash
ORC_STAGE_PLUGIN_PATHS=<build-dir>/lib/orc-stage-plugins orc-cli plugins list
```

`orc_add_stage_plugin()` takes:

```cmake
orc_add_stage_plugin(<target>
    SOURCES        <file> ...          # required
    [OUTPUT_NAME    <name>]            # shared-library output name
    [PLUGIN_VERSION <version>]         # sets the ORC_STAGE_PLUGIN_VERSION define
    [LINK_LIBRARIES <lib> ...]         # the plugin's own third-party dependencies
    [RUNTIME_DEPENDENCIES <path> ...]) # extra files installed beside the plugin
```

It builds a `SHARED` library linked privately against `orc::plugin-sdk` and any
`LINK_LIBRARIES`, adds the source directory to the include path, places the
output in `<build-dir>/lib/orc-stage-plugins/`, adds install rules for the
platform plugin directory, and — when an `instructions.md` sits beside the
`CMakeLists.txt` — copies it next to the built library (renamed to match it,
e.g. `lib<OUTPUT_NAME>.md`, or `<OUTPUT_NAME>.md` on Windows) so
`ORC_STAGE_INSTRUCTIONS_MD` can find it at runtime.

The installed `decode-orc-plugin-sdk` CMake package provides:
- `orc::plugin-sdk` — imported INTERFACE target with the SDK include paths,
  spdlog/fmt, and the `orc::orc-sdk-support` static library (support-tier
  helper symbols). No host library is linked
- `orc::orc-sdk-support` — the support-tier helper static library, pulled in
  transitively by `orc::plugin-sdk`
- `orc_add_stage_plugin()` — plugin target helper macro
- The public SDK headers (`<orc/abi/...>`, `<orc/stage/...>`, `<orc/support/...>`)
- `check_plugin_private_includes.sh` / `check_plugin_private_links.sh` — SDK enforcement gate scripts

The package's only `find_dependency` requirements are **spdlog** and **fmt**.

This exact flow is validated in decode-orc CI on every push: the fixture
plugin `orc-tests/fixtures/external-stage-plugin/` is built against a
freshly installed prefix and loaded into the installed `orc-cli`.

### Validating SDK-only compliance locally

To run the same enforcement gates that execute in Decode-Orc CI/CD, use the
scripts bundled with the installed SDK package (located at
`<install-prefix>/lib/cmake/decode-orc-plugin-sdk/`):

```bash
# Check every include in your plugin tree against the SDK allowlist
bash <sdk-install>/check_plugin_private_includes.sh /path/to/your-plugin-repo

# Check for forbidden private-link dependencies in your plugin target
bash <sdk-install>/check_plugin_private_links.sh /path/to/your-plugin-repo
```

When pointed at a tree that is not a decode-orc checkout, both scripts run in
standalone mode: the whole tree is scanned as a single plugin (includes may
resolve anywhere within it). Both scripts exit with code 1 and print a
diagnostic on violations. Run them before opening a pull request or tagging a
release.

### Output naming

Release assets must follow the artifact naming convention so the host can
validate and cache plugin binaries correctly:

```
orc-plugin_<stage-name>_<platform>[_abi<N>].<ext>
```

See [GitHub release assets](#github-release-assets) below.

## Implementing a Stage

### Minimal plugin structure

The registration helpers in `<orc/abi/orc_plugin_registration.h>` (pulled
in by the umbrella header) expand the entire plugin boilerplate — descriptor
version/toolchain fields, both entrypoints, service-table storage, and stage
registration — from two statements:

```cpp
#include <orc/abi/orc_plugin_sdk.h>

#include "my_stage.h"  // Your DAGStage subclass

namespace {

// The ABI version, API version, and toolchain tag are filled in from the SDK
// the plugin is compiled against. All pointer fields point to static storage.
constexpr orc::StagePluginDescriptor kDescriptor =
    ORC_STAGE_PLUGIN_DESCRIPTOR("com.example.stage.my-filter",  // plugin_id
                                "1.0.0",  // plugin_version
                                "MIT",    // license_spdx
                                false);   // is_core_plugin

}  // namespace

// Expands to both required entrypoints. MyStage is registered under the
// stage name reported by its own get_node_type_info().stage_name — the stage
// class is the single source of truth for registration metadata. List
// additional stage types as further arguments to register several stages
// from one plugin.
ORC_DEFINE_STAGE_PLUGIN(kDescriptor, MyStage)
```

Stage types passed to `ORC_DEFINE_STAGE_PLUGIN` must be either
default-constructible or constructible from `orc::IStageServices*`; in the
latter case the host's consolidated stage services are injected automatically
when the stage is created.

The same pattern is used by every bundled stage plugin — see
`orc/plugins/stages/mask_line/plugin.cpp` (single stage) or
`orc/plugins/stages/cvbs_source/plugin.cpp` (multiple stages) in the
Decode-Orc repository for complete real-world examples.

### Raw entrypoints (custom registration logic)

Plugins that need custom registration logic — stages with other constructor
signatures, conditional registration, dynamic stage sets — can write the two
exported entrypoints by hand instead:

```cpp
#include <orc/abi/orc_plugin_sdk.h>

#include "my_stage.h"

namespace {

// Factory the host calls to create stage instances on demand.
// Must match orc::OrcStageFactoryFn: std::shared_ptr<DAGStage> (*)().
orc::DAGStagePtr create_my_stage() { return std::make_shared<MyStage>(); }

// Plugin descriptor. Fields are positional and must follow the declaration
// order in <orc/abi/orc_plugin_abi.h>. All pointer fields must point to
// static storage.
constexpr orc::StagePluginDescriptor kDescriptor{
    "com.example.stage.my-filter",    // plugin_id
    "1.0.0",                          // plugin_version
    orc::kStagePluginHostAbiVersion,  // host_abi_version
    orc::kStagePluginApiVersion,      // plugin_api_version
    "MIT",                            // license_spdx
    false,                            // is_core_plugin
    ORC_SDK_TOOLCHAIN_TAG,            // toolchain_tag
};

}  // namespace

ORC_STAGE_PLUGIN_EXPORT const orc::StagePluginDescriptor*
orc_get_stage_plugin_descriptor() {
  return &kDescriptor;
}

ORC_STAGE_PLUGIN_EXPORT bool orc_register_stage_plugin(
    const orc::OrcPluginServices* services, void* context,
    bool (*register_stage)(void* context, const char* stage_name,
                           orc::OrcStageFactoryFn factory),
    const char** error_message) {
  // Store the host service table before registering any stage; the pointer
  // remains valid for the lifetime of the loaded plugin.
  orc::plugin::set_services(services);

  if (!register_stage) {
    if (error_message) {
      *error_message = "Missing stage registration callback";
    }
    return false;
  }

  if (!register_stage(context, "my_filter", &create_my_stage)) {
    if (error_message) {
      *error_message = "Host rejected stage registration";
    }
    return false;
  }

  return true;
}
```

### Stage documentation

Every stage ships an `instructions.md` beside its binary (see the
[Plugin Author Guide](plugin-author-guide.md#4-write-instructionsmd)). Put the
`ORC_STAGE_INSTRUCTIONS_MD` macro (from `<orc/support/stage_instructions.h>`,
also re-exported by `<orc/plugin/orc_stage_tooling.h>`) in the stage's class
body: it overrides `DAGStage::get_instructions()` to read that file from
alongside the loaded plugin library at runtime, which is what the GUI
**Help…** dialog and `orc-cli stages help` display. A plugin that cannot ship
the file may embed the text with the legacy `ORC_STAGE_INSTRUCTIONS(content)`
macro instead.

### Stage interfaces

Implement one or more of these interfaces depending on stage behaviour:

| Interface | Use when |
|-----------|----------|
| `DAGStage` | Base interface — all stages |
| `ParameterizedStage` | Stage exposes user-configurable parameters |
| `TriggerableStage` | Stage responds to external trigger events |

### Receiving data

Override `DAGStage::execute()` to receive `ArtifactPtr` inputs from upstream
nodes. Return a vector of `ArtifactPtr` outputs to pass downstream.

### Transform stages: VideoFrameRepresentationWrapper contract

Transform stages that expose video output extend
`VideoFrameRepresentationWrapper` (in
`<orc/stage/video_frame_representation.h>`). The wrapper's public read API
**always describes the stage's output**: downstream stages, observers, and
analysis sinks can never reach unprocessed upstream data through any
accessor, and stages themselves may only read from the wrapped input
(`source_`), never from anything further upstream.

The accessors fall into two groups:

- **Pass-through primitives** (forwarded to the wrapped input): navigation
  (`frame_range`, `frame_count`, `has_frame`, `get_frame_descriptor`),
  `get_frame`, `has_separate_channels`, `get_frame_luma`,
  `get_frame_chroma`, `get_dropout_hints`, `get_video_parameters`, and the
  audio / EFM / AC3 accessors. A stage whose output differs from its input
  for one of these **must** override it. A stage that remaps frame IDs must
  override every per-frame accessor in this group so IDs are translated —
  for audio that means `get_audio_samples` (per channel pair): the output
  frame at timeline index *p* must serve exactly `audio_pairs_in_frame(p)`
  pairs, truncating or silence-padding by one pair when the mapping breaks
  the NTSC/PAL-M cadence phase (see `<orc/stage/audio/audio_channel_pair.h>`).
- **Derived accessors** (`get_line`, `get_line_samples`, `get_frame_copy`,
  `get_line_luma`, `get_line_chroma`): implemented by the wrapper in terms
  of the object's own virtual primitives, never forwarded. Overriding the
  frame-level primitives is therefore sufficient for correctness on every
  read path; overriding a derived accessor is purely an optimisation (for
  example, a stage that does not modify sample data may forward line reads
  to the wrapped input to preserve a source's seek-one-line-from-disk fast
  path).

This contract is verified by
`orc-tests/core/unit/contracts/video_frame_representation_wrapper_contract_test.cpp`.

### Parameters

`ParameterizedStage` exposes three methods:

- `get_parameter_descriptors(VideoSystem, SourceType)` — returns the list of
  `ParameterDescriptor` records this stage supports, filtered by the project's
  video format and source type (a no-argument convenience overload passes
  `Unknown` for both).
- `get_parameters()` — returns the current values as a
  `std::map<std::string, ParameterValue>`.
- `set_parameters(const std::map<std::string, ParameterValue>&)` — applies new
  values; returns `true` if all parameters were valid and set successfully.

`ParameterValue` is a `std::variant` holding one of: `int32_t`, `uint32_t`,
`double`, `bool`, or `std::string`. The corresponding `ParameterType` enum
additionally distinguishes `FILE_PATH` (a string for which the GUI shows a
file browser).

#### Reserved parameter: `input_node_ids`

A multi-input stage receives its inputs as an unnamed vector whose order
follows the order the connections were made, and artifacts carry no node
identity — so a stage that has to distinguish one input from another cannot do
it on its own.

Declare a `STRING` parameter named `orc::kInputNodeIdsParameter`
(`"input_node_ids"`, from `<orc/stage/params/parameter_types.h>`) and the host
fills it in when it builds the execution graph: the source node IDs of the
node's incoming connections, comma-separated, in the same order as the
`inputs` vector handed to `execute()` — for example `"2,4,16"`. Node IDs are
drawn on the nodes in the graph editor, so they are also what a user can put
in a parameter of their own to name an input (the core `source_join` stage
does exactly this).

The value is host-owned: the parameter is filtered out of the GUI and CLI
parameter surfaces, is never editable, and is never written to the project
file. A stage that declares it must still behave sensibly when handed an empty
value — no host support, or nothing connected yet — by falling back to
positional handling of its inputs. Stages that do not declare the parameter
are never handed it.

### Configuration status

Stages can report their readiness to the host UI via `set_configuration_status()`,
which accepts a value from the `orc::ConfigurationStatus` enum:

| Value | Meaning |
|-------|---------|
| `ConfigurationStatus::Green` | Fully configured or requires no parameters |
| `ConfigurationStatus::Yellow` | Partially configured; some parameters are missing or empty |
| `ConfigurationStatus::Red` | Unconfigured; critical required parameters are absent |

The host renders a small coloured dot on the stage node in the pipeline graph to
give the user at-a-glance feedback.

**When to call `set_configuration_status()`:**

- In the constructor — set `Yellow` if the stage requires at least one parameter
  that has no sensible default, `Green` if all parameters have working defaults.
- At the end of `set_parameters()` — re-evaluate and call again based on the
  values that were just applied.

```cpp
MyStage::MyStage() {
    set_configuration_status(orc::ConfigurationStatus::Yellow);
}

bool MyStage::set_parameters(
    const std::map<std::string, orc::ParameterValue>& params) {
    auto it = params.find("output_path");
    const bool ready =
        it != params.end() &&
        std::holds_alternative<std::string>(it->second) &&
        !std::get<std::string>(it->second).empty();

    set_configuration_status(ready ? orc::ConfigurationStatus::Green
                                   : orc::ConfigurationStatus::Yellow);
    return true;
}
```

Read the current value with `get_configuration_status()` if needed. The default
(when `set_configuration_status()` is never called) is `Green`.

### Host services

The host passes an `OrcPluginServices` table as the first argument to
`orc_register_stage_plugin()`. Store it with `orc::plugin::set_services()`
before registering any stage. The table provides:

- `log` — pre-formatted message logging routed to the host logger. Use the
  `ORC_PLUGIN_LOG_TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR` / `CRITICAL`
  macros (from `<orc/plugin/orc_plugin_services_helpers.h>`) rather than
  calling the function pointer directly. Logging does **not** go through
  `IStageServices`.
- `render_colour_preview` — converts a decoded `ColourFrameCarrier` to a
  display-ready `PreviewImage` for preview-capable stages.
- `stage_services` — optional pointer to the consolidated `IStageServices`
  interface; may be `nullptr` when the capability is unavailable. Retrieve it
  with `orc::plugin::get_stage_services()` (no arguments), which returns
  `nullptr` if the services table is absent or predates the field.
- `observation_service` — optional pointer to the `IObservationService`
  interface (added in ABI 9). Retrieve it with
  `orc::plugin::get_observation_service()` (no arguments), which returns
  `nullptr` if the services table is absent or predates the field (any host on
  ABI 8 or earlier). See [Observation service](#observation-service-abi-9).

`IStageServices` currently exposes exactly three factory methods, used by sink
stages for buffered file output:

- `create_buffered_file_writer_uint8(size_t buffer_size)` — returns a
  `std::shared_ptr<IFileWriterUint8>`
- `create_buffered_file_writer_uint16(size_t buffer_size)` — returns a
  `std::shared_ptr<IFileWriterUint16>`
- `create_buffered_file_writer_int16(size_t buffer_size)` — returns a
  `std::shared_ptr<IFileWriterInt16>` (16-bit signed PCM output, e.g. WAV
  audio)

New `IStageServices` methods are appended after existing entries (append-only
convention), so plugins built against an older SDK keep working with a newer
host within the same version pair.

Progress reporting and artifact-delivery callbacks are **not** part of the
current services contract. If your stage needs a host capability that is
missing, request an SDK extension rather than working around it.

#### Stage measurement store

A stage that measures its whole input before it can produce a frame (the
Stacker groups its inputs by pressing from frames spread across the stack) can
keep that measurement with the host's observations instead of taking it in
every copy of the DAG. The GUI runs several copies — the preview, the
observers' renderer and a pool of background observation workers — each with
its own stage instances.

Opt in by additionally inheriting `orc::IStageMeasurementConsumer` from
`<orc/stage/observation/stage_measurement_store.h>`. Before each `execute()`
the host calls `set_measurement_store()` with an `IStageMeasurementStore`
bound to the executing node's provenance, or `nullptr` when it keeps none (as
`orc-cli` does); guard the handle, since several threads may execute one stage
instance. `get_or_measure(id, version, measure)` returns the stored
`StageMeasurement` (named `ObservationValue`s) or takes it with `measure` and
stores it; a concurrent caller for the same measurement waits for it rather
than measuring again. The record is kept, persisted and collected with the
node's observations, so a later session reuses it until the node's provenance
changes. The node's provenance does not include the plugin's build, so change
`version` whenever what `measure` computes or how it encodes it changes.

This is a new capability interface reached by `dynamic_cast`, so it needs no
ABI bump; a host that predates it never calls `set_measurement_store()` and the
stage measures for itself.

#### Observation service (ABI 9)

`IObservationService`
(`<orc/stage/observation/observation_service_interface.h>`) is a host-owned
service that runs the standard observers. The observer *implementations* live
once, in the host; a plugin selects one by **stable string id** instead of
linking the concrete observer classes. This mirrors `render_colour_preview`:
the capability crosses the boundary as a host-owned interface, not as object
code compiled into every plugin.

Retrieve it with `orc::plugin::get_observation_service()`, which returns
`nullptr` on any host that predates ABI 9. A plugin **must** null-check the
result and degrade gracefully (skip the observation, log a warning) when the
service is absent.

The interface exposes:

- `available_observers()` — returns one `ObserverInfo { id, version,
  provided_observations }` per observer the host offers, in a stable order.
- `create_observer(observer_id)` — returns an owning
  `std::unique_ptr<IObserverHandle>` for a **stateful** session, or `nullptr`
  for an unknown id. Use this when observations accumulate across frames (e.g.
  the closed-caption observer pairs successive fields). Drive it with
  `handle->process_frame(representation, frame_id, context)`.
- `run_observer(observer_id, representation, frame_id, context)` — a one-shot
  convenience wrapper that creates a throwaway handle, processes exactly one
  frame, and returns `false` for an unknown id (leaving `context` untouched).
  Do **not** use it where cross-frame state matters.

No method throws across the plugin boundary: an unknown id yields a null handle
or `false`, never an exception. Thread-safety is documented on each interface
method — in short, the service is safe to call concurrently, but a single
`IObserverHandle` must be driven from one thread at a time.

##### Migrating from the observer classes

The concrete observer classes under `<orc/stage/observation/*_observer.h>`
(`BiphaseObserver`, `WhiteSNRObserver`, …) were **deprecated in ABI 9** and are
**removed in ABI 10**: the headers have left the plugin-facing SDK and
`orc-sdk-support` no longer ships their object code. Replace direct construction
with a service call keyed by the id below:

| Removed class | Observer id | Former header (removed in ABI 10) |
|---------------|-------------|-----------------------------------|
| `BiphaseObserver` | `biphase` | `<orc/stage/observation/biphase_observer.h>` |
| `BlackPSNRObserver` | `black_psnr` | `<orc/stage/observation/black_psnr_observer.h>` |
| `BurstLevelObserver` | `burst_level` | `<orc/stage/observation/burst_level_observer.h>` |
| `ClosedCaptionObserver` | `closed_caption` | `<orc/stage/observation/closed_caption_observer.h>` |
| `ColourFramePhaseObserver` | `colour_frame_phase` | `<orc/stage/observation/colour_frame_phase_observer.h>` |
| `FieldQualityObserver` | `disc_quality` | `<orc/stage/observation/field_quality_observer.h>` |
| `FmCodeObserver` | `fm_code` | `<orc/stage/observation/fm_code_observer.h>` |
| `WhiteFlagObserver` | `white_flag` | `<orc/stage/observation/white_flag_observer.h>` |
| `WhiteSNRObserver` | `white_snr` | `<orc/stage/observation/white_snr_observer.h>` |

Before (linked observer class):

```cpp
#include <orc/stage/observation/closed_caption_observer.h>

orc::ClosedCaptionObserver observer_;  // persistent member, stateful
observer_.process_frame(representation, frame_id, context);
```

After (host service, id-selected):

```cpp
#include <orc/stage/observation/observation_service_interface.h>

// Once, when the stage is constructed:
orc::IObservationService* observation_service =
    orc::plugin::get_observation_service();
if (observation_service) {
  observer_ = observation_service->create_observer("closed_caption");
}

// Per frame (observer_ is a std::unique_ptr<orc::IObserverHandle> member):
if (observer_) {
  observer_->process_frame(representation, frame_id, context);
}
```

For a stateless one-off, skip the handle and call
`observation_service->run_observer("closed_caption", representation, frame_id,
context)`.

### Optional: Stage tools

If your stage provides an interactive tool (e.g., a custom editor or analysis
view), implement `StageToolProvider` (`<orc/plugin/orc_stage_tooling.h>`) and
return a vector of `StageToolDescriptor` from `get_stage_tools()`. The host
discovers these descriptors through the presenter layer and routes them to the
GUI without any hardcoded stage-name dispatch.

`StageToolDescriptor` carries `tool_id`, `display_name`, `description`, a
`kind` (`StageToolKind::ConfigDialog`, `NonModalEditor`, `BatchAnalysis`,
`PreviewUtility`, `Custom` — the default — `PluginView` or `Analysis`), a
`non_modal` flag, and an optional `contract_id` naming the payload contract.
The host does not read `non_modal`: the kind decides how the tool opens, and
a view's window decides its own modality.

A `BatchAnalysis` tool belongs to a sink whose trigger is the analysis.
`orc-cli tools run` triggers the sink and then prints what the stage
reports, so implement `orc::IStageResultsReportProvider` from
`<orc/stage/tooling/stage_results_report.h>` as well:

```cpp
bool results_report(orc::StageResultsReport* report) const override {
  if (!has_results_) return false;  // not triggered, or the trigger failed
  report->kind = "flicker";
  report->total_frames = total_frames_;
  report->statistics["frames_analysed"] =
      orc::StageReportValue::of_integer(frames_.size());
  report->columns = {"frame", "flicker"};
  for (const auto& f : frames_) {
    report->rows.push_back(
        {orc::StageReportValue::of_integer(f.frame),
         f.measured ? orc::StageReportValue::of_real(f.flicker)
                    : orc::StageReportValue::absent()});
  }
  return true;
}
```

The host prints it unchanged: `statistics` as the report's statistics, and
the rows under `results.frames`, one object per row keyed by column name, an
absent value as `null`. In `orc-gui` the tool opens the window your view
binary registered for its `contract_id`, as for a `PluginView` tool.

A `ConfigDialog` tool is a dialog that sets some of the stage's parameters
for the user, such as a preset picker. `orc-gui` opens the dialog your view
binary registered for its `contract_id`, as for a `PluginView` tool; the
dialog reads the node's values with `parameters()` and writes its choice with
`apply_parameters()`. `orc-cli` lists the tool and refuses to run it, and
points the user at `orc-cli nodes set` instead.

A `NonModalEditor` tool is a window the user keeps open beside the main one
while editing what the stage does, such as the Dropout Editor. `orc-gui`
opens it in the same way, from your view binary; to stay open across the
graph rebuild its own `apply_parameters()` causes, it sets a graph-rebuilt
handler. `orc-cli` lists it and refuses to run it.

An `Analysis` tool measures the node's inputs and proposes values for the
stage's own parameters, such as FM Beat Cancel's FM Beat Calibration, which
finds the carrier, Channel Equalise's Channel Calibration, which measures
the edge smear, Frame Map's Disc Mapper, which maps a side's frames by
their picture numbers, or Source Align's Source Alignment Analysis, which
places the captures of a stack by theirs. Give it a `contract_id` and
implement
`orc::IStageAnalysisTool` from `<orc/stage/tooling/stage_analysis_tool.h>`:

```cpp
std::vector<orc::ParameterDescriptor> analysis_tool_parameters(
    const std::string& tool_id) const override {
  return {};  // the parameters the tool takes, with defaults
}

orc::StageAnalysisReport run_analysis_tool(
    const std::string& tool_id,
    const std::map<std::string, orc::ParameterValue>& parameters,
    orc::IToolJobContext& context) override {
  orc::StageAnalysisReport report;
  // Read context.inputs(); call context.report_progress() as you go and
  // return Status::Cancelled soon after context.is_cancelled() turns true.
  report.status = orc::StageAnalysisReport::Status::Success;
  report.summary = "Carrier at black: 7.0762 MHz.";
  report.statistics["fieldsSearched"] = int64_t{24};
  report.parameter_changes["carrier_black_mhz"] = 7.0762;
  return report;
}
```

The run is called as a tool job is: on a worker thread, on a stage object
built for the run from a copy of the project, with the node's inputs.
`parameter_changes` holds what applying the result writes, by the names and
types your stage declares; applying writes them over the node's stored
values and leaves the others. Leave it empty when a successful run found
nothing to set, and the host tells the user there is nothing to apply.

The host runs the tool everywhere. `orc-cli tools run` prints the report
(`summary`, `statistics`, `items` and `parameter_changes` in `--json`), and
`--apply` writes the changes. `tools refresh-config` can make it the node's
refresh tool, which `tools refresh` then runs. In `orc-gui` the tool opens
the window your view binary registered for its `contract_id`, or, if there
is none, the host's own analysis window (`orc::view::AnalysisToolWindow`
from `orc-view-support`). That window shows the tool's description and
parameters, runs the tool, shows the summary, writes the changes with Apply
to Stage, and offers the refresh option. A view can subclass the window to
offer different parameter controls. The host reaches the interface by
`dynamic_cast`, so adding it did not change the stage host ABI.

A project refresh (`orc-cli tools refresh`) runs its tools one after
another. A tool that spends its time reading a capture, and that a refresh
may run several times (once per disc of a stack, as the Frame Map's Disc
Mapper is), can run beside the others instead, each run on its own copy of
the project. Say so for that tool:

```cpp
bool analysis_tool_runs_alongside(const std::string& tool_id) const override {
  return tool_id == kMapperToolId;
}
```

The default is false.

`<orc/plugin/orc_stage_tooling.h>` also declares `AnalysisToolProvider`
(`get_analysis_tools()`, returning `AnalysisToolDescriptor` records). The
host does not read it, and no in-tree stage implements it: offer an analysis
as an `Analysis` or `BatchAnalysis` tool, as above. It stays in the header
until the next stage ABI change removes it, so that a plugin implementing it
still builds.

### View plugins

A stage binary cannot carry a window, because it has no Qt in it. A plugin
whose tool needs its own window ships a second binary, the **view binary**,
built against Qt and the `<orc/view/...>` headers. The design is in
[Plugin Architecture: View Plugins](plugin-architecture.md#view-plugins); this
section is the recipe.

**1. Advertise the tool from the stage.** Give it kind
`StageToolKind::PluginView` and a contract id you own:

```cpp
std::vector<orc::StageToolDescriptor> get_stage_tools() const override {
  return {{"demo_browser", "Demo Browser", "Browse what the last run found",
           orc::StageToolKind::PluginView, /*non_modal=*/true,
           "com.example.demo.browser.v1"}};
}
```

**2. Answer the view's requests in the stage.** Implement
`orc::IStageToolRequestHandler` from
`<orc/stage/tooling/stage_tool_requests.h>`. The verbs and the bytes are
yours, so version them with the contract id:

```cpp
orc::StageToolReply handle_tool_request(
    const std::string& contract_id, const std::string& verb,
    const std::vector<uint8_t>& payload) override {
  orc::StageToolReply reply;
  if (!has_results_) {
    reply.error = "Trigger the stage first";
    return reply;
  }
  if (verb == "list") {
    reply.data = encode_list(results_);  // your format
    reply.ok = true;
  }
  return reply;
}
```

The host calls it on the render worker, never during a trigger. Never run the
stage from here; answer from what the last trigger left.

**3. Build the view binary.** Make a separate shared library that links
`Qt6::Core`, `Qt6::Gui` and `Qt6::Widgets` and nothing else from Qt. Its file
must be named `orc-view_<name>`, with no `lib` prefix:

```cmake
add_library(orc-view_demo SHARED view/demo_view.cpp view/demo_dialog.cpp)
target_link_libraries(orc-view_demo PRIVATE orc::plugin-sdk Qt6::Widgets)
set_target_properties(orc-view_demo PROPERTIES PREFIX "" AUTOMOC ON)
```

Define the entry points with `<orc/view/orc_view_registration.h>`. Include a
Qt header first, so the descriptor records the Qt version you built against:

```cpp
#include <QDialog>
#include <orc/view/orc_view_registration.h>

namespace {
QWidget* make_browser(const orc::view::ViewOpenRequest& request) {
  return new DemoDialog(request.session, request.parent);
}
constexpr orc::view::ViewPluginDescriptor kViewDescriptor =
    ORC_VIEW_PLUGIN_DESCRIPTOR("com.example.stage.demo", "1.0.0");
}  // namespace

ORC_DEFINE_VIEW_PLUGIN(kViewDescriptor,
                       {"com.example.demo.browser.v1", &make_browser})
```

**4. Talk to the stage through the session.** The widget keeps the
`std::shared_ptr<orc::view::IStageToolSession>` it was given and asks for
data with `request()`. The reply comes back later on the GUI thread, so show
a pending state until it does:

```cpp
session_->request("list", {}, [this](orc::view::ToolReply reply) {
  if (!reply.ok) {
    showMessage(QString::fromStdString(reply.error));
    return;
  }
  showList(decode_list(reply.data));
});
session_->set_results_changed_handler([this] { reload(); });
```

Replies arrive in the order you asked. None arrives after your widget is
destroyed, so a handler capturing `this` is safe. The view must not include
`<orc/stage/...>` or any host header: the session is all it gets.

**Long jobs.** Work that takes long enough to need progress and a Cancel
button goes through `run_job()` instead of `request()`. It runs on a job
worker, so previews and requests carry on. The stage implements
`orc::IStageToolJobHandler` from `<orc/stage/tooling/stage_tool_jobs.h>`:

```cpp
orc::StageToolReply run_tool_job(const std::string& contract_id,
                                 const std::string& verb,
                                 const std::vector<uint8_t>& payload,
                                 orc::IToolJobContext& context) override {
  orc::StageToolReply reply;
  const auto& inputs = context.inputs();  // as execute() receives them
  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    if (context.is_cancelled()) {
      return reply;  // the host replies "Cancelled"
    }
    context.report_progress(static_cast<double>(frame) / frame_count,
                            "Measuring frame " + std::to_string(frame + 1));
    // ...
  }
  reply.data = encode_findings(findings);  // your format
  reply.ok = true;
  return reply;
}
```

The host calls it on a stage object built for the job from a copy of the
project, never the object previews and triggers use. A job only computes;
the view writes what should change.

`context.inputs()` reads through the graph's stages and frame caches, so one
thread reads them at a time. A job that reads with more threads calls
`context.open_private_inputs()` once for each extra thread, on the job's own
thread before starting it: the host builds the graph again and runs it to
the node's inputs, so each copy shares no stage or cache with any other and
costs what opening the sources costs. Keep the returned `owner` until the
thread is done with the copy's `inputs`. The copy's inputs are empty when
the host cannot make one; then read `inputs()` from one thread. Frame Map's
Disc Mapper reads a whole side with four such copies, through the sweep in
`<orc/support/observation_pass.h>`.

In the view:

```cpp
job_ = session_->run_job(
    "measure", {},
    [this](orc::view::ToolJobProgress p) { showProgress(p.fraction, p.text); },
    [this](orc::view::ToolReply reply) { showFindings(reply); });
// Cancel button: job_->cancel();
```

`run_job()` returns `nullptr` while the session's previous job is still
running. Closing the window cancels the job.

**Writing parameters.** `apply_parameters()` writes values to the node, as
its parameter editor would. Values are text, written as on the `orc-cli`
command line; the host checks each against the parameter the stage declares
and writes all of them or none:

```cpp
session_->apply_parameters({{"ranges", ranges_text}}, "Apply ranges",
                           [this](orc::view::ToolReply reply) {
                             if (!reply.ok) showMessage(reply.error);
                           });
```

`parameters()` gives the node's stored values in the same text form, so a
dialog that edits some of them starts from what is there; send back only
what it changes. A stored file path is as the project holds it: a relative
one is relative to `project_directory()`, the project file's folder (empty
until the project is saved), and a file the user picks should be stored
relative to it the same way.

The write rebuilds the graph, which replaces every stage object and the
results of the last trigger with it. A view is closed on a rebuild unless it
has called `set_graph_rebuilt_handler()`; a view that should outlive edits
(an analysis window, say) sets one and asks for fresh data after the next
trigger.

**The previewer.** `show_frame(n)` moves the main preview to frame `n`
(0-based), and `set_preview_frame_handler()` follows the preview as the user
moves it; set it while opening, and it is told the frame the preview shows as
soon as the window is open. `render_frame(n, options, handler)` renders a frame of the node, or
by default of its first input, as 8-bit RGB with the lines of both fields in
order, for a view that draws frames itself. The reply's `dropouts` are the
dropouts the rendered node reports on that frame, in image coordinates (so a
line is the frame-flat line); they are not drawn into the image.

**Signal levels.** `amplitude_unit()` is the unit the project shows signal
levels in (IRE, millivolts or 10-bit samples, from Project Properties), and
`set_amplitude_unit_handler()` runs when the user changes it. The unit and
the video system are view-tier types (`<orc/view/signal_units.h>`), so a
view never needs a stage header for them; if your stage's figures depend on
the system, send it in your own format. Convert 10-bit values with
`<orc/view-support/amplitude_units.h>`, which `orc-gui`'s own windows use
too, to show the same figures they do.

**Shared widgets.** `orc-gui`'s own plot is in the `orc-view-support`
library (`<orc/view-support/plot_widget.h>`, namespace `orc::view`), with
the decimator its analysis graphs use
(`<orc/view-support/analysis_series_decimator.h>`) and the amplitude
conversions. So is a frame viewer for frames from `render_frame()`:
`FrameViewportWidget` (`<orc/view-support/frame_viewport_widget.h>`) zooms,
pans and corrects the aspect ratio, maps widget to image coordinates, and
draws what a subclass adds in `buildOverlay()` (quads and lines, with helpers
in `<orc/view-support/overlay_primitive_builder.h>`) over the frame. In
`orc-gui` it draws on the GPU when the preview does; you do not set that up.
Link the library to draw with the same widgets and theme colours as
`orc-gui`:

```cmake
target_link_libraries(orc-view_demo PRIVATE orc::plugin-sdk orc::view-support Qt6::Widgets)
```

`orc::view-support` is defined when the SDK package was built with
`orc-gui` and Qt 6 Widgets is found. At run time the view uses the copy
`orc-gui` has loaded.

**An analysis tool's window.** A session opened for a `StageToolKind::Analysis`
tool also answers `analysis_info()` (the tool's description and its
parameters as text, with defaults, ranges, choices and dependencies) and
`run_analysis(parameters, on_progress, on_report)`, which runs the tool as a
job and reports the summary and, as text for `apply_parameters()`, what
applying the result writes. `refresh_state()` and `set_refresh()` read and
store whether `orc-cli tools refresh` runs the tool for the node. The host's
`orc::view::AnalysisToolWindow` (`<orc/view-support/analysis_tool_window.h>`)
is built on these; a view that registers a window for an analysis tool can
subclass it and override `buildParameters()`, `currentParameters()` and
`showParameters()` to offer controls of its own. For any other kind of tool
the info is empty and `run_analysis()` reports a failure.

**5. Publish both binaries.** List the view in the release manifest after
the stage binary for the same platform, with `kind: view`, the view ABI as
`abi`, and `qt`. See
[View entries](plugin-architecture.md#view-entries-manifest_schema-2).

Build the view against the [view Qt baseline](#view-qt-baseline). `orc-cli`
never loads a view, so anything a script needs must also be written to files
by the stage when it is triggered.

For local testing, put the view binary in a folder named by
`ORC_STAGE_PLUGIN_PATHS` beside the stage binary. `orc-gui` loads both.

#### View Qt baseline

A view loads only into an `orc-gui` whose Qt has the same major version and
the same or a newer minor than the Qt it was built against. It also has to
match that `orc-gui`'s toolchain tag, as a stage binary does. So build
released views against the **view Qt baseline: Qt 6.9**. That is the oldest
Qt any released `orc-gui` runs.

| Release `orc-gui` | Qt | Toolchain tag |
|---|---|---|
| Linux (Flatpak, KDE runtime 6.9) | 6.9 | `gcc14/libstdc++` |
| macOS (DMG) | Homebrew's current Qt, at least 6.9 | `clang21/libc++` (AppleClang 21) |
| Windows (MSI) | 6.9 | `msvc19/msvc-stl/release-crt` |

- **Windows and macOS:** build the view with Qt 6.9 from
  `jurplel/install-qt-action` (`version: '6.9.*'`). The skeleton's CI shows
  how.
- **Linux:** build with the Flatpak's own compiler and Qt, from
  `org.kde.Sdk//6.9` (GCC 14, Qt 6.9). This applies to the stage binary as
  well as the view: the toolchain tag has to match. The skeleton's
  `scripts/build_in_kde_sdk.sh` does this build, with fmt and spdlog at the
  Flatpak's versions. On Linux the decode-orc flake and the skeleton's flake
  build from `nixos-25.05`, which has the same GCC 14 and Qt 6.9, so a Nix
  build has the same toolchain tag as the Flatpak. Released plugins load in
  a Nix-built `orc-gui`, and a Nix-built plugin loads in the Flatpak. Keep a
  plugin flake's `nixpkgs-linux` input on the same release as decode-orc's.

The baseline moves only when the Flatpak's KDE runtime and the Windows Qt
move. `orc-plugin-registry/tools/validate_index.py --online` warns about a
listed plugin whose latest release declares a view built for a newer Qt.

## Testing

Unit-test your stage implementation in isolation. Inject a mock `IStageServices`
rather than depending on a live host. The skeleton template includes example
Google Test suites and CMake test registration.

Recommended ctest labels for plugin test suites:

| Label | Scope |
|-------|-------|
| `unit` | All fast, isolated stage unit tests |
| `sources` | Source stage tests |
| `transforms` | Transform stage tests |
| `sinks` | Sink stage tests |

## Distribution

### GitHub release assets

Publish platform binaries as GitHub release assets using the naming convention:

```
orc-plugin_<stage-name>_<platform>[_abi<N>].<ext>
```

where `<platform>` is `linux`/`macos`/`windows`, `<ext>` is
`so`/`dylib`/`dll`, and the optional `_abi<N>` token records the host ABI the
binary was built against (e.g. `orc-plugin_my-stage_linux_abi18.so`). Examples:

```
orc-plugin_my-stage_linux.so            # untagged
orc-plugin_my-stage_linux_abi18.so      # ABI-tagged (recommended)
orc-plugin_my-stage_macos_abi18.dylib
orc-plugin_my-stage_windows_abi18.dll
```

The name is a validity convention only: the host does not choose an asset by
parsing names. Every release must also upload an `orc-plugin-manifest.yaml`
asset declaring each binary's platform, host ABI, toolchain tag and SHA-256
digest; the host installs the artifact the manifest declares for its platform,
pre-checks the declared ABI and toolchain tag against its own, and reports
"needs a rebuild" or the precise mismatch before it ever downloads a build it
cannot load. A release without a valid manifest cannot be installed or updated
to. See the [release manifest](plugin-architecture.md#release-manifest)
reference and the [Plugin Publishing Guide](plugin-publishing.md).

The skeleton CI workflows produce and upload these artifacts automatically on
tagged releases.

### Registry entry

Users register your plugin by adding an entry to their plugin registry YAML:

```yaml
- plugin_id:          com.example.my-stage
  plugin_version:     "1.0.0"
  source_repo_url:    https://github.com/example-org/orc-plugin_my-stage
  artifact_source:    github_release_asset
  release_asset_url:  https://github.com/example-org/orc-plugin_my-stage/releases/download/v1.0.0/orc-plugin_my-stage_linux_abi19.so
  release_tag:        v1.0.0
  release_asset_name: orc-plugin_my-stage_linux_abi19.so   # platform-specific
  target_platform:    linux
  required_host_abi:  19
  enabled:            true
  trust_state:        untrusted
  license_spdx:       MIT
  sha256:             9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
```

The host downloads and caches the binary automatically the first time it
starts with this entry present — but only once the entry is trusted;
entries with `trust_state: untrusted` are neither downloaded nor loaded.
Trust is always an explicit confirmation, in both front ends: adding,
installing, updating or enabling a plugin (local file, URL, or from the
curated index) asks up front — a dialog in the GUI Plugin Manager, a `[y/N]`
prompt in `orc-cli plugins`, which takes `--yes` for scripted use. Entries
supplied any other way — such as the hand-written snippet above — default to
untrusted; the Plugin Manager shows them unticked in its **Enabled** column
and ticking it prompts the same confirmation before the entry becomes
loadable, and `orc-cli plugins enable <selector>` asks the same question.
`orc-cli plugins trust <selector>` grants it directly. The SHA-256 digest your
release manifest declares is what users record in the optional `sha256` field:
the host then verifies the download (and every cache hit) against it and
quarantines mismatching files. Without a `sha256`
the plugin still loads, with a warning that its integrity was not verified.
Plugin binaries are not code-signed; see the "Distribution integrity"
section of
[plugin-architecture.md](plugin-architecture.md#distribution-integrity) for
exactly what is and is not verified.

### Curated index and discovery

To reach users without hand-written YAML, list your plugin in the curated
index ([`orc-plugin-registry/`](https://github.com/decode-orc/decode-orc/blob/main/orc-plugin-registry/README.md){target="_blank"}). Open a
pull request adding an entry naming your repository; a maintainer's merge
publishes it immediately, and every release you publish afterwards is
installable without a further index change. Users then discover and install it
without knowing your URL:

```console
$ orc-cli plugins search                 # the whole index
$ orc-cli plugins search deinterlace     # or just the matches
$ orc-cli plugins info com.example.my-stage
$ orc-cli plugins install com.example.my-stage   # asks to confirm trust
```

The GUI exposes the same flow through **Plugin Manager → Browse Plugins…**.
Installing asks the user to confirm that the binary may run — the browse
dialog with a trust dialog, the CLI with a `[y/N]` prompt (`--yes` to confirm
without prompting) — and records the entry trusted, with the `sha256` and
`abi` its [release manifest](plugin-architecture.md#release-manifest)
declares. Hosts resolve the
artifact the manifest declares for their platform and ABI, so a user on an
unsupported host is told "no compatible build for this host" instead of
downloading an incompatible binary.

## Versioning and Compatibility Policy

### Binary compatibility model

The plugin boundary is a **C++ ABI**, not a C ABI. `std::shared_ptr`,
`std::string`, STL containers, exceptions, and virtual-function tables all
cross the host/plugin boundary. Matching version numbers are therefore
necessary but not sufficient: a plugin must be built with the same compiler
family, the same C++ standard library, and a compatible build configuration
as the host. On Windows this includes the CRT flavour — a Debug-CRT plugin
cannot be loaded by a Release-CRT host.

Since ABI v5 this requirement is enforced at load time: the descriptor's
`toolchain_tag` field (populated by the `ORC_SDK_TOOLCHAIN_TAG` macro, and
automatically by `ORC_STAGE_PLUGIN_DESCRIPTOR`) encodes the compiler family
and major version, the C++ standard library, and — on Windows — the CRT
flavour, e.g. `gcc14/libstdc++`, `clang17/libc++`,
`msvc19/msvc-stl/release-crt`. The host requires the plugin's tag to equal
its own tag exactly and rejects the plugin with a diagnostic naming both
tags otherwise. The policy is deliberately conservative, consistent with the
exact-match rule for the version numbers.

The host requires **exact equality** for both `host_abi_version` and
`plugin_api_version`; a mismatch in either causes the plugin to be rejected
with a logged diagnostic. The `services_size` field in `OrcPluginServices`
is an intra-version safety net only: it guards access to service-table
fields appended within the current ABI version. It is not a cross-version
compatibility mechanism.

### Version history

The table below is generated from `orc/sdk/abi_history.yaml` — the single
source of truth for the ABI/API version log. Do not edit it by hand; run
`tools/gen_abi_history_docs.sh` and splice the output between the markers. The
`AbiVersionDocsSync` CTest (label `sdk`) fails when this block is stale.

<!-- BEGIN GENERATED ABI VERSION HISTORY (source: orc/sdk/abi_history.yaml; regenerate with tools/gen_abi_history_docs.sh) -->

| `host_abi_version` | `plugin_api_version` | Change |
|--------------------|----------------------|--------|
| 1 | — | Initial internal release; `plugin_api_version` not yet in descriptor |
| 2 | 1 | `plugin_api_version` added to `StagePluginDescriptor`; public SDK headers published |
| 3 | 1 | `OrcPluginServices` table added; `orc_register_stage_plugin` now receives `const OrcPluginServices*` as its first parameter; plugins must use the services table for logging instead of resolving host symbols directly |
| 4 | 2 | Decode-Orc 2.0: `VideoFrameRepresentation` replaces `VideoFieldRepresentation` as the primary frame-data contract; `DAGStage::execute()` operates on frame-based artifacts; `DropoutRun` replaces `DropoutRegion`; `FieldID`/`FieldIDRange` removed — use `FrameID`/`FrameIDRange`. All plugins must be rebuilt against the v2.0 SDK |
| 5 | 2 | `StagePluginDescriptor` gains the appended `toolchain_tag` field (populate with `ORC_SDK_TOOLCHAIN_TAG`); the loader requires the plugin's tag to equal the host's exactly. Registration helpers (`ORC_STAGE_PLUGIN_DESCRIPTOR`, `ORC_DEFINE_STAGE_PLUGIN`) added in `<orc/abi/orc_plugin_registration.h>` |
| 6 | 2 | Multi-track audio: `VideoFrameRepresentation`'s single-track audio accessors (`audio_locked()`, `get_audio_sample_count(FrameID)`, `get_audio_samples(FrameID)`) are replaced by a track-indexed API — `audio_track_count()`, `get_audio_track_descriptor(track)`, per-track frame-locked accessors, and per-track free-running stream accessors (`get_audio_stream_pair_count` / `get_audio_stream_samples`). New contract header `<orc/stage/audio_track.h>`. The vtable layout change requires all plugins to be rebuilt |
| 7 | 2 | Channel-pair audio (SMPTE 272M-1994): the track-indexed audio API is replaced by the channel-pair API — `audio_channel_pair_count()`, `get_audio_channel_pair_descriptor(pair)`, and `get_audio_samples(pair, id)` returning 24-bit-in-int32 samples. All audio is 48 kHz frame-locked (synchronous); the free-running stream accessors are removed. Contract header `<orc/stage/audio_track.h>` is replaced by `<orc/stage/audio/audio_channel_pair.h>`. The vtable layout change requires all plugins to be rebuilt |
| 8 | 2 | `VideoFrameRepresentation` gains `prime_audio_decode()`: a hook that forces a deferred whole-stream audio decode (e.g. EFM audio) to run up front with progress reporting, forwarded down the wrapper chain so sinks can meter it on the progress dialog. The appended virtual changes the vtable layout, requiring all plugins to be rebuilt |
| 9 | 2 | `OrcPluginServices` gains the appended `observation_service` pointer (`IObservationService`, new contract header `<orc/stage/observation/observation_service_interface.h>`): a host-owned service that runs the standard observers by stable string id, reached via `plugin::get_observation_service()`. Guarded by `services_size`; older hosts leave it null. Appended field only — plugins need not be rebuilt to keep working against ABI 8 behaviour |
| 10 | 2 | The concrete observer classes (the nine `<orc/stage/observation/*_observer.h>` headers — `BiphaseObserver`, `WhiteSNRObserver`, …) and the `Observer` base (`<orc/stage/observation/observer.h>`) are removed from the plugin SDK: observers are now host-internal and reached exclusively through the `IObservationService` added in ABI 9, selected by stable string id. `orc-sdk-support` no longer ships observer object code, and the deprecated pre-tier observation include-path shims (`<orc/stage/observers/...>` and the flat `<orc/stage/observation_*.h>` paths) are removed. `observation_schema.h`, `observation_context*.h`, and `observation_service_interface.h` remain the contract. Source-breaking for any plugin still including the observer classes — migrate to `IObservationService::create_observer(id)` |
| 11 | 3 | `FrameDescriptor` drops the `colour_frame_index`, `frame_number`, and `timecode` fields. Signal-measured facts are no longer baked in by source stages: colour-sequence phase is measured uniformly for TBC and CVBS sources via the host `colour_frame_phase` observer, reached through the new contract header `<orc/stage/observation/colour_frame_phase_query.h>` (`orc::observation::measure_frame_phase()` / `measure_colour_frame_index()`), and VBI picture numbers / timecodes come from the host `biphase` observer's interpreted `vbi.*` keys (the dropped `frame_number`/`timecode` fields were never populated by any source). The analysis-sink result types in `<orc/stage/common_types.h>` also become canonical per-frame records: the aggregated-per-bucket fields are removed (`FrameDropoutStats` now carries integer `dropout_count` and `dropout_length_samples` in place of the double `dropout_count`/`total_dropout_length`; `FrameSNRStats` drops `field_count`/`white_snr_count`/`black_psnr_count`; `FrameBurstLevelStats` drops `field_count`), each record now representing exactly one analysed frame with display bucketing moved to a host-side decimation utility. `ObserverInfo` in `<orc/stage/observation/observation_service_interface.h>` also gains an appended `bool stateless` field classifying whether an observer's per-frame result is order-independent (stateless) or models a cross-frame stream (stateful, e.g. `closed_caption`, `colour_frame_phase`); it drives background-scheduler frame ordering and never changes a per-frame result. `VideoFrameRepresentation` in `<orc/stage/video_frame_representation.h>` gains an appended virtual `video_passthrough_source(FrameID)` (default nullptr) letting a transform declare, from metadata alone, that a frame's CVBS video content is byte-identical to its upstream input; the host uses it to share frame-content-keyed stored observations across stages instead of re-analysing identical frames. Layout- and source-breaking for any plugin reading the removed fields or these result structs. |
| 12 | 4 | `NodeTypeInfo` drops the free-text `menu_category` field and the unused `SinkCategory` enum/`sink_category` field. The Add Stage menu category is no longer plugin-declared: it is derived from the stage's `NodeType` via the new `StageCategory` enum and `stage_category_for()` helper (SOURCE → Source, TRANSFORM/MERGER/COMPLEX → Transform, ANALYSIS_SINK → Analysis, SINK → Sink), so a plugin can never place a stage under an invented category. Layout- and source-breaking: the `NodeTypeInfo` constructor loses its trailing `SinkCategory` and `menu_category` parameters. |
| 13 | 4 | Format-specific results contracts out, a generic one in. The new `<orc/stage/tooling/catalogue_results.h>` carries `CatalogueDataset` and the `ICatalogueResults` interface a stage implements to hand the host a browsable set of items with something drawable for each — a character-cell grid, a 2D display list, a text document or a table — and `StageToolKind` gains `CatalogueBrowser`, appended so the values released plugins encode do not move. `<orc/stage/analysis_sink_results.h>` in exchange loses every teletext and NABTS type it carried (`TeletextCataloguedPage`, `TeletextAnalysisDataset`, `ITeletextAnalysisResults`, `NabtsCataloguedRecord`, `NabtsAnalysisDataset`, `INabtsAnalysisResults`, `nabts_caption_cues()` and their neighbours), along with its includes of the format decoders; it keeps only `IDropoutAnalysisResults`, `ISNRAnalysisResults` and `IBurstLevelAnalysisResults`. Those types were never generic plugin contract, and while they sat in the ABI-frozen stage tier a teletext catalogue change bumped the host ABI for every plugin in the ecosystem; they now live privately with their sink plugins. The five `orc/support/{teletext_*,nabts_page}.h` decoder headers leave the SDK in the same breath. Layout- and source-breaking for anything that included that header for the removed types, which nothing outside this repository could have done. |
| 14 | 4 | `CatalogueDrawOp` gains a `blink_to` colour. A blink process is defined by ANSI X3.110 §5.3.2.7.2 over a colour map *entry*, alternating it with a blink-to entry the service names (§5.3.2.7.3) — so a blinking figure need not appear and disappear, it can twinkle between two colours, and a renderer given only `blinking` cannot tell which was meant. The member is inserted beside `blinking` rather than appended, so the struct's layout moves; it is meaningful only where `blinking` is set and defaults to black, which is the ground the C1 BLINK START of §6.2.8.1 blinks to in colour modes 0 and 1. |
| 15 | 4 | `CatalogueDisplayList` gains `display_aspect_height`, `nominal_width`, `nominal_height` and `pixel_aligned`. A display list is geometry rather than pixels, but a source authored for a receiver of a particular resolution has geometry that only resolves against that resolution: ANSI X3.110 §5.3.2.2.6 defines the logical pel over "all of those pixels that lie under any portion of the logical pel as it is mapped to the display screen" and guarantees it "will always map to at least one and possibly many display pixels", so a stroke of no stated width is one *pixel* wide. A renderer given no resolution can only fall back on a device pixel, which makes stroke width depend on how large the list happens to be drawn. `nominal_width` and `nominal_height` state the grid; both default to 0, which is the previous behaviour. `display_aspect_height` separates the shape the drawable area is drawn at from `aspect_height`, which keeps its meaning as the extent of unit y the list covers — the two differ wherever the authoring receiver's pixels were not square, as §4.2.2's 4:3 display area over a 256 by 200 grid makes them. It defaults to 0, meaning square pixels and so the previous shape. `pixel_aligned` says the operations tile that grid — one per pixel of it, or per run of pixels along a row — rather than describing shapes drawn on it, which is a different thing to draw: the edges are the source's own pixels rather than an artefact of drawing them, so a renderer lays them down hard and aligned to whole device pixels. Smoothing them blurs pixels a receiver had sharp, and antialiasing both sides of the boundary where two runs abut leaves a seam along a join that should be invisible; it defaults to false, which is the previous behaviour for every list that does not say otherwise. The four members are appended, so no existing member moves, but the struct grows and `CatalogueDisplayList` is embedded by value in `CataloguePayload`; layout-breaking for anything built against abi 14. `CatalogueSchema` gains `view_label`, `view_options` and `view_option`, and `ICatalogueResults` gains a virtual `catalogue(view_option)`, so a stage can offer the reader several ways of presenting the same items and be re-asked for the one picked. Which receiver a page is resolved against is exactly such a property — it changes nothing the decode found and everything about how it looks — and carrying it only as a stage parameter puts it where changing it rebuilds the graph and discards the results being looked at. The new virtual has a default that ignores the option and returns `catalogue()`, so a stage offering none needs no change, but it takes a vtable slot: the interface's layout moves and every plugin implementing it must be rebuilt. `CatalogueSchema` further gains `toggles`, and `ICatalogueResults` a virtual `catalogue(view_option, active_toggles)`, so a browser can offer presentation choices that are on or off as well as the several it chooses between. The two are separate controls because they are independent axes: folding a two-state choice into the view dropdown multiplies the options out rather than adding to them, and every further such choice doubles the list again. The new `CatalogueViewToggle` carries the state the catalogue was built under, so the host shows what it is looking at without remembering what it asked for, and a stage offering no toggles leaves the browser exactly as it was. This virtual likewise defaults to forwarding to `catalogue(view_option)` and likewise takes a vtable slot. |
| 16 | 4 | The vectorscope contract grows a second acquisition mode. `UVSample` carried a field index and nothing else, which is all a grading scope needs because the decoder has already thrown away everything a measurement scope reads: a composite acquisition plots the burst as well as the picture and does not undo the PAL V-switch, so a sample is only interpretable alongside which part of the line it came from, which line that was, and — ITU-R BT.470-6 Table 2 item 2.16 — the V-switch state of that line. `UVSample` therefore gains `sample_class`, `line_phase`, `reserved` and `line_number`; the struct grows and every sample of a `VectorscopeData` moves. `VectorscopeData` gains `acquisition_mode`, `sample_window`, `first_line`, `last_line`, `sample_stride` and an embedded `VectorscopeMeasurements` so the renderer can pick the right graticule and show burst readouts, and `VectorscopeData` is embedded by value in `ColourFrameCarrier`, so that carrier's layout moves too. The new `VectorscopeAcquisitionMode`, `VectorscopeSampleClass`, `VectorscopeLinePhase` and `VectorscopeSampleWindow` enums are the vocabulary those members are stated in. `PreviewCoordinate` gains `vectorscope_window`, `vectorscope_first_line` and `vectorscope_last_line`: a composite scope's line-select is part of the request, not of the stage's configuration, and putting it in the shared coordinate keeps it out of the graph. Which of the two acquisitions runs is deliberately *not* among them — a colour-domain data type has decoder planes to plot and a signal-domain one has a carrier to demodulate, so the type already settles it and a second, independently settable mode could only disagree with it. The three members are appended, so no existing member moves, but the struct grows. |
| 17 | 4 | The preview contract grows a second pixel delivery. A render whose frame ends up on a graphics device does not need display RGB from the render worker: converting decoder-domain components to sRGB is a pass over four million samples that a fragment shader does for free, so `PreviewRenderResult` can now carry the components themselves. `PreviewPlanes` holds them — float Y/U/V, or composite samples alone in the signal domain — in the display row order the image would have had, together with every constant `render_preview_from_colour_carrier()` would have applied: the picture-black floor and the Y and U/V excursions, the composite modulation factors, the luma coefficients of the matrix named by the carrier's colorimetry, and a shared tabulation of the transfer decode and sRGB encode. It also carries the dropout regions, whether the renderer would have burned them in, and — as `PreviewPlaneBand` rectangles — the inactive area a masked preview dims, because the consumer draws those over the converted frame rather than into the samples. `PreviewPixelDelivery` names which representation was asked for and `PreviewPlaneDomain` which set of planes came back; the two deliveries are exclusive, because producing both would pay the conversion the planes exist to avoid. `PreviewRenderResult` embeds `PreviewPlanes` by value ahead of `vectorscope_data`, so the struct grows and its trailing members move. |
| 18 | 4 | `VideoFrameRepresentation` gains four appended virtuals for a sequential, possibly open-ended source (e.g. `cvbs_stream_source`/`tbc_stream_source` reading stdin with no declared `frame_count`): `is_exhausted()` and `stream_error()` (default false/empty) report a stream that has permanently ended; `has_unbounded_frame_range()` (default false) flags a `frame_range()` whose upper bound is a placeholder, not the real length; `max_concurrent_frame_requests()` (default `SIZE_MAX`) caps how many frames a consumer may request at once within the source's read-ahead window. `VideoFrameRepresentationWrapper` forwards all four. The appended virtuals change the vtable layout, requiring all plugins to be rebuilt; the defaults keep plugin source compatible, so the API version is unchanged |
| 19 | 4 | `FrameDescriptor` gains three appended fields, `held_frames_before` and `held_frames_after` (default 0): how far either side of the frame its picture is held, as the stacker finds it from the still-frame repeats it stacks, and `held_checked` (default false): whether a stage looked. The video sink's `held:` decoder schedule entry and `still_only` noise reduction read them. The struct grows, so every plugin must be rebuilt; the defaults keep plugin source compatible, so the API version is unchanged notes: >- ABI-neutral while abi 19 is current: `orc/stage/cvbs_signal_constants.h` gains `kPalMColourBurstStart`/`kPalMColourBurstEnd`, and the inline `colour_burst_range()` returns them for PAL_M instead of NTSC's window (ITU-R BT.1700-1: PAL-M's burst starts 0.5 us later). Header-only constants and an inline function; no layout or vtable change. A plugin built against the older header keeps the NTSC window for PAL_M until it is rebuilt. Also ABI-neutral: new contract header `orc/stage/observation/stage_measurement_store.h` adds the optional capability interface `IStageMeasurementConsumer` (reached by `dynamic_cast`, like the other stage capabilities) and the `IStageMeasurementStore` the host hands it, through which a stage keeps a whole-input measurement with the node's observations. No existing layout or vtable changes; a host that predates it never offers a store and the stage measures for itself. Also ABI-neutral: in `orc/stage/audio/audio_sample_feed.h`, the inline `audio_carrier_to_s32()` multiplies by 256 instead of shifting left by 8, because shifting a negative value is undefined in C++17. The result is the same for every 24-bit carrier; no layout or vtable change. |
| 20 | 4 | Stdio piping and the stream source stages are removed (issue #378). The contract header `orc/stage/streaming_capability.h` and its `IStreamingCompatibility` interface are removed, as are the support-tier headers `orc/support/pipe_io.h` (the "-" path convention, named-pipe detection and network stream URLs) and `orc/support/throttled_ring_reader.h`. `VideoFrameRepresentation` loses three of the four virtuals ABI 18 appended for open-ended sources — `is_exhausted()`, `stream_error()` and `has_unbounded_frame_range()` — and `VideoFrameRepresentationWrapper` no longer forwards them; `max_concurrent_frame_requests()` stays, now describing the readers a bounded frame cache is sized for. The reserved parameter name `kStreamReaderCountParameter` is removed from `orc/stage/params/parameter_types.h`. The vtable changes, so every plugin must be rebuilt; a plugin that inherits `IStreamingCompatibility` or overrides a removed virtual must drop it. The stage contract is otherwise unchanged, so the API version is unchanged |
| 21 | 4 | The host catalogue contract is removed (issue #377). The contract header `orc/stage/tooling/catalogue_results.h` is gone, with `CatalogueDataset`, the `ICatalogueResults` interface and `kCatalogueBrowserContractId`, and `StageToolKind` loses `CatalogueBrowser`, so `PluginView` moves from 6 to 5. The host no longer has a catalogue browser of its own: the teletext plugin, its only user, presents its catalogue from its own view binary through `StageToolKind::PluginView`, in a format private to the plugin, so a change to it no longer bumps the host ABI for every plugin. An enum value moves and a contract header is removed, so every plugin must be rebuilt; a plugin that names `CatalogueBrowser` or includes the removed header must move to a plugin view. While ABI 21 was still unreleased, `orc/stage/analysis_sink_results.h` went too (issue #382), with `IDropoutAnalysisResults`, `ISNRAnalysisResults` and `IBurstLevelAnalysisResults`: the three analysis sinks answer their views' requests and report to orc-cli through `IStageResultsReportProvider` instead. Nothing else in the stage contract changes, so the API version is unchanged |

<!-- END GENERATED ABI VERSION HISTORY -->

### ABI impact decision table

Use this table to decide whether a change forces a `host_abi_version` bump.
When in doubt, treat the change as ABI-breaking. The bump-procedure guard
(`tools/check_abi_bump.sh`, run in CI) references this table in its failure
message.

| Change | Bumps the host ABI? |
|--------|---------------------|
| Reorder, resize, or retype a `StagePluginDescriptor` field | **Yes** |
| Change an entrypoint signature or the `register_stage` callback contract | **Yes** |
| Add, remove, or reorder a virtual on a contract type crossing the boundary (e.g. `VideoFrameRepresentation`, `DAGStage`, an observer interface) | **Yes** |
| Change the layout of any type passed across the boundary by value or by reference | **Yes** |
| Change the meaning/encoding of an existing cross-boundary type without a layout change | **Yes** (semantic break) |
| **Append** a field to `OrcPluginServices` guarded by `services_size` | No |
| Append a new capability **interface** reached via an accessor (see below) | No |
| Change a `support`-tier header (`orc/support/…`: `lru_cache.h`, `logging.h`, helpers) | No — recompile at leisure |
| Change registration-template internals (`ORC_DEFINE_STAGE_PLUGIN`) without altering the descriptor layout | No |
| Edit comments, docs, or `instructions.md` | No |

#### Preferred additive patterns

Two mechanisms let the host and the stage contract grow **without** a bump —
prefer them over editing an existing layout:

- **`services_size` append-only growth.** New host services are appended to
  `OrcPluginServices`; a plugin checks `services->services_size` before reading
  a field its compiled ABI does not guarantee. This is the sanctioned way to
  add host capabilities.
- **Capability-accessor factoring.** To grow a churn-prone domain (audio has
  driven five of the last ABI bumps), add a **new interface reached through an
  accessor** rather than new virtuals on the one vtable every plugin depends
  on — e.g. `frame.audio()` returning an `IFrameAudio*` (header
  `orc/stage/audio/`). Adding a brand-new interface is additive and changes no
  existing vtable layout, whereas appending a virtual to
  `VideoFrameRepresentation` invalidates every plugin. Apply this
  opportunistically at the next planned `VideoFrameRepresentation` revision
  rather than as a standalone bump.

See also the compatibility-gating section of
[plugin-architecture.md](plugin-architecture.md#compatibility-gating).

### When the host increments a version

If `host_abi_version` increases, you must rebuild your plugin against the new
SDK. The host will refuse to load binaries built against an older ABI.

If `plugin_api_version` increases, your plugin must update its stage
implementation to match the new stage contract and recompile.

Neither increment is made without a corresponding Decode-Orc release and
migration notes.

## Repository Naming Convention

External plugin repository names should use the prefix `orc-plugin_`:

```
orc-plugin_<name>     e.g.  orc-plugin_skeleton
                            orc-plugin_example_stage
```

This convention applies to repositories hosted under the decode-orc
organisation and is recommended for all third-party plugin authors.
