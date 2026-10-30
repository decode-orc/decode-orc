/*
 * File:        stage_registry_contract_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Phase 5 contracts for stage registry and node-type discovery
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/tooling/catalogue_results.h>

#include <optional>
#include <set>
#include <string>

#include "../../../orc/core/include/stage_registry.h"
#include "../include/public_stage_inventory.h"

namespace orc_unit_test {
namespace {
struct StageChain {
  std::string source;
  std::string middle;
  std::string sink;
};

std::vector<std::string> public_stage_names() {
  std::vector<std::string> names;
  names.reserve(public_stage_specs().size());

  for (const auto& spec : public_stage_specs()) {
    if (!spec.registry_backed) {
      continue;
    }
    names.push_back(spec.create()->get_node_type_info().stage_name);
  }

  return names;
}

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

std::optional<std::pair<std::string, std::string>> find_many_to_one_pair() {
  const auto names = public_stage_names();
  for (const auto& source_name : names) {
    const auto* source_info = orc::get_node_type_info(source_name);
    if (!source_info || source_info->min_outputs <= 1) {
      continue;
    }

    for (const auto& target_name : names) {
      const auto* target_info = orc::get_node_type_info(target_name);
      if (target_info && target_info->max_inputs == 1) {
        return std::make_pair(source_name, target_name);
      }
    }
  }

  return std::nullopt;
}
}  // namespace

TEST(StageRegistryContractTest, AllPublicStages_AreRegistered) {
  auto& registry = orc::StageRegistry::instance();
  const auto registered_stage_names = registry.get_registered_stages();
  const std::set<std::string> registered(registered_stage_names.begin(),
                                         registered_stage_names.end());

  for (const auto& spec : public_stage_specs()) {
    if (!spec.registry_backed) {
      continue;
    }
    const auto stage_name = spec.create()->get_node_type_info().stage_name;
    EXPECT_TRUE(registered.count(stage_name) > 0)
        << "Missing registered stage '" << stage_name << "'";
  }
}

TEST(StageRegistryContractTest, EachPublicStage_CanBeCreatedFromRegistry) {
  auto& registry = orc::StageRegistry::instance();

  for (const auto& spec : public_stage_specs()) {
    if (!spec.registry_backed) {
      continue;
    }
    const auto expected_name = spec.create()->get_node_type_info().stage_name;
    auto created = registry.create_stage(expected_name);
    ASSERT_NE(created, nullptr)
        << "Registry failed to create '" << expected_name << "'";
    EXPECT_EQ(created->get_node_type_info().stage_name, expected_name);
  }
}

// A stage that advertises a catalogue browser must actually be one: the host
// routes on the tool kind alone and reaches the data by dynamic_cast, so a
// stage that advertises the kind without implementing ICatalogueResults opens
// a viewer that can never be filled. The contract id goes with the kind for
// the same reason — it is what a host that routes on the contract string
// matches.
TEST(StageRegistryContractTest,
     CatalogueBrowserStages_ExposeICatalogueResults) {
  int browsers = 0;
  for (const auto& spec : public_stage_specs()) {
    auto stage = spec.create();
    ASSERT_NE(stage, nullptr);
    const auto* provider =
        dynamic_cast<const orc::StageToolProvider*>(stage.get());
    if (provider == nullptr) {
      continue;
    }
    for (const auto& tool : provider->get_stage_tools()) {
      if (tool.kind != orc::StageToolKind::CatalogueBrowser) {
        continue;
      }
      ++browsers;
      const auto stage_name = stage->get_node_type_info().stage_name;
      EXPECT_EQ(tool.contract_id, orc::kCatalogueBrowserContractId)
          << "Stage '" << stage_name << "' tool '" << tool.tool_id
          << "' advertises a catalogue browser under the wrong contract id";
      EXPECT_NE(dynamic_cast<orc::ICatalogueResults*>(stage.get()), nullptr)
          << "Stage '" << stage_name
          << "' advertises a catalogue browser but does not implement "
             "orc::ICatalogueResults";
    }
  }
  // The teletext and NABTS sinks are the two in the tree; a zero here means
  // the inventory stopped covering them rather than that the rule holds.
  EXPECT_GE(browsers, 2);
}

TEST(StageRegistryContractTest, UnknownStage_FailsCleanly) {
  auto& registry = orc::StageRegistry::instance();
  EXPECT_THROW(registry.create_stage("unregistered_stage"),
               orc::StageRegistryError);
}

TEST(StageRegistryContractTest, DuplicateRegistration_IsRejected) {
  auto& registry = orc::StageRegistry::instance();
  const auto existing_name =
      public_stage_specs().front().create()->get_node_type_info().stage_name;

  EXPECT_THROW(
      registry.register_stage(
          existing_name,
          [existing_name]() -> orc::DAGStagePtr {
            return orc::StageRegistry::instance().create_stage(existing_name);
          }),
      orc::StageRegistryError);
}

TEST(StageRegistryContractTest, Migrated_StagesLoadFromRuntimePlugins) {
  auto& registry = orc::StageRegistry::instance();
  const auto& loaded_plugins = registry.get_loaded_plugins();

  std::set<std::string> loaded_stage_names;
  for (const auto& plugin : loaded_plugins) {
    for (const auto& stage_name : plugin.registered_stage_names) {
      loaded_stage_names.insert(stage_name);
    }
  }

  EXPECT_TRUE(loaded_stage_names.count("tbc_source") > 0);
  EXPECT_TRUE(loaded_stage_names.count("PAL_CVBS_Source") > 0);
  EXPECT_TRUE(loaded_stage_names.count("NTSC_CVBS_Source") > 0);
  EXPECT_TRUE(loaded_stage_names.count("PAL_M_CVBS_Source") > 0);
  EXPECT_TRUE(loaded_stage_names.count("frame_map") > 0);
  EXPECT_TRUE(loaded_stage_names.count("dropout_map") > 0);
  EXPECT_TRUE(loaded_stage_names.count("dropout_correct") > 0);
  EXPECT_TRUE(loaded_stage_names.count("source_align") > 0);
  EXPECT_TRUE(loaded_stage_names.count("mask_line") > 0);
  EXPECT_TRUE(loaded_stage_names.count("picture_band_limit") > 0);
  EXPECT_TRUE(loaded_stage_names.count("fm_beat_cancel") > 0);
  EXPECT_TRUE(loaded_stage_names.count("efm_audio_decode") > 0);
  EXPECT_TRUE(loaded_stage_names.count("audio_import") > 0);
  EXPECT_TRUE(loaded_stage_names.count("audio_channel_map") > 0);
  EXPECT_TRUE(loaded_stage_names.count("audio_align") > 0);
  EXPECT_TRUE(loaded_stage_names.count("video_params") > 0);
  EXPECT_TRUE(loaded_stage_names.count("stacker") > 0);
  EXPECT_TRUE(loaded_stage_names.count("AudioSink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("CCSink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("video_sink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("daphne_vbi_sink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("tbc_sink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("CVBSSink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("RawEFMSink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("EFMSink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("AC3RFSink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("dropout_analysis_sink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("snr_analysis_sink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("burst_level_analysis_sink") > 0);
  EXPECT_TRUE(loaded_stage_names.count("teletext_sink") > 0);
}

TEST(NodeTypeContractTest, All_PublicStagesAppearInNodeTypeDiscovery) {
  const auto& all_node_types = orc::get_all_node_types();
  std::set<std::string> discovered_names;

  for (const auto& info : all_node_types) {
    discovered_names.insert(info.stage_name);
  }

  for (const auto& spec : public_stage_specs()) {
    if (!spec.registry_backed) {
      continue;
    }
    const auto expected_name = spec.create()->get_node_type_info().stage_name;
    EXPECT_TRUE(discovered_names.count(expected_name) > 0)
        << "Node discovery omitted '" << expected_name << "'";
    EXPECT_NE(orc::get_node_type_info(expected_name), nullptr);
  }
}

TEST(NodeTypeContractTest,
     RepresentativeSourceTransformSinkConnections_AreDiscoverable) {
  const auto chain = find_representative_chain();
  if (!chain.has_value()) {
    FAIL() << "Expected chain to have a value";
    return;
  }

  EXPECT_NE(orc::get_node_type_info(chain->source), nullptr);
  EXPECT_NE(orc::get_node_type_info(chain->middle), nullptr);
  EXPECT_NE(orc::get_node_type_info(chain->sink), nullptr);
  EXPECT_TRUE(orc::is_connection_valid(chain->source, chain->middle));
  EXPECT_TRUE(orc::is_connection_valid(chain->middle, chain->sink));
}

TEST(NodeTypeContractTest, ManyOutputToSingleInput_IsRejectedWhenAvailable) {
  const auto pair = find_many_to_one_pair();
  if (!pair.has_value()) {
    GTEST_SKIP()
        << "No public many-output to single-input pair is currently registered";
  }

  EXPECT_FALSE(orc::is_connection_valid(pair->first, pair->second));
}

TEST(NodeTypeContractTest, UnknownStages_AreRejected) {
  EXPECT_EQ(orc::get_node_type_info("unregistered_stage"), nullptr);
  EXPECT_FALSE(orc::is_connection_valid(
      "unregistered_stage",
      public_stage_specs().front().create()->get_node_type_info().stage_name));
}
}  // namespace orc_unit_test