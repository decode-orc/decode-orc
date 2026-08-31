/*
 * File:        project_presenter.h
 * Module:      orc-presenters
 * Purpose:     Project management presenter - MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/field_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/orc_source_parameters.h>  // For public_api::SourceParameters
#include <orc/stage/params/parameter_types.h>
#include <orc/support/log_destination.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "i_project_presenter.h"
#include "project_presenter_types.h"

// Forward declare core Project type
namespace orc {
class Project;
}

namespace orc::presenters {

// === Application Initialization ===

/**
 * @brief Initialize core logging system
 * @param level Log level (trace, debug, info, warn, error, critical, off)
 * @param pattern Optional custom pattern (default: "[%Y-%m-%d %H:%M:%S.%e] [%n]
 * [%^%l%$] %v")
 * @param log_file Optional file path to write logs to
 * @param destination Which sinks to install (console, file, or both)
 *
 * This function provides access to core's logging initialization through the
 * presenters layer, maintaining MVP architecture compliance.
 */
void initCoreLogging(
    const std::string& level = "info",
    const std::string& pattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v",
    const std::string& log_file = "",
    LogDestination destination = LogDestination::kBoth);

/**
 * @brief Reconfigure core logging while the application is running
 * @param level Log level (trace, debug, info, warn, error, critical, off)
 * @param pattern Log pattern applied to every installed sink
 * @param log_file File to write to; ignored when empty
 * @param destination Which sinks to install (console, file, or both)
 * @param truncate_log_file True to replace the log file's contents, false to
 *        append to a file this run already opened
 * @param error_message Optional; receives the reason a requested log file
 *        could not be opened
 * @return True when the requested destination was installed in full
 *
 * The core logger object is preserved, so loaded stage plugins - which share
 * that logger - follow the change without being reloaded. Used by the GUI's
 * logging dialogue to turn debug logging on and off at runtime.
 */
bool reconfigureCoreLogging(const std::string& level,
                            const std::string& pattern,
                            const std::string& log_file,
                            LogDestination destination, bool truncate_log_file,
                            std::string* error_message = nullptr);

/**
 * @brief Set the background observation worker-pool size.
 *
 * @param count Number of worker threads; 0 (the default) means "auto" — half
 *              the hardware concurrency, at least 1. Call once at startup
 *              (e.g. from a CLI flag) before opening a project.
 */
void setBackgroundObservationWorkerCount(unsigned count);

/**
 * @brief Resolve the configured worker count to an effective thread count.
 * @return The configured count, or the auto default (half the cores, min 1)
 *         when unset.
 */
unsigned resolveBackgroundObservationWorkerCount();

/**
 * @brief ProjectPresenter - Manages project creation, loading, and modification
 *
 * This presenter extracts all project-related business logic from the GUI
 * layer. It provides a clean interface for:
 * - Creating quick/template projects
 * - Loading and saving projects
 * - Managing nodes and edges in the project DAG
 * - Querying project metadata
 * - Triggering batch operations
 *
 * The presenter owns the core Project object and coordinates all operations.
 */
class ProjectPresenter : public IProjectPresenter {
 public:
  /**
   * @brief Construct presenter with new empty project
   */
  ProjectPresenter();

  /**
   * @brief Construct presenter wrapping an existing project
   * @param project_handle Opaque handle to existing project
   */
  explicit ProjectPresenter(void* project_handle);

  /**
   * @brief Construct presenter by loading existing project
   * @param project_path Path to .orcprj file
   */
  explicit ProjectPresenter(const std::string& project_path);

  /**
   * @brief Destructor
   */
  ~ProjectPresenter() override;

  // Disable copy, enable move
  ProjectPresenter(const ProjectPresenter&) = delete;
  ProjectPresenter& operator=(const ProjectPresenter&) = delete;
  ProjectPresenter(ProjectPresenter&&) noexcept;
  ProjectPresenter& operator=(ProjectPresenter&&) noexcept;

  // === Utility Methods (Static) ===

  /**
   * @brief Read video parameters from a TBC metadata file
   * @param metadata_path Path to .tbc.db metadata file
   * @return SourceParameters if successful, nullopt if file doesn't exist or
   * can't be read
   *
   * This is a utility method for reading metadata before creating a project,
   * allowing the GUI to determine video format and other parameters from
   * existing TBC files.
   */
  static std::optional<orc::SourceParameters> readVideoParameters(
      const std::string& metadata_path);

  /**
   * @brief Read video parameters from a CVBS .meta metadata file
   * @param meta_path Path to .meta SQLite file
   * @return SourceParameters with system set if successful, nullopt on failure
   *
   * Reads the preset field from the cvbs_file table to determine the video
   * system (PAL, NTSC, or PAL_M) for use when creating a quick project from
   * a CVBS source file.
   */
  static std::optional<orc::SourceParameters> readCVBSVideoParameters(
      const std::string& meta_path);

  /**
   * @brief Signal-state fields of a CVBS .meta file that the UI reports on.
   *
   * CVBS file format spec v1.6.0: the signal_state_preset describes the
   * sampling/processing chain, while sequence_continuous declares whether the
   * stored content is one unbroken sequence (nullopt = NULL = unknown).
   */
  struct CVBSSignalState {
    std::string signal_state_preset;
    std::optional<bool> sequence_continuous;
  };

  /**
   * @brief Read the signal state from a CVBS .meta metadata file
   * @param meta_path Path to .meta SQLite file
   * @return The signal state fields, or nullopt when they cannot be read
   *
   * Kept separate from readCVBSVideoParameters() because SourceParameters is
   * an ABI struct shared with plugins and the signal state is only wanted by
   * the UI, to tell the user when a source's content is not continuous.
   */
  static std::optional<CVBSSignalState> readCVBSSignalState(
      const std::string& meta_path);

  // === Project Lifecycle ===

  /**
   * @brief Create a quick project from template
   * @param format Video format (NTSC/PAL)
   * @param source Source type (Composite/SVideo)
   * @param input_files List of input TBC files
   * @return true on success
   */
  bool createQuickProject(VideoFormat format, SourceType source,
                          const std::vector<std::string>& input_files) override;

  /**
   * @brief Load project from file
   * @param project_path Path to .orcprj file
   * @return true on success
   */
  bool loadProject(const std::string& project_path) override;

  /**
   * @brief Save project to file
   * @param project_path Path to save to
   * @return true on success
   */
  bool saveProject(const std::string& project_path) override;

  /**
   * @brief Clear the current project
   */
  void clearProject() override;

  /**
   * @brief Check if project has been modified since last save
   */
  bool isModified() const override;

  /**
   * @brief Clear modified state (treat current project as unmodified)
   */
  void clearModifiedFlag() override;

  /**
   * @brief Get project file path
   */
  std::string getProjectPath() const override;

  // === Project Metadata ===

  /**
   * @brief Get project name
   */
  std::string getProjectName() const override;

  /**
   * @brief Set project name
   */
  void setProjectName(const std::string& name) override;

  /**
   * @brief Get project description
   */
  std::string getProjectDescription() const override;

  /**
   * @brief Set project description
   */
  void setProjectDescription(const std::string& description) override;

  /**
   * @brief Get video format
   */
  VideoFormat getVideoFormat() const override;

  /**
   * @brief Set video format
   */
  void setVideoFormat(VideoFormat format) override;

  /**
   * @brief Get source format
   */
  SourceType getSourceFormat() const override { return getSourceType(); }

  /**
   * @brief Set source format
   */
  void setSourceFormat(SourceType source) override { setSourceType(source); }

  /**
   * @brief Create a snapshot copy of the project
   * @return Shared pointer to immutable project copy (opaque handle)
   */
  std::shared_ptr<const void> createSnapshot() const override;

  /**
   * @brief Get source type
   */
  SourceType getSourceType() const override;

  /**
   * @brief Set source type
   */
  void setSourceType(SourceType source) override;

  /**
   * @brief Get amplitude display unit
   */
  orc::AmplitudeDisplayUnit getAmplitudeUnit() const override;

  /**
   * @brief Set amplitude display unit
   */
  void setAmplitudeUnit(orc::AmplitudeDisplayUnit unit) override;

  // === DAG Management ===

  /**
   * @brief Add a node to the project
   * @param stage_name Internal stage name
   * @param x_position X coordinate
   * @param y_position Y coordinate
   * @return NodeID of created node
   */
  NodeID addNode(const std::string& stage_name, double x_position,
                 double y_position) override;

  /**
   * @brief Remove a node from the project
   * @param node_id Node to remove
   * @return true on success
   */
  bool removeNode(NodeID node_id) override;

  /**
   * @brief Check if a node can be removed
   * @param node_id Node to check
   * @param reason Output parameter for reason if cannot remove
   * @return true if can remove
   */
  bool canRemoveNode(NodeID node_id, std::string* reason) const override;

  /**
   * @brief Set node position
   */
  void setNodePosition(NodeID node_id, double x, double y) override;

  /**
   * @brief Set node label
   */
  void setNodeLabel(NodeID node_id, const std::string& label) override;

  /**
   * @brief Set node parameters
   * @param node_id Node to configure
   * @param parameters Map of parameter name -> value
   */
  void setNodeParameters(
      NodeID node_id,
      const std::map<std::string, std::string>& parameters) override;

  /**
   * @brief Add an edge between two nodes
   * @param source_node Source node
   * @param target_node Target node
   */
  void addEdge(NodeID source_node, NodeID target_node) override;

  /**
   * @brief Remove an edge
   */
  void removeEdge(NodeID source_node, NodeID target_node) override;

  /**
   * @brief Get all nodes in the project
   */
  std::vector<NodeInfo> getNodes() const override;

  /**
   * @brief Get the first node in the DAG (if any)
   * @return NodeID of first node, or invalid NodeID if no nodes
   */
  NodeID getFirstNode() const override;

  /**
   * @brief Check if a node exists in the project
   * @param node_id Node to check
   * @return true if node exists
   */
  bool hasNode(NodeID node_id) const override;

  /**
   * @brief Get all edges in the project
   */
  std::vector<EdgeInfo> getEdges() const override;

  /**
   * @brief Get information about a specific node
   */
  NodeInfo getNodeInfo(NodeID node_id) const override;

  // Interface-friendly stage registry wrappers
  std::vector<StageInfo> listAvailableStagesForFormat(
      VideoFormat format) const override {
    return getAvailableStages(format);
  }
  std::vector<StageInfo> listAllStages() const override {
    return ProjectPresenter::getAllStages();
  }
  bool stageExists(const std::string& stage_name) const override {
    return ProjectPresenter::hasStage(stage_name);
  }
  std::shared_ptr<void> instantiateStage(
      const std::string& stage_name) const override {
    return ProjectPresenter::createStageInstance(stage_name);
  }
  std::vector<LoadedPluginInfo> listLoadedPlugins() const override {
    return ProjectPresenter::getLoadedPlugins();
  }
  std::vector<PluginDiagnosticInfo> listPluginDiagnostics() const override {
    return ProjectPresenter::getPluginDiagnostics();
  }
  std::vector<std::string> listPluginSearchPaths() const override {
    return ProjectPresenter::getPluginSearchPaths();
  }
  PluginRegistryInfo getPluginRegistry() const override {
    return ProjectPresenter::readPluginRegistry();
  }
  PluginRegistryMutationResult addPlugin(const std::string& path,
                                         const std::string& plugin_id,
                                         const std::string& plugin_version,
                                         const std::string& license_spdx,
                                         bool is_core_plugin,
                                         bool trusted) const override {
    return ProjectPresenter::addPluginToRegistry(
        path, plugin_id, plugin_version, license_spdx, is_core_plugin, trusted);
  }
  PluginRegistryMutationResult addPluginEntry(
      const PluginRegistryEntryInfo& entry_info) const override {
    return ProjectPresenter::addPluginRegistryEntry(entry_info);
  }
  PluginRegistryMutationResult addPluginFromUrl(const std::string& releases_url,
                                                bool trusted) const override {
    return ProjectPresenter::addPluginFromReleasesUrl(releases_url, trusted);
  }
  PluginSelectorResolution resolvePluginSelector(
      const std::string& selector) const override {
    return ProjectPresenter::resolvePluginRegistrySelector(selector);
  }
  PluginRegistryMutationResult removePluginEntry(
      const std::string& selector) const override {
    return ProjectPresenter::removePluginRegistryEntry(selector);
  }
  PluginRegistryMutationResult setPluginEnabled(const std::string& selector,
                                                bool enabled) const override {
    return ProjectPresenter::setPluginRegistryEntryEnabled(selector, enabled);
  }
  PluginRegistryMutationResult setPluginTrusted(const std::string& selector,
                                                bool trusted) const override {
    return ProjectPresenter::setPluginRegistryEntryTrusted(selector, trusted);
  }
  PluginIndexInfo fetchPluginIndex() const override {
    return ProjectPresenter::readPluginIndex();
  }
  PluginRegistryMutationResult installPluginFromIndex(
      const std::string& plugin_id) const override {
    return ProjectPresenter::installIndexedPlugin(plugin_id);
  }
  std::vector<PluginUpdateStatusInfo> checkPluginUpdates() const override {
    return ProjectPresenter::checkRegisteredPluginUpdates();
  }
  PluginRegistryMutationResult updatePluginToLatestRelease(
      const std::string& selector) const override {
    return ProjectPresenter::updateRegisteredPluginToLatestRelease(selector);
  }

  // === Stage Registry ===

  /**
   * @brief Get all available stages for a video format
   * @param format Video format to filter by
   * @return List of available stages
   */
  static std::vector<StageInfo> getAvailableStages(VideoFormat format);

  /**
   * @brief Get all available stages (no filtering)
   */
  static std::vector<StageInfo> getAllStages();

  /**
   * @brief Check if a stage type exists in the registry
   * @param stage_name Stage type name
   * @return true if stage exists
   */
  static bool hasStage(const std::string& stage_name);

  /**
   * @brief Get runtime-loaded plugin metadata from the stage registry
   */
  static std::vector<LoadedPluginInfo> getLoadedPlugins();

  /**
   * @brief Get runtime plugin diagnostics from the stage registry
   */
  static std::vector<PluginDiagnosticInfo> getPluginDiagnostics();

  /**
   * @brief Get configured plugin search paths used by the stage registry
   */
  static std::vector<std::string> getPluginSearchPaths();

  /**
   * @brief Get persistent plugin registry metadata used during startup
   * reconciliation
   */
  static PluginRegistryInfo readPluginRegistry();

  /**
   * @brief Resolve a plugin selector against the persistent registry
   *
   * Accepts anything the registry listing prints for an entry: its plugin id,
   * a `path:`/`url:` selector, or a bare path or release asset URL. Reports
   * Ambiguous (with candidates) rather than guessing between matches.
   */
  static PluginSelectorResolution resolvePluginRegistrySelector(
      const std::string& selector);

  /**
   * @brief Add a plugin entry to the persistent registry
   */
  static PluginRegistryMutationResult addPluginToRegistry(
      const std::string& path, const std::string& plugin_id,
      const std::string& plugin_version, const std::string& license_spdx,
      bool is_core_plugin, bool trusted);

  /**
   * @brief Add a fully specified plugin registry entry
   */
  static PluginRegistryMutationResult addPluginRegistryEntry(
      const PluginRegistryEntryInfo& entry_info);

  /**
   * @brief Add a remote plugin by resolving a GitHub releases URL
   *
   * The entry is recorded untrusted unless @p trusted is set: trust is granted
   * only through an explicit confirmation step, so that adding a URL and
   * trusting its binary are distinct decisions.
   */
  static PluginRegistryMutationResult addPluginFromReleasesUrl(
      const std::string& releases_url, bool trusted = false);

  /**
   * @brief Remove a plugin entry from the persistent registry by selector
   */
  static PluginRegistryMutationResult removePluginRegistryEntry(
      const std::string& selector);

  /**
   * @brief Enable or disable a plugin entry in the persistent registry
   */
  static PluginRegistryMutationResult setPluginRegistryEntryEnabled(
      const std::string& selector, bool enabled);

  /**
   * @brief Mark a plugin entry as trusted or untrusted in the persistent
   * registry
   *
   * Untrusted non-core entries are neither downloaded nor loaded at startup.
   * Core plugin entries are implicitly trusted and cannot be changed.
   */
  static PluginRegistryMutationResult setPluginRegistryEntryTrusted(
      const std::string& selector, bool trusted);

  /**
   * @brief Clear persistent plugin registry entries for safe startup mode
   *
   * This resets the user plugin registry to an empty set so the runtime loads
   * only core plugins discovered from build/install default plugin paths.
   */
  static PluginRegistryMutationResult clearPluginRegistryForSafeMode();

  /**
   * @brief Fetch the curated plugin index, resolving host compatibility
   *
   * Refreshes from the configured index URL, falling back to the last-good
   * cached copy when offline. Each entry is annotated with whether a build
   * compatible with this host's platform and ABI exists.
   */
  static PluginIndexInfo readPluginIndex();

  /**
   * @brief Install a plugin from the curated index by id
   *
   * Resolves the artifact matching this host's platform and ABI, then records
   * a remote registry entry (carrying the index-declared sha256) as
   * untrusted-until-confirmed. Fails when no compatible build exists.
   */
  static PluginRegistryMutationResult installIndexedPlugin(
      const std::string& plugin_id);

  /**
   * @brief Check registered plugins for newer upstream releases
   *
   * For every non-core registry entry with a GitHub source repository, the
   * repository's latest published release is fetched and compared against the
   * installed version. Entries without a usable repository URL are reported
   * NotApplicable; network failures are reported Unreachable. Results for the
   * same repository are fetched once and shared between entries.
   *
   * Performs one network request per distinct repository; callers should run
   * it off the UI thread.
   */
  static std::vector<PluginUpdateStatusInfo> checkRegisteredPluginUpdates();

  /**
   * @brief Update a registered remote plugin to its latest upstream release
   *
   * Re-resolves the plugin's release asset from its recorded source
   * repository (like the add-from-URL path) and rewrites the registry entry
   * to point at the latest release. The recorded sha256 pin is cleared (the
   * new artifact has no reviewed digest) and the entry reverts to untrusted,
   * so the user must explicitly re-confirm trust before the new binary is
   * downloaded or loaded.
   */
  static PluginRegistryMutationResult updateRegisteredPluginToLatestRelease(
      const std::string& selector);

  /**
   * @brief Get stage instance for parameter editing
   * @param stage_name Stage type name
   * @return Fresh stage instance or nullptr if not found
   */
  static std::shared_ptr<void> createStageInstance(
      const std::string& stage_name);

  // === Batch Operations ===

  /**
   * @brief Check if a node can be triggered
   * @param node_id Node to check
   * @param reason Output parameter for reason if cannot trigger
   * @return true if can trigger
   */
  bool canTriggerNode(NodeID node_id, std::string* reason) const override;

  /**
   * @brief Trigger batch processing for a node
   * @param node_id Node to trigger
   * @param progress_callback Optional progress callback
   * @return true on success
   */
  bool triggerNode(NodeID node_id, ProgressCallback progress_callback) override;

  /**
   * @brief Trigger all sink nodes in the project
   * @param progress_callback Optional progress callback
   * @return true if all sinks succeeded
   *
   * Finds all triggerable sink nodes in the project and executes them
   * sequentially. Progress callback is invoked for each sink node being
   * processed.
   */
  bool triggerAllSinks(ProgressCallback progress_callback) override;

  // === Validation ===

  /**
   * @brief Validate the project for errors
   * @return true if project is valid
   */
  bool validateProject() const override;

  /**
   * @brief Get validation errors
   */
  std::vector<std::string> getValidationErrors() const override;

  orc::ConfigurationStatus getNodeConfigurationStatus(
      NodeID node_id) const override;

  // === Project Snapshots ===

  /**
   * @brief Get the current DAG for the project
   * @return Shared pointer to DAG (as void* for encapsulation)
   */
  std::shared_ptr<void> getDAG() const override;

  // === DAG Operations ===

  /**
   * @brief Build DAG from current project structure
   * @return Opaque DAG handle (void*) or nullptr on failure
   *
   * Rebuilds the executable DAG from the project graph.
   * Call this whenever the DAG structure changes (nodes/edges added/removed).
   */
  std::shared_ptr<void> buildDAG() override;

  /**
   * @brief Validate DAG structure
   * @return true if DAG is valid and can be executed
   *
   * Checks for cycles, disconnected subgraphs, missing parameters, etc.
   */
  bool validateDAG() override;

  // === Parameter Operations ===

  /**
   * @brief Get the help text (Markdown) for a stage type
   * @param stage_name Internal stage name
   * @return Markdown string, or empty string if no documentation is available
   */
  std::string getStageInstructions(
      const std::string& stage_name) const override;

  /**
   * @brief Get parameter descriptors for a stage type
   * @param stage_name Internal stage name
   * @return Vector of parameter descriptors
   *
   * Returns all parameters that can be configured for this stage type.
   */
  std::vector<ParameterDescriptor> getStageParameters(
      const std::string& stage_name) override;

  /**
   * @brief Get current parameters for a specific node
   * @param node_id Node to query
   * @return Map of parameter name -> current value
   */
  std::map<std::string, ParameterValue> getNodeParameters(
      NodeID node_id) override;

  /**
   * @brief Set parameters for a specific node
   * @param node_id Node to configure
   * @param params Map of parameter name -> new value
   * @return true on success
   *
   * Updates the node's parameter values and marks project as modified.
   */
  bool setNodeParameters(
      NodeID node_id,
      const std::map<std::string, ParameterValue>& params) override;

  /**
   * @brief Get raw project pointer for low-level access
   * @return Non-owning pointer to core Project
   *
   * This is needed for components that require direct Project access (e.g.,
   * RenderPresenter). The presenter retains ownership of the Project.
   */
  /**
   * @brief Get opaque handle to core project
   * @return Opaque handle to project
   *
   * @note This method provides direct Project access for components like
   * RenderPresenter that manage DAG lifecycle. The presenter retains ownership.
   * New GUI code should use presenter methods instead.
   */
  void* getCoreProjectHandle() override {
    return external_project_ ? external_project_ : project_.get();
  }

 private:
  // Helper to get project pointer (owned or external)
  orc::Project* getProject() {
    return external_project_ ? external_project_ : project_.get();
  }
  const orc::Project* getProject() const {
    return external_project_ ? external_project_ : project_.get();
  }

  std::unique_ptr<orc::Project>
      project_;  ///< Owned project (if constructed without existing project)
  orc::Project* external_project_ =
      nullptr;  ///< Non-owned external project (if constructed with existing
                ///< project)
  std::string project_path_;
  bool is_modified_;
  mutable std::shared_ptr<void> dag_;  ///< Cached DAG instance
};

}  // namespace orc::presenters
