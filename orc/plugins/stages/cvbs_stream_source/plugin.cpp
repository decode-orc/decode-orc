/*
 * File:        plugin.cpp
 * Module:      orc-stage-plugin-cvbs_stream_source
 * Purpose:     Runtime plugin bundle for FixedFormatCVBSStreamSourceStage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <orc/abi/orc_plugin_sdk.h>

#include "cvbs_stream_source_stage.h"

#ifndef ORC_STAGE_PLUGIN_VERSION
#define ORC_STAGE_PLUGIN_VERSION "dev"
#endif

namespace {

constexpr orc::StagePluginDescriptor kPluginDescriptor =
    ORC_STAGE_PLUGIN_DESCRIPTOR("decode-orc.stage.cvbs_stream_source",
                                ORC_STAGE_PLUGIN_VERSION, "GPL-3.0-or-later",
                                true);

}  // namespace

ORC_DEFINE_STAGE_PLUGIN(kPluginDescriptor, orc::PALCVBSStreamSourceStage,
                        orc::NTSCCVBSStreamSourceStage,
                        orc::PALMCVBSStreamSourceStage)
