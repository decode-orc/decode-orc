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

The SDK surface is split into three tiers with distinct stability promises:

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
| `<orc/stage/analysis_sink_results.h>` | Interfaces for accessing analysis sink stage results across |
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

#### `orc/support/` — compiled-into-plugin utilities

NOT part of the binary ABI. Changes never force an ABI bump; recompile the
plugin at the author's convenience.

| Header | Provides |
|--------|----------|
| `<orc/support/colour_preview_conversion.h>` | Render-boundary conversion from colour carriers to PreviewImage. |
| `<orc/support/dropout_util.h>` | Frame-flat ↔ field/line/sample coordinate conversion utilities |
| `<orc/support/eia608_decoder.h>` | EIA-608 Closed Caption Decoder for timed text conversion |
| `<orc/support/eia608_service_demux.h>` | EIA-608 data-channel / caption-vs-text service demultiplexer |
| `<orc/support/frame_line_util.h>` | Per-line sample count and offset helpers for 4FSC CVBS flat |
| `<orc/support/log_destination.h>` | Log output destination selection (console, file, both) |
| `<orc/support/logging.h>` | Logging system implementation |
| `<orc/support/lru_cache.h>` | Thread-safe least-recently-used cache |
| `<orc/support/preview_helpers.h>` | Helper functions for stage preview rendering |
| `<orc/support/stage_instructions.h>` | Runtime loader for a stage's instructions.md (platform file I/O) |
| `<orc/support/vbi_types.h>` | VBI line data structures shared by the VBI decoder and observers |
| `<orc/support/vbi_utilities.h>` | VBI bit-extraction and manchester/biphase decode helpers |

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
  coupling, allowlisted because several plugins legitimately use it for frame
  caching — preferable to each plugin vendoring a copy.

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
| `orc/core/analysis/*.h`, `orc/core/observers/*.h` | Host analysis/observer internals (the observer headers plugins may use live under `<orc/stage/observation/...>`) |
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
`decode-orc-plugin-sdk-<version>-<platform>.tar.gz` (headers + static library +
CMake config + enforcement scripts). Unpack it anywhere and point
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

Follow the artifact naming convention so the host can validate and cache
plugin binaries correctly:

```
orc-plugin_<stage-name>_<platform>.<ext>
```

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
view), implement `StageToolProvider` and return a vector of `StageToolDescriptor`
from `get_stage_tools()`. The host discovers these descriptors through the
presenter layer and routes them to the GUI without any hardcoded stage-name
dispatch.

#### Catalogue browsers

A stage whose triggered results are a list of things to browse advertises
`StageToolKind::CatalogueBrowser` with the contract id
`kCatalogueBrowserContractId` and implements `ICatalogueResults`
(`<orc/stage/tooling/catalogue_results.h>`). The host routes it to a generic
browser that knows nothing about the service being catalogued: the schema says
what the columns are called, what one item is called, and whether there is a
find box.

Two of the schema's fields let the reader change how the same items are
presented, and the host re-asks the stage each time one changes:

| Field | Control | Use for |
|-------|---------|---------|
| `view_options` | A dropdown | A choice between three or more presentations — which receiver a page is resolved against, say |
| `toggles` | Checkboxes beside it | A two-state choice independent of the rest — whether damaged data is presented as recovered or as transmitted |

Both are round-tripped as opaque ids through
`catalogue(view_option, active_toggles)`; ids the stage does not recognise mean
its own settings, so a host need not police what it hands back. Each carries the
state the catalogue was built under, so the host shows what it is looking at
without remembering what it asked for.

Keep the two apart. A two-state choice folded into the dropdown multiplies the
options out instead of adding to them, and every further such choice doubles the
list again. A stage that offers neither gets the plain browser and needs to
implement only the no-argument `catalogue()`.

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
binary was built against (e.g. `orc-plugin_my-stage_linux_abi8.so`). Examples:

```
orc-plugin_my-stage_linux.so            # legacy, untagged
orc-plugin_my-stage_linux_abi8.so       # ABI-tagged (recommended)
orc-plugin_my-stage_macos_abi8.dylib
orc-plugin_my-stage_windows_abi8.dll
```

Tagging lets a single release ship builds for several host ABI versions side
by side. When resolving a release the host prefers the asset tagged for its
own ABI (`_abi<host>`), then falls back to a legacy untagged name (validated
at load time), and only selects an asset tagged for a different ABI as a last
resort — reporting "needs a rebuild" before it ever downloads a build it
cannot load. Untagged names remain valid for backward compatibility.

The skeleton CI workflows produce and upload these artifacts automatically on
tagged releases.

### Registry entry

Users register your plugin by adding an entry to their plugin registry YAML:

```yaml
- plugin_id:          com.example.my-stage
  plugin_version:     "1.0.0"
  source_repo_url:    https://github.com/example-org/orc-plugin_my-stage
  artifact_source:    github_release_asset
  release_asset_url:  https://github.com/example-org/orc-plugin_my-stage/releases/download/v1.0.0/orc-plugin_my-stage_linux.so
  release_tag:        v1.0.0
  release_asset_name: orc-plugin_my-stage_linux.so   # platform-specific
  target_platform:    linux
  required_host_abi:  8
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
`orc-cli plugins trust <selector>` grants it directly. Publish the artifact's
SHA-256 digest
so users can record it in the optional `sha256` field: the host then
verifies the download (and every
cache hit) against it and quarantines mismatching files. Without a `sha256`
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
