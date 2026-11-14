/*
 * File:        guiproject.cpp
 * Module:      orc-gui
 * Purpose:     GUI project management
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "guiproject.h"

#include <orc/stage/common_types.h>  // For VideoSystem, SourceType

#include <QFileInfo>
#include <algorithm>

#include "logging.h"
#include "presenters/include/i_project_presenter.h"
#include "presenters/include/project_presenter.h"

// Note: DAG is forward-declared in header, actual type comes through presenter

GUIProject::GUIProject()
    : presenter_(std::make_unique<orc::presenters::ProjectPresenter>()) {}

GUIProject::GUIProject(
    std::unique_ptr<orc::presenters::IProjectPresenter> presenter)
    : presenter_(std::move(presenter)) {
  if (!presenter_) {
    presenter_ = std::make_unique<orc::presenters::ProjectPresenter>();
  }
}

GUIProject::~GUIProject() = default;

QString GUIProject::projectName() const {
  if (!presenter_) return QString();

  if (project_path_.isEmpty()) {
    return QString::fromStdString(presenter_->getProjectName());
  }
  return QFileInfo(project_path_).completeBaseName();
}

bool GUIProject::isModified() const {
  return presenter_ ? presenter_->isModified() : false;
}

void GUIProject::setModified(bool modified) {
  // Can't directly set modified flag in presenter
  // Modifications are tracked automatically
  (void)modified;
}

bool GUIProject::newEmptyProject(const QString& project_name,
                                 orc::presenters::VideoFormat video_format,
                                 orc::presenters::SourceType source_format,
                                 QString* error) {
  try {
    if (!presenter_) {
      presenter_ = std::make_unique<orc::presenters::ProjectPresenter>();
    }

    // Reset to empty project using the presenter seam
    presenter_->clearProject();

    // Set project metadata
    presenter_->setProjectName(project_name.toStdString());
    presenter_->setVideoFormat(video_format);
    presenter_->setSourceType(source_format);

    // Default amplitude unit follows industry convention for the video system:
    // NTSC → IRE (SMPTE 170M-2004), PAL → mV (EBU Tech. 3280-E).
    orc::VideoSystem sys = orc::VideoSystem::Unknown;
    switch (video_format) {
      case orc::presenters::VideoFormat::NTSC:
        sys = orc::VideoSystem::NTSC;
        break;
      case orc::presenters::VideoFormat::PAL:
        sys = orc::VideoSystem::PAL;
        break;
      case orc::presenters::VideoFormat::PAL_M:
        sys = orc::VideoSystem::PAL_M;
        break;
      default:
        break;
    }
    presenter_->setAmplitudeUnit(orc::default_amplitude_unit(sys));

    // New blank project should start unmodified
    presenter_->clearModifiedFlag();

    return true;
  } catch (const std::exception& e) {
    if (error) {
      *error = QString("Failed to create project: %1").arg(e.what());
    }
    return false;
  }
}

bool GUIProject::saveToFile(const QString& path, QString* error) {
  try {
    if (!presenter_) {
      throw std::runtime_error("No project to save");
    }

    presenter_->saveProject(path.toStdString());
    project_path_ = path;
    return true;
  } catch (const std::exception& e) {
    if (error) {
      *error = QString("Failed to save project: %1").arg(e.what());
    }
    return false;
  }
}

bool GUIProject::loadFromFile(const QString& path, QString* error) {
  try {
    ORC_LOG_DEBUG("Loading project from: {}", path.toStdString());

    if (!presenter_) {
      presenter_ = std::make_unique<orc::presenters::ProjectPresenter>();
    }

    presenter_->loadProject(path.toStdString());
    project_path_ = path;

    // Detect nodes whose stage plugin is not registered. project_to_dag would
    // fail with "Unknown stage type", but that message is swallowed by
    // buildDAG() (returns nullptr), and hasSource() reports false for an
    // unregistered *source* stage — so the guard below is bypassed and the
    // project silently loads into a broken state that hangs the preview
    // (issue #209, typical after an upgrade leaves stale/mismatched plugins).
    // Surface a clear, actionable error instead, naming the plugin to install.
    const std::string missing_stages = presenter_->missingStagesMessage();
    if (!missing_stages.empty()) {
      throw std::runtime_error(missing_stages);
    }

    ORC_LOG_DEBUG("Building DAG from project");
    auto dag_handle = presenter_->buildDAG();

    // Validate DAG was built successfully
    if (hasSource() && !dag_handle) {
      // Project has source nodes but DAG build failed - this is an error
      throw std::runtime_error(
          "Failed to build DAG from project - check that all source files are "
          "valid");
    }

    return true;
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("Failed to load project: {}", e.what());
    if (error) {
      *error = QString("Failed to load project: %1").arg(e.what());
    }
    return false;
  }
}

void GUIProject::clear() {
  if (!presenter_) {
    presenter_ = std::make_unique<orc::presenters::ProjectPresenter>();
  }
  presenter_->clearProject();
  presenter_->clearModifiedFlag();
  dag_cache_.reset();
  project_path_.clear();
}

bool GUIProject::hasSource() const { return sourceNodeCount() > 0; }

QString GUIProject::getSourceName() const {
  if (!presenter_) return QString();

  auto nodes = presenter_->getNodes();
  auto all_stages = presenter_->listAllStages();

  for (const auto& node : nodes) {
    auto stage_it = std::find_if(all_stages.begin(), all_stages.end(),
                                 [&node](const orc::presenters::StageInfo& s) {
                                   return s.name == node.stage_name;
                                 });
    if (stage_it != all_stages.end() && stage_it->is_source) {
      return QString::fromStdString(node.label.empty() ? node.stage_name
                                                       : node.label);
    }
  }
  return QString();
}

int GUIProject::sourceNodeCount() const {
  if (!presenter_) return 0;

  auto nodes = presenter_->getNodes();
  auto all_stages = presenter_->listAllStages();

  return static_cast<int>(std::count_if(
      nodes.begin(), nodes.end(), [&all_stages](const auto& node) {
        auto stage_it =
            std::find_if(all_stages.begin(), all_stages.end(),
                         [&node](const orc::presenters::StageInfo& s) {
                           return s.name == node.stage_name;
                         });
        return stage_it != all_stages.end() && stage_it->is_source;
      }));
}

GUIProject::SourceReloadResult GUIProject::reloadSources() {
  SourceReloadResult result;
  result.source_count = sourceNodeCount();
  if (result.source_count == 0) {
    // Nothing to re-read; leave the existing DAG (if any) in place rather
    // than discarding it for no gain.
    return result;
  }

  rebuildDAG();
  result.rebuilt = dag_cache_ != nullptr;
  return result;
}

std::shared_ptr<void> GUIProject::getDAG() const {
  if (!presenter_) return nullptr;

  // Get DAG from presenter and cache it
  auto dag_handle = presenter_->getDAG();
  if (dag_handle) {
    dag_cache_ = dag_handle;
  }
  return dag_cache_;
}

void GUIProject::rebuildDAG() {
  if (!presenter_) return;

  dag_cache_.reset();

  if (!hasSource()) {
    ORC_LOG_DEBUG("No source in project, skipping DAG build");
    return;
  }

  try {
    ORC_LOG_DEBUG("Building DAG via ProjectPresenter");
    auto dag_handle = presenter_->buildDAG();

    if (dag_handle) {
      dag_cache_ = dag_handle;
      ORC_LOG_DEBUG("DAG built successfully from project");
    } else {
      ORC_LOG_ERROR("Failed to build DAG from project");
    }
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("Failed to build DAG from project: {}", e.what());
    dag_cache_.reset();
  }
}
