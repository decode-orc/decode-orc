/*
 * File:        mock_project_presenter.h
 * Module:      orc-gui-unit-tests
 * Purpose:     GMock scaffold for IProjectPresenter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <gmock/gmock.h>

#include "presenters/include/i_project_presenter.h"

namespace orc::presenters::test {

class MockProjectPresenter : public IProjectPresenter {
 public:
  MOCK_METHOD(bool, createQuickProject,
              (VideoFormat format, SourceType source,
               const std::vector<std::string>& input_files),
              (override));
  MOCK_METHOD(bool, loadProject, (const std::string& project_path), (override));
  MOCK_METHOD(bool, saveProject, (const std::string& project_path), (override));
  MOCK_METHOD(void, clearProject, (), (override));
  MOCK_METHOD(bool, isModified, (), (const, override));
  MOCK_METHOD(void, clearModifiedFlag, (), (override));
  MOCK_METHOD(std::string, getProjectPath, (), (const, override));

  MOCK_METHOD(std::string, getProjectName, (), (const, override));
  MOCK_METHOD(void, setProjectName, (const std::string& name), (override));
  MOCK_METHOD(std::string, getProjectDescription, (), (const, override));
  MOCK_METHOD(void, setProjectDescription, (const std::string& description),
              (override));
  MOCK_METHOD(VideoFormat, getVideoFormat, (), (const, override));
  MOCK_METHOD(void, setVideoFormat, (VideoFormat format), (override));
  MOCK_METHOD(SourceType, getSourceFormat, (), (const, override));
  MOCK_METHOD(void, setSourceFormat, (SourceType source), (override));
  MOCK_METHOD(std::shared_ptr<const void>, createSnapshot, (),
              (const, override));
  MOCK_METHOD(SourceType, getSourceType, (), (const, override));
  MOCK_METHOD(void, setSourceType, (SourceType source), (override));
  MOCK_METHOD(orc::AmplitudeDisplayUnit, getAmplitudeUnit, (),
              (const, override));
  MOCK_METHOD(void, setAmplitudeUnit, (orc::AmplitudeDisplayUnit unit),
              (override));

  MOCK_METHOD(NodeID, addNode,
              (const std::string& stage_name, double x_position,
               double y_position),
              (override));
  MOCK_METHOD(bool, removeNode, (NodeID node_id), (override));
  MOCK_METHOD(bool, canRemoveNode, (NodeID node_id, std::string* reason),
              (const, override));
  MOCK_METHOD(void, setNodePosition, (NodeID node_id, double x, double y),
              (override));
  MOCK_METHOD(void, setNodeLabel, (NodeID node_id, const std::string& label),
              (override));
  MOCK_METHOD(void, setNodeParameters,
              (NodeID node_id,
               (const std::map<std::string, std::string>& parameters)),
              (override));
  MOCK_METHOD(void, addEdge, (NodeID source_node, NodeID target_node),
              (override));
  MOCK_METHOD(void, removeEdge, (NodeID source_node, NodeID target_node),
              (override));
  MOCK_METHOD(std::vector<NodeInfo>, getNodes, (), (const, override));
  MOCK_METHOD(NodeID, getFirstNode, (), (const, override));
  MOCK_METHOD(bool, hasNode, (NodeID node_id), (const, override));
  MOCK_METHOD(std::vector<EdgeInfo>, getEdges, (), (const, override));
  MOCK_METHOD(NodeInfo, getNodeInfo, (NodeID node_id), (const, override));

  MOCK_METHOD(std::vector<StageInfo>, listAvailableStagesForFormat,
              (VideoFormat format), (const, override));
  MOCK_METHOD(std::vector<StageInfo>, listAllStages, (), (const, override));
  MOCK_METHOD(bool, stageExists, (const std::string& stage_name),
              (const, override));
  MOCK_METHOD(std::shared_ptr<void>, instantiateStage,
              (const std::string& stage_name), (const, override));
  MOCK_METHOD(std::vector<LoadedPluginInfo>, listLoadedPlugins, (),
              (const, override));
  MOCK_METHOD(std::vector<PluginDiagnosticInfo>, listPluginDiagnostics, (),
              (const, override));
  MOCK_METHOD(std::vector<std::string>, listPluginSearchPaths, (),
              (const, override));
  MOCK_METHOD(PluginRegistryInfo, getPluginRegistry, (), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, addPlugin,
              (const std::string& path, const std::string& plugin_id,
               const std::string& plugin_version,
               const std::string& license_spdx, bool is_core_plugin,
               bool trusted),
              (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, addPluginEntry,
              (const PluginRegistryEntryInfo& entry_info), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, addPluginFromUrl,
              (const std::string& releases_url, bool trusted),
              (const, override));
  MOCK_METHOD(PluginSelectorResolution, resolvePluginSelector,
              (const std::string& selector), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, removePluginEntry,
              (const std::string& selector), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, setPluginEnabled,
              (const std::string& selector, bool enabled), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, setPluginTrusted,
              (const std::string& selector, bool trusted), (const, override));
  MOCK_METHOD(PluginIndexInfo, fetchPluginIndex, (), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, installPluginFromIndex,
              (const std::string& plugin_id), (const, override));
  MOCK_METHOD(std::vector<PluginUpdateStatusInfo>, checkPluginUpdates, (),
              (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, updatePluginToLatestRelease,
              (const std::string& plugin_id), (const, override));

  MOCK_METHOD(bool, canTriggerNode, (NodeID node_id, std::string* reason),
              (const, override));
  MOCK_METHOD(bool, triggerNode,
              (NodeID node_id, ProgressCallback progress_callback), (override));
  MOCK_METHOD(bool, triggerAllSinks, (ProgressCallback progress_callback),
              (override));

  MOCK_METHOD(bool, validateProject, (), (const, override));
  MOCK_METHOD(std::vector<std::string>, getValidationErrors, (),
              (const, override));

  MOCK_METHOD(orc::ConfigurationStatus, getNodeConfigurationStatus,
              (NodeID node_id), (const, override));

  MOCK_METHOD(std::shared_ptr<void>, getDAG, (), (const, override));
  MOCK_METHOD(std::shared_ptr<void>, buildDAG, (), (override));
  MOCK_METHOD(bool, validateDAG, (), (override));

  MOCK_METHOD(std::string, getStageInstructions,
              (const std::string& stage_name), (const, override));

  MOCK_METHOD(std::vector<ParameterDescriptor>, getStageParameters,
              (const std::string& stage_name), (override));
  MOCK_METHOD((std::map<std::string, ParameterValue>), getNodeParameters,
              (NodeID node_id), (override));
  MOCK_METHOD(bool, setNodeParameters,
              (NodeID node_id,
               (const std::map<std::string, ParameterValue>& params)),
              (override));

  MOCK_METHOD(void*, getCoreProjectHandle, (), (override));
};

}  // namespace orc::presenters::test
