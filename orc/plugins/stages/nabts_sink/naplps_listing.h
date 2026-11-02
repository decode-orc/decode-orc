/*
 * File:        naplps_listing.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     A NAPLPS record's codes, listed for a reader
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NAPLPS_LISTING_H
#define ORC_NAPLPS_LISTING_H

#include <cstdint>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief A record's codes as a reader can follow them
 *
 * A record that lost its start with its header gives no say in which set its
 * first bytes were meant or how far into a command they fall, so how it is
 * drawn is the interpreter's best reading rather than a certainty. This lists
 * what its codes are when read from the start, from the default code
 * environment of X3.110 §4.3.1.3, as the interpreter reads them: a run of
 * characters as text, each control and each picture description instruction
 * by name (an instruction with the count of its operand bytes), a definition
 * with the code it defines and its length, and a stretch the recording lost as
 * its length. One line to a run of text, a command, or a run of controls.
 *
 * @p data holds the record's bytes with their parity as received (CEA-516
 * §3.3); a control that arrived failing it is marked "?", and a character
 * that did is shown as U+FFFD. @p present holds one entry per byte, zero
 * where no copy of the record delivered it; empty, every byte arrived.
 *
 * Pure: no I/O.
 */
std::string naplps_code_listing(const std::vector<uint8_t>& data,
                                const std::vector<uint8_t>& present);

}  // namespace orc

#endif  // ORC_NAPLPS_LISTING_H
