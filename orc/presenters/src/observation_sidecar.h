/*
 * File:        observation_sidecar.h
 * Module:      orc-presenters
 * Purpose:     The per-source observation sidecar: where it lives, and an
 *              observation store persisted to it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <memory>
#include <string>

namespace orc {
class DAG;
class ObservationStore;
}  // namespace orc

namespace orc::presenters {

// Observation sidecar path for a DAG: a per-source database in the user
// config dir, named by a stable digest of orc::sidecar_source_key() (the SOURCE
// nodes' stage, version and parameters — which include the source file path).
// Used for every project, saved or not: content-addressed records make one
// cache per source the natural unit, shared across quick sessions, saved
// projects and orc-cli runs. File identity (mtime/size) is deliberately
// excluded so the file name survives touches; staleness is already handled by
// the fingerprint record keys inside. The legacy "quick-" filename prefix is
// kept so caches created before saved projects switched to this path remain
// warm. A hash collision merely shares a database file — content-addressed
// keys keep that correct. Returns "" when the DAG has no sources, when a source
// has not been given its file yet, or when the directory cannot be created (the
// store then runs in-memory only).
std::string perSourceSidecarPath(const orc::DAG& dag);

// An observation store persisted to |dag|'s per-source sidecar, for a run
// that only reads and adds records (orc-cli's sink runs keep the stages'
// measurements there). The sidecar directory's retention limits are applied
// first, as the GUI applies them, and the sidecar is stamped as used.
// Records the run adds are written when the store is destroyed. nullptr when
// the DAG has no sidecar or it cannot be opened.
std::shared_ptr<orc::ObservationStore> openSidecarStore(const orc::DAG& dag);

}  // namespace orc::presenters
