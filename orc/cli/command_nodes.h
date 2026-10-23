/*
 * File:        command_nodes.h
 * Module:      orc-cli
 * Purpose:     Project node subcommand (list / get / set)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

namespace orc {
namespace cli {

/**
 * @brief Execute the `nodes` subcommand.
 *
 * Reads and edits the stages of an existing project, so a script can point a
 * template project at new captures: list the project's stages, show one
 * stage's parameters, and set parameters with the same type and constraint
 * checks the GUI's parameter dialog applies.
 *
 * @param argc Argument count.
 * @param argv Arguments, with argv[0] = the program name (for usage messages)
 *             and argv[1] = the subcommand.
 * @return Exit code from the contract in cli_exit_codes.h.
 */
int nodes_command(int argc, char* argv[]);

}  // namespace cli
}  // namespace orc
