/*
 * File:        named_pipe_detection_fs_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     Named pipes (FIFOs) are treated as non-seekable streams by pipe
 *              validation and the GUI execution guard, like "-"
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * Functional (not unit): creates real FIFOs on disk (AGENTS.md §4.2). POSIX
 * only — Windows has no FIFOs, and every test skips itself there.
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/streaming_capability.h>
#include <orc/stage/triggerable_stage.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "project.h"
#include "project_presenter.h"
#include "project_to_dag.h"
#include "stage_registry.h"

namespace orc {
namespace {

class DummyArtifact : public Artifact {
 public:
  DummyArtifact() : Artifact(ArtifactID("named-pipe-artifact"), Provenance{}) {}
  std::string type_name() const override { return "DummyArtifact"; }
};

// A streaming-compatible source with one FILE_PATH input. It declares the
// shared-stdin reader count, to show that a named pipe is still limited to
// one sink even for a source that can split stdin.
class FifoSourceStage : public DAGStage,
                        public ParameterizedStage,
                        public IStreamingCompatibility {
 public:
  std::string version() const override { return "1.0"; }
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::SOURCE,
                        "functional_test_fifo_source",
                        "FIFO Source (test-only)",
                        "Test-only stage",
                        0,
                        0,
                        1,
                        UINT32_MAX,
                        VideoFormatCompatibility::ALL};
  }
  std::vector<ArtifactPtr> execute(const std::vector<ArtifactPtr>&,
                                   const std::map<std::string, ParameterValue>&,
                                   ObservationContext&) override {
    return {std::make_shared<DummyArtifact>()};
  }
  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem, SourceType) const override {
    ParameterDescriptor path;
    path.name = "input_path";
    path.type = ParameterType::FILE_PATH;
    ParameterDescriptor readers;
    readers.name = kStreamReaderCountParameter;
    readers.type = ParameterType::UINT32;
    readers.constraints.default_value = static_cast<uint32_t>(1);
    return {path, readers};
  }
  std::map<std::string, ParameterValue> get_parameters() const override {
    return {{"input_path", input_path_}};
  }
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override {
    if (auto it = params.find("input_path");
        it != params.end() && std::holds_alternative<std::string>(it->second)) {
      input_path_ = std::get<std::string>(it->second);
    }
    return true;
  }
  bool supports_streaming_execution() const override { return true; }

 private:
  std::string input_path_;
};

class FileSinkStage : public DAGStage,
                      public ParameterizedStage,
                      public TriggerableStage,
                      public IStreamingCompatibility {
 public:
  std::string version() const override { return "1.0"; }
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::SINK,
                        "functional_test_file_sink",
                        "File Sink (test-only)",
                        "Test-only stage",
                        1,
                        1,
                        0,
                        0,
                        VideoFormatCompatibility::ALL};
  }
  std::vector<ArtifactPtr> execute(const std::vector<ArtifactPtr>&,
                                   const std::map<std::string, ParameterValue>&,
                                   ObservationContext&) override {
    return {};
  }
  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 0; }
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem, SourceType) const override {
    ParameterDescriptor path;
    path.name = "output_path";
    path.type = ParameterType::FILE_PATH;
    return {path};
  }
  std::map<std::string, ParameterValue> get_parameters() const override {
    return {{"output_path", output_path_}};
  }
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override {
    if (auto it = params.find("output_path");
        it != params.end() && std::holds_alternative<std::string>(it->second)) {
      output_path_ = std::get<std::string>(it->second);
    }
    return true;
  }
  bool trigger(const std::vector<ArtifactPtr>&,
               const std::map<std::string, ParameterValue>&,
               IObservationContext&) override {
    return true;
  }
  std::string get_trigger_status() const override { return "ok"; }
  bool supports_streaming_execution() const override { return true; }

 private:
  std::string output_path_;
};

void register_test_stages() {
  static const bool registered = [] {
    auto& registry = StageRegistry::instance();
    if (!registry.has_stage("functional_test_fifo_source")) {
      registry.register_stage("functional_test_fifo_source", [] {
        return std::make_shared<FifoSourceStage>();
      });
    }
    if (!registry.has_stage("functional_test_file_sink")) {
      registry.register_stage("functional_test_file_sink",
                              [] { return std::make_shared<FileSinkStage>(); });
    }
    return true;
  }();
  (void)registered;
}

// A per-test temporary directory holding one FIFO and one regular file.
class NamedPipeDetection : public ::testing::Test {
 protected:
  void SetUp() override {
#if defined(_WIN32)
    GTEST_SKIP() << "Named pipes (FIFOs) are POSIX only";
#else
    dir_ =
        std::filesystem::temp_directory_path() /
        ("orc-named-pipe-test-" +
         std::string(
             ::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    fifo_ = dir_ / "capture.fifo";
    ASSERT_EQ(::mkfifo(fifo_.c_str(), 0600), 0) << "mkfifo failed";
    regular_ = dir_ / "capture.cvbs";
    std::ofstream(regular_).put('x');
#endif
  }

  void TearDown() override {
    std::error_code ec;
    if (!dir_.empty()) std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::filesystem::path fifo_;
  std::filesystem::path regular_;
};

TEST_F(NamedPipeDetection, IsStreamTarget_RecognisesAFifo) {
  EXPECT_TRUE(is_stream_target(fifo_.string(), ""));
  EXPECT_FALSE(is_stream_target(regular_.string(), ""));
}

// A project file stores paths relative to its own directory; the FIFO must be
// found the same way execution resolves it.
TEST_F(NamedPipeDetection, IsStreamTarget_ResolvesRelativeToProjectRoot) {
  EXPECT_TRUE(is_stream_target("capture.fifo", dir_.string()));
  EXPECT_FALSE(is_stream_target("capture.cvbs", dir_.string()));
}

// The GUI refuses to preview or trigger a node whose graph reads a FIFO, as
// it does for "-": the read would block the GUI process.
TEST_F(NamedPipeDetection, GuiGuard_FlagsAFifoParameter) {
  EXPECT_TRUE(node_parameters_target_pipe_or_network(
      {{"input_path", ParameterValue{fifo_.string()}}}));
  EXPECT_FALSE(node_parameters_target_pipe_or_network(
      {{"input_path", ParameterValue{regular_.string()}}}));
}

// A named pipe can be read once and, unlike stdin, is not split between
// sinks: two sinks depending on it are refused before anything runs.
TEST_F(NamedPipeDetection, ValidatePipeExecution_RejectsFanOutFromAFifo) {
  register_test_stages();
  auto project = project_io::create_empty_project("fifo-fan-out");
  auto src = project_io::add_node(project, "functional_test_fifo_source", 0, 0);
  auto sink1 = project_io::add_node(project, "functional_test_file_sink", 1, 0);
  auto sink2 = project_io::add_node(project, "functional_test_file_sink", 1, 1);
  project_io::set_node_parameters(
      project, src, {{"input_path", ParameterValue{fifo_.string()}}});
  project_io::set_node_parameters(
      project, sink1,
      {{"output_path", ParameterValue{(dir_ / "a.bin").string()}}});
  project_io::set_node_parameters(
      project, sink2,
      {{"output_path", ParameterValue{(dir_ / "b.bin").string()}}});
  project_io::add_edge(project, src, sink1);
  project_io::add_edge(project, src, sink2);

  presenters::ProjectPresenter presenter(static_cast<void*>(&project));
  const auto errors = presenter.validatePipeExecution();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].find("only be consumed once"), std::string::npos)
      << errors[0];
}

TEST_F(NamedPipeDetection, ValidatePipeExecution_AcceptsAFifoFeedingOneSink) {
  register_test_stages();
  auto project = project_io::create_empty_project("fifo-single");
  auto src = project_io::add_node(project, "functional_test_fifo_source", 0, 0);
  auto sink = project_io::add_node(project, "functional_test_file_sink", 1, 0);
  project_io::set_node_parameters(
      project, src, {{"input_path", ParameterValue{fifo_.string()}}});
  project_io::set_node_parameters(
      project, sink,
      {{"output_path", ParameterValue{(dir_ / "a.bin").string()}}});
  project_io::add_edge(project, src, sink);

  presenters::ProjectPresenter presenter(static_cast<void*>(&project));
  EXPECT_TRUE(presenter.validatePipeExecution().empty());
}

}  // namespace
}  // namespace orc
