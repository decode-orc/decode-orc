/*
 * File:        i_project_presenter.h
 * Module:      orc-presenters
 * Purpose:     Project presenter interface seam for testability
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/node_type.h>
#include <orc/stage/orc_source_parameters.h>
#include <orc/stage/params/parameter_types.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "project_presenter_types.h"

namespace orc::presenters {

class IProjectPresenter {
 public:
  virtual ~IProjectPresenter() = default;

  // === Project Lifecycle ===
  virtual bool createQuickProject(
      VideoFormat format, SourceType source,
      const std::vector<std::string>& input_files) = 0;
  virtual bool loadProject(const std::string& project_path) = 0;
  virtual bool saveProject(const std::string& project_path) = 0;
  virtual void clearProject() = 0;
  virtual bool isModified() const = 0;
  virtual void clearModifiedFlag() = 0;
  virtual std::string getProjectPath() const = 0;

  // === Project Metadata ===
  virtual std::string getProjectName() const = 0;
  virtual void setProjectName(const std::string& name) = 0;
  virtual std::string getProjectDescription() const = 0;
  virtual void setProjectDescription(const std::string& description) = 0;
  virtual VideoFormat getVideoFormat() const = 0;
  virtual void setVideoFormat(VideoFormat format) = 0;
  virtual SourceType getSourceFormat() const = 0;
  virtual void setSourceFormat(SourceType source) = 0;
  virtual std::shared_ptr<const void> createSnapshot() const = 0;
  virtual SourceType getSourceType() const = 0;
  virtual void setSourceType(SourceType source) = 0;
  virtual orc::AmplitudeDisplayUnit getAmplitudeUnit() const = 0;
  virtual void setAmplitudeUnit(orc::AmplitudeDisplayUnit unit) = 0;

  // === DAG Management ===
  virtual NodeID addNode(const std::string& stage_name, double x_position,
                         double y_position) = 0;
  virtual bool removeNode(NodeID node_id) = 0;
  virtual bool canRemoveNode(NodeID node_id, std::string* reason) const = 0;
  virtual void setNodePosition(NodeID node_id, double x, double y) = 0;
  virtual void setNodeLabel(NodeID node_id, const std::string& label) = 0;
  virtual void setNodeParameters(
      NodeID node_id, const std::map<std::string, std::string>& parameters) = 0;
  virtual void addEdge(NodeID source_node, NodeID target_node) = 0;
  virtual void removeEdge(NodeID source_node, NodeID target_node) = 0;
  virtual std::vector<NodeInfo> getNodes() const = 0;
  virtual NodeID getFirstNode() const = 0;
  virtual bool hasNode(NodeID node_id) const = 0;
  virtual std::vector<EdgeInfo> getEdges() const = 0;
  virtual NodeInfo getNodeInfo(NodeID node_id) const = 0;

  // === Stage Registry ===
  virtual std::vector<StageInfo> listAvailableStagesForFormat(
      VideoFormat format) const = 0;
  virtual std::vector<StageInfo> listAllStages() const = 0;
  virtual bool stageExists(const std::string& stage_name) const = 0;
  virtual std::shared_ptr<void> instantiateStage(
      const std::string& stage_name) const = 0;
  virtual std::vector<LoadedPluginInfo> listLoadedPlugins() const = 0;
  virtual std::vector<PluginDiagnosticInfo> listPluginDiagnostics() const = 0;
  virtual std::vector<std::string> listPluginSearchPaths() const = 0;
  virtual PluginRegistryInfo getPluginRegistry() const = 0;
  virtual PluginRegistryMutationResult addPlugin(
      const std::string& path, const std::string& plugin_id,
      const std::string& plugin_version, const std::string& license_spdx,
      bool is_core_plugin, bool trusted) const = 0;
  virtual PluginRegistryMutationResult addPluginEntry(
      const PluginRegistryEntryInfo& entry_info) const = 0;
  virtual PluginRegistryMutationResult addPluginFromUrl(
      const std::string& releases_url, bool trusted) const = 0;
  /// Resolve any selector a front end printed back to a registry entry. Every
  /// caller passes the selector straight through; nobody matches identity
  /// fields themselves.
  virtual PluginSelectorResolution resolvePluginSelector(
      const std::string& selector) const = 0;
  virtual PluginRegistryMutationResult removePluginEntry(
      const std::string& selector) const = 0;
  virtual PluginRegistryMutationResult setPluginEnabled(
      const std::string& selector, bool enabled) const = 0;
  virtual PluginRegistryMutationResult setPluginTrusted(
      const std::string& selector, bool trusted) const = 0;

  // === Curated plugin index discovery ===
  virtual PluginIndexInfo fetchPluginIndex() const = 0;
  virtual PluginRegistryMutationResult installPluginFromIndex(
      const std::string& plugin_id) const = 0;

  // === Plugin update checks ===
  virtual std::vector<PluginUpdateStatusInfo> checkPluginUpdates() const = 0;
  virtual PluginRegistryMutationResult updatePluginToLatestRelease(
      const std::string& selector) const = 0;

  // === Batch Operations ===
  virtual bool canTriggerNode(NodeID node_id, std::string* reason) const = 0;
  virtual bool triggerNode(NodeID node_id,
                           ProgressCallback progress_callback) = 0;
  virtual bool triggerAllSinks(ProgressCallback progress_callback) = 0;

  // === Validation ===
  virtual bool validateProject() const = 0;
  virtual std::vector<std::string> getValidationErrors() const = 0;

  // === Configuration Status ===
  virtual orc::ConfigurationStatus getNodeConfigurationStatus(
      NodeID node_id) const = 0;

  // === DAG Operations ===
  virtual std::shared_ptr<void> getDAG() const = 0;
  virtual std::shared_ptr<void> buildDAG() = 0;
  virtual bool validateDAG() = 0;

  // === Stage Help ===
  virtual std::string getStageInstructions(
      const std::string& stage_name) const = 0;

  // === Parameter Operations ===
  virtual std::vector<ParameterDescriptor> getStageParameters(
      const std::string& stage_name) = 0;
  virtual std::map<std::string, ParameterValue> getNodeParameters(
      NodeID node_id) = 0;
  virtual bool setNodeParameters(
      NodeID node_id, const std::map<std::string, ParameterValue>& params) = 0;

  // === Low-level access ===
  virtual void* getCoreProjectHandle() = 0;
};

}  // namespace orc::presenters
