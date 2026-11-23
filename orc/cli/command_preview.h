/*
 * File:        command_preview.h
 * Module:      orc-cli
 * Purpose:     Preview subcommand (outputs / render / compare)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

namespace orc {
namespace cli {

/**
 * @brief Execute the `preview` subcommand.
 *
 * Renders what the GUI's preview shows, without the GUI: list the outputs a
 * stage offers, write one as a PNG, or write an A/B comparison of two stages
 * as a lossless animated WebP that flickers between them.
 *
 * @param argc Argument count.
 * @param argv Arguments, with argv[0] = the program name (for usage messages)
 *             and argv[1] = the subcommand.
 * @return Exit code from the contract in cli_exit_codes.h.
 */
int preview_command(int argc, char* argv[]);

}  // namespace cli
}  // namespace orc
