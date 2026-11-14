/*
 * File:        stage_plugin_loader.cpp
 * Module:      orc-core
 * Purpose:     Runtime stage plugin loading contract and loader
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "include/stage_plugin_loader.h"

#include <fmt/format.h>
#include <orc/stage/file_io_interface.h>
#include <orc/support/colour_preview_conversion.h>
// Application logging (get_app_logger): plugin log messages are routed to
// the host application logger, not the core pipeline logger.
#include <logging.h>

#include <cstring>
#include <utility>

#include "../../sdk/include/orc/abi/orc_plugin_services.h"
#include "../../sdk/include/orc/plugin/orc_stage_services.h"
#include "core_observation_service.h"
#include "factories.h"
#include "include/plugin_safe_call.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace orc {
using orc::core_internal::plugin_safe_call;

namespace {

class FileWriterUint8ServiceAdapter final : public IFileWriterUint8 {
 public:
  explicit FileWriterUint8ServiceAdapter(
      std::shared_ptr<IFileWriter<uint8_t>> writer)
      : writer_(std::move(writer)) {}

  bool open(const std::string& filepath) override {
    return writer_ && writer_->open(filepath);
  }

  void write(const uint8_t* data, size_t count) override {
    if (writer_) {
      writer_->write(data, count);
    }
  }

  void write(const std::vector<uint8_t>& data) override {
    if (writer_) {
      writer_->write(data);
    }
  }

  void flush() override {
    if (writer_) {
      writer_->flush();
    }
  }

  void close() override {
    if (writer_) {
      writer_->close();
    }
  }

 private:
  std::shared_ptr<IFileWriter<uint8_t>> writer_;
};

class FileWriterUint16ServiceAdapter final : public IFileWriterUint16 {
 public:
  explicit FileWriterUint16ServiceAdapter(
      std::shared_ptr<IFileWriter<uint16_t>> writer)
      : writer_(std::move(writer)) {}

  bool open(const std::string& filepath) override {
    return writer_ && writer_->open(filepath);
  }

  void write(const uint16_t* data, size_t count) override {
    if (writer_) {
      writer_->write(data, count);
    }
  }

  void write(const std::vector<uint16_t>& data) override {
    if (writer_) {
      writer_->write(data);
    }
  }

  void flush() override {
    if (writer_) {
      writer_->flush();
    }
  }

  void close() override {
    if (writer_) {
      writer_->close();
    }
  }

 private:
  std::shared_ptr<IFileWriter<uint16_t>> writer_;
};

class FileWriterInt16ServiceAdapter final : public IFileWriterInt16 {
 public:
  explicit FileWriterInt16ServiceAdapter(
      std::shared_ptr<IFileWriter<int16_t>> writer)
      : writer_(std::move(writer)) {}

  bool open(const std::string& filepath) override {
    return writer_ && writer_->open(filepath);
  }

  void write(const int16_t* data, size_t count) override {
    if (writer_) {
      writer_->write(data, count);
    }
  }

  void write(const std::vector<int16_t>& data) override {
    if (writer_) {
      writer_->write(data);
    }
  }

  void flush() override {
    if (writer_) {
      writer_->flush();
    }
  }

  void close() override {
    if (writer_) {
      writer_->close();
    }
  }

 private:
  std::shared_ptr<IFileWriter<int16_t>> writer_;
};

class CoreStageServicesAdapter final : public IStageServices {
 public:
  std::shared_ptr<IFileWriterUint8> create_buffered_file_writer_uint8(
      size_t buffer_size) override {
    auto factories = Factories::instance();
    if (!factories) {
      return nullptr;
    }
    auto writer =
        factories->create_instance_buffered_file_writer_uint8(buffer_size);
    if (!writer) {
      return nullptr;
    }
    return std::make_shared<FileWriterUint8ServiceAdapter>(std::move(writer));
  }

  std::shared_ptr<IFileWriterUint16> create_buffered_file_writer_uint16(
      size_t buffer_size) override {
    auto factories = Factories::instance();
    if (!factories) {
      return nullptr;
    }
    auto writer =
        factories->create_instance_buffered_file_writer_uint16(buffer_size);
    if (!writer) {
      return nullptr;
    }
    return std::make_shared<FileWriterUint16ServiceAdapter>(std::move(writer));
  }

  std::shared_ptr<IFileWriterInt16> create_buffered_file_writer_int16(
      size_t buffer_size) override {
    auto factories = Factories::instance();
    if (!factories) {
      return nullptr;
    }
    auto writer =
        factories->create_instance_buffered_file_writer_int16(buffer_size);
    if (!writer) {
      return nullptr;
    }
    return std::make_shared<FileWriterInt16ServiceAdapter>(std::move(writer));
  }
};

struct RegisterContext {
  const StagePluginLoader::RegisterStageCallback* callback = nullptr;
  LoadedStagePlugin* plugin = nullptr;
  std::string* last_error = nullptr;
  // Keep-alive token shared with every factory registered by this plugin.
  std::shared_ptr<void> library;
};

void* open_shared_library(const std::string& path, std::string& error_message) {
#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(path.c_str());
  if (!handle) {
    error_message = "LoadLibrary failed";
    return nullptr;
  }
  return reinterpret_cast<void*>(handle);
#else
  // RTLD_LOCAL: each plugin's symbols stay private to that plugin. This
  // prevents ODR unification across plugins (e.g. two plugins carrying
  // different versions of a common helper, or SDK inline variables such as
  // orc::plugin::g_services resolving to the first-loaded plugin's copy).
  // Cross-boundary dynamic_cast still works: typeinfo names have default
  // visibility, so libstdc++ falls back to string comparison.
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* error = dlerror();
    error_message = error ? error : "dlopen failed";
    return nullptr;
  }
  return handle;
#endif
}

void close_shared_library(void* handle) {
  if (!handle) {
    return;
  }

#if defined(_WIN32)
  FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
  dlclose(handle);
#endif
}

void* get_symbol(void* handle, const char* symbol_name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol_name));
#else
  return dlsym(handle, symbol_name);
#endif
}

// RAII owner of an opened plugin shared library and its host service table.
// Shared (via shared_ptr) between the loader's handle entry, every registered
// stage factory, and every live stage instance; the library is closed when
// the last of those references is released. The service table lives here so
// that a plugin's stored OrcPluginServices pointer stays valid for as long as
// any of its stages can still run.
struct LoadedLibrary {
  explicit LoadedLibrary(void* h) : handle(h) {}
  ~LoadedLibrary() { close_shared_library(handle); }

  LoadedLibrary(const LoadedLibrary&) = delete;
  LoadedLibrary& operator=(const LoadedLibrary&) = delete;

  void* handle = nullptr;
  std::shared_ptr<OrcPluginServices> services;
};

}  // namespace

namespace core_internal {

std::function<DAGStagePtr()> make_keepalive_stage_factory(
    OrcStageFactoryFn factory, std::shared_ptr<void> library_keep_alive) {
  return
      [factory, keep_alive = std::move(library_keep_alive)]() -> DAGStagePtr {
        DAGStagePtr stage = factory();
        if (!stage) {
          return nullptr;
        }
        DAGStage* raw = stage.get();
        // The returned pointer's deleter owns both the stage and the keep-alive
        // token: the stage destructor runs first (while the plugin code is
        // still mapped), and only then is the token released, which may close
        // the library if this was the last reference.
        return DAGStagePtr(
            raw, [stage = std::move(stage), keep_alive](DAGStage*) mutable {
              stage.reset();
              keep_alive.reset();
            });
      };
}

}  // namespace core_internal

StagePluginLoader::~StagePluginLoader() { unload_all(); }

StagePluginLoader::LoadResult StagePluginLoader::load_plugin(
    const std::string& path,
    const RegisterStageCallback& register_stage_callback) {
  LoadResult result;

  std::string open_error;
  void* handle = open_shared_library(path, open_error);
  if (!handle) {
    result.error_message =
        "Failed to open plugin '" + path + "': " + open_error;
    return result;
  }

  // RAII ownership from here on: every early return below releases `library`,
  // which closes the shared library automatically. On success, ownership is
  // shared with each registered stage factory (see RegisterContext::library).
  auto library = std::make_shared<LoadedLibrary>(handle);

  auto* descriptor_fn = reinterpret_cast<OrcGetStagePluginDescriptorFn>(
      get_symbol(handle, kGetStagePluginDescriptorSymbol));
  auto* register_fn = reinterpret_cast<OrcRegisterStagePluginFn>(
      get_symbol(handle, kRegisterStagePluginSymbol));

  if (!descriptor_fn || !register_fn) {
    result.error_message =
        "Plugin '" + path + "' is missing required stage plugin entrypoints";
    return result;
  }

  const StagePluginDescriptor* descriptor = nullptr;
  std::string fault_error;
  // Fault-guard note (see plugin_safe_call.h): the guarded body only invokes
  // a raw plugin function pointer and assigns a raw pointer — no host-side
  // C++ objects are constructed inside the guarded region.
  if (!plugin_safe_call([&] { descriptor = descriptor_fn(); }, fault_error)) {
    result.error_message =
        "Plugin '" + path + "' crashed in descriptor function: " + fault_error;
    return result;
  }
  if (!descriptor) {
    result.error_message = "Plugin '" + path + "' returned null descriptor";
    return result;
  }

  // Copy version values from descriptor BEFORE any early return. Once the
  // library shared_ptr is released the memory is unmapped and descriptor
  // would be dangling.
  const uint32_t plugin_host_abi = descriptor->host_abi_version;
  const uint32_t plugin_api = descriptor->plugin_api_version;

  if (plugin_host_abi != kStagePluginHostAbiVersion) {
    result.error_message =
        "Plugin '" + path +
        "' ABI mismatch: plugin=" + std::to_string(plugin_host_abi) +
        ", host=" + std::to_string(kStagePluginHostAbiVersion);
    return result;
  }

  if (plugin_api != kStagePluginApiVersion) {
    result.error_message =
        "Plugin '" + path +
        "' API version mismatch: plugin=" + std::to_string(plugin_api) +
        ", host=" + std::to_string(kStagePluginApiVersion);
    return result;
  }

  // Toolchain tag (ABI v5): reject plugins built with an incompatible
  // compiler family/major version, C++ standard library, or (Windows) CRT
  // flavour. Reading the appended descriptor field is safe here because the
  // exact-match ABI check above guarantees the v5 descriptor layout.
  const std::string plugin_toolchain =
      sanitize_c_string(descriptor->toolchain_tag);
  if (plugin_toolchain != ORC_SDK_TOOLCHAIN_TAG) {
    result.error_message = "Plugin '" + path + "' toolchain mismatch: plugin=" +
                           (plugin_toolchain.empty() ? std::string("(missing)")
                                                     : plugin_toolchain) +
                           ", host=" ORC_SDK_TOOLCHAIN_TAG;
    return result;
  }

  // One copy per plugin id. A second copy (say an installed one beside a
  // development build's) is refused here, before it can register anything,
  // so its stages never collide with the first copy's.
  const std::string plugin_id = sanitize_c_string(descriptor->plugin_id);
  if (!plugin_id.empty()) {
    for (const auto& loaded : loaded_plugins_) {
      if (loaded.plugin_id != plugin_id) {
        continue;
      }
      result.superseded_by = loaded.path;
      result.error_message = "Plugin '" + plugin_id + "' at '" + path +
                             "' was not loaded: the copy at '" + loaded.path +
                             "' is already loaded";
      return result;
    }
  }

  // The service table lives inside LoadedLibrary so the plugin's stored
  // pointer stays valid for as long as any of its stages can still run.
  library->services = std::make_shared<OrcPluginServices>();

  PluginHandleEntry entry;
  entry.library = library;
  entry.plugin.path = path;
  entry.plugin.plugin_id = plugin_id;
  entry.plugin.plugin_version = sanitize_c_string(descriptor->plugin_version);
  entry.plugin.license_spdx = sanitize_c_string(descriptor->license_spdx);
  entry.plugin.is_core_plugin = descriptor->is_core_plugin;

  // Build the service table that the plugin receives as the first argument to
  // orc_register_stage_plugin().  Callbacks wrap host-internal functions via
  // stateless function pointers so that no host-internal symbols are resolved
  // directly via the dynamic linker from within the plugin.
  OrcPluginServices& services = *library->services;
  services = {};
  static CoreStageServicesAdapter stage_services_adapter;
  services.services_size = static_cast<uint32_t>(sizeof(OrcPluginServices));
  services.log = [](OrcPluginLogLevel level, const char* message) {
    auto logger = orc::get_app_logger();
    if (!logger || !message) {
      return;
    }
    switch (level) {
      case OrcPluginLogLevel::Trace:
        SPDLOG_LOGGER_TRACE(logger, "{}", message);
        break;
      case OrcPluginLogLevel::Debug:
        SPDLOG_LOGGER_DEBUG(logger, "{}", message);
        break;
      case OrcPluginLogLevel::Info:
        SPDLOG_LOGGER_INFO(logger, "{}", message);
        break;
      case OrcPluginLogLevel::Warn:
        SPDLOG_LOGGER_WARN(logger, "{}", message);
        break;
      case OrcPluginLogLevel::Error:
        SPDLOG_LOGGER_ERROR(logger, "{}", message);
        break;
      case OrcPluginLogLevel::Critical:
        SPDLOG_LOGGER_CRITICAL(logger, "{}", message);
        break;
    }
  };
  services.render_colour_preview =
      [](const ColourFrameCarrier* carrier) -> PreviewImage {
    if (!carrier) {
      return PreviewImage{};
    }
    return render_preview_from_colour_carrier(*carrier);
  };
  services.stage_services = &stage_services_adapter;
  // Host-owned observation service (ABI 9). Stateless, so a single shared
  // instance backs every plugin; its lifetime spans the whole process.
  static CoreObservationService observation_service;
  services.observation_service = &observation_service;

  std::string last_error;
  RegisterContext context{&register_stage_callback, &entry.plugin, &last_error,
                          library};
  const char* plugin_error = nullptr;

  bool register_ok = false;
  // Fault-guard note (see plugin_safe_call.h): this guarded region does NOT
  // meet the "raw C function pointers only" constraint — register_fn
  // re-enters the host through register_stage_trampoline, which allocates
  // std::string/std::function/std::vector. If the plugin faults mid-
  // registration, siglongjmp abandons those host-side objects in flight
  // (leaking memory and, in principle, leaving heap metadata inconsistent).
  // This residual risk is accepted: registration runs once at startup, and
  // surviving a faulty third-party plugin with a diagnostic is preferred
  // over crashing the host outright.
  if (!plugin_safe_call(
          [&] {
            register_ok = register_fn(
                library->services.get(), &context,
                &StagePluginLoader::register_stage_trampoline, &plugin_error);
          },
          fault_error)) {
    result.error_message =
        "Plugin '" + path +
        "' crashed during stage registration: " + fault_error;
    return result;
  }
  if (!register_ok) {
    std::string error_from_plugin = sanitize_c_string(plugin_error);
    if (!last_error.empty()) {
      result.error_message =
          "Plugin '" + path + "' stage registration failed: " + last_error;
    } else if (!error_from_plugin.empty()) {
      result.error_message =
          "Plugin '" + path +
          "' stage registration failed: " + error_from_plugin;
    } else {
      result.error_message = "Plugin '" + path + "' stage registration failed";
    }
    return result;
  }

  loaded_plugins_.push_back(entry.plugin);
  handle_entries_.push_back(std::move(entry));

  result.success = true;
  result.plugin = loaded_plugins_.back();
  return result;
}

void StagePluginLoader::unload_all() {
  // Releases only the loader's references. Each library is closed by
  // ~LoadedLibrary when the last registered factory or live stage instance
  // sharing it is destroyed (possibly right here, if none remain).
  handle_entries_.clear();
  loaded_plugins_.clear();
}

bool StagePluginLoader::register_stage_trampoline(void* context,
                                                  const char* stage_name,
                                                  OrcStageFactoryFn factory) {
  auto* register_context = reinterpret_cast<RegisterContext*>(context);
  if (!register_context || !register_context->callback || !factory ||
      !stage_name) {
    if (register_context && register_context->last_error) {
      *register_context->last_error =
          "invalid stage registration callback payload";
    }
    return false;
  }

  // Wrap the raw plugin factory so each created stage shares ownership of
  // the plugin library; the library cannot be unmapped while any stage
  // instance created through this factory is alive.
  StagePluginLoader::RegisterStageFactory wrapped_factory =
      core_internal::make_keepalive_stage_factory(factory,
                                                  register_context->library);

  const std::string stage_name_string(stage_name);
  const bool ok =
      (*register_context->callback)(stage_name_string, wrapped_factory);
  if (!ok) {
    if (register_context->last_error) {
      *register_context->last_error =
          "host rejected stage '" + stage_name_string + "'";
    }
    return false;
  }

  if (register_context->plugin) {
    register_context->plugin->registered_stage_names.push_back(
        stage_name_string);
  }

  return true;
}

std::string StagePluginLoader::sanitize_c_string(const char* value) {
  if (!value) {
    return "";
  }
  // Use strnlen to guard against non-null-terminated strings from untrusted
  // plugin binaries.  512 characters is well beyond any legitimate field
  // value; anything longer is truncated rather than letting the read run
  // off into unmapped memory.
  constexpr std::size_t kMaxLen = 512;
  return std::string(value, strnlen(value, kMaxLen));
}

}  // namespace orc
