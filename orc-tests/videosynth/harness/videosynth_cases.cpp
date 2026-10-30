/*
 * File:        videosynth_cases.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     Opening a generated case through cvbs_source, and reading its
 *              dropout sidecar, for the videosynth suite's tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "videosynth_cases.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <sqlite3.h>

#include <map>

#include "cvbs_source_stage.h"
#include "videosynth_harness.h"

namespace orc_videosynth {

namespace {

std::unique_ptr<orc::FixedFormatCVBSSourceStage> make_source(
    orc::VideoSystem system) {
  switch (system) {
    case orc::VideoSystem::PAL:
      return std::make_unique<orc::PALCVBSSourceStage>();
    case orc::VideoSystem::NTSC:
      return std::make_unique<orc::NTSCCVBSSourceStage>();
    case orc::VideoSystem::PAL_M:
      return std::make_unique<orc::PALMCVBSSourceStage>();
    default:
      return nullptr;
  }
}

}  // namespace

std::optional<std::filesystem::path> case_base(const CaseFile& file) {
  const auto dir = case_dir(file.case_name);
  if (!dir) {
    ADD_FAILURE() << "ORC_VIDEOSYNTH_OUTPUT_DIR is not set; run through ctest";
    return std::nullopt;
  }
  return *dir / file.stem;
}

orc::ArtifactPtr open_case(const CaseFile& file) {
  const auto base = case_base(file);
  if (!base) return nullptr;
  auto source = make_source(file.system);
  if (!source) {
    ADD_FAILURE() << "no cvbs_source for the system of " << file.case_name;
    return nullptr;
  }
  std::map<std::string, orc::ParameterValue> parameters;
  if (file.yc) {
    parameters["y_path"] = base->string() + ".cvbsy";
    parameters["c_path"] = base->string() + ".cvbsc";
  } else {
    parameters["input_path"] = base->string() + ".cvbs";
  }
  if (!source->set_parameters(parameters)) {
    ADD_FAILURE() << "cvbs_source rejected " << base->string();
    return nullptr;
  }
  orc::ObservationContext context;
  const auto outputs = source->execute({}, parameters, context);
  if (outputs.size() != 1) {
    ADD_FAILURE() << "cvbs_source gave no output for " << file.case_name;
    return nullptr;
  }
  return outputs[0];
}

VfrPtr as_vfr(const orc::ArtifactPtr& artifact) {
  return std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      artifact);
}

std::vector<RunKey> read_sidecar_runs(const std::filesystem::path& path) {
  std::vector<RunKey> runs;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    ADD_FAILURE() << "cannot open " << path;
    if (db != nullptr) sqlite3_close(db);
    return runs;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT frame_id, sample_start, sample_count, "
                         "severity FROM dropout_run "
                         "ORDER BY frame_id, sample_start",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    ADD_FAILURE() << "cannot query dropout_run in " << path << ": "
                  << sqlite3_errmsg(db);
    sqlite3_close(db);
    return runs;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    runs.emplace_back(static_cast<orc::FrameID>(sqlite3_column_int64(stmt, 0)),
                      static_cast<uint64_t>(sqlite3_column_int64(stmt, 1)),
                      static_cast<uint32_t>(sqlite3_column_int(stmt, 2)),
                      static_cast<uint8_t>(sqlite3_column_int(stmt, 3)));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return runs;
}

}  // namespace orc_videosynth
