/*
 * File:        named_pipe_detection_fs_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     Named pipes (FIFOs) are treated as non-seekable streams by pipe
 *              validation and the GUI execution guard, and can be split
 *              between several sinks, like "-"
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
#include <orc/support/pipe_io.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
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

// A streaming-compatible source with one FILE_PATH input that declares the
// reserved reader count, so it can split a pipe input between sinks.
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

// The same source without the reserved reader count: it can only read its
// input directly.
class PlainFifoSourceStage : public FifoSourceStage {
 public:
  NodeTypeInfo get_node_type_info() const override {
    auto info = FifoSourceStage::get_node_type_info();
    info.stage_name = "functional_test_plain_fifo_source";
    return info;
  }
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem system, SourceType source_type) const override {
    auto descriptors =
        FifoSourceStage::get_parameter_descriptors(system, source_type);
    descriptors.resize(1);  // input_path only
    return descriptors;
  }
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
    if (!registry.has_stage("functional_test_plain_fifo_source")) {
      registry.register_stage("functional_test_plain_fifo_source", [] {
        return std::make_shared<PlainFifoSourceStage>();
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

struct FanOut {
  Project project;
  NodeID src;
  NodeID sink1;
  NodeID sink2;
};

// One source reading `input` and feeding two sinks.
FanOut make_fan_out(const std::string& source_stage, const std::string& input,
                    const std::filesystem::path& dir) {
  register_test_stages();
  FanOut f{project_io::create_empty_project("fifo-fan-out"), {}, {}, {}};
  f.src = project_io::add_node(f.project, source_stage, 0, 0);
  f.sink1 = project_io::add_node(f.project, "functional_test_file_sink", 1, 0);
  f.sink2 = project_io::add_node(f.project, "functional_test_file_sink", 1, 1);
  project_io::set_node_parameters(f.project, f.src,
                                  {{"input_path", ParameterValue{input}}});
  project_io::set_node_parameters(
      f.project, f.sink1,
      {{"output_path", ParameterValue{(dir / "a.bin").string()}}});
  project_io::set_node_parameters(
      f.project, f.sink2,
      {{"output_path", ParameterValue{(dir / "b.bin").string()}}});
  project_io::add_edge(f.project, f.src, f.sink1);
  project_io::add_edge(f.project, f.src, f.sink2);
  return f;
}

// A source that splits its pipe input feeds both sinks from one FIFO, as it
// would from stdin: validation passes, the sinks are grouped to run side by
// side, and each source instance is told there are two readers.
TEST_F(NamedPipeDetection, FanOutFromAFifo_Passes_WhenTheSourceShares) {
  auto f = make_fan_out("functional_test_fifo_source", fifo_.string(), dir_);

  presenters::ProjectPresenter presenter(static_cast<void*>(&f.project));
  EXPECT_TRUE(presenter.validatePipeExecution().empty());

  const auto groups = shared_pipe_sink_groups(f.project);
  ASSERT_EQ(groups.size(), 1u);
  EXPECT_EQ(groups[0], (std::vector<NodeID>{f.sink1, f.sink2}));

  const auto dag = project_to_dag(f.project);
  const auto& nodes = dag->nodes();
  const auto src_node =
      std::find_if(nodes.begin(), nodes.end(),
                   [&](const DAGNode& n) { return n.node_id == f.src; });
  ASSERT_NE(src_node, nodes.end());
  const auto readers = src_node->parameters.find(kStreamReaderCountParameter);
  ASSERT_NE(readers, src_node->parameters.end());
  EXPECT_EQ(std::get<uint32_t>(readers->second), 2u);
}

// A source that reads the FIFO directly cannot give each sink its own copy.
TEST_F(NamedPipeDetection,
       FanOutFromAFifo_IsRejected_WhenTheSourceCannotShare) {
  auto f =
      make_fan_out("functional_test_plain_fifo_source", fifo_.string(), dir_);

  presenters::ProjectPresenter presenter(static_cast<void*>(&f.project));
  const auto errors = presenter.validatePipeExecution();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].find("only be consumed once"), std::string::npos)
      << errors[0];
  EXPECT_TRUE(shared_pipe_sink_groups(f.project).empty());
}

// The split itself, on a real FIFO: two readers of the same path each get
// every byte a writer puts through it, although the FIFO hands each byte to
// only one opener.
TEST_F(NamedPipeDetection, OpenPipeReader_SplitsAFifoBetweenReaders) {
  std::string bytes(200'003, '\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>((i * 31 + i / 251) & 0xFF);
  }

  std::thread writer([&] {
    std::ofstream out(fifo_, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  });

  std::string results[2];
  std::thread readers[2];
  for (int i = 0; i < 2; ++i) {
    readers[i] = std::thread([&, i] {
      auto in = pipe_io::open_pipe_reader(fifo_.string(), 2);
      if (!in) return;
      results[i].assign(std::istreambuf_iterator<char>(*in),
                        std::istreambuf_iterator<char>());
    });
  }
  for (auto& reader : readers) reader.join();
  writer.join();

  EXPECT_EQ(results[0], bytes);
  EXPECT_EQ(results[1], bytes);
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
