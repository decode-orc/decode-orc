/*
 * File:        project.h
 * Module:      orc-core
 * Purpose:     Project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// =============================================================================
// MVP Architecture Enforcement
// =============================================================================
// This header is part of the CORE internal implementation.
// GUI and CLI code must NOT include this header directly.
// Use ProjectPresenter from orc/presenters instead.
// =============================================================================
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/project.h. Use ProjectPresenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/project.h. Use ProjectPresenter instead."
#endif

#include <orc/stage/node_id.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>

#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace orc {

// Forward declarations
class Project;

// Forward declare structs and project_io namespace functions
struct ProjectDAGNode;
struct ProjectDAGEdge;

/**
 * @brief Describes the capabilities and constraints for a DAG node
 *
 * Used by the GUI to determine which operations are valid for a node
 * (e.g., can it be removed, triggered, or inspected).
 */
struct NodeCapabilities {
  bool can_remove = false;    ///< Whether the node can be removed from the DAG
  std::string remove_reason;  ///< Explanation if node cannot be removed

  bool can_trigger =
      false;  ///< Whether the node can be triggered (batch processing)
  std::string trigger_reason;  ///< Explanation if node cannot be triggered

  NodeID node_id;          ///< Node identifier
  std::string stage_name;  ///< Stage type name
  std::string node_label;  ///< User-visible label
};

/**
 * @brief Progress callback for triggerable stages
 *
 * Called periodically during batch processing to report progress.
 *
 * @param current Current item being processed
 * @param total Total items to process
 * @param message Status message describing current operation
 */
using TriggerProgressCallback = std::function<void(size_t current, size_t total,
                                                   const std::string& message)>;

// Forward declaration from triggerable_stage.h
class TriggerableStage;

namespace project_io {
Project load_project(const std::string& filename);
Project load_project_from_yaml(const std::string& yaml_text,
                               const std::string& filename_hint);
void save_project(Project& project, const std::string& filename);
std::string serialize_project_to_yaml(const Project& project,
                                      const std::string& filename_hint);
Project create_empty_project(const std::string& project_name,
                             VideoSystem video_format = VideoSystem::Unknown,
                             SourceType source_format = SourceType::Unknown);
void update_project_dag(Project& project,
                        const std::vector<ProjectDAGNode>& nodes,
                        const std::vector<ProjectDAGEdge>& edges);
NodeID generate_unique_node_id(const Project& project);
NodeID add_node(Project& project, const std::string& stage_name,
                double x_position, double y_position);
void remove_node(Project& project, NodeID node_id);
bool can_remove_node(const Project& project, NodeID node_id,
                     std::string* reason);
void set_node_parameters(
    Project& project, NodeID node_id,
    const std::map<std::string, ParameterValue>& parameters);
void set_node_position(Project& project, NodeID node_id, double x_position,
                       double y_position);
void set_node_label(Project& project, NodeID node_id, const std::string& label);
void add_edge(Project& project, NodeID source_node_id, NodeID target_node_id);
void remove_edge(Project& project, NodeID source_node_id,
                 NodeID target_node_id);
void clear_project(Project& project);
bool can_trigger_node(const Project& project, NodeID node_id,
                      std::string* reason);
bool trigger_node(Project& project, NodeID node_id, std::string& status_out,
                  TriggerProgressCallback progress_callback = nullptr);
std::future<std::pair<bool, std::string>> trigger_node_async(
    Project& project, NodeID node_id,
    TriggerProgressCallback progress_callback = nullptr);

std::string find_source_file_for_node(const Project& project, NodeID node_id);

// Get all capabilities for a node in a single call
NodeCapabilities get_node_capabilities(const Project& project, NodeID node_id);

// Project metadata setters
void set_project_name(Project& project, const std::string& name);
void set_project_description(Project& project, const std::string& description);
void set_video_format(Project& project, VideoSystem video_format);
void set_source_format(Project& project, SourceType source_format);
void set_amplitude_unit(Project& project, AmplitudeDisplayUnit unit);
}  // namespace project_io

/**
 * Node in a project DAG
 * All nodes are uniform - SOURCE nodes use a source stage with input_path
 * parameter
 */
struct ProjectDAGNode {
  NodeID node_id;
  std::string stage_name;    // e.g., "tbc_source", "dropout_correct", etc.
  NodeType node_type;        // Node type (SOURCE, SINK, TRANSFORM, etc.)
  std::string display_name;  // Display name for GUI (e.g., "Source: video.tbc",
                             // "Noise Filter")
  std::string
      user_label;     // User-editable label (initially same as display_name)
  double x_position;  // Position for GUI layout
  double y_position;
  std::map<std::string, ParameterValue>
      parameters;  // Stage parameters (e.g., input_path/db_path for sources)
};

/**
 * Edge in a project DAG
 */
struct ProjectDAGEdge {
  NodeID source_node_id;
  NodeID target_node_id;
};

struct ProjectPluginRequirement {
  std::string plugin_id;
  std::string plugin_version;
  std::string source_repo_url;
  std::string artifact_source = "local_path";
  std::string release_asset_url;
  std::string release_tag;
  std::string release_asset_name;
  std::string target_platform;
  std::string local_dev_path;
  std::string license_spdx;
  bool is_core_plugin = false;
  uint32_t required_host_abi = 0;
  std::vector<std::string> stage_names;
};

/**
 * Project - encapsulates processing DAG
 *
 * A project file (.orc-project) is a YAML file containing:
 * - Project metadata (name, description, version)
 * - DAG structure (nodes, edges, parameters)
 * - Optional required_plugins metadata for third-party plugin-backed stages
 * - SOURCE nodes use the configured source stage with input_path in parameters
 *
 * The project file format is shared between orc-gui and orc-cli.
 * Both tools can load and save projects in the same format.
 *
 * The Project class owns and caches the source representation,
 * ensuring a single source of truth for all consumers.
 *
 * ARCHITECTURE NOTE - STRICT ENCAPSULATION:
 * ============================================
 * ALL fields in this class are PRIVATE and MUST remain private.
 *
 * Access Rules:
 * - READ access: Use public const getters (get_name(), get_nodes(), etc.)
 * - WRITE access: ONLY via project_io namespace functions
 *
 * The GUI, CLI, and all external code:
 * - CANNOT directly modify any Project fields
 * - MUST use project_io functions: add_node(), remove_node(),
 *   set_node_parameters(), trigger_node(), etc.
 * - Can ONLY read via const reference getters
 *
 * This enforces:
 * - Single point of modification (project_io functions)
 * - Consistent modification tracking (is_modified_ flag)
 * - Clear separation between business logic (core) and UI (gui)
 *
 * DO NOT break this architecture by making fields public or adding
 * non-const getters. If you need to modify Project state, add a new
 * project_io function with proper friend declaration.
 */
class Project {
 public:
  // Public const accessors - GUI must use these
  const std::string& get_name() const { return name_; }
  const std::string& get_description() const { return description_; }
  const std::string& get_version() const { return version_; }
  VideoSystem get_video_format() const { return video_format_; }
  SourceType get_source_format() const { return source_format_; }
  AmplitudeDisplayUnit get_amplitude_unit() const { return amplitude_unit_; }
  const std::vector<ProjectDAGNode>& get_nodes() const { return nodes_; }
  const std::vector<ProjectDAGEdge>& get_edges() const { return edges_; }
  const std::vector<ProjectPluginRequirement>& get_required_plugins() const {
    return required_plugins_;
  }
  const std::string& get_project_root() const { return project_root_; }

  // Modification tracking
  void clear_modified_flag() const { is_modified_ = false; }
  bool has_unsaved_changes() const { return is_modified_; }

  /**
   * Check if project has a source node
   */
  bool has_source() const;

  /**
   * Get the source type (Composite or YC) from the project's source nodes
   * @return SourceType::Composite for composite sources (e.g. .tbc),
   *         SourceType::YC for YC sources (e.g. .tbcy/.tbcc),
   *         SourceType::Unknown if no sources or cannot determine
   */
  SourceType get_source_type() const;

 private:
  std::string name_;
  std::string description_;
  std::string version_;
  std::string
      project_root_;  // Absolute path to directory containing the YAML file
  VideoSystem video_format_ = VideoSystem::Unknown;  // NTSC or PAL
  SourceType source_format_ = SourceType::Unknown;   // Composite or YC
  AmplitudeDisplayUnit amplitude_unit_ = AmplitudeDisplayUnit::Samples10Bit;
  std::vector<ProjectDAGNode> nodes_;
  std::vector<ProjectDAGEdge> edges_;
  std::vector<ProjectPluginRequirement> required_plugins_;
  mutable bool is_modified_ = false;

  // Grant project_io namespace functions access (declared below)
  friend Project project_io::load_project(const std::string& filename);
  friend Project project_io::load_project_from_yaml(
      const std::string& yaml_text, const std::string& filename_hint);
  friend void project_io::save_project(Project& project,
                                       const std::string& filename);
  friend std::string project_io::serialize_project_to_yaml(
      const Project& project, const std::string& filename_hint);
  friend Project project_io::create_empty_project(
      const std::string& project_name, VideoSystem video_format,
      SourceType source_format);
  friend void project_io::update_project_dag(
      Project& project, const std::vector<ProjectDAGNode>& nodes,
      const std::vector<ProjectDAGEdge>& edges);
  friend NodeID project_io::generate_unique_node_id(const Project& project);
  friend NodeID project_io::add_node(Project& project,
                                     const std::string& stage_name,
                                     double x_position, double y_position);
  friend void project_io::remove_node(Project& project, NodeID node_id);
  friend bool project_io::can_remove_node(const Project& project,
                                          NodeID node_id, std::string* reason);
  friend void project_io::set_node_parameters(
      Project& project, NodeID node_id,
      const std::map<std::string, ParameterValue>& parameters);
  friend void project_io::set_node_position(Project& project, NodeID node_id,
                                            double x_position,
                                            double y_position);
  friend void project_io::set_node_label(Project& project, NodeID node_id,
                                         const std::string& label);
  friend void project_io::add_edge(Project& project, NodeID source_node_id,
                                   NodeID target_node_id);
  friend void project_io::remove_edge(Project& project, NodeID source_node_id,
                                      NodeID target_node_id);
  friend void project_io::clear_project(Project& project);
  friend bool project_io::can_trigger_node(const Project& project,
                                           NodeID node_id, std::string* reason);
  friend bool project_io::trigger_node(
      Project& project, NodeID node_id, std::string& status_out,
      TriggerProgressCallback progress_callback);
  friend std::string project_io::find_source_file_for_node(
      const Project& project, NodeID node_id);
  friend NodeCapabilities project_io::get_node_capabilities(
      const Project& project, NodeID node_id);
  friend void project_io::set_project_name(Project& project,
                                           const std::string& name);
  friend void project_io::set_project_description(
      Project& project, const std::string& description);
  friend void project_io::set_video_format(Project& project,
                                           VideoSystem video_format);
  friend void project_io::set_source_format(Project& project,
                                            SourceType source_format);
  friend void project_io::set_amplitude_unit(Project& project,
                                             AmplitudeDisplayUnit unit);
};

/**
 * Project file I/O
 */
namespace project_io {
/**
 * Load a project from YAML file
 * @param filename Path to .orc-project file
 * @return Project structure
 * @throws std::runtime_error on parse or I/O errors
 */
Project load_project(const std::string& filename);

/**
 * Save a project to YAML file
 *
 * On success the project's root directory is updated to the directory the
 * file was written to, so that relative file-path parameters resolve the same
 * way in this session as they will when the saved file is reloaded.
 *
 * @param project Project structure to save
 * @param filename Path to .orc-project file
 * @throws std::runtime_error on I/O errors
 */
void save_project(Project& project, const std::string& filename);

/**
 * Create a new empty project with no sources
 * @param project_name Name for the project
 * @param video_format Video format (NTSC or PAL), defaults to Unknown
 * @param source_format Source format (Composite or YC), defaults to Unknown
 * @return Empty project structure
 */
Project create_empty_project(const std::string& project_name,
                             VideoSystem video_format,
                             SourceType source_format);

/**
 * Update project DAG nodes and edges
 * Replaces all nodes and edges with new ones
 * @param project Project to modify
 * @param nodes New DAG nodes
 * @param edges New DAG edges
 */
void update_project_dag(Project& project,
                        const std::vector<ProjectDAGNode>& nodes,
                        const std::vector<ProjectDAGEdge>& edges);

/**
 * Generate a unique node ID for a project
 * Finds the next available ID by examining existing nodes
 * @param project Project to check for existing IDs
 * @return Unique node ID (e.g., "node_1", "node_2", etc.)
 */
NodeID generate_unique_node_id(const Project& project);

/**
 * Add a new node to the project DAG
 * @param project Project to modify
 * @param stage_name Stage type name (e.g., "Passthrough", "DropoutCorrect")
 * @param x_position X position for GUI layout
 * @param y_position Y position for GUI layout
 * @return ID of the newly created node
 * @throws std::runtime_error if stage_name is invalid
 */
NodeID add_node(Project& project, const std::string& stage_name,
                double x_position, double y_position);

/**
 * Remove a node from the project DAG
 * Also removes all edges connected to this node
 * @param project Project to modify
 * @param node_id ID of node to remove
 * @throws std::runtime_error if node_id not found
 */
void remove_node(Project& project, NodeID node_id);

/**
 * Change a node's stage type
 * @param project Project to modify
 * @param node_id ID of node to modify
 * @param new_stage_name New stage type name
 * @throws std::runtime_error if node_id not found or new_stage_name invalid
 */
void change_node_type(Project& project, NodeID node_id,
                      const std::string& new_stage_name);

/**
 * Check if a node's type can be changed
 * @param project Project to check
 * @param node_id ID of node to check
 * @param reason Optional output parameter for why node type cannot be changed
 * @return true if node type can be changed, false otherwise
 */
bool can_change_node_type(const Project& project, NodeID node_id,
                          std::string* reason = nullptr);

/**
 * Update a node's parameters
 * @param project Project to modify
 * @param node_id ID of node to modify
 * @param parameters New parameter map
 * @throws std::runtime_error if node_id not found
 */
void set_node_parameters(
    Project& project, NodeID node_id,
    const std::map<std::string, ParameterValue>& parameters);

/**
 * Update a node's position
 * @param project Project to modify
 * @param node_id ID of node to modify
 * @param x_position New X position
 * @param y_position New Y position
 * @throws std::runtime_error if node_id not found
 */
void set_node_position(Project& project, NodeID node_id, double x_position,
                       double y_position);

/**
 * Update a node's user-defined label
 * @param project Project to modify
 * @param node_id ID of node to modify
 * @param label New user-defined label
 * @throws std::runtime_error if node_id not found
 */
void set_node_label(Project& project, NodeID node_id, const std::string& label);

/**
 * Add an edge to the project DAG
 * @param project Project to modify
 * @param source_node_id Source node ID
 * @param target_node_id Target node ID
 * @throws std::runtime_error if nodes not found or connection invalid
 */
void add_edge(Project& project, NodeID source_node_id, NodeID target_node_id);

/**
 * Remove an edge from the project DAG
 * @param project Project to modify
 * @param source_node_id Source node ID
 * @param target_node_id Target node ID
 * @throws std::runtime_error if edge not found
 */
void remove_edge(Project& project, NodeID source_node_id,
                 NodeID target_node_id);

/**
 * Clear all project data, resetting to empty state
 * Clears name, sources, nodes, edges, and resets modification flag
 * @param project Project to clear
 */
void clear_project(Project& project);

/**
 * Trigger a stage node (for sink stages)
 * Builds DAG, executes to get inputs, and calls trigger() on the stage
 *
 * SAFETY: performs real I/O against whatever the node's (and everything
 * upstream of it) parameters name, including the "-" stdio convention and
 * live network stream URLs (see orc/support/pipe_io.h) — this function has
 * no way to tell whether the caller already validated that use is safe. The
 * CLI (ProjectPresenter::triggerAllSinks(), which calls the batch overload
 * below) always calls ProjectPresenter::validatePipeExecution() first; any
 * other caller — in particular a future GUI action wired to this function or
 * to ProjectPresenter::triggerNode()/triggerAllSinks() — MUST do the same
 * before calling this, exactly as RenderPresenter::triggerStage() checks
 * dag_subgraph_targets_pipe_or_network() before its own trigger() call.
 * Unlike RenderPresenter::triggerStage(), this function is shared with the
 * CLI's legitimate, validated pipe use and so cannot refuse "-"/network URLs
 * unconditionally itself.
 *
 * @param project Project containing the node
 * @param node_id ID of node to trigger
 * @param status_out Output parameter for status message
 * @param progress_callback Optional callback for progress updates (current,
 * total, message)
 * @return true if trigger succeeded, false otherwise
 * @throws std::runtime_error if node not found or not triggerable
 */
bool trigger_node(Project& project, NodeID node_id, std::string& status_out,
                  TriggerProgressCallback progress_callback);

/**
 * Trigger a stage node asynchronously (for sink stages)
 * Builds DAG, executes to get inputs, and calls trigger() on the stage in a
 * background thread. The DAG is kept alive until the trigger operation
 * completes.
 * @param project Project containing the node
 * @param node_id ID of node to trigger
 * @param progress_callback Optional callback for progress updates (current,
 * total, message)
 * @return Future that resolves to pair<success, status_message>
 * @throws std::runtime_error if node not found or not triggerable
 */
std::future<std::pair<bool, std::string>> trigger_node_async(
    Project& project, NodeID node_id,
    TriggerProgressCallback progress_callback);

/**
 * Find source file for a node by tracing back through the DAG
 * @param project Project to search
 * @param node_id ID of node to find source for
 * @return Path to source file, or empty string if not found
 */
std::string find_source_file_for_node(const Project& project, NodeID node_id);
}  // namespace project_io

}  // namespace orc
