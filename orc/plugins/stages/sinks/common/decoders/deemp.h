/*
 * File:        deemp.h
 * Module:      orc-core
 * Purpose:     Chroma post-filter coefficients for the NTSC comb decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2021 Chad Page
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

// The coefficients below were originally produced by ld-decode's
// scripts/filtermaker (which generated a much larger header covering the
// whole ld-decode chain); only the filters used by the orc chroma decoders
// are kept here. Each filter's scipy design call is recorded alongside it so
// the coefficients can be regenerated or adjusted without the original
// script. The sample rate is 4fsc NTSC (freq4 = 4 * 315/88 = 14.31818 MHz)
// unless noted otherwise.

#ifndef DEEMP_H
#define DEEMP_H

#include <array>

// SMPTE 170M-2004 §7.2 (equiband): I and Q chroma lowpass filter.
// Design target: < 2 dB attenuation at 1.3 MHz; ≥ 20 dB at 3.6 MHz at 4fsc
// (14.3182 MHz). Verified response: −0.02 dB at 1.3 MHz, −34.8 dB at 3.6 MHz.
// Applied equally to both I and Q (equiband per SMPTE 170M-2004 §7.2 Note).
// Design: scipy.signal.remez(17, [0.0, 1.3, 3.8, freq4 / 2], [1.0, 0.0],
//         [1.0, 1.0], fs=freq4).
const std::array<double, 17> c_colorlp_b = {
    2.236562025869846e-03,  9.679572273064329e-03,  6.100849475810623e-03,
    -2.082153208645807e-02, -4.872917723725065e-02, -1.535300561979003e-02,
    1.137084573789944e-01,  2.775133160099456e-01,  3.533666167131518e-01,
    2.775133160099456e-01,  1.137084573789944e-01,  -1.535300561979003e-02,
    -4.872917723725065e-02, -2.082153208645807e-02, 6.100849475810623e-03,
    9.679572273064329e-03,  2.236562025869846e-03};

#endif
