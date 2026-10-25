# Plugin Author Guide

A step-by-step walkthrough that takes you from an empty directory to a working
stage plugin loaded into the Decode-Orc host. It builds against the **standalone
Plugin SDK package only** — no decode-orc source tree, and none of the host's
FFmpeg / yaml-cpp / PNG / SQLite3 dependency stack, is required.

This guide is the tutorial companion to two reference documents:

- [Plugin SDK Developer Guide](plugin-sdk.md) — the tiered header reference,
  CMake integration, stage interfaces, and the versioning/ABI policy.
- [Plugin Architecture](plugin-architecture.md) — runtime flow, compatibility
  gating, the registry, and the curated index.

When you are ready to ship, continue with the
[Plugin Publishing Guide](plugin-publishing.md).

The canonical minimal example referenced throughout is the in-repo CI fixture
[`orc-tests/fixtures/external-stage-plugin/`](https://github.com/decode-orc/decode-orc/tree/main/orc-tests/fixtures/external-stage-plugin){target="_blank"}
— three files that configure, build, and load exactly as an external plugin,
and that decode-orc CI builds against a freshly installed SDK on every push, so
it never drifts from the shipped package.

---

## 1. Obtain the SDK

You need the `decode-orc-plugin-sdk` CMake package on your machine. There are
two ways to get it.

### Option A — the published SDK tarball (recommended)

Decode-Orc CI publishes the SDK on its own as
`decode-orc-plugin-sdk-<version>-linux-x86_64.tar.gz` (headers + the
`orc-sdk-support` static library + CMake config + enforcement scripts),
attached to each tagged Decode-Orc release. The tarball is currently built for
Linux x86-64 only; on macOS and Windows use Option B. Unpack it anywhere and
point `CMAKE_PREFIX_PATH` at the unpacked directory:

```bash
mkdir -p ~/orc-sdk && tar -xzf decode-orc-plugin-sdk-*-linux-x86_64.tar.gz -C ~/orc-sdk
export ORC_SDK_PREFIX=~/orc-sdk/decode-orc-plugin-sdk-<version>-linux-x86_64
```

No decode-orc source tree is required. The package's only `find_dependency`
requirements are **spdlog** and **fmt**.

### Option B — install from a decode-orc checkout

From a decode-orc source tree, build and install to a scratch prefix:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
cmake --install build --prefix ~/orc-sdk-prefix
export ORC_SDK_PREFIX=~/orc-sdk-prefix
```

This prefix also contains the `orc-cli` / `orc-gui` binaries used later to load
and test your plugin. (With the tarball, use an `orc-cli` from a normal host
install.)

Either way, the prefix provides:

- the CMake package (`lib/cmake/decode-orc-plugin-sdk/`),
- the public SDK headers (`include/decode-orc-plugin-sdk/`: `<orc/abi/...>`,
  `<orc/stage/...>`, `<orc/support/...>`),
- the `orc-sdk-support` static library (`lib/liborc-sdk-support.a`),
- the enforcement gate scripts (`check_plugin_private_includes.sh`,
  `check_plugin_private_links.sh`).

---

## 2. Scaffold the project

You can clone the official skeleton for a scaffold that already includes unit
tests and CI workflows:

```bash
git clone https://github.com/decode-orc/orc-plugin_skeleton my-orc-plugin
```

> The skeleton may lag the SDK. If in doubt, mirror the always-in-sync fixture
> at [`orc-tests/fixtures/external-stage-plugin/`](https://github.com/decode-orc/decode-orc/tree/main/orc-tests/fixtures/external-stage-plugin){target="_blank"}.

Or start from scratch — a minimal plugin is just three files:

```
my-orc-plugin/
├── CMakeLists.txt
├── my_filter_stage.h      # your DAGStage subclass
└── plugin.cpp             # descriptor + registration
```

---

## 3. Implement a minimal transform stage

A stage subclasses `orc::DAGStage`. The header below is a copy-paste passthrough
transform — it forwards its single input artifact unchanged. Replace the body of
`execute()` with your own processing.

`my_filter_stage.h`:

```cpp
/*
 * File:        my_filter_stage.h
 * Module:      my-orc-plugin
 * Purpose:     Minimal passthrough transform stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Your Name
 */

#pragma once

#include <orc/stage/stage.h>
#include <orc/support/stage_instructions.h>

#include <map>
#include <string>
#include <vector>

namespace my_plugin {

class MyFilterStage : public orc::DAGStage {
 public:
  // Serves instructions.md (installed beside the plugin) to the host's help.
  ORC_STAGE_INSTRUCTIONS_MD

  std::string version() const override { return "1.0.0"; }

  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::TRANSFORM,
                             "my_filter",                 // stage id (unique)
                             "My Filter",                 // display name
                             "Passes frames through unchanged",  // description
                             1,                           // min inputs
                             1,                           // max inputs
                             1,                           // min outputs
                             1,                           // max outputs
                             orc::VideoFormatCompatibility::ALL};
  }

  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>& inputs,
      const std::map<std::string, orc::ParameterValue>& parameters,
      orc::ObservationContext& observation_context) override {
    (void)parameters;
    (void)observation_context;
    if (inputs.size() != 1) {
      throw orc::DAGExecutionError("MyFilterStage expects exactly one input");
    }
    return {inputs.front()};
  }

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }
};

}  // namespace my_plugin
```

`plugin.cpp` — the descriptor and registration. The two macros expand the entire
plugin boilerplate (descriptor version/toolchain fields, both entrypoints,
service-table storage, and stage registration):

```cpp
/*
 * File:        plugin.cpp
 * Module:      my-orc-plugin
 * Purpose:     Descriptor and registration for MyFilterStage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Your Name
 */

#include <orc/abi/orc_plugin_sdk.h>

#include "my_filter_stage.h"

#ifndef ORC_STAGE_PLUGIN_VERSION
#define ORC_STAGE_PLUGIN_VERSION "dev"
#endif

namespace {

constexpr orc::StagePluginDescriptor kPluginDescriptor =
    ORC_STAGE_PLUGIN_DESCRIPTOR("com.example.my-filter",   // plugin id
                                ORC_STAGE_PLUGIN_VERSION,
                                "GPL-3.0-or-later",         // SPDX license
                                false);                     // is_core = false

}  // namespace

ORC_DEFINE_STAGE_PLUGIN(kPluginDescriptor, my_plugin::MyFilterStage)
```

You never hand-set the ABI, API, or toolchain fields — the registration macro
injects them from the SDK you compiled against. See
[Implementing a Stage](plugin-sdk.md#implementing-a-stage) for parameters,
configuration status, host services, and stage tools.

---

## 4. Write `instructions.md`

Every stage documents itself with a single Markdown file shipped beside the
plugin binary. The host help dialog and `orc-cli stages help` render it,
`orc_add_stage_plugin()` copies it next to the built plugin automatically, and
the `ORC_STAGE_INSTRUCTIONS_MD` line in the stage class above is what reads it
back at runtime. Create `instructions.md` in the project root (beside
`CMakeLists.txt`):

```markdown
# My Filter

## What it does
Passes each input frame through unchanged. A template for real processing.

## When to use
As a starting point for a custom transform stage.

## Parameters
None.

## Tools
None.
```

Keep this file in sync with your stage whenever parameters, tools, or behaviour
change — this is the same non-negotiable rule bundled stages follow
(AGENTS.md §9.1).

---

## 5. Build against the SDK

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my-orc-plugin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(decode-orc-plugin-sdk REQUIRED)

orc_add_stage_plugin(
    my-orc-plugin-my-filter
    OUTPUT_NAME    orc-plugin_my-filter_linux
    PLUGIN_VERSION "${PROJECT_VERSION}"
    SOURCES        plugin.cpp
)
```

`orc_add_stage_plugin()` links the SDK (and only the SDK), injects the
`ORC_STAGE_PLUGIN_VERSION` define, copies `instructions.md` beside the binary,
sets the output name, and adds install rules. Configure and build with the SDK
prefix on `CMAKE_PREFIX_PATH`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$ORC_SDK_PREFIX"
cmake --build build -j
```

The built plugin lands in `build/lib/orc-stage-plugins/`.

---

## 6. Run the SDK enforcement gates standalone

The same hard-fail gates decode-orc runs in CI ship inside the SDK package. Run
them against your repository before every release — pointed at a tree that is
not a decode-orc checkout, both scripts run in **standalone mode** and scan the
whole tree as one plugin:

```bash
# Every include must resolve to an allowlisted SDK header, a plugin-local
# header, a standard-library/platform header, or a permitted third-party header.
bash "$ORC_SDK_PREFIX"/lib/cmake/decode-orc-plugin-sdk/check_plugin_private_includes.sh .

# No plugin target may link a private host target (orc-core, orc-common, ...).
bash "$ORC_SDK_PREFIX"/lib/cmake/decode-orc-plugin-sdk/check_plugin_private_links.sh .
```

Both exit non-zero and print a diagnostic on any violation. If an include is
rejected, you are reaching past the SDK contract — fix the include rather than
working around the gate.

---

## 7. Load the plugin into the host

Point the host's plugin search-path environment variable at your build output
and list what loaded:

```bash
ORC_STAGE_PLUGIN_PATHS=build/lib/orc-stage-plugins orc-cli plugins list
```

Your stage should appear in the list. `ORC_STAGE_PLUGIN_PATHS` accepts multiple
directories, separated by `:` (`;` on Windows). If the host refuses to load the binary, the
diagnostic names the mismatch — most often the ABI version or the toolchain tag
(the plugin must be built with the same compiler family and standard library as
the host; see
[Versioning and Compatibility Policy](plugin-sdk.md#versioning-and-compatibility-policy)).

Launch `orc-gui` with the same environment variable set to use the stage from
the graphical node editor; its **Help…** context-menu entry renders your
`instructions.md`.

---

## 8. Next steps

- Add unit tests for your stage (inject a mock `IStageServices`; the skeleton
  ships example Google Test suites) — see [Testing](plugin-sdk.md#testing).
- Add parameters, configuration status, host-service use, or stage tools — see
  [Implementing a Stage](plugin-sdk.md#implementing-a-stage).
- Package release assets, publish digests, and submit a curated-index entry —
  see the [Plugin Publishing Guide](plugin-publishing.md).
