/*
 * File:        project_to_dag_contract_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Phase 5 contracts for project-to-DAG wiring
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../../orc/core/include/dag_executor.h"
#include "../../../orc/core/include/project.h"
#include "../../../orc/core/include/project_to_dag.h"
#include "../include/public_stage_inventory.h"

namespace orc_unit_test {
namespace {
struct StageChain {
  std::string source;
  std::string middle;
  std::string sink;
};

std::optional<StageChain> find_representative_chain() {
  std::vector<std::string> source_names;
  std::vector<std::string> middle_names;
  std::vector<std::string> sink_names;

  for (const auto& spec : public_stage_specs()) {
    if (!spec.registry_backed) {
      continue;
    }
    const auto info = spec.create()->get_node_type_info();

    if (spec.family == PublicStageFamily::Source) {
      source_names.push_back(info.stage_name);
    } else if (spec.family == PublicStageFamily::Transform) {
      middle_names.push_back(info.stage_name);
    } else {
      sink_names.push_back(info.stage_name);
    }
  }

  for (const auto& source : source_names) {
    for (const auto& middle : middle_names) {
      if (!orc::is_connection_valid(source, middle)) {
        continue;
      }

      for (const auto& sink : sink_names) {
        if (orc::is_connection_valid(middle, sink)) {
          return StageChain{source, middle, sink};
        }
      }
    }
  }

  return std::nullopt;
}

std::string first_source_stage_name() {
  for (const auto& spec : public_stage_specs()) {
    if (spec.registry_backed && spec.family == PublicStageFamily::Source) {
      return spec.create()->get_node_type_info().stage_name;
    }
  }

  return {};
}
}  // namespace

TEST(ProjectToDagContractTest, Converts_RepresentativePublicPipeline) {
  const auto chain = find_representative_chain();
  if (!chain.has_value()) {
    FAIL() << "Expected chain to have a value";
    return;
  }

  auto project = orc::project_io::create_empty_project(
      "contract-test-project", orc::VideoSystem::Unknown,
      orc::SourceType::Unknown);
  const auto source_id =
      orc::project_io::add_node(project, chain->source, 0.0, 0.0);
  const auto middle_id =
      orc::project_io::add_node(project, chain->middle, 100.0, 0.0);
  const auto sink_id =
      orc::project_io::add_node(project, chain->sink, 200.0, 0.0);

  orc::project_io::add_edge(project, source_id, middle_id);
  orc::project_io::add_edge(project, middle_id, sink_id);

  const auto dag = orc::project_to_dag(project);

  ASSERT_NE(dag, nullptr);
  EXPECT_TRUE(dag->validate());
  EXPECT_EQ(dag->nodes().size(), 3u);
  ASSERT_EQ(dag->output_nodes().size(), 1u);
  EXPECT_EQ(dag->output_nodes().front(), sink_id);
}

TEST(ProjectToDagContractTest, Placeholder_SourcesPassSourceValidation) {
  const auto source_stage_name = first_source_stage_name();
  ASSERT_FALSE(source_stage_name.empty());

  auto project = orc::project_io::create_empty_project(
      "placeholder-source-project", orc::VideoSystem::Unknown,
      orc::SourceType::Unknown);
  orc::project_io::add_node(project, source_stage_name, 0.0, 0.0);

  const auto dag = orc::project_to_dag(project);
  ASSERT_NE(dag, nullptr);
  EXPECT_NO_THROW(orc::validate_source_nodes(dag));
}

TEST(ProjectToDagContractTest,
     Cvbs_SourceParametersPersistWithoutTbcMetadataSidecar) {
  auto project = orc::project_io::create_empty_project(
      "cvbs-params-project", orc::VideoSystem::PAL, orc::SourceType::Composite);

  const auto source_id =
      orc::project_io::add_node(project, "PAL_CVBS_Source", 0.0, 0.0);

  std::map<std::string, orc::ParameterValue> params = {
      {"input_path", std::string("fixtures/test.cvbs")},
      {"use_metadata", false},
      {"sample_encoding", std::string("CVBS_TPG21_4FSC")}};

  EXPECT_NO_THROW(
      orc::project_io::set_node_parameters(project, source_id, params));

  const auto& nodes = project.get_nodes();
  const auto node_it = std::find_if(
      nodes.begin(), nodes.end(),
      [source_id](const auto& node) { return node.node_id == source_id; });

  ASSERT_NE(node_it, nodes.end());
  EXPECT_EQ(node_it->parameters, params);
}

TEST(ProjectToDagContractTest, UnknownStageInProject_FailsCleanly) {
  auto project = orc::project_io::create_empty_project(
      "invalid-stage-project", orc::VideoSystem::Unknown,
      orc::SourceType::Unknown);

  std::vector<orc::ProjectDAGNode> nodes = {{orc::NodeID(1),
                                             "unregistered_stage",
                                             orc::NodeType::TRANSFORM,
                                             "Missing",
                                             "Missing",
                                             0.0,
                                             0.0,
                                             {}}};

  orc::project_io::update_project_dag(project, nodes, {});

  EXPECT_THROW(orc::project_to_dag(project), orc::ProjectConversionError);
}

// ── Stage instance reuse across rebuilds ─────────────────────────────────
//
// Every edit in the DAG editor rebuilds the whole DAG. Discarding every stage
// object each time also discards what execute() had accumulated behind them —
// for a source, the representation it loaded, which on a long capture is
// seconds of work. A node the edit did not touch keeps its instance so an
// unrelated change no longer pays for that.

namespace {

// The stage instance behind |node_id|, or nullptr if the DAG has no such node.
const orc::DAGStage* stage_of(const std::shared_ptr<orc::DAG>& dag,
                              orc::NodeID node_id) {
  for (const auto& node : dag->nodes()) {
    if (node.node_id == node_id) return node.stage.get();
  }
  return nullptr;
}

}  // namespace

TEST(ProjectToDagContractTest,
     ReusesStageInstances_ForNodesTheEditDidNotTouch) {
  const auto chain = find_representative_chain();
  ASSERT_TRUE(chain.has_value());

  auto project = orc::project_io::create_empty_project(
      "reuse-project", orc::VideoSystem::Unknown, orc::SourceType::Unknown);
  const auto source_id =
      orc::project_io::add_node(project, chain->source, 0.0, 0.0);
  const auto middle_id =
      orc::project_io::add_node(project, chain->middle, 100.0, 0.0);
  const auto sink_id =
      orc::project_io::add_node(project, chain->sink, 200.0, 0.0);
  orc::project_io::add_edge(project, source_id, middle_id);
  orc::project_io::add_edge(project, middle_id, sink_id);

  const auto first = orc::project_to_dag(project);
  ASSERT_NE(first, nullptr);

  // The edit: drop the link between the middle stage and the sink. It says
  // nothing about the source, which is where the expensive state lives.
  orc::project_io::remove_edge(project, middle_id, sink_id);
  const auto second = orc::project_to_dag(project, first.get());
  ASSERT_NE(second, nullptr);

  EXPECT_EQ(stage_of(second, source_id), stage_of(first, source_id));
  EXPECT_EQ(stage_of(second, middle_id), stage_of(first, middle_id));
}

TEST(ProjectToDagContractTest, BuildsAFreshStage_WhenANodesWiringChanged) {
  const auto chain = find_representative_chain();
  ASSERT_TRUE(chain.has_value());

  auto project = orc::project_io::create_empty_project(
      "rewire-project", orc::VideoSystem::Unknown, orc::SourceType::Unknown);
  const auto source_id =
      orc::project_io::add_node(project, chain->source, 0.0, 0.0);
  const auto middle_id =
      orc::project_io::add_node(project, chain->middle, 100.0, 0.0);
  const auto sink_id =
      orc::project_io::add_node(project, chain->sink, 200.0, 0.0);
  orc::project_io::add_edge(project, source_id, middle_id);
  orc::project_io::add_edge(project, middle_id, sink_id);

  const auto first = orc::project_to_dag(project);
  ASSERT_NE(first, nullptr);

  // The sink loses its input, so anything it cached was computed from a graph
  // it is no longer part of. Its instance must not be carried over — a stage
  // that does not declare the reserved input-node-ids parameter has no other
  // way to notice.
  orc::project_io::remove_edge(project, middle_id, sink_id);
  const auto second = orc::project_to_dag(project, first.get());
  ASSERT_NE(second, nullptr);

  EXPECT_NE(stage_of(second, sink_id), stage_of(first, sink_id));
}

TEST(ProjectToDagContractTest, BuildsFreshStages_DownstreamOfAChangedNode) {
  const auto chain = find_representative_chain();
  ASSERT_TRUE(chain.has_value());

  auto project = orc::project_io::create_empty_project(
      "downstream-project", orc::VideoSystem::Unknown,
      orc::SourceType::Unknown);
  const auto source_id =
      orc::project_io::add_node(project, chain->source, 0.0, 0.0);
  const auto middle_id =
      orc::project_io::add_node(project, chain->middle, 100.0, 0.0);
  const auto sink_id =
      orc::project_io::add_node(project, chain->sink, 200.0, 0.0);
  orc::project_io::add_edge(project, source_id, middle_id);
  orc::project_io::add_edge(project, middle_id, sink_id);

  const auto first = orc::project_to_dag(project);
  ASSERT_NE(first, nullptr);

  // Cutting the middle stage off from the source rebuilds it, because its own
  // wiring changed. The sink's configuration and wiring are untouched — it
  // still takes one input, still from the middle stage — but what the middle
  // stage now produces is different, so anything the sink cached came from a
  // graph that no longer exists.
  orc::project_io::remove_edge(project, source_id, middle_id);
  const auto second = orc::project_to_dag(project, first.get());
  ASSERT_NE(second, nullptr);

  EXPECT_NE(stage_of(second, middle_id), stage_of(first, middle_id));
  EXPECT_NE(stage_of(second, sink_id), stage_of(first, sink_id));

  // The source sits upstream of the change, so it keeps whatever it loaded.
  // This is the whole point: an edit downstream must not cost a source reload.
  EXPECT_EQ(stage_of(second, source_id), stage_of(first, source_id));
}

TEST(ProjectToDagContractTest, BuildsAFreshStage_WhenAParameterChanged) {
  auto project = orc::project_io::create_empty_project(
      "reparam-project", orc::VideoSystem::PAL, orc::SourceType::Composite);
  const auto source_id =
      orc::project_io::add_node(project, "PAL_CVBS_Source", 0.0, 0.0);
  orc::project_io::set_node_parameters(
      project, source_id,
      {{"input_path", std::string("fixtures/before.cvbs")}});

  const auto first = orc::project_to_dag(project);
  ASSERT_NE(first, nullptr);

  // Pointing the source somewhere else is exactly the case where the loaded
  // representation must not survive.
  orc::project_io::set_node_parameters(
      project, source_id, {{"input_path", std::string("fixtures/after.cvbs")}});
  const auto second = orc::project_to_dag(project, first.get());
  ASSERT_NE(second, nullptr);

  EXPECT_NE(stage_of(second, source_id), stage_of(first, source_id));
}

TEST(ProjectToDagContractTest, BuildsAFreshStage_WhenANodeIdChangedStage) {
  const auto chain = find_representative_chain();
  ASSERT_TRUE(chain.has_value());
  ASSERT_NE(chain->middle, chain->sink);

  // Built through update_project_dag() throughout, because it is the only way
  // to put a different stage behind an id that is already in use. (It also
  // preserves SOURCE nodes, so the two stages swapped here are not sources.)
  auto project = orc::project_io::create_empty_project(
      "restage-project", orc::VideoSystem::Unknown, orc::SourceType::Unknown);
  const orc::NodeID node_id(1);

  orc::project_io::update_project_dag(project,
                                      {{node_id,
                                        chain->middle,
                                        orc::NodeType::TRANSFORM,
                                        "Middle",
                                        "Middle",
                                        0.0,
                                        0.0,
                                        {}}},
                                      {});
  const auto first = orc::project_to_dag(project);
  ASSERT_NE(first, nullptr);

  // A node id can be re-used by a different stage type, and matching on the id
  // alone would hand that stage the previous stage's object entirely.
  orc::project_io::update_project_dag(project,
                                      {{node_id,
                                        chain->sink,
                                        orc::NodeType::SINK,
                                        "Sink",
                                        "Sink",
                                        0.0,
                                        0.0,
                                        {}}},
                                      {});
  const auto second = orc::project_to_dag(project, first.get());
  ASSERT_NE(second, nullptr);

  EXPECT_NE(stage_of(second, node_id), stage_of(first, node_id));
}

TEST(ProjectToDagContractTest, BuildsEveryStageFresh_WhenGivenNoPreviousDag) {
  const auto chain = find_representative_chain();
  ASSERT_TRUE(chain.has_value());

  auto project = orc::project_io::create_empty_project(
      "no-previous-project", orc::VideoSystem::Unknown,
      orc::SourceType::Unknown);
  const auto source_id =
      orc::project_io::add_node(project, chain->source, 0.0, 0.0);

  const auto first = orc::project_to_dag(project);
  // The default: a build with no previous DAG owns its stages outright, which
  // is what a throwaway conversion check relies on.
  const auto second = orc::project_to_dag(project);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  EXPECT_NE(stage_of(second, source_id), stage_of(first, source_id));
}

// ── DAG cloning for per-thread execution ─────────────────────────────────
//
// Stages are stateful and several re-apply their parameter map from
// execute(), so a consumer that executes a shared DAG on its own thread
// (every background observation worker) takes a clone first. The clone must
// be the same pipeline, configured identically, with nothing shared behind
// it — anything less puts two threads back inside one stage object.

TEST(DagCloneContractTest, Clone_ReproducesTheDagWithItsOwnStages) {
  const auto chain = find_representative_chain();
  ASSERT_TRUE(chain.has_value());

  auto project = orc::project_io::create_empty_project(
      "clone-test-project", orc::VideoSystem::Unknown,
      orc::SourceType::Unknown);
  const auto source_id =
      orc::project_io::add_node(project, chain->source, 0.0, 0.0);
  const auto middle_id =
      orc::project_io::add_node(project, chain->middle, 100.0, 0.0);
  const auto sink_id =
      orc::project_io::add_node(project, chain->sink, 200.0, 0.0);
  orc::project_io::add_edge(project, source_id, middle_id);
  orc::project_io::add_edge(project, middle_id, sink_id);

  const auto dag = orc::project_to_dag(project);
  ASSERT_NE(dag, nullptr);

  const auto clone = orc::clone_dag_with_fresh_stages(*dag);
  ASSERT_NE(clone, nullptr);

  EXPECT_TRUE(clone->validate());
  ASSERT_EQ(clone->nodes().size(), dag->nodes().size());
  EXPECT_EQ(clone->output_nodes(), dag->output_nodes());

  for (size_t i = 0; i < clone->nodes().size(); ++i) {
    const auto& original = dag->nodes()[i];
    const auto& copy = clone->nodes()[i];

    EXPECT_EQ(copy.node_id, original.node_id);
    EXPECT_EQ(copy.input_node_ids, original.input_node_ids);
    EXPECT_EQ(copy.input_indices, original.input_indices);
    EXPECT_EQ(copy.parameters, original.parameters);

    ASSERT_NE(copy.stage, nullptr);
    // The point of the exercise: a different object of the same stage.
    EXPECT_NE(copy.stage.get(), original.stage.get());
    EXPECT_EQ(copy.stage->get_node_type_info().stage_name,
              original.stage->get_node_type_info().stage_name);
  }
}

// A clone that came up with default parameters would quietly render something
// other than the pipeline the user configured.
TEST(DagCloneContractTest, Clone_CarriesTheConfiguredParameters) {
  auto project = orc::project_io::create_empty_project(
      "clone-params-project", orc::VideoSystem::Unknown,
      orc::SourceType::Unknown);
  const auto node_id =
      orc::project_io::add_node(project, "frame_map", 0.0, 0.0);
  orc::project_io::set_node_parameters(
      project, node_id, {{"ranges", orc::ParameterValue{std::string("3-7")}}});

  const auto dag = orc::project_to_dag(project);
  ASSERT_NE(dag, nullptr);
  const auto clone = orc::clone_dag_with_fresh_stages(*dag);
  ASSERT_NE(clone, nullptr);
  ASSERT_EQ(clone->nodes().size(), 1u);

  auto* parameterized =
      dynamic_cast<orc::ParameterizedStage*>(clone->nodes()[0].stage.get());
  ASSERT_NE(parameterized, nullptr);
  const auto params = parameterized->get_parameters();
  const auto it = params.find("ranges");
  ASSERT_NE(it, params.end());
  EXPECT_EQ(std::get<std::string>(it->second), "3-7");
}

// ── Reserved input-identity parameter ────────────────────────────────────
//
// A stage that orders its inputs by node ID (source_join does) can only learn
// which node feeds each entry of execute()'s inputs vector from the host:
// artifacts carry no node identity. project_to_dag() fills the reserved
// parameter in for the stages that declare it, and leaves every other stage
// alone.

namespace {
std::optional<std::string> parameter_of(const orc::DAG& dag,
                                        orc::NodeID node_id,
                                        const std::string& name) {
  for (const auto& node : dag.nodes()) {
    if (node.node_id != node_id) continue;
    const auto it = node.parameters.find(name);
    if (it == node.parameters.end()) return std::nullopt;
    return std::get<std::string>(it->second);
  }
  return std::nullopt;
}
}  // namespace

TEST(ProjectToDagInputIdentityTest, FillsReservedParameterInConnectionOrder) {
  auto project = orc::project_io::create_empty_project(
      "join-identity-project", orc::VideoSystem::Unknown,
      orc::SourceType::Unknown);
  const auto first = orc::project_io::add_node(project, "frame_map", 0.0, 0.0);
  const auto second = orc::project_io::add_node(project, "frame_map", 0.0, 0.0);
  const auto join = orc::project_io::add_node(project, "source_join", 0.0, 0.0);
  orc::project_io::add_edge(project, first, join);
  orc::project_io::add_edge(project, second, join);

  const auto dag = orc::project_to_dag(project);
  ASSERT_NE(dag, nullptr);

  const auto value = parameter_of(*dag, join, orc::kInputNodeIdsParameter);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, std::to_string(first.value()) + "," +
                        std::to_string(second.value()));
}

// The value is host-owned, so it has to reach the stage instance the executor
// runs — not just the node's parameter map.
TEST(ProjectToDagInputIdentityTest, ReservedParameterReachesTheStageInstance) {
  auto project = orc::project_io::create_empty_project(
      "join-identity-stage-project", orc::VideoSystem::Unknown,
      orc::SourceType::Unknown);
  const auto upstream =
      orc::project_io::add_node(project, "frame_map", 0.0, 0.0);
  const auto join = orc::project_io::add_node(project, "source_join", 0.0, 0.0);
  orc::project_io::add_edge(project, upstream, join);

  const auto dag = orc::project_to_dag(project);
  ASSERT_NE(dag, nullptr);

  for (const auto& node : dag->nodes()) {
    if (node.node_id != join) continue;
    auto* parameterized =
        dynamic_cast<orc::ParameterizedStage*>(node.stage.get());
    ASSERT_NE(parameterized, nullptr);
    const auto params = parameterized->get_parameters();
    const auto it = params.find(orc::kInputNodeIdsParameter);
    ASSERT_NE(it, params.end());
    EXPECT_EQ(std::get<std::string>(it->second),
              std::to_string(upstream.value()));
    return;
  }
  FAIL() << "join node missing from the DAG";
}

TEST(ProjectToDagInputIdentityTest, LeavesStagesThatDoNotDeclareItAlone) {
  auto project = orc::project_io::create_empty_project(
      "no-identity-project", orc::VideoSystem::Unknown,
      orc::SourceType::Unknown);
  const auto upstream =
      orc::project_io::add_node(project, "frame_map", 0.0, 0.0);
  const auto downstream =
      orc::project_io::add_node(project, "frame_map", 0.0, 0.0);
  orc::project_io::add_edge(project, upstream, downstream);

  const auto dag = orc::project_to_dag(project);
  ASSERT_NE(dag, nullptr);
  EXPECT_FALSE(
      parameter_of(*dag, downstream, orc::kInputNodeIdsParameter).has_value());
}

// ── Format-aware default parameter tests ─────────────────────────────────
//
// These tests guard against a class of bug where a project file does not
// store every parameter explicitly (e.g. a PAL project saved before
// decoder_type was persisted), causing project_to_dag() to leave the stage
// at its constructor default ("ntsc2d") rather than the correct format
// default ("pal2d").  The fix in project_to_dag.cpp seeds the parameter map
// from get_parameter_descriptors(video_format, source_type) *before*
// overlaying whatever is actually in the project file.

namespace {
// Extract the runtime decoder_type from the stage instance wired by
// project_to_dag.
std::string decoder_type_from_dag(const orc::DAG& dag, orc::NodeID node_id) {
  for (const auto& node : dag.nodes()) {
    if (node.node_id != node_id) {
      continue;
    }
    auto* p = dynamic_cast<orc::ParameterizedStage*>(node.stage.get());
    if (!p) {
      return "";
    }
    auto params = p->get_parameters();
    auto it = params.find("decoder_type");
    if (it == params.end()) {
      return "";
    }
    return std::holds_alternative<std::string>(it->second)
               ? std::get<std::string>(it->second)
               : "";
  }
  return "";
}
}  // namespace

TEST(ProjectToDagFormatDefaultsTest,
     Pal_ProjectGivesVideoSinkPalDecodeDefault) {
  // Simulate a PAL project that has an video_sink node with NO
  // stored decoder_type (as would be the case for any project created
  // before the parameter was explicitly persisted).
  auto project = orc::project_io::create_empty_project(
      "pal-defaults", orc::VideoSystem::PAL, orc::SourceType::Composite);

  const auto sink_id =
      orc::project_io::add_node(project, "video_sink", 0.0, 0.0);
  // Deliberately set NO parameters on the node.

  const auto dag = orc::project_to_dag(project);
  ASSERT_NE(dag, nullptr);

  EXPECT_EQ(decoder_type_from_dag(*dag, sink_id), "pal2d")
      << "PAL project with no stored decoder_type should default to 'pal2d', "
         "not the constructor default 'ntsc2d'";
}

TEST(ProjectToDagFormatDefaultsTest,
     Ntsc_ProjectGivesVideoSinkNtscDecodeDefault) {
  auto project = orc::project_io::create_empty_project(
      "ntsc-defaults", orc::VideoSystem::NTSC, orc::SourceType::Composite);

  const auto sink_id =
      orc::project_io::add_node(project, "video_sink", 0.0, 0.0);

  const auto dag = orc::project_to_dag(project);
  ASSERT_NE(dag, nullptr);

  EXPECT_EQ(decoder_type_from_dag(*dag, sink_id), "ntsc2d")
      << "NTSC project with no stored decoder_type should default to 'ntsc2d'";
}

TEST(ProjectToDagFormatDefaultsTest, Stored_DecoderTypeOverridesFormatDefault) {
  // If an explicit decoder_type IS stored (e.g. user changed it to "mono"),
  // it must win over the format-derived default.
  auto project = orc::project_io::create_empty_project(
      "pal-explicit", orc::VideoSystem::PAL, orc::SourceType::Composite);

  const auto sink_id =
      orc::project_io::add_node(project, "video_sink", 0.0, 0.0);

  std::map<std::string, orc::ParameterValue> stored;
  stored["decoder_type"] = std::string("mono");
  orc::project_io::set_node_parameters(project, sink_id, stored);

  const auto dag = orc::project_to_dag(project);
  ASSERT_NE(dag, nullptr);

  EXPECT_EQ(decoder_type_from_dag(*dag, sink_id), "mono")
      << "Explicitly stored decoder_type='mono' must override the PAL format "
         "default";
}

TEST(ProjectToDagFormatDefaultsTest,
     AllRegistryStagesHave_DefaultsForPALFormat) {
  // Every parameter descriptor returned for VideoSystem::PAL must carry a
  // default_value so that project_to_dag() can seed any parameter that is
  // absent from the project file.  Parameters that only appear in NTSC
  // descriptors (e.g. ntsc_phase_comp) are invisible here and are not
  // checked — format-filtering is intentional.
  for (const auto& spec : public_stage_specs()) {
    if (!spec.registry_backed) {
      continue;
    }

    auto stage = spec.create();
    auto* p = dynamic_cast<orc::ParameterizedStage*>(stage.get());
    if (!p) {
      continue;
    }

    const auto descriptors = p->get_parameter_descriptors(
        orc::VideoSystem::PAL, orc::SourceType::Composite);

    for (const auto& desc : descriptors) {
      EXPECT_TRUE(desc.constraints.default_value.has_value())
          << spec.inventory_id << ": PAL descriptor for '" << desc.name
          << "' has no default_value — project_to_dag cannot seed this "
             "parameter "
             "when it is absent from the project file";
    }
  }
}

TEST(ProjectToDagFormatDefaultsTest,
     AllRegistryStagesHave_DefaultsForNTSCFormat) {
  // Same check for VideoSystem::NTSC.
  for (const auto& spec : public_stage_specs()) {
    if (!spec.registry_backed) {
      continue;
    }

    auto stage = spec.create();
    auto* p = dynamic_cast<orc::ParameterizedStage*>(stage.get());
    if (!p) {
      continue;
    }

    const auto descriptors = p->get_parameter_descriptors(
        orc::VideoSystem::NTSC, orc::SourceType::Composite);

    for (const auto& desc : descriptors) {
      EXPECT_TRUE(desc.constraints.default_value.has_value())
          << spec.inventory_id << ": NTSC descriptor for '" << desc.name
          << "' has no default_value — project_to_dag cannot seed this "
             "parameter "
             "when it is absent from the project file";
    }
  }
}
// ---------------------------------------------------------------------------
// Path parameter resolution
//
// Project files store paths as the user gave them, so anything that hands a
// node's stored parameters to a stage — execution and the status dot alike —
// has to resolve them against the project root first.
// ---------------------------------------------------------------------------

TEST(ProjectPathParameterTest, ResolvesARelativePathAgainstTheProjectRoot) {
  std::map<std::string, orc::ParameterValue> parameters{
      {"input_path", std::string("captures/take1.tbc")}};

  orc::resolve_path_parameters(parameters, "/projects/demo");

  EXPECT_EQ(std::get<std::string>(parameters.at("input_path")),
            "/projects/demo/captures/take1.tbc");
}

TEST(ProjectPathParameterTest, ExpandsTheProjectRootVariable) {
  std::map<std::string, orc::ParameterValue> parameters{
      {"output_path", std::string("${PROJECT_ROOT}/out/frames.mkv")}};

  orc::resolve_path_parameters(parameters, "/projects/demo");

  EXPECT_EQ(std::get<std::string>(parameters.at("output_path")),
            "/projects/demo/out/frames.mkv");
}

TEST(ProjectPathParameterTest, LeavesAnAbsolutePathAlone) {
  std::map<std::string, orc::ParameterValue> parameters{
      {"y_path", std::string("/captures/take1.tbcy")}};

  orc::resolve_path_parameters(parameters, "/projects/demo");

  EXPECT_EQ(std::get<std::string>(parameters.at("y_path")),
            "/captures/take1.tbcy");
}

TEST(ProjectPathParameterTest, LeavesNonPathAndEmptyParametersUntouched) {
  std::map<std::string, orc::ParameterValue> parameters{
      {"format", std::string("bt8x8-pal")},
      {"input_path", std::string("")},
      {"first_field", uint32_t{1}},
  };

  orc::resolve_path_parameters(parameters, "/projects/demo");

  EXPECT_EQ(std::get<std::string>(parameters.at("format")), "bt8x8-pal");
  EXPECT_EQ(std::get<std::string>(parameters.at("input_path")), "");
  EXPECT_EQ(std::get<uint32_t>(parameters.at("first_field")), 1u);
}

TEST(ProjectPathParameterTest, IsANoOpWithoutAProjectRoot) {
  std::map<std::string, orc::ParameterValue> parameters{
      {"input_path", std::string("captures/take1.tbc")}};

  orc::resolve_path_parameters(parameters, "");

  EXPECT_EQ(std::get<std::string>(parameters.at("input_path")),
            "captures/take1.tbc");
}

}  // namespace orc_unit_test