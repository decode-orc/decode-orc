/*
 * File:        orc_plugin_abi.h
 * Module:      decode-orc Plugin SDK
 * Purpose:     Host ABI contract for decode-orc stage plugins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 *            Breaking changes to any type or constant defined here require a
 *            host_abi_version bump and a deprecation window.
 *
 * USAGE:
 *   Plugin implementors should include <orc/abi/orc_plugin_sdk.h> (the
 *   umbrella header) rather than including this file directly.
 *
 * VERSIONING:
 *   host_abi_version — binary compatibility boundary.
 *     Bumped when StagePluginDescriptor layout, entrypoint signatures, or the
 *     fundamental callback contract change in a binary-incompatible way.
 *     Plugins encode the host_abi_version they were compiled against.
 *     The host rejects plugins with a mismatched host_abi_version.
 *
 *   plugin_api_version — stage contract compatibility boundary.
 *     Bumped when the DAGStage interface, parameter schema, observation
 *     contract, or lifecycle semantics change in an incompatible way.
 *     Plugins encode the plugin_api_version they were compiled against.
 *     The host rejects plugins with a mismatched plugin_api_version.
 */

#pragma once

// SDK TIER: abi — frozen binary contract (descriptor/entrypoints/registration/
// services). Any change to this header bumps the host ABI version.

#include <cstdint>
#include <memory>

namespace orc {

// =============================================================================
// Forward declarations
// =============================================================================

/// Forward declaration of DAGStage. Full definition is in orc_stage_api.h and
/// in the stage-specific headers included by plugin implementations.
class DAGStage;

/// Forward declaration of the host service table.  Full definition is in
/// orc_plugin_services.h (included by the orc_plugin_sdk.h umbrella).
struct OrcPluginServices;

// =============================================================================
// Version constants
// =============================================================================

/// Host ABI version — binary compatibility boundary.
///
/// The full per-version change log is maintained in machine-readable form in
/// orc/sdk/abi_history.yaml (the single source of truth), from which the
/// "Version history" table in docs/technical/plugin-sdk.md is generated. When
/// bumping this constant, append a matching entry to that file — the
/// AbiHistorySync CTest (label "sdk") fails otherwise — and regenerate the
/// docs table with tools/gen_abi_history_docs.sh.
inline constexpr uint32_t kStagePluginHostAbiVersion = 17;

/// Preprocessor alias for kStagePluginHostAbiVersion.  Allows plugin code to
/// use conditional compilation:
///   #if ORC_SDK_ABI_VERSION >= 4
///     // use VideoFrameRepresentation
///   #endif
#define ORC_SDK_ABI_VERSION 17

static_assert(kStagePluginHostAbiVersion == ORC_SDK_ABI_VERSION,
              "ORC_SDK_ABI_VERSION must be kept in sync with "
              "kStagePluginHostAbiVersion; update both when bumping the ABI");

/// Plugin API version — stage contract compatibility boundary.
///
/// The per-version change log is tracked alongside the host ABI log in
/// orc/sdk/abi_history.yaml (see kStagePluginHostAbiVersion above).
inline constexpr uint32_t kStagePluginApiVersion = 4;

// =============================================================================
// Toolchain tag
// =============================================================================
//
// The plugin boundary is a C++ ABI: std::shared_ptr, std::string, exceptions,
// and vtables cross it. Matching version numbers are therefore necessary but
// not sufficient — the plugin must also be built with a compatible toolchain.
// ORC_SDK_TOOLCHAIN_TAG encodes the ABI-relevant build environment as a
// string literal at compile time:
//
//   <compiler-family><major> "/" <standard-library> [ "/" <crt-flavour> ]
//
// Examples: "gcc14/libstdc++", "clang17/libc++", "msvc19/msvc-stl/release-crt"
//
// Plugins populate StagePluginDescriptor::toolchain_tag with this macro; the
// loader requires exact string equality with the host's own tag (a
// deliberately conservative policy, consistent with the exact-match rule for
// the version numbers above).

#define ORC_SDK_TOOLCHAIN_STR_IMPL(x) #x
#define ORC_SDK_TOOLCHAIN_STR(x) ORC_SDK_TOOLCHAIN_STR_IMPL(x)

// Compiler family and major version. __clang__ is tested first because clang
// also defines __GNUC__ (and clang-cl defines _MSC_VER).
#if defined(__clang__)
#define ORC_SDK_TOOLCHAIN_COMPILER \
  "clang" ORC_SDK_TOOLCHAIN_STR(__clang_major__)
#elif defined(__GNUC__)
#define ORC_SDK_TOOLCHAIN_COMPILER "gcc" ORC_SDK_TOOLCHAIN_STR(__GNUC__)
#elif defined(_MSC_VER)
// MSVC guarantees binary compatibility across the v14x toolset family
// (_MSC_VER 19xx), so only the compiler major version is encoded.
#if _MSC_VER >= 2000 && _MSC_VER < 2100
#define ORC_SDK_TOOLCHAIN_COMPILER "msvc20"
#elif _MSC_VER >= 1900 && _MSC_VER < 2000
#define ORC_SDK_TOOLCHAIN_COMPILER "msvc19"
#else
#define ORC_SDK_TOOLCHAIN_COMPILER "msvc" ORC_SDK_TOOLCHAIN_STR(_MSC_VER)
#endif
#else
#define ORC_SDK_TOOLCHAIN_COMPILER "unknown-compiler"
#endif

// C++ standard library. The <memory> include above guarantees the library's
// identification macros are visible here. libstdc++'s debug mode
// (_GLIBCXX_DEBUG) changes container layouts, so it is a distinct library
// flavour for ABI purposes.
#if defined(_LIBCPP_VERSION)
#define ORC_SDK_TOOLCHAIN_STDLIB "libc++"
#elif defined(__GLIBCXX__)
#if defined(_GLIBCXX_DEBUG)
#define ORC_SDK_TOOLCHAIN_STDLIB "libstdc++-dbg"
#else
#define ORC_SDK_TOOLCHAIN_STDLIB "libstdc++"
#endif
#elif defined(_MSC_VER)
#define ORC_SDK_TOOLCHAIN_STDLIB "msvc-stl"
#else
#define ORC_SDK_TOOLCHAIN_STDLIB "unknown-stdlib"
#endif

// CRT flavour is ABI-relevant only with the MSVC runtime: a Debug-CRT plugin
// cannot be loaded by a Release-CRT host. On Itanium-ABI platforms the
// Debug/Release build type does not change the C++ ABI, so no configuration
// component is encoded there.
#if defined(_MSC_VER)
#if defined(_DEBUG)
#define ORC_SDK_TOOLCHAIN_TAG \
  ORC_SDK_TOOLCHAIN_COMPILER "/" ORC_SDK_TOOLCHAIN_STDLIB "/debug-crt"
#else
#define ORC_SDK_TOOLCHAIN_TAG \
  ORC_SDK_TOOLCHAIN_COMPILER "/" ORC_SDK_TOOLCHAIN_STDLIB "/release-crt"
#endif
#else
#define ORC_SDK_TOOLCHAIN_TAG \
  ORC_SDK_TOOLCHAIN_COMPILER "/" ORC_SDK_TOOLCHAIN_STDLIB
#endif

// =============================================================================
// Plugin entrypoint symbol names
// =============================================================================

/// Symbol name of the descriptor query entrypoint exported by every plugin.
inline constexpr const char* kGetStagePluginDescriptorSymbol =
    "orc_get_stage_plugin_descriptor";

/// Symbol name of the stage registration entrypoint exported by every plugin.
inline constexpr const char* kRegisterStagePluginSymbol =
    "orc_register_stage_plugin";

// =============================================================================
// Plugin descriptor
// =============================================================================

/// Plugin descriptor returned by orc_get_stage_plugin_descriptor().
///
/// All pointer fields must point to static storage valid for the lifetime of
/// the loaded plugin binary.
///
/// ABI note: fields must not be reordered. New fields are always appended.
/// Any layout change requires a host_abi_version bump.
struct StagePluginDescriptor {
  const char* plugin_id;       ///< Reverse-domain unique ID, e.g.
                               ///< "com.example.stage.my_filter"
  const char* plugin_version;  ///< Semantic version string, e.g. "1.2.3"
  uint32_t host_abi_version;  ///< Must equal orc::kStagePluginHostAbiVersion at
                              ///< load time
  uint32_t plugin_api_version;  ///< Must equal orc::kStagePluginApiVersion at
                                ///< load time
  const char*
      license_spdx;     ///< SPDX license expression, e.g. "GPL-3.0-or-later"
  bool is_core_plugin;  ///< true only for stages bundled with the Decode-Orc
                        ///< distribution
  const char* toolchain_tag;  ///< Build-environment tag; populate with
                              ///< ORC_SDK_TOOLCHAIN_TAG. Must equal the
                              ///< host's tag exactly at load time (ABI v5).
};

// =============================================================================
// Function pointer types
// =============================================================================

/// Factory function that allocates and returns a new stage instance.
/// The host calls this to create stage objects on demand.
using OrcStageFactoryFn = std::shared_ptr<DAGStage> (*)();

/// Signature of the orc_get_stage_plugin_descriptor() entrypoint.
using OrcGetStagePluginDescriptorFn = const StagePluginDescriptor* (*)();

/// Signature of the orc_register_stage_plugin() entrypoint.
///
/// @param services      Host-supplied service table.  Plugins must store this
///                      pointer before calling register_stage; the pointer
///                      remains valid for the lifetime of the plugin.
///                      Plugins should check services->services_size before
///                      accessing fields beyond what their compiled ABI version
///                      guarantees.
/// @param context       Opaque context pointer provided by the host; must be
///                      passed unchanged to the register_stage callback.
/// @param register_stage Callback the plugin calls once per stage it exports.
///                      Returns false if the host rejects the registration.
/// @param error_message Optional: plugin may set this to a static error string
///                      on failure. Must point to static storage.
/// @return true if all stages were registered successfully; false otherwise.
using OrcRegisterStagePluginFn =
    bool (*)(const OrcPluginServices* services, void* context,
             bool (*register_stage)(void* context, const char* stage_name,
                                    OrcStageFactoryFn factory),
             const char** error_message);

// =============================================================================
// Platform export macro
// =============================================================================

/// Apply this macro to both required plugin entrypoints so the linker exports
/// them with C linkage and default visibility.
///
/// Example:
///   ORC_STAGE_PLUGIN_EXPORT const orc::StagePluginDescriptor*
///   orc_get_stage_plugin_descriptor() { ... } ORC_STAGE_PLUGIN_EXPORT bool
///   orc_register_stage_plugin(void*, ...) { ... }
#if defined(_WIN32)
#define ORC_STAGE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define ORC_STAGE_PLUGIN_EXPORT \
  extern "C" __attribute__((visibility("default")))
#endif

}  // namespace orc
