/*
 * File:        catalogue_animation_clock.cpp
 * Module:      orc-gui
 * Purpose:     Time source a catalogue view plays a timed payload against
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_animation_clock.h"

CatalogueMonotonicClock::CatalogueMonotonicClock() { elapsed_.start(); }

int64_t CatalogueMonotonicClock::nowMs() const { return elapsed_.elapsed(); }
