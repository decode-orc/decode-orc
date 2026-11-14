/*
 * File:        view_support_export.h
 * Module:      decode-orc Plugin SDK (view-support tier)
 * Purpose:     Symbol export for the orc-view-support shared library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

// SDK TIER: view-support — widgets orc-gui ships in a shared library for view
// binaries to link. Built with the host's Qt; see plugin-sdk.md, "View
// support library".

#include <QtGlobal>

#if defined(ORC_VIEW_SUPPORT_BUILDING)
#define ORC_VIEW_SUPPORT_EXPORT Q_DECL_EXPORT
#else
#define ORC_VIEW_SUPPORT_EXPORT Q_DECL_IMPORT
#endif
