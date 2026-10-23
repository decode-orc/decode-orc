/*
 * File:        command_tools.h
 * Module:      orc-cli
 * Purpose:     Stage tool subcommand (list / run)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

namespace orc {
namespace cli {

/**
 * @brief Execute the `tools` subcommand.
 *
 * Lists the stage tools a project's nodes offer, and runs one against a node:
 * a dry run reports what the tool found and what it would write, `--apply`
 * writes it and saves the project. The analysis sinks' batch tools run the
 * sink and report its per-frame results.
 *
 * @param argc Argument count.
 * @param argv Arguments, with argv[0] = the program name (for usage messages)
 *             and argv[1] = the subcommand.
 * @return Exit code from the contract in cli_exit_codes.h.
 */
int tools_command(int argc, char* argv[]);

}  // namespace cli
}  // namespace orc
