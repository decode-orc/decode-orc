/*
 * File:        interleave.h
 * Purpose:     EFM-library - Data interleaving functions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef INTERLEAVE_H
#define INTERLEAVE_H

#include <cstdint>
#include <vector>

class Interleave {
 public:
  Interleave();
  void deinterleave(std::vector<uint8_t>& inputData,
                    std::vector<uint8_t>& inputError,
                    std::vector<uint8_t>& inputPadded,
                    std::vector<uint8_t>& inputDoubt);
};

#endif  // INTERLEAVE_H