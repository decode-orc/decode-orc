/*
 * File:        plugin.cpp
 * Module:      orc-stage-plugin-cvbs_source
 * Purpose:     Runtime plugin bundle for CVBSSourceStage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/abi/orc_plugin_sdk.h>

#include "cvbs_source_stage.h"

#ifndef ORC_STAGE_PLUGIN_VERSION
#define ORC_STAGE_PLUGIN_VERSION "dev"
#endif

namespace {

constexpr orc::StagePluginDescriptor kPluginDescriptor =
    ORC_STAGE_PLUGIN_DESCRIPTOR("decode-orc.stage.cvbs_source",
                                ORC_STAGE_PLUGIN_VERSION, "GPL-3.0-or-later",
                                true);

}  // namespace

ORC_DEFINE_STAGE_PLUGIN(kPluginDescriptor, orc::PALCVBSSourceStage,
                        orc::NTSCCVBSSourceStage, orc::PALMCVBSSourceStage,
                        orc::SECAMCVBSSourceStage)
