/*
 * File:        preview_view_registry.cpp
 * Module:      orc-core
 * Purpose:     Preview view registry implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "preview_view_registry.h"

#include <orc/stage/preview/colour_preview_provider.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <utility>

#include "analysis/vectorscope/composite_vectorscope.h"
#include "analysis/vectorscope/vectorscope_analysis.h"
#include "carrier_scope_extraction.h"
#include "orc_histogram.h"
#include "preview_renderer.h"

namespace orc {
namespace {

constexpr const char* kLineScopeViewId = "preview.linescope";
constexpr const char* kFrameTimingViewId = "preview.frame_timing";

bool is_generic_vfr_view_id(const std::string& view_id) {
  return view_id == kLineScopeViewId || view_id == kFrameTimingViewId;
}

const DAGNode* find_node(const DAG& dag, NodeID node_id) {
  const auto& nodes = dag.nodes();
  auto it = std::find_if(
      nodes.begin(), nodes.end(),
      [node_id](const DAGNode& node) { return node.node_id == node_id; });

  if (it == nodes.end()) {
    return nullptr;
  }

  return &(*it);
}

PreviewOutputType output_type_for_data_type(VideoDataType data_type) {
  switch (data_type) {
    case VideoDataType::CompositeNTSC:
    case VideoDataType::CompositePAL:
    case VideoDataType::YC_NTSC:
    case VideoDataType::YC_PAL:
      return PreviewOutputType::Frame_Field1;
    case VideoDataType::ColourNTSC:
    case VideoDataType::ColourPAL:
      return PreviewOutputType::Frame_Field1_First;
  }

  return PreviewOutputType::Frame_Field1;
}

class ImagePreviewView final : public IPreviewView {
 public:
  ImagePreviewView(NodeID node_id, PreviewRenderer* renderer)
      : node_id_(node_id), renderer_(renderer) {}

  std::vector<VideoDataType> supported_data_types() const override {
    return {
        VideoDataType::CompositeNTSC, VideoDataType::CompositePAL,
        VideoDataType::YC_NTSC,       VideoDataType::YC_PAL,
        VideoDataType::ColourNTSC,    VideoDataType::ColourPAL,
    };
  }

  PreviewViewDataResult request_data(
      VideoDataType data_type, const PreviewCoordinate& coordinate) override {
    PreviewViewDataResult result{};
    if (!renderer_) {
      result.error_message = "Preview renderer is not initialized";
      return result;
    }

    if (!coordinate.is_valid()) {
      result.error_message = "Preview coordinate is invalid";
      return result;
    }

    last_type_ = output_type_for_data_type(data_type);
    last_index_ = coordinate.field_index;

    auto rendered = renderer_->render_output(node_id_, last_type_, last_index_);
    if (!rendered.success || !rendered.image.is_valid()) {
      result.error_message = rendered.error_message.empty()
                                 ? "Failed to render preview image"
                                 : rendered.error_message;
      return result;
    }

    result.success = true;
    result.payload_kind = PreviewViewPayloadKind::Image;
    result.image = std::move(rendered.image);
    return result;
  }

  PreviewViewExportResult export_as(const std::string& format,
                                    const std::string& path) const override {
    PreviewViewExportResult result{};

    if (!renderer_) {
      result.error_message = "Preview renderer is not initialized";
      return result;
    }

    if (format != "png") {
      result.error_message = "Unsupported export format for image view";
      return result;
    }

    if (!renderer_->save_png(node_id_, last_type_, last_index_, path)) {
      result.error_message = "Failed to export image as PNG";
      return result;
    }

    result.success = true;
    return result;
  }

 private:
  NodeID node_id_;
  PreviewRenderer* renderer_{nullptr};
  PreviewOutputType last_type_{PreviewOutputType::Frame_Field1};
  uint64_t last_index_{0};
};

class VectorscopePreviewView final : public IPreviewView {
 public:
  VectorscopePreviewView(const DAG* dag, NodeID node_id,
                         PreviewRenderer* renderer)
      : dag_(dag), node_id_(node_id), renderer_(renderer) {}

  // The decoded (grading) acquisition needs a stage that produces colour
  // carriers; the composite (measurement) acquisition needs only a CVBS or
  // Y/C carrier, which every signal-domain stage has.  Both are offered on
  // every type so the dialog's mode selector is reachable wherever either can
  // be satisfied.
  std::vector<VideoDataType> supported_data_types() const override {
    return {
        VideoDataType::CompositeNTSC, VideoDataType::CompositePAL,
        VideoDataType::YC_NTSC,       VideoDataType::YC_PAL,
        VideoDataType::ColourNTSC,    VideoDataType::ColourPAL,
    };
  }

  PreviewViewDataResult request_data(
      VideoDataType data_type, const PreviewCoordinate& coordinate) override {
    PreviewViewDataResult result{};

    if (!dag_) {
      result.error_message = "DAG is not initialized";
      return result;
    }

    if (!coordinate.is_valid()) {
      result.error_message = "Preview coordinate is invalid";
      return result;
    }

    // The acquisition follows the data type rather than a caller's choice:
    // a colour-domain output has decoder planes to plot, a signal-domain one
    // has a carrier to demodulate, and neither can stand in for the other.
    if (data_type != VideoDataType::ColourNTSC &&
        data_type != VideoDataType::ColourPAL) {
      return request_composite_data(coordinate);
    }

    const DAGNode* node = find_node(*dag_, node_id_);
    if (!node || !node->stage) {
      result.error_message = "Target node not found";
      return result;
    }

    auto* provider =
        dynamic_cast<const IColourPreviewProvider*>(node->stage.get());
    if (!provider) {
      result.error_message = "Stage does not provide colour preview carriers";
      return result;
    }

    auto carrier_opt =
        provider->get_colour_preview_carrier(coordinate.field_index);
    if (!carrier_opt.has_value()) {
      result.error_message = "Failed to fetch colour preview carrier";
      return result;
    }

    const PreviewScopeSelection selection{
        coordinate.vectorscope_active_area_only,
        coordinate.vectorscope_first_line, coordinate.vectorscope_last_line};

    last_vectorscope_ = extract_vectorscope_from_carrier(
        *carrier_opt, selection, coordinate.field_index);

    if (!last_vectorscope_.has_value()) {
      result.error_message =
          "Vectorscope data is not available for requested frame";
      return result;
    }

    result.success = true;
    result.payload_kind = PreviewViewPayloadKind::Vectorscope;
    result.vectorscope = last_vectorscope_;
    return result;
  }

  PreviewViewExportResult export_as(const std::string& format,
                                    const std::string& path) const override {
    PreviewViewExportResult result{};

    if (format != "csv") {
      result.error_message = "Unsupported export format for vectorscope view";
      return result;
    }

    if (!last_vectorscope_.has_value()) {
      result.error_message =
          "No vectorscope data has been requested for export";
      return result;
    }

    std::ofstream out(path);
    if (!out.is_open()) {
      result.error_message = "Failed to open export path";
      return result;
    }

    out << "u,v,field_id,line,sample_class,line_phase\n";
    for (const auto& sample : last_vectorscope_->samples) {
      out << sample.u << ',' << sample.v << ','
          << static_cast<int>(sample.field_id) << ',' << sample.line_number
          << ',' << static_cast<int>(sample.sample_class) << ','
          << static_cast<int>(sample.line_phase) << '\n';
    }

    out.flush();
    if (!out.good()) {
      result.error_message = "Failed while writing vectorscope CSV";
      return result;
    }

    result.success = true;
    return result;
  }

 private:
  // Composite (measurement) acquisition: demodulate the CVBS (or Y/C chroma)
  // carrier the stage produces, with no decoder in the path.  For a
  // signal-domain data type PreviewCoordinate::field_index addresses the frame
  // to acquire.
  PreviewViewDataResult request_composite_data(
      const PreviewCoordinate& coordinate) {
    PreviewViewDataResult result{};

    if (!renderer_) {
      result.error_message = "Preview renderer is not initialized";
      return result;
    }

    auto representation = renderer_->get_representation_at_node(node_id_);
    if (!representation) {
      result.error_message =
          "Stage does not provide a composite carrier to measure";
      return result;
    }

    const FrameID frame_id = static_cast<FrameID>(coordinate.field_index);
    if (!representation->has_frame(frame_id)) {
      result.error_message = "Requested frame is not available at this stage";
      return result;
    }

    CompositeVectorscopeOptions options;
    options.window = coordinate.vectorscope_window;
    options.active_lines_only = coordinate.vectorscope_active_area_only;
    options.first_line = coordinate.vectorscope_first_line;
    options.last_line = coordinate.vectorscope_last_line;

    auto data =
        extract_composite_vectorscope(*representation, frame_id, options);
    if (!data.has_value() || data->samples.empty()) {
      result.error_message =
          "Composite vectorscope data is not available for requested frame";
      return result;
    }

    last_vectorscope_ = std::move(data);
    result.success = true;
    result.payload_kind = PreviewViewPayloadKind::Vectorscope;
    result.vectorscope = last_vectorscope_;
    return result;
  }

  const DAG* dag_{nullptr};
  NodeID node_id_;
  PreviewRenderer* renderer_{nullptr};
  std::optional<VectorscopeData> last_vectorscope_;
};

class HistogramPreviewView final : public IPreviewView {
 public:
  HistogramPreviewView(const DAG* dag, NodeID node_id)
      : dag_(dag), node_id_(node_id) {}

  std::vector<VideoDataType> supported_data_types() const override {
    return {
        VideoDataType::ColourNTSC,
        VideoDataType::ColourPAL,
    };
  }

  PreviewViewDataResult request_data(
      VideoDataType /*data_type*/,
      const PreviewCoordinate& coordinate) override {
    PreviewViewDataResult result{};

    if (!dag_) {
      result.error_message = "DAG is not initialized";
      return result;
    }

    if (!coordinate.is_valid()) {
      result.error_message = "Preview coordinate is invalid";
      return result;
    }

    const DAGNode* node = find_node(*dag_, node_id_);
    if (!node || !node->stage) {
      result.error_message = "Target node not found";
      return result;
    }

    auto* provider =
        dynamic_cast<const IColourPreviewProvider*>(node->stage.get());
    if (!provider) {
      result.error_message = "Stage does not provide colour preview carriers";
      return result;
    }

    auto carrier_opt =
        provider->get_colour_preview_carrier(coordinate.field_index);
    if (!carrier_opt.has_value()) {
      result.error_message = "Failed to fetch colour preview carrier";
      return result;
    }

    last_histogram_ =
        extract_histogram_from_carrier(*carrier_opt, coordinate.field_index);

    result.success = true;
    result.payload_kind = PreviewViewPayloadKind::Histogram;
    result.histogram = last_histogram_;
    return result;
  }

  PreviewViewExportResult export_as(const std::string& format,
                                    const std::string& path) const override {
    PreviewViewExportResult result{};

    if (format != "csv") {
      result.error_message = "Unsupported export format for histogram view";
      return result;
    }

    if (!last_histogram_.has_value()) {
      result.error_message = "No histogram data has been requested for export";
      return result;
    }

    std::ofstream out(path);
    if (!out.is_open()) {
      result.error_message = "Failed to open export path";
      return result;
    }

    out << "bin,y,u,v,i,q\n";
    for (size_t b = 0; b < VideoHistogramData::kBinCount; ++b) {
      out << b << ',' << last_histogram_->y_bins[b] << ','
          << last_histogram_->u_bins[b] << ',' << last_histogram_->v_bins[b]
          << ',' << last_histogram_->i_bins[b] << ','
          << last_histogram_->q_bins[b] << '\n';
    }

    out.flush();
    if (!out.good()) {
      result.error_message = "Failed while writing histogram CSV";
      return result;
    }

    result.success = true;
    return result;
  }

 private:
  const DAG* dag_{nullptr};
  NodeID node_id_;
  std::optional<VideoHistogramData> last_histogram_;
};

class GenericVfrVisualizationPreviewView final : public IPreviewView {
 public:
  explicit GenericVfrVisualizationPreviewView(std::string visualization_name)
      : visualization_name_(std::move(visualization_name)) {}

  std::vector<VideoDataType> supported_data_types() const override {
    return {
        VideoDataType::CompositeNTSC, VideoDataType::CompositePAL,
        VideoDataType::YC_NTSC,       VideoDataType::YC_PAL,
        VideoDataType::ColourNTSC,    VideoDataType::ColourPAL,
    };
  }

  PreviewViewDataResult request_data(VideoDataType,
                                     const PreviewCoordinate&) override {
    PreviewViewDataResult result{};
    result.error_message =
        visualization_name_ +
        " is driven by dedicated requests and has no direct preview payload";
    return result;
  }

  PreviewViewExportResult export_as(const std::string&,
                                    const std::string&) const override {
    PreviewViewExportResult result{};
    result.error_message = visualization_name_ +
                           " does not support export via preview view registry";
    return result;
  }

 private:
  std::string visualization_name_;
};

}  // namespace

bool PreviewViewRegistry::register_view(PreviewViewDescriptor descriptor,
                                        ViewFactory factory) {
  if (descriptor.id.empty() || !factory) {
    return false;
  }

  const std::string view_id = descriptor.id;

  if (entries_.find(view_id) != entries_.end()) {
    return false;
  }

  entries_.emplace(view_id, Entry{std::move(descriptor), std::move(factory)});
  return true;
}

bool PreviewViewRegistry::unregister_view(const std::string& view_id) {
  if (view_id.empty()) {
    return false;
  }

  size_t removed_entries = entries_.erase(view_id);
  if (removed_entries == 0) {
    return false;
  }

  for (auto it = view_cache_.begin(); it != view_cache_.end();) {
    if (it->first.view_id == view_id) {
      it = view_cache_.erase(it);
    } else {
      ++it;
    }
  }

  return true;
}

std::vector<PreviewViewDescriptor> PreviewViewRegistry::list_views() const {
  std::vector<PreviewViewDescriptor> result;
  result.reserve(entries_.size());

  for (const auto& entry : entries_) {
    result.push_back(entry.second.descriptor);
  }

  std::sort(result.begin(), result.end(),
            [](const PreviewViewDescriptor& a, const PreviewViewDescriptor& b) {
              return a.id < b.id;
            });

  return result;
}

std::vector<PreviewViewDescriptor> PreviewViewRegistry::get_applicable_views(
    const DAG& dag, NodeID node_id, VideoDataType data_type) const {
  std::vector<PreviewViewDescriptor> result;

  const DAGNode* node = find_node(dag, node_id);
  if (!node || !node->stage) {
    return result;
  }

  const bool supports_stage_data_type =
      node_supports_data_type(dag, node_id, data_type);

  for (const auto& entry : entries_) {
    if (is_generic_vfr_view_id(entry.second.descriptor.id)) {
      result.push_back(entry.second.descriptor);
      continue;
    }

    if (contains_data_type(entry.second.descriptor.supported_data_types,
                           data_type) &&
        supports_stage_data_type) {
      result.push_back(entry.second.descriptor);
    }
  }

  std::sort(result.begin(), result.end(),
            [](const PreviewViewDescriptor& a, const PreviewViewDescriptor& b) {
              return a.id < b.id;
            });

  return result;
}

PreviewViewDataResult PreviewViewRegistry::request_data(
    const DAG& dag, NodeID node_id, const std::string& view_id,
    VideoDataType data_type, const PreviewCoordinate& coordinate) {
  PreviewViewDataResult result{};

  const DAGNode* node = find_node(dag, node_id);
  if (!node || !node->stage) {
    result.error_message = "Target node not found";
    return result;
  }

  auto entry_it = entries_.find(view_id);
  if (entry_it == entries_.end()) {
    result.error_message = "Preview view is not registered";
    return result;
  }

  if (!is_generic_vfr_view_id(view_id)) {
    if (!contains_data_type(entry_it->second.descriptor.supported_data_types,
                            data_type)) {
      result.error_message =
          "Preview view does not support requested data type";
      return result;
    }

    if (!node_supports_data_type(dag, node_id, data_type)) {
      result.error_message =
          "Requested data type is not supported by the stage";
      return result;
    }
  }

  NodeViewKey key{node_id, view_id};
  auto cache_it = view_cache_.find(key);
  if (cache_it == view_cache_.end()) {
    auto instance = create_view(node_id, view_id);
    if (!instance) {
      result.error_message = "Failed to construct preview view";
      return result;
    }

    cache_it = view_cache_.emplace(std::move(key), std::move(instance)).first;
  }

  return cache_it->second->request_data(data_type, coordinate);
}

PreviewViewExportResult PreviewViewRegistry::export_as(
    NodeID node_id, const std::string& view_id, const std::string& format,
    const std::string& path) const {
  PreviewViewExportResult result{};

  NodeViewKey key{node_id, view_id};
  auto it = view_cache_.find(key);
  if (it == view_cache_.end()) {
    result.error_message = "Preview view has no cached request data";
    return result;
  }

  return it->second->export_as(format, path);
}

void PreviewViewRegistry::clear_cache_for_node(NodeID node_id) {
  for (auto it = view_cache_.begin(); it != view_cache_.end();) {
    if (it->first.node_id == node_id) {
      it = view_cache_.erase(it);
    } else {
      ++it;
    }
  }
}

void PreviewViewRegistry::register_default_views(
    PreviewViewRegistry& registry, const std::shared_ptr<const DAG>& dag,
    PreviewRenderer* renderer) {
  registry.register_view(
      PreviewViewDescriptor{
          "preview.image",
          "Preview Image",
          {
              VideoDataType::CompositeNTSC,
              VideoDataType::CompositePAL,
              VideoDataType::YC_NTSC,
              VideoDataType::YC_PAL,
              VideoDataType::ColourNTSC,
              VideoDataType::ColourPAL,
          },
      },
      [renderer](NodeID node_id) {
        return std::make_unique<ImagePreviewView>(node_id, renderer);
      });

  registry.register_view(
      PreviewViewDescriptor{
          "preview.vectorscope",
          "Vectorscope",
          {
              VideoDataType::CompositeNTSC,
              VideoDataType::CompositePAL,
              VideoDataType::YC_NTSC,
              VideoDataType::YC_PAL,
              VideoDataType::ColourNTSC,
              VideoDataType::ColourPAL,
          },
      },
      [dag, renderer](NodeID node_id) {
        return std::make_unique<VectorscopePreviewView>(dag.get(), node_id,
                                                        renderer);
      });

  registry.register_view(
      PreviewViewDescriptor{
          "preview.histogram",
          "Video Histogram",
          {
              VideoDataType::ColourNTSC,
              VideoDataType::ColourPAL,
          },
      },
      [dag](NodeID node_id) {
        return std::make_unique<HistogramPreviewView>(dag.get(), node_id);
      });

  registry.register_view(
      PreviewViewDescriptor{
          kLineScopeViewId,
          "Line Scope",
          {
              VideoDataType::CompositeNTSC,
              VideoDataType::CompositePAL,
              VideoDataType::YC_NTSC,
              VideoDataType::YC_PAL,
              VideoDataType::ColourNTSC,
              VideoDataType::ColourPAL,
          },
      },
      [](NodeID) {
        return std::make_unique<GenericVfrVisualizationPreviewView>(
            "Line scope");
      });

  registry.register_view(
      PreviewViewDescriptor{
          kFrameTimingViewId,
          "Frame Timing",
          {
              VideoDataType::CompositeNTSC,
              VideoDataType::CompositePAL,
              VideoDataType::YC_NTSC,
              VideoDataType::YC_PAL,
              VideoDataType::ColourNTSC,
              VideoDataType::ColourPAL,
          },
      },
      [](NodeID) {
        return std::make_unique<GenericVfrVisualizationPreviewView>(
            "Frame timing");
      });
}

size_t PreviewViewRegistry::NodeViewKeyHash::operator()(
    const NodeViewKey& key) const {
  const size_t node_hash = std::hash<NodeID>{}(key.node_id);
  const size_t id_hash = std::hash<std::string>{}(key.view_id);
  return node_hash ^ (id_hash << 1);
}

bool PreviewViewRegistry::node_supports_data_type(const DAG& dag,
                                                  NodeID node_id,
                                                  VideoDataType data_type) {
  const DAGNode* node = find_node(dag, node_id);
  if (!node || !node->stage) {
    return false;
  }

  auto* capability_stage =
      dynamic_cast<const IStagePreviewCapability*>(node->stage.get());
  if (!capability_stage) {
    return false;
  }

  const StagePreviewCapability capability =
      capability_stage->get_preview_capability();
  if (!capability.is_valid()) {
    return false;
  }

  return contains_data_type(capability.supported_data_types, data_type);
}

bool PreviewViewRegistry::contains_data_type(
    const std::vector<VideoDataType>& list, VideoDataType data_type) {
  return std::find(list.begin(), list.end(), data_type) != list.end();
}

std::unique_ptr<IPreviewView> PreviewViewRegistry::create_view(
    NodeID node_id, const std::string& view_id) const {
  auto it = entries_.find(view_id);
  if (it == entries_.end() || !it->second.factory) {
    return nullptr;
  }

  return it->second.factory(node_id);
}

}  // namespace orc
