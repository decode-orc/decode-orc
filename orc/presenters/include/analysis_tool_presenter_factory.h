/*
 * File:        analysis_tool_presenter_factory.h
 * Module:      orc-presenters
 * Purpose:     Tool id to analysis tool presenter selection, shared by the
 *              GUI's analysis dialog and orc-cli
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_PRESENTERS_ANALYSIS_TOOL_PRESENTER_FACTORY_H
#define ORC_PRESENTERS_ANALYSIS_TOOL_PRESENTER_FACTORY_H

#include <memory>
#include <string>

#include "analysis_tool_presenter.h"

namespace orc::presenters {

/**
 * @brief Make the presenter that runs and applies a stage tool
 *
 * Tools with a dedicated presenter (Disc Mapper, Source Alignment, ...) get
 * it; any other registered tool gets a generic presenter that runs the core
 * tool directly. A tool added for one front end is therefore available to the
 * other without a second dispatch table.
 *
 * @param tool_id Analysis tool id (e.g. "field_mapping")
 * @param project_handle Opaque handle to the project the tool works on
 * @return The presenter; never null
 * @throws std::invalid_argument if project_handle is null
 */
std::unique_ptr<AnalysisToolPresenter> makeAnalysisToolPresenter(
    const std::string& tool_id, void* project_handle);

/**
 * @brief Whether a tool id has a dedicated presenter
 *
 * False means makeAnalysisToolPresenter() falls back to the generic one.
 */
bool hasDedicatedAnalysisToolPresenter(const std::string& tool_id);

}  // namespace orc::presenters

#endif  // ORC_PRESENTERS_ANALYSIS_TOOL_PRESENTER_FACTORY_H
