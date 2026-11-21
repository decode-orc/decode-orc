/*
 * File:        orc_cli.cpp
 * Module:      orc-cli
 * Purpose:     CLI application with subcommands
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/stage/error_types.h>

#include <cstdlib>
#include <filesystem>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "command_nodes.h"
#include "command_plugins.h"
#include "command_process.h"
#include "command_stages.h"
#include "command_tools.h"
#include "crash_handler.h"
#include "logging.h"
#include "project_presenter.h"
#include "version.h"

namespace fs = std::filesystem;

using namespace orc;  // NOLINT(google-build-using-namespace)

/**
 * @brief Print command-line usage information
 *
 * Displays help text showing available commands, options, and examples
 * for the orc-cli command-line tool.
 *
 * @param program_name Name of the executable (argv[0])
 */
void print_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name << " <project-file> [options]\n";
  std::cerr << "       " << program_name << " plugins <subcommand> [options]\n";
  std::cerr << "       " << program_name << " stages <subcommand> [options]\n";
  std::cerr << "       " << program_name
            << " nodes <subcommand> <project> [options]\n";
  std::cerr << "       " << program_name
            << " tools <subcommand> <project> [options]\n";
  std::cerr << "\n";
  std::cerr << "Commands:\n";
  std::cerr << "  --process                      Process the whole DAG chain "
               "(trigger all sinks)\n";
  std::cerr << "\n";
  std::cerr << "Plugin Management (see 'plugins --help' for options):\n";
  std::cerr << "  plugins list                   Show installed plugins (core "
               "plugins are hidden)\n";
  std::cerr << "  plugins add <path>|--url URL   Add a plugin from a local "
               "file or releases URL\n";
  std::cerr << "  plugins remove <selector>      Remove a plugin from the "
               "persistent registry\n";
  std::cerr << "  plugins enable <selector>      Enable a registered plugin "
               "(grants trust if needed)\n";
  std::cerr << "  plugins disable <selector>     Disable a registered plugin\n";
  std::cerr << "  plugins trust <selector>       Mark a registered plugin as "
               "trusted\n";
  std::cerr << "  plugins untrust <selector>     Mark a registered plugin as "
               "untrusted\n";
  std::cerr << "  plugins search [term]          List or search the available "
               "plugins in the curated index\n";
  std::cerr << "  plugins info <selector>        Show details for an installed "
               "or available plugin\n";
  std::cerr << "  plugins install <id>           Install a plugin from the "
               "curated index\n";
  std::cerr << "  plugins updates                Check registered plugins for "
               "newer releases\n";
  std::cerr << "  plugins update <selector>|--all  Update registered plugins "
               "to their latest release\n";
  std::cerr << "  plugins doctor                 Report plugin search paths "
               "and load diagnostics\n";
  std::cerr << "\n";
  std::cerr << "Stage Introspection:\n";
  std::cerr << "  stages list                    List the stages this build "
               "can run\n";
  std::cerr << "  stages info <stage>            Describe a stage and every "
               "parameter it takes\n";
  std::cerr << "  stages help <stage>            Show the instructions shipped "
               "with a stage\n";
  std::cerr << "\n";
  std::cerr << "Project Editing (see 'nodes --help' for options):\n";
  std::cerr << "  nodes list <project>           List a project's stages with "
               "their status and tools\n";
  std::cerr << "  nodes get <project> <node>     Show a stage's parameters\n";
  std::cerr << "  nodes set <project> <node> key=value ...\n";
  std::cerr << "                                 Set a stage's parameters "
               "and save the project\n";
  std::cerr
      << "  nodes reset <project> <node>   Reset a stage's parameters, as "
         "its Reset button does\n";
  std::cerr << "\n";
  std::cerr << "Stage Tools (see 'tools --help' for options):\n";
  std::cerr << "  tools list <project> [<node>]  List the tools each stage "
               "offers\n";
  std::cerr << "  tools run <project> <tool> --node <node> [--apply]\n";
  std::cerr << "                                 Run a tool against a stage; "
               "--apply saves its result\n";
  std::cerr << "  tools refresh-config <project> <node> <tool>\n";
  std::cerr << "                                 Choose the tool 'tools "
               "refresh' runs for a stage\n";
  std::cerr << "  tools refresh <project> [--process]\n";
  std::cerr << "                                 Run every stage's refresh "
               "tool in order, then save\n";
  std::cerr << "\n";
  std::cerr << "Options:\n";
  std::cerr << "  --log-level LEVEL              Set logging verbosity\n";
  std::cerr << "                                 (trace, debug, info, warn, "
               "error, critical, off)\n";
  std::cerr << "                                 Default: info\n";
  std::cerr
      << "  --log-file FILE                Write logs to specified file\n";
  std::cerr << "  --log-out OUTPUT               Where to send log output "
               "(console, file, both)\n";
  std::cerr << "                                 'file' and 'both' need "
               "--log-file\n";
  std::cerr << "                                 Default: both\n";
  std::cerr << "  --safe-core-plugins            Permanently clear the user "
               "plugin registry\n";
  std::cerr << "                                 and ignore "
               "ORC_STAGE_PLUGIN_PATHS for this run\n";
  std::cerr << "                                 (core plugins only)\n";
  std::cerr << "\n";
  std::cerr << "Examples:\n";
  std::cerr << "  " << program_name << " project.orcprj --process\n";
  std::cerr << "  " << program_name
            << " project.orcprj --process --log-level debug\n";
  std::cerr << "  " << program_name
            << " project.orcprj --process --log-file run.log --log-out file\n";
  std::cerr << "  " << program_name << " plugins list\n";
  std::cerr << "  " << program_name
            << " plugins add /path/to/libmyplugin.so --id com.example.my "
               "--license MIT\n";
  std::cerr << "  " << program_name << " stages list --kind source --core\n";
  std::cerr << "  " << program_name << " stages info tbc_source\n";
  std::cerr << "  " << program_name << " nodes list disc.orcprj --json\n";
  std::cerr << "  " << program_name
            << " nodes set disc.orcprj \"Disc 1\" input_path=disc1.tbc\n";
  std::cerr << "  " << program_name
            << " tools run disc.orcprj field_mapping --node \"Frame Map\" "
               "--apply\n";
  std::cerr << "  " << program_name
            << " tools refresh stack.orcprj --json --process\n";
}

/**
 * @brief Whether an argument is one of the command-line pipeline options that
 * have been removed
 *
 * They are recognised so that an old script fails with a pointer to the
 * migration notes rather than a bare "unknown option".
 */
bool is_removed_option(const std::string& arg) {
  return arg == "--source" || arg == "-i" || arg == "--filters" ||
         arg == "-f" || arg == "--sink" || arg == "-o" ||
         arg == "--export-project" || arg == "--video-format" ||
         arg == "--source-type";
}

/**
 * @brief Main entry point for orc-cli
 *
 * Parses command-line arguments and dispatches to the appropriate command
 * handler. Supports processing projects, analyzing frame mappings, and
 * analyzing source alignments.
 *
 * @param argc Argument count
 * @param argv Argument values
 * @return Exit code (0 = success, non-zero = error)
 */
int main(int argc, char* argv[]) {
#if defined(__GLIBC__)
  // glibc gives threads up to 8 arenas per core, and a run makes tens of
  // thousands of short-lived threads that each free frame-sized buffers into
  // their own: measured on an 11-input stack, 4 arenas held 2.3 GB less at
  // peak and took two thirds less system time, at the same user time. An
  // explicit MALLOC_ARENA_MAX still wins.
  if (std::getenv("MALLOC_ARENA_MAX") == nullptr) mallopt(M_ARENA_MAX, 4);
#endif
  try {
    // Parse command line arguments
    std::string project_path;
    std::string log_level = "info";
    std::string log_file;
    std::string log_out = "both";
    bool log_out_provided = false;
    bool safe_core_plugins = false;

    // Command flags
    bool do_process = false;

    // Check for help or empty args
    if (argc < 2) {
      std::cerr << "Error: No project file or command specified\n\n";
      print_usage(argv[0]);
      return 1;
    }

    std::string first_arg = argv[1];
    if (first_arg == "--help" || first_arg == "-h") {
      print_usage(argv[0]);
      return 0;
    }

    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--safe-core-plugins") {
        safe_core_plugins = true;
        break;
      }
    }

    auto apply_safe_mode = []() -> bool {
#if defined(_WIN32)
      _putenv_s("ORC_STAGE_PLUGIN_PATHS", "");
#else
      unsetenv("ORC_STAGE_PLUGIN_PATHS");
#endif

      const auto clear_result =
          orc::presenters::ProjectPresenter::clearPluginRegistryForSafeMode();
      if (!clear_result.success) {
        std::cerr << "Error: Failed to clear plugin registry for safe startup: "
                  << clear_result.error_message << "\n";
        return false;
      }

      return true;
    };

    if (safe_core_plugins && !apply_safe_mode()) {
      return 1;
    }

    // Route 'plugins' subcommand — does not require a project file
    if (first_arg == "plugins") {
      // Rewrite argv so that argv[0] is the program name for usage messages,
      // and forward all remaining args to the plugins command handler.
      // plugins_command expects argv[0] = program name, argv[1] = subcommand.
      // Skip argv[1] ("plugins") since plugins_command handles its own
      // subcommand routing.
      std::vector<char*> plugins_argv;
      plugins_argv.push_back(argv[0]);
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--safe-core-plugins") {
          continue;
        }
        plugins_argv.push_back(argv[i]);
      }
      return cli::plugins_command(static_cast<int>(plugins_argv.size()),
                                  plugins_argv.data());
    }

    // Route 'stages' the same way: introspection needs no project file, and
    // --safe-core-plugins has already been applied above.
    if (first_arg == "stages") {
      std::vector<char*> stages_argv;
      stages_argv.push_back(argv[0]);
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--safe-core-plugins") {
          continue;
        }
        stages_argv.push_back(argv[i]);
      }
      return cli::stages_command(static_cast<int>(stages_argv.size()),
                                 stages_argv.data());
    }

    // Route 'nodes' the same way: it names its project as an argument rather
    // than taking the project-file form above.
    if (first_arg == "nodes") {
      std::vector<char*> nodes_argv;
      nodes_argv.push_back(argv[0]);
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--safe-core-plugins") {
          continue;
        }
        nodes_argv.push_back(argv[i]);
      }
      return cli::nodes_command(static_cast<int>(nodes_argv.size()),
                                nodes_argv.data());
    }

    if (first_arg == "tools") {
      std::vector<char*> tools_argv;
      tools_argv.push_back(argv[0]);
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--safe-core-plugins") {
          continue;
        }
        tools_argv.push_back(argv[i]);
      }
      return cli::tools_command(static_cast<int>(tools_argv.size()),
                                tools_argv.data());
    }

    // Parse all arguments
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];

      if (arg == "--help" || arg == "-h") {
        print_usage(argv[0]);
        return 0;
      } else if (arg == "--log-level" && i + 1 < argc) {
        log_level = argv[++i];
      } else if (arg == "--log-file" && i + 1 < argc) {
        log_file = argv[++i];
      } else if (arg == "--log-out" && i + 1 < argc) {
        log_out = argv[++i];
        log_out_provided = true;
      } else if (arg == "--safe-core-plugins") {
        // Handled before dispatch.
      } else if (arg == "--process") {
        do_process = true;
      } else if (is_removed_option(arg)) {
        std::cerr << "Error: " << arg
                  << " has been removed. Build a project (in orc-gui, or by "
                     "editing a template\n"
                     "with 'nodes set') and run it with --process. See "
                     "\"Removed command-line\n"
                     "pipelines and stdio piping\" in the migration notes.\n";
        return 1;
      } else if (arg[0] != '-') {
        // Positional argument - project file
        if (project_path.empty()) {
          project_path = arg;
        } else {
          std::cerr << "Error: Multiple project files specified\n";
          print_usage(argv[0]);
          return 1;
        }
      } else {
        std::cerr << "Error: Unknown option: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
      }
    }

    const auto parsed_log_out = orc::parse_log_destination(log_out);
    if (!parsed_log_out.has_value()) {
      std::cerr << "Error: Invalid --log-out value: " << log_out
                << " (expected console, file or both)\n\n";
      print_usage(argv[0]);
      return 1;
    }
    const orc::LogDestination log_destination = *parsed_log_out;

    if (project_path.empty()) {
      std::cerr << "Error: No project file specified\n\n";
      print_usage(argv[0]);
      return 1;
    }
    if (!do_process) {
      std::cerr << "Error: No command specified. You must use --process\n\n";
      print_usage(argv[0]);
      return 1;
    }

    // Initialize logging - both app logger and core logger
    orc::init_app_logging(log_level, "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v",
                          log_file, "cli", log_destination);
    orc::presenters::initCoreLogging(log_level,
                                     "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v",
                                     log_file, log_destination);

    // Only warn when the destination was asked for explicitly: the default
    // ("both" with no log file) is plain console logging, which is not
    // noteworthy.
    if (log_out_provided && log_destination != orc::LogDestination::kConsole &&
        log_file.empty()) {
      ORC_LOG_WARN(
          "--log-out {} was requested without --log-file; logging to the "
          "console only",
          log_out);
    }

    if (safe_core_plugins) {
      ORC_LOG_WARN(
          "Safe startup mode enabled: plugin registry cleared and "
          "ORC_STAGE_PLUGIN_PATHS ignored for this run");
    }

    // Initialize crash handler
    CrashHandlerConfig crash_config;
    crash_config.application_name = "orc-cli";
    crash_config.version = ORC_VERSION;
    crash_config.output_directory = fs::current_path().string();
    crash_config.primary_log_file = log_file;
    crash_config.enable_coredump = true;
    crash_config.auto_upload_info = true;

    // Add callback for custom application state
    crash_config.custom_info_callback = [&project_path]() -> std::string {
      std::ostringstream info;
      info << "Project file: " << project_path << "\n";
      info << "Working directory: " << fs::current_path().string() << "\n";
      return info.str();
    };

    if (!init_crash_handler(crash_config)) {
      ORC_LOG_WARN("Failed to initialize crash handler");
    }

    // Execute processing command
    int exit_code = 0;

    try {
      cli::ProcessOptions options;
      options.project_path = project_path;

      exit_code = cli::process_command(options);
    } catch (const UserDataError& e) {
      ORC_LOG_WARN("Processing failed: {}", e.what());
      std::cerr << "\nWARNING: " << e.what() << "\n";

      cleanup_crash_handler();
      return 1;
    } catch (const std::exception& e) {
      const std::string error_message = e.what();

      std::cerr << "\nFATAL ERROR: " << error_message << "\n";

      // Create crash bundle for unhandled exceptions
      std::string bundle_path =
          create_crash_bundle(std::string("Exception: ") + error_message);
      if (!bundle_path.empty()) {
        std::cerr << "\nDiagnostic bundle created: " << bundle_path << "\n";
        std::cerr << "Please report this issue at: "
                     "https://github.com/decode-orc/decode-orc/issues\n";
      }

      cleanup_crash_handler();
      return 1;
    } catch (...) {
      std::cerr << "\nFATAL ERROR: Unknown exception occurred\n";

      // Create crash bundle for unknown exceptions
      std::string bundle_path = create_crash_bundle("Unknown exception");
      if (!bundle_path.empty()) {
        std::cerr << "\nDiagnostic bundle created: " << bundle_path << "\n";
        std::cerr << "Please report this issue at: "
                     "https://github.com/decode-orc/decode-orc/issues\n";
      }

      cleanup_crash_handler();
      return 1;
    }

    cleanup_crash_handler();
    return exit_code;
  } catch (const std::exception& e) {
    std::cerr << "\nFATAL ERROR: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "\nFATAL ERROR: Unknown exception\n";
    return 1;
  }
}
