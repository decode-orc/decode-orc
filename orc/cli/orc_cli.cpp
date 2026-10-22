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
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "command_filter.h"
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
  std::cerr << "       " << program_name
            << " --source/--filters/--sink <...>\n";
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
  std::cerr << "Filtergraph — source/filters/sink triad:\n";
  std::cerr << "  --source GRAPH, -i GRAPH       Input (source) stage(s)\n";
  std::cerr << "  --filters GRAPH, -f GRAPH      Processing stage(s)\n";
  std::cerr << "  --sink GRAPH, -o GRAPH         Output (sink) stage(s)\n";
  std::cerr << "\n";
  std::cerr << "Filtergraph export:\n";
  std::cerr << "  --export-project FILE          Save the assembled "
               "filtergraph as a .orcprj file\n";
  std::cerr << "                                 instead of running it (for "
               "the GUI, or later\n";
  std::cerr << "                                 reuse with --process). "
               "Requires a video format\n";
  std::cerr << "                                 and source signal type — "
               "see --video-format/\n";
  std::cerr << "                                 --source-type below if no "
               "stage implies one.\n";
  std::cerr << "  --video-format NTSC|PAL|PAL-M  Set the video format if "
               "none of the stages used\n";
  std::cerr << "                                 imply one — e.g. "
               "tbc_source reads its own\n";
  std::cerr << "                                 format from its metadata "
               "file, so it never\n";
  std::cerr << "                                 implies one. Works when "
               "running directly too\n";
  std::cerr << "                                 (so the same graph "
               "behaves identically either\n";
  std::cerr << "                                 way); only --export-project "
               "actually requires it.\n";
  std::cerr << "  --source-type composite|yc     Same idea as --video-format, "
               "but for the source\n";
  std::cerr << "                                 signal type — needed only "
               "if no source stage's\n";
  std::cerr << "                                 parameters reveal it (e.g. "
               "tbc_source with only\n";
  std::cerr << "                                 pcm_path set)\n";
  std::cerr << "\n";
  std::cerr << "Note: video format and source signal type are usually "
               "detected automatically\n";
  std::cerr << "from the stage modules used (see --video-format/"
               "--source-type above for the\n";
  std::cerr << "rare exception).\n";
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
  std::cerr << "\n";
  std::cerr << "Stage Tools (see 'tools --help' for options):\n";
  std::cerr << "  tools list <project> [<node>]  List the tools each stage "
               "offers\n";
  std::cerr << "  tools run <project> <tool> --node <node> [--apply]\n";
  std::cerr << "                                 Run a tool against a stage; "
               "--apply saves its result\n";
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
  std::cerr << "  --safe-core-plugins            Clear plugin registry and "
               "ignore ORC_STAGE_PLUGIN_PATHS\n";
  std::cerr
      << "                                 for this run (core plugins only)\n";
  std::cerr << "\n";
  std::cerr << "Examples:\n";
  std::cerr << "  " << program_name << " project.orcprj --process\n";
  std::cerr << "  " << program_name
            << " project.orcprj --process --log-level debug\n";
  std::cerr << "  " << program_name
            << " project.orcprj --process --log-file run.log --log-out file\n";
  std::cerr << "  " << program_name
            << " --source tbc_source=input_path=a.tbc \\\n";
  std::cerr << "      --filters dropout_correct --sink "
               "video_sink=output_path=a.mp4\n";
  std::cerr << "  " << program_name
            << " -i \"NTSC_CVBS_Source=input_path=a.cvbs\" -o video_sink"
               "=output_path=a.mp4\n";
  std::cerr << "  " << program_name
            << " --source tbc_source=input_path=a.tbc --sink video_sink "
               "\\\n";
  std::cerr << "      --export-project a.orcprj\n";
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
  try {
    // Parse command line arguments
    std::string project_path;
    std::string log_level = "info";
    std::string log_file;
    std::string log_out = "both";
    bool log_out_provided = false;
    bool safe_core_plugins = false;

    // Filtergraph mode: the input/filters/output triad. Mutually exclusive
    // with a project file. There is deliberately no video-format/source-
    // format/project-name option here — those are auto-detected from the
    // stages used.
    std::string input_stages;
    std::string filters_stages;
    std::string output_stages;
    bool triad_provided = false;        // --source / --filters / --sink
    std::string export_project_path;    // --export-project
    std::string video_format_override;  // --video-format (export-only override)
    std::string source_type_override;   // --source-type (export-only override)

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
      } else if ((arg == "--source" || arg == "-i") && i + 1 < argc) {
        input_stages = argv[++i];
        triad_provided = true;
      } else if ((arg == "--filters" || arg == "-f") && i + 1 < argc) {
        filters_stages = argv[++i];
        triad_provided = true;
      } else if ((arg == "--sink" || arg == "-o") && i + 1 < argc) {
        output_stages = argv[++i];
        triad_provided = true;
      } else if (arg == "--export-project" && i + 1 < argc) {
        export_project_path = argv[++i];
      } else if (arg == "--video-format" && i + 1 < argc) {
        video_format_override = argv[++i];
      } else if (arg == "--source-type" && i + 1 < argc) {
        source_type_override = argv[++i];
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

    const bool filtergraph_mode = triad_provided;

    // --video-format/--source-type apply to the source/filters/sink triad
    // (with or without --export-project — see filter_command()) but have
    // nowhere to go with a plain .orcprj file, which already carries its
    // own video_format/source_format from the YAML; reject rather than
    // silently ignore.
    if (!video_format_override.empty() && !filtergraph_mode) {
      std::cerr << "Error: --video-format only makes sense with "
                   "--source/--filters/--sink\n\n";
      print_usage(argv[0]);
      return 1;
    }
    if (!source_type_override.empty() && !filtergraph_mode) {
      std::cerr << "Error: --source-type only makes sense with "
                   "--source/--filters/--sink\n\n";
      print_usage(argv[0]);
      return 1;
    }

    if (filtergraph_mode) {
      if (!project_path.empty()) {
        std::cerr << "Error: --source/--filters/--sink cannot be "
                     "combined with a project file\n\n";
        print_usage(argv[0]);
        return 1;
      }
      if (do_process) {
        std::cerr << "Error: --process is only for a project file and "
                     "cannot be combined with --source/--filters/--sink\n\n";
        print_usage(argv[0]);
        return 1;
      }
      if (input_stages.empty() && filters_stages.empty() &&
          output_stages.empty()) {
        std::cerr << "Error: --source, --filters and --sink were all empty\n\n";
        print_usage(argv[0]);
        return 1;
      }
    } else {
      // Check if project file was provided
      if (project_path.empty()) {
        std::cerr << "Error: No project file specified\n\n";
        print_usage(argv[0]);
        return 1;
      }
      if (!export_project_path.empty()) {
        std::cerr << "Error: --export-project only makes sense with "
                     "--source/--filters/--sink (a project file is "
                     "already a project)\n\n";
        print_usage(argv[0]);
        return 1;
      }

      // Check if at least one command was specified
      if (!do_process) {
        std::cerr << "Error: No command specified. You must use --process\n\n";
        print_usage(argv[0]);
        return 1;
      }
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
    crash_config.custom_info_callback = [&project_path,
                                         filtergraph_mode]() -> std::string {
      std::ostringstream info;
      if (filtergraph_mode) {
        info << "Mode: filtergraph\n";
      } else {
        info << "Project file: " << project_path << "\n";
      }
      info << "Working directory: " << fs::current_path().string() << "\n";
      return info.str();
    };

    if (!init_crash_handler(crash_config)) {
      ORC_LOG_WARN("Failed to initialize crash handler");
    }

    // Execute processing command
    int exit_code = 0;

    try {
      if (filtergraph_mode) {
        cli::FilterOptions options;
        options.input_stages = input_stages;
        options.filters_stages = filters_stages;
        options.output_stages = output_stages;
        options.export_project_path = export_project_path;
        options.video_format_override = video_format_override;
        options.source_type_override = source_type_override;

        exit_code = cli::filter_command(options);
      } else {
        cli::ProcessOptions options;
        options.project_path = project_path;

        exit_code = cli::process_command(options);
      }
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
